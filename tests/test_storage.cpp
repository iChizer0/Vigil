// Characterization tests for Storage over no device (docs/PLAN.md section 9). Two premises keep them off the host's
// devices, and every case must keep both: a Storage is built with `run_manager_thread` false, and the netlink pass
// runs only after `attachUeventSocket` gave it a socket pair. Without either, as root, the pass would open the real
// uevent socket and scan /sys/class/block, and its Slots would check and mount the host's partitions under /mnt.
// The device and mount paths are in a temporary directory and do not exist, so nothing is mounted, formatted, or
// removed, and a Slot's own threads probe a device that is not there, which is enough for the lifecycle under
// test; the bounce cases name three fixed paths, /dev/vigil-test-bounce, /mnt/rc_vigil-test-bounce, and
// /dev/vigil-test-internal, which they require absent. A Slot over no device admits no task, so what closing its
// admission does to the pending tasks bound to it is pinned by
// `closing_admission_retires_unclaimed_tasks_and_the_thread_bodies_serve_both_channels_at_once`
// (`tests/test_channel_worker.cpp`), and a Slot made READY by hand pins what the failure mark and a saturation do.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

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

    // The entry for `slot`'s device replaced with `slot`: what the netlink pass leaves once a removal erased the old
    // Slot and the pass built a new one for the re-added device under the same key.
    static void replace(Storage &storage, const std::shared_ptr<Slot> &slot)
    {
        const std::unique_lock lock(storage._slots_mutex);
        storage._slots[slot->devPath().string()] = slot;
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

    // The Slot held for `dev_path` (null: none).
    static std::shared_ptr<Slot> get(const Storage &storage, const std::string &dev_path)
    {
        const std::shared_lock lock(storage._slots_mutex);
        const auto it = storage._slots.find(dev_path);
        return it == storage._slots.end() ? nullptr : it->second;
    }

    // Makes the netlink pass read its uevents from `fd`, which Storage closes with itself, with the initial scan
    // marked done: a pass then handles the messages queued on `fd` and nothing else.
    static void attachUeventSocket(Storage &storage, int fd)
    {
        storage._netlink_fd = fd;
        storage._initial_scan_done = true;
    }

    // The netlink pass of the storage manager, without the enabling that follows it (syncEnabledSlotWithConfig).
    static void pass(Storage &storage)
    {
        storage.syncAndUpdateSlotsWithNetlink();
    }

    // The enabling that follows the netlink pass in the storage manager's loop.
    static vg_sta_e sync(Storage &storage)
    {
        return storage.syncEnabledSlotWithConfig();
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

    // The UUID the probe would read from a device.
    static void setUUID(Slot &slot, std::string uuid)
    {
        const std::unique_lock lock(slot._info_mutex);
        slot._uuid = std::move(uuid);
    }

    // The rotation flag the Slot would have read from its `.quota_no_rotate` marker when it was configured.
    static void setQuotaRotation(Slot &slot, bool rotate)
    {
        slot._quota_rotate.store(rotate, std::memory_order_release);
    }

    // The manager's state sync of a READY or SATURATED Slot, with the stats its round would have read and no index
    // (no quota limit is set, so the free space alone decides).
    static void syncReadyOrSaturated(Slot &slot, const SlotStats &stats)
    {
        const auto s = slot._ctx.action_settings.get().settings;
        slot.doSyncStateReadyOrSaturated(s->watermarks, nullptr, stats, slot._state.load(std::memory_order_acquire));
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

// The log lines of one case, for what a Slot reports only there; vg_config_basic_t callbacks carry no user_data,
// so the record is process-wide and a case clears it first.
struct LogRecorder
{
    static vg_sta_e callback(vg_log_level_e level, const char *msg, size_t bytes_len)
    {
        {
            const std::lock_guard lock(mutex());
            lines().emplace_back(msg ? msg : "", bytes_len);
        }
        return TestLogCallback(level, msg, bytes_len);
    }

    static void clear()
    {
        const std::lock_guard lock(mutex());
        lines().clear();
    }

    static bool contains(std::string_view text)
    {
        const std::lock_guard lock(mutex());
        for (const auto &line: lines())
            if (line.find(text) != std::string::npos)
                return true;
        return false;
    }

private:
    static std::mutex &mutex()
    {
        static std::mutex m;
        return m;
    }

    static std::vector<std::string> &lines()
    {
        static std::vector<std::string> v;
        return v;
    }
};

// Waits for `pred` with a bound, yielding: a case's barrier, never a sleep as evidence.
template<typename Pred>
bool WaitFor(const Pred &pred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// The unmount callbacks of one case: how many, the last one's thread and mount path; and, when a case armed it, a
// hold inside the next callback until the case releases it, which keeps the caller at a fixed point inside
// Slot::drop with the Slot's locks taken: the hold a test has on an eject between its lookup and its erase.
struct UnmountRecorder
{
    static void callback(const char *, const char *mount_path)
    {
        {
            const std::lock_guard lock(mutex());
            ++count_();
            thread_() = std::this_thread::get_id();
            mount_() = mount_path ? mount_path : "";
        }
        if (hold_next_().exchange(false))
        {
            held_() = true;
            while (!release_().load())
                std::this_thread::yield();
        }
    }

    static void clear()
    {
        const std::lock_guard lock(mutex());
        count_() = 0;
        thread_() = std::thread::id();
        mount_().clear();
        hold_next_() = false;
        held_() = false;
        release_() = false;
    }

    // Arms the hold for the next callback.
    static void armHold()
    {
        hold_next_() = true;
    }

    // Whether a callback is inside its hold.
    static bool held()
    {
        return held_().load();
    }

    // Lets the held callback return.
    static void release()
    {
        release_() = true;
    }

    static int count()
    {
        const std::lock_guard lock(mutex());
        return count_();
    }

    static std::thread::id thread()
    {
        const std::lock_guard lock(mutex());
        return thread_();
    }

    static std::string mount()
    {
        const std::lock_guard lock(mutex());
        return mount_();
    }

private:
    static std::mutex &mutex()
    {
        static std::mutex m;
        return m;
    }

    static int &count_()
    {
        static int n = 0;
        return n;
    }

    static std::thread::id &thread_()
    {
        static std::thread::id id;
        return id;
    }

    static std::string &mount_()
    {
        static std::string s;
        return s;
    }

    static std::atomic<bool> &hold_next_()
    {
        static std::atomic<bool> b = false;
        return b;
    }

    static std::atomic<bool> &held_()
    {
        static std::atomic<bool> b = false;
        return b;
    }

    static std::atomic<bool> &release_()
    {
        static std::atomic<bool> b = false;
        return b;
    }
};

// What a case changes in the fake configuration, which has no storage callback and no internal device.
struct FixtureOptions
{
    vg_sta_e (*log_cb)(vg_log_level_e, const char *, size_t) = nullptr; // replaces the logging callback
    void (*on_unmount_cb)(const char *, const char *) = nullptr;        // the storage callback of that name
    const char *internal_device_path = nullptr; // internal_device_path, mounted at the fixture's `internal_mount`
};

struct Fixture
{
    std::string root;
    std::string dev;
    std::string mount;
    std::string internal_mount;
    vg_config_t config;
    std::shared_ptr<Context> ctx;

    explicit Fixture(const FixtureOptions &options = {})
        : root(MakeTempDir()), dev(root + "/no-such-dev"), mount(root + "/no-such-mnt"),
          internal_mount(root + "/no-such-internal-mnt"), config(MakeFakeConfig(root.c_str()))
    {
        if (options.log_cb)
            config.basic.log_cb = options.log_cb;
        config.storage.on_unmount_cb = options.on_unmount_cb;
        if (options.internal_device_path)
        {
            config.storage.internal_device_path = options.internal_device_path;
            config.storage.internal_mount_point = internal_mount.c_str();
        }
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

// A socket pair standing for the uevent socket: `storage_fd` is attached to the Storage, which closes it, and
// `send` queues one datagram laid out as the kernel's (the header line, then KEY=VALUE pairs, each NUL-terminated)
// with the four keys the pass reads, which it parses as it does a real one.
struct UeventPeer
{
    int storage_fd;
    int fd;

    UeventPeer()
    {
        int pair[2];
        if (::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, pair) != 0)
            throw std::runtime_error("socketpair failed");
        storage_fd = pair[0];
        fd = pair[1];
    }

    ~UeventPeer()
    {
        ::close(fd);
    }

    void send(std::string_view action, std::string_view devname, std::string_view devtype = "partition") const
    {
        std::string msg;
        const auto append = [&msg](std::string_view s) {
            msg += s;
            msg += '\0';
        };
        append(std::string(action) + "@/devices/virtual/block/" + std::string(devname));
        append("ACTION=" + std::string(action));
        append("DEVNAME=" + std::string(devname));
        append("SUBSYSTEM=block");
        append("DEVTYPE=" + std::string(devtype));
        if (::send(fd, msg.data(), msg.size(), 0) != static_cast<ssize_t>(msg.size()))
            throw std::runtime_error("send failed");
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

VG_TEST_CASE(saturation_without_rotation_cancels_the_pending_task_and_with_rotation_leaves_it_waiting)
{
    // The manager's state sync (docs/PLAN.md section 7.2), run by hand with the stats it would have read, over a
    // Slot set READY and registered by hand as in the previous case. It admits a Photo task. A saturation with
    // rotation on leaves the task pending, since the rotate step brings READY back; rotation turned off while the
    // Slot is saturated leaves it too (an edge kept, docs/RETURN.md section 4.2), as does READY returning; a
    // saturation with rotation off cancels it, as the ERROR states do, the Slot refusing admission while saturated
    // and staying registered, and READY returning admits again. Premise: as above, the temporary filesystem has
    // more than 532 MiB free.
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

    auto &photo = f.ctx->file_channels.photo;
    const auto owner = std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event);
    VG_REQUIRE(f.ctx->dispatchRuleEvent(owner)); // refused only when the premise above does not hold
    const auto task = photo.current();
    VG_REQUIRE(task != nullptr);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Pending);

    const SlotStats full(FileSize { 1 } << 40, 0), roomy(FileSize { 1 } << 40, FileSize { 1 } << 40);
    SlotTestAccess::setQuotaRotation(*slot, true);
    SlotTestAccess::syncReadyOrSaturated(*slot, full);
    VG_CHECK(slot->isSaturated());
    VG_CHECK(!slot->isReadyToWrite());
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Pending);
    SlotTestAccess::setQuotaRotation(*slot, false);
    SlotTestAccess::syncReadyOrSaturated(*slot, full); // rotation turned off while saturated: nothing is canceled
    VG_CHECK(slot->isSaturated());
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Pending);
    SlotTestAccess::syncReadyOrSaturated(*slot, roomy);
    VG_CHECK(slot->isReadyToWrite());
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Pending);
    VG_CHECK(photo.current() == task);

    SlotTestAccess::syncReadyOrSaturated(*slot, roomy); // READY stays READY: nothing is canceled
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Pending);
    SlotTestAccess::syncReadyOrSaturated(*slot, full);
    VG_CHECK(slot->isSaturated());
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(photo.current() == nullptr);
    VG_CHECK(!slot->isReadyToWrite());
    VG_CHECK(!f.ctx->slot_admission.isReadyToWrite());
    VG_CHECK(!f.ctx->dispatchRuleEvent(owner));
    VG_CHECK(f.registered(slot.get()));
    VG_CHECK_EQ(slot->registration(), registration);

    SlotTestAccess::syncReadyOrSaturated(*slot, roomy);
    VG_CHECK(slot->isReadyToWrite());
    VG_CHECK(f.ctx->dispatchRuleEvent(owner));
    const auto next = photo.current();
    VG_CHECK(next != nullptr && next != task);
    VG_CHECK(next->event->status.load() == WriteEvent::Status::Pending);
}

VG_TEST_CASE(a_selector_that_matches_several_slots_enables_one_and_keeps_the_enabled_one)
{
    // Of several Slots a selector matches (a UUID shared by cards cloned from one image), the sync enables one:
    // the one already enabled stays the target, whatever the map's iteration order, and every other enabled Slot
    // is disabled first, so at most one admission is registered (docs/PLAN.md section 9). Until 2026-10-21 the
    // last match in map order was the target and an enabled match was never disabled, so a second match was
    // enabled next to the first, whose registration the second's open overwrote. The UUIDs are set by hand, since
    // a Slot over no device has none; the selections below reach the defect in either map order.
    Fixture f;
    const std::string uuid = "1234-ABCD";
    Storage storage(StorageConfig(std::string(), std::string()), *f.ctx, /*run_manager_thread*/ false);
    const auto make = [&](const char *name) {
        auto slot = std::make_shared<Slot>(*f.ctx, f.root + "/no-such-" + name, f.root + "/no-such-mnt-" + name,
            /*removable*/ true, /*internal*/ false, "DCIM");
        SlotTestAccess::setUUID(*slot, uuid);
        StorageTestAccess::add(storage, slot);
        return slot;
    };
    auto a = make("a"), b = make("b");
    const auto select = [&](std::string dev_path, std::string by_uuid) {
        VG_CHECK_EQ(storage.setConfig(StorageConfig(std::move(dev_path), std::move(by_uuid))), 0);
        VG_CHECK_EQ(StorageTestAccess::sync(storage), 0);
    };
    const auto only = [&](const std::shared_ptr<Slot> &enabled, const std::shared_ptr<Slot> &other) {
        return enabled->isEnabled() && !other->isEnabled() && f.registered(enabled.get());
    };

    select(a->devPath().string(), "");
    VG_CHECK(only(a, b));
    select("", uuid); // both match: the enabled one stays
    VG_CHECK(only(a, b));
    select(b->devPath().string(), ""); // the other by path: a switch
    VG_CHECK(only(b, a));
    select("", uuid);
    VG_CHECK(only(b, a));
    size_t selected = 0;
    for (const auto &status: storage.getStatus().slots_status)
        selected += status.selected;
    VG_CHECK_EQ(selected, size_t { 2 }); // each matching slot reports it
    select("", "no-such-uuid");          // matches none: the enabled one is disabled
    VG_CHECK(!a->isEnabled() && !b->isEnabled() && f.registered(nullptr));
}

VG_TEST_CASE(an_eject_or_a_removal_unmounts_the_slot_on_its_own_thread_before_the_map_lets_it_go)
{
    // Slot::drop, which an eject and a removal run: the Slot is disabled and unmounted, on_unmount_cb included, on
    // the dropping thread and before the map's reference goes, so that a holder which lets go later (a storage
    // status copy, a file operation) unmounts nothing, which by then could be a new Slot's mount of the same device
    // at the same path; a dropped Slot is never enabled again. Over no device the unmount itself fails, since
    // nothing is mounted, and the callback is what the case observes.
    UnmountRecorder::clear();
    Fixture f(FixtureOptions { .on_unmount_cb = &UnmountRecorder::callback });
    Storage storage(StorageConfig(std::string(f.dev), std::string()), *f.ctx, /*run_manager_thread*/ false);

    // A removal, with the test holding a reference: the callback has run, here, when the branch returns, and the
    // late release on another thread adds no call.
    auto slot = f.makeSlot();
    StorageTestAccess::add(storage, slot);
    VG_REQUIRE(slot->enable() == 0);
    StorageTestAccess::remove(storage, f.dev);
    VG_CHECK_EQ(UnmountRecorder::count(), 1);
    VG_CHECK(UnmountRecorder::thread() == std::this_thread::get_id());
    VG_CHECK_EQ(UnmountRecorder::mount(), f.mount);
    VG_CHECK(!slot->isEnabled());
    VG_CHECK_EQ(slot->enable(), ECANCELED);
    auto next = f.makeSlot(); // the same device, plugged in again, before the old Slot's last reference goes
    StorageTestAccess::add(storage, next);
    VG_REQUIRE(next->enable() == 0);
    std::thread([s = std::move(slot)]() mutable { s.reset(); }).join();
    VG_CHECK_EQ(UnmountRecorder::count(), 1);

    // An eject, with the test holding a reference: the callback has run, here, when the eject answers.
    VG_CHECK_EQ(storage.ejectSlot(f.dev), 0);
    VG_CHECK_EQ(UnmountRecorder::count(), 2);
    VG_CHECK(UnmountRecorder::thread() == std::this_thread::get_id());
    VG_CHECK(!StorageTestAccess::holds(storage, f.dev));
    VG_CHECK(!next->isEnabled());
    VG_CHECK_EQ(next->enable(), ECANCELED);
    VG_CHECK_EQ(next->drop(), 0); // a removal that raced the eject: dropped already, nothing runs again
    VG_CHECK_EQ(next->disable(), 0);
    VG_CHECK_EQ(UnmountRecorder::count(), 2);
    next.reset();
    VG_CHECK_EQ(UnmountRecorder::count(), 2);
}

VG_TEST_CASE(an_eject_erases_only_the_slot_it_dropped)
{
    // Storage::ejectSlot erases its Slot's map entry after the drop, and by then the entry may hold another Slot: a
    // removal (the device pulled) that won the race on the Slot's mutex dropped and erased this one, and the same
    // netlink pass built a replacement for the re-added device under the same key, as a device that bounces within
    // one pass gets a new Slot in that pass. The eject's drop then finds the Slot dropped, and its erase must leave
    // the replacement alone: erased undropped, the replacement's destructor would unmount the present device under
    // the map lock, on the request thread, and the device would be unmanaged until its next add event. A test
    // cannot order two drops on one mutex, so it reaches that state from the other side: the eject's own drop is
    // held inside the unmount callback, with the Slot's locks taken, while the test replaces the map entry.
    UnmountRecorder::clear();
    Fixture f(FixtureOptions { .on_unmount_cb = &UnmountRecorder::callback });
    Storage storage(StorageConfig(std::string(f.dev), std::string()), *f.ctx, /*run_manager_thread*/ false);
    auto slot = f.makeSlot();
    StorageTestAccess::add(storage, slot);
    VG_REQUIRE(slot->enable() == 0);

    UnmountRecorder::armHold();
    vg_sta_e ejected = -1;
    std::thread eject([&] { ejected = storage.ejectSlot(f.dev); });
    struct Release // lets the held eject go and joins it, also when a required check throws
    {
        std::thread &eject;
        ~Release()
        {
            UnmountRecorder::release();
            if (eject.joinable())
                eject.join();
        }
    } release { eject };
    VG_REQUIRE(WaitFor([] { return UnmountRecorder::held(); }));
    auto replacement = f.makeSlot(); // the re-added device's Slot, built by the pass once the removal erased the old
    StorageTestAccess::replace(storage, replacement);
    UnmountRecorder::release();
    eject.join();
    VG_CHECK_EQ(ejected, 0); // the Slot the request named is gone
    VG_CHECK_EQ(UnmountRecorder::count(), 1);
    VG_CHECK(StorageTestAccess::get(storage, f.dev) == replacement); // left alone
    VG_CHECK(!slot->isEnabled());
    VG_CHECK_EQ(slot->enable(), ECANCELED);
    VG_REQUIRE(replacement->enable() == 0); // undropped: it serves the device
    VG_CHECK(f.registered(replacement.get()));
    StorageTestAccess::remove(storage, f.dev); // its own removal, on this thread
    VG_CHECK_EQ(UnmountRecorder::count(), 2);
    VG_CHECK(!StorageTestAccess::holds(storage, f.dev));
}

VG_TEST_CASE(a_disable_syncs_the_filesystem_at_the_mount_path)
{
    // Slot::doFsync runs at a disable (a switch) and before an unmount (an eject, a removal, deinit): it opens the
    // mount path read-only, as a directory cannot be opened for writing (EISDIR), which had left the sync unrun since
    // 2026-05-29 and logged a failure at every disable. Over a directory in the temporary root the disable must
    // report no such failure (the sync is of the temporary filesystem); over the fixture's nonexistent mount path
    // the open fails as before, which shows the record catches the line.
    LogRecorder::clear();
    Fixture f(FixtureOptions { .log_cb = &LogRecorder::callback });
    const auto mount = f.root + "/mnt";
    VG_REQUIRE(std::filesystem::create_directory(mount));
    {
        Slot slot(*f.ctx, f.dev, mount, /*removable*/ true, /*internal*/ false, "DCIM");
        VG_CHECK_EQ(slot.disable(), 0);
    }
    VG_CHECK(!LogRecorder::contains("for syncing"));
    {
        Slot slot(*f.ctx, f.dev, f.mount, /*removable*/ true, /*internal*/ false, "DCIM");
        VG_CHECK_EQ(slot.disable(), 0);
    }
    VG_CHECK(LogRecorder::contains("for syncing: 2")); // ENOENT
}

VG_TEST_CASE(a_device_that_bounces_within_one_pass_gets_a_new_slot_and_one_added_and_removed_within_it_gets_none)
{
    // The netlink pass reads every queued uevent before it acts, and arrival order decides for a device whose
    // events are both queued: a remove discards an add that came before it and leaves one that came after. A
    // device that bounces within one pass (a remove, then an add) has its old Slot dropped, disabled first, and a
    // new one created in the same pass, which the sync that follows the pass enables; one added and removed within
    // one pass has none. The devices named here do not exist, so the pass's Slots are not removable and never
    // mount; a whole disk's add names no partition and is ignored, and its remove finds no Slot.
    Fixture f;
    UeventPeer uevents;
    const std::string devname = "vigil-test-bounce";
    const std::string dev_path = "/dev/" + devname;
    VG_REQUIRE(!std::filesystem::exists(dev_path) && !std::filesystem::exists("/mnt/rc_" + devname)); // fixed paths
    Storage storage(StorageConfig(std::string(dev_path), std::string()), *f.ctx, /*run_manager_thread*/ false);
    StorageTestAccess::attachUeventSocket(storage, uevents.storage_fd);

    uevents.send("add", devname);
    StorageTestAccess::pass(storage);
    auto first = StorageTestAccess::get(storage, dev_path);
    VG_REQUIRE(first != nullptr);
    VG_CHECK(!first->isRemovable());
    VG_CHECK(!first->isInternal());
    VG_REQUIRE(first->enable() == 0);
    VG_CHECK(f.registered(first.get()));

    uevents.send("remove", devname);
    uevents.send("add", devname);
    StorageTestAccess::pass(storage);
    auto second = StorageTestAccess::get(storage, dev_path);
    VG_REQUIRE(second != nullptr);
    VG_CHECK(second != first);
    VG_CHECK(!first->isEnabled());
    VG_CHECK_EQ(first->registration(), uint64_t { 0 });
    VG_CHECK(!second->isEnabled()); // enabled by the sync that follows the pass, not by the pass
    VG_CHECK(f.registered(nullptr));
    VG_CHECK_EQ(storage.getStatus().slots_status.size(), size_t { 1 });

    VG_REQUIRE(second->enable() == 0);
    uevents.send("add", devname); // before the remove: discarded by it (the hold alone would not: removeSlot drops it
                                  // before the loop that erases the held devices' adds runs)
    uevents.send("remove", devname);
    StorageTestAccess::pass(storage);
    VG_CHECK(!StorageTestAccess::holds(storage, dev_path));
    VG_CHECK(!second->isEnabled());
    VG_CHECK(f.registered(nullptr));
    VG_CHECK_EQ(storage.getStatus().slots_status.size(), size_t { 0 });

    uevents.send("remove", devname); // unknown by now: nothing to do
    uevents.send("remove", "vigil-test-disk", "disk");
    uevents.send("add", "vigil-test-disk", "disk");
    StorageTestAccess::pass(storage);
    VG_CHECK_EQ(storage.getStatus().slots_status.size(), size_t { 0 });
}

VG_TEST_CASE(the_internal_device_keeps_its_slot_kind_across_a_bounce_of_its_node)
{
    // The netlink add branch builds internal_device_path's Slot as the one-time scan does, whatever the node's
    // type: internal, at the internal mount point, not removable. So a bounce of its node (a remove, then an add,
    // within one pass or across passes) brings the internal Slot back, rather than an ordinary one at /mnt, which
    // an eject could drop and which would lose the internal format guards, the preserved space, and the internal
    // root policy (docs/PLAN.md section 9). Over a nonexistent device, with the scan marked done, the first add
    // stands for the scan's Slot.
    const std::string devname = "vigil-test-internal";
    const std::string dev_path = "/dev/" + devname;
    VG_REQUIRE(!std::filesystem::exists(dev_path)); // a fixed path
    Fixture f(FixtureOptions { .internal_device_path = dev_path.c_str() });
    UeventPeer uevents;
    Storage storage(StorageConfig(std::string(dev_path), std::string()), *f.ctx, /*run_manager_thread*/ false);
    StorageTestAccess::attachUeventSocket(storage, uevents.storage_fd);

    uevents.send("add", devname);
    StorageTestAccess::pass(storage);
    auto first = StorageTestAccess::get(storage, dev_path);
    VG_REQUIRE(first != nullptr);
    VG_CHECK(first->isInternal());
    VG_CHECK(!first->isRemovable());
    VG_CHECK_EQ(first->mountPath().string(), f.internal_mount);
    VG_REQUIRE(first->enable() == 0);

    uevents.send("remove", devname);
    uevents.send("add", devname, "disk"); // the node's type is not read for the internal device
    StorageTestAccess::pass(storage);
    auto second = StorageTestAccess::get(storage, dev_path);
    VG_REQUIRE(second != nullptr);
    VG_CHECK(second != first);
    VG_CHECK(!first->isEnabled());
    VG_CHECK(second->isInternal());
    VG_CHECK(!second->isRemovable());
    VG_CHECK_EQ(second->mountPath().string(), f.internal_mount);
    VG_CHECK_EQ(storage.ejectSlot(dev_path), ENOTSUP); // refused for the internal Slot, as for the scan's
    VG_CHECK_EQ(storage.getStatus().slots_status.size(), size_t { 1 });
}

VG_TEST_MAIN()
