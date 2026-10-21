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
#include "vigil/slot.hpp"
#include "vigil/storage.hpp"

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

using namespace vigil;
using namespace vigil::configs;
using namespace vigil::slot;
using namespace vigil::storage;
using namespace vigil::tests;

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

VG_TEST_MAIN()
