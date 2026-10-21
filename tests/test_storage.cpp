// Characterization tests for Storage over no device (docs/PLAN.md section 9): the manager thread is not started,
// and each test drives the Slot map through StorageTestAccess. The device and mount paths do not exist, so
// nothing is mounted, formatted, or removed; a Slot's own threads probe a device that is not there, which is
// enough for the lifecycle under test. Such a Slot admits no task, so what closing its admission does to the
// pending tasks bound to it is pinned by
// `closing_admission_retires_unclaimed_tasks_and_the_thread_bodies_serve_both_channels_at_once`
// (`tests/test_channel_worker.cpp`).

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "vigil/configs.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/slot.hpp"
#include "vigil/storage.hpp"
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"

namespace vigil::storage {

struct StorageTestAccess
{
    static void add(Storage &storage, const std::shared_ptr<Slot> &slot)
    {
        const std::unique_lock lock(storage._slots_mutex);
        storage._slots.emplace(slot->devPath().string(), slot);
    }

    static bool holds(const Storage &storage, const std::string &dev_path)
    {
        const std::shared_lock lock(storage._slots_mutex);
        return storage._slots.contains(dev_path);
    }

    // The removal branch of the netlink pass, for one removed device.
    static void remove(Storage &storage, const std::string &dev_path)
    {
        storage.removeSlot(dev_path);
    }
};

} // namespace vigil::storage

namespace vigil::slot {

struct SlotTestAccess
{
    // What a channel worker does at its consecutive failure limit.
    static void markFailed(Slot &slot)
    {
        slot.markFailed();
    }

    // READY and registered by hand, for a Slot that was never enabled, so that no thread of its own moves its
    // state: what the manager thread and `enable` do for a Slot over a device.
    static void openReady(Slot &slot)
    {
        slot._state.store(SlotState::Ready, std::memory_order_release);
        slot._registration.store(slot._ctx.slot_admission.open(slot), std::memory_order_release);
    }
};

} // namespace vigil::slot

using namespace vigil;
using namespace vigil::configs;
using namespace vigil::slot;
using namespace vigil::storage;
using namespace vigil::tests;
using namespace vigil::types;

namespace {

std::string MakeTempDir()
{
    std::string path = (std::filesystem::temp_directory_path() / "vigil-test-XXXXXX").string();
    if (::mkdtemp(path.data()) == nullptr)
        throw std::runtime_error("mkdtemp failed");
    return path;
}

struct Fixture
{
    std::string root;
    std::string dev;
    std::string mount;
    vg_config_t config;
    std::shared_ptr<Context> ctx;

    Fixture()
        : root(MakeTempDir()), dev(root + "/no-such-dev"), mount(root + "/no-such-mnt"),
          config(MakeFakeConfig(root.c_str()))
    {
        ctx = std::make_shared<Context>(config);
    }

    ~Fixture()
    {
        ctx.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::shared_ptr<Slot> makeSlot() const
    {
        return std::make_shared<Slot>(*ctx, dev, mount, /*removable*/ true, /*internal*/ false, "DCIM");
    }

    // Whether `slot` is the Slot registered for admission (null: none is).
    bool registered(const Slot *slot) const
    {
        return ctx->slot_admission.with([slot](const Context::SlotAdmission *p) { return p == slot; });
    }
};

} // namespace

VG_TEST_CASE(a_removed_slot_is_disabled_and_its_admission_closed_before_the_map_drops_it)
{
    // Hot-unplug: the removal branch disables the Slot, which closes its admission and joins its threads, before
    // the map's reference goes, as eject does. The test's own reference stands for a copy of the map taken by
    // getStatus on another thread, so the map's reference is not the last one; and the Slot is unregistered when
    // the branch returns, so a replacement opens the only registration.
    Fixture f;
    Storage storage(StorageConfig(std::string(f.dev), std::string()), *f.ctx, /*run_manager_thread*/ false);
    auto slot = f.makeSlot();
    StorageTestAccess::add(storage, slot);
    VG_REQUIRE(slot->enable() == 0);
    VG_CHECK(slot->isEnabled());
    const auto registration = slot->registration();
    VG_CHECK(registration != 0);
    VG_CHECK(f.registered(slot.get()));
    VG_CHECK_EQ(storage.getStatus().slots_status.size(), size_t { 1 });

    StorageTestAccess::remove(storage, f.dev);
    VG_CHECK(!StorageTestAccess::holds(storage, f.dev));
    VG_CHECK_EQ(storage.getStatus().slots_status.size(), size_t { 0 });
    VG_CHECK(!slot->isEnabled());
    VG_CHECK_EQ(slot->registration(), uint64_t { 0 });
    VG_CHECK(f.registered(nullptr));

    auto next = f.makeSlot(); // the same device, plugged in again
    StorageTestAccess::add(storage, next);
    VG_REQUIRE(next->enable() == 0);
    VG_CHECK(next->registration() > registration);
    VG_CHECK(f.registered(next.get()));

    // The removed Slot's last reference goes on another thread, which joins nothing: its threads are gone.
    std::thread([s = std::move(slot)]() mutable { s.reset(); }).join();
    VG_CHECK(f.registered(next.get()));

    StorageTestAccess::remove(storage, f.dev);
    VG_CHECK(!next->isEnabled());
    VG_CHECK(f.registered(nullptr));
    StorageTestAccess::remove(storage, f.dev); // unknown by now: nothing to do
    VG_CHECK_EQ(storage.getStatus().slots_status.size(), size_t { 0 });
}

VG_TEST_CASE(a_failed_slot_stays_enabled_and_registered_until_removal)
{
    // The consecutive failure limit (docs/PLAN.md section 7.2): the worker marks the Slot failed before it
    // completes its task, and the Slot's admission stays registered as before (rule info keeps reporting the
    // Slot) while the manager's next poll takes the state to ERROR. The Slot stays enabled until a person
    // recovers it, and removal, like disable, closes the admission. Over no device the Slot is never READY, so
    // what the mark refuses and cancels is pinned by the next case.
    Fixture f;
    Storage storage(StorageConfig(std::string(f.dev), std::string()), *f.ctx, /*run_manager_thread*/ false);
    auto slot = f.makeSlot();
    StorageTestAccess::add(storage, slot);
    VG_REQUIRE(slot->enable() == 0);
    const auto registration = slot->registration();
    VG_CHECK(registration != 0);

    SlotTestAccess::markFailed(*slot);
    VG_CHECK(f.registered(slot.get()));
    VG_CHECK_EQ(slot->registration(), registration);
    VG_CHECK(!slot->isReadyToWrite());
    VG_CHECK(!f.ctx->slot_admission.isReadyToWrite());
    VG_CHECK(slot->isEnabled());

    StorageTestAccess::remove(storage, f.dev);
    VG_CHECK(!slot->isEnabled());
    VG_CHECK(f.registered(nullptr));
}

VG_TEST_CASE(a_failed_slot_refuses_admission_at_once_and_cancels_what_it_admitted)
{
    // Both halves of the mark on the Slot's side, on one thread: a Slot that was never enabled, so that no thread
    // of its own moves its state, is set READY and registered by hand over an existing directory as its mount
    // path, which is how an enabled Slot admits (docs/PLAN.md sections 7.2 and 8.1). It admits a Photo task; the
    // mark cancels that task, the Slot and the registry refuse from the mark on, and the Slot stays registered.
    // Premise: the temporary filesystem has more than 532 MiB free, one Photo's critical mark, as the capture
    // endpoint tests need, since the admission probes the mount path's free space.
    Fixture f;
    const auto mount = f.root + "/mnt";
    VG_REQUIRE(std::filesystem::create_directory(mount));
    const RecordConfig cfg(nlohmann::json::parse(R"({"iSchemaVersion":2,"bRuleEnabled":true,
        "bContinuousRecordEnabled":false,"dActions":{"dPhoto":{"bEnabled":true,"sFormat":"JPG","iIntervalMs":5000},
        "dVideo":{"bEnabled":false,"iIntervalMs":5000},"dSound":{"bEnabled":false}}})"),
        *f.ctx);
    VG_REQUIRE(cfg.sync(*f.ctx, cfg.prepare(*f.ctx)) == 0);
    auto slot = std::make_shared<Slot>(*f.ctx, f.dev, mount, /*removable*/ true, /*internal*/ false, "DCIM");
    SlotTestAccess::openReady(*slot);
    const auto registration = slot->registration();
    VG_CHECK(registration != 0);
    VG_CHECK(f.registered(slot.get()));
    VG_CHECK(f.ctx->slot_admission.isReadyToWrite());

    auto &photo = f.ctx->file_channels.photo;
    const auto owner = std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event);
    VG_REQUIRE(f.ctx->dispatchRuleEvent(owner)); // refused only when the premise above does not hold
    const auto task = photo.current();
    VG_REQUIRE(task != nullptr);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Pending);

    SlotTestAccess::markFailed(*slot);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(photo.current() == nullptr);
    VG_CHECK(!slot->isReadyToWrite());
    VG_CHECK(!f.ctx->slot_admission.isReadyToWrite());
    VG_CHECK(!f.ctx->dispatchRuleEvent(owner));
    VG_CHECK(f.registered(slot.get()));
    VG_CHECK_EQ(slot->registration(), registration);
}

VG_TEST_MAIN()
