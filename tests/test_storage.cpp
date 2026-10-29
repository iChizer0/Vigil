// Storage over no device. Two premises keep these cases off the host's devices, and every case must keep both: a
// Storage is built with `run_manager_thread` false, and the netlink pass runs only on the socket pair of
// `attachUeventSocket`; otherwise, as root, the pass would scan /sys/class/block and mount the host's partitions.
// The device and mount paths are in a temporary directory, but for the bounce cases' fixed paths,
// /dev/vigil-test-bounce, /mnt/rc_vigil-test-bounce, and /dev/vigil-test-internal, which they require absent, and one
// case's directory under /dev/shm, which it removes. No device exists but one case's zero-filled file, on which blkid
// finds no filesystem (it must stay so: a Slot mounts a device it recognizes). One case times a wait, since what it
// pins is the absence of work over time.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vigil/configs.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/slot.hpp"
#include "vigil/storage.hpp"
#include "vigil/types.hpp"
#include "vigil/utils.hpp"
#include "vigil/writers.hpp"

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

    // Whether the Slot still holds its manager or a channel worker thread, which a disable joins and releases.
    static bool ownsThreads(const Slot &slot)
    {
        return slot._manager_thread || slot._photo_worker_thread || slot._video_worker_thread;
    }

    // A publication as a channel worker makes it, and the cache directory its files come from.
    static bool publish(Slot &slot, CacheFileInfo &&info)
    {
        return slot.publish(std::move(info));
    }

    static std::filesystem::path recordCachePath(const Slot &slot)
    {
        return slot.getRecordCachePath();
    }

    // The stats the manager's round would have read; a never-enabled Slot has none.
    static void setStats(Slot &slot, const SlotStats &stats)
    {
        slot._stats.store(stats, std::memory_order_release);
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

// The CPU time the process has used so far, every thread's: what a case bounds when it pins a thread that waits.
std::chrono::microseconds ProcessCpuTime()
{
    rusage usage {};
    ::getrusage(RUSAGE_SELF, &usage);
    return std::chrono::seconds(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
           + std::chrono::microseconds(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
}

// The unmount callbacks of one case: how many, the last one's thread and mount path; and, when armed, a hold inside
// the next callback, which keeps an eject inside Slot::drop with the Slot's locks taken, between its lookup and its
// erase.
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

// The file callbacks of one case, counted.
struct FileEventRecorder
{
    static inline std::atomic<int> count = 0;

    static void callback(const vg_file_evt_t *)
    {
        ++count;
    }
};

// What a case changes in the fake configuration, which has no storage callback and no internal device.
struct FixtureOptions
{
    vg_sta_e (*log_cb)(vg_log_level_e, const char *, size_t) = nullptr; // replaces the logging callback
    void (*on_unmount_cb)(const char *, const char *) = nullptr;        // the storage callback of that name
    const char *internal_device_path = nullptr; // internal_device_path, mounted at the fixture's `internal_mount`
    void (*on_file_event_cb)(const vg_file_evt_t *) = nullptr; // the file callback
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
        config.basic.on_file_event_cb = options.on_file_event_cb;
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

// A host sink that creates the file it is given and then fails: a video write that fails at the first frame, an image
// capture that fails after writing.
struct FailingSink
{
    static inline std::string created_path;

    static vg_sta_e Create(int *fd, const char *path, const char *, void *)
    {
        created_path = path;
        *fd = ::open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        return *fd < 0 ? EIO : 0;
    }

    static vg_sta_e WriteVideo(int, const vg_video_frame_t *, void *)
    {
        return EIO;
    }

    static vg_sta_e WriteAudio(int, const vg_audio_frame_t *, void *)
    {
        return EIO;
    }

    static vg_sta_e Finish(int fd, void *)
    {
        ::close(fd);
        return 0;
    }

    static vg_sta_e Capture(const char *path, const char *, void *)
    {
        created_path = path;
        std::ofstream(path) << "partial";
        return EIO;
    }
};

} // namespace

VG_TEST_CASE(a_removed_slot_is_disabled_and_its_admission_closed_before_the_map_drops_it)
{
    // The test's own reference stands for a copy of the map taken by getStatus on another thread, so the map's
    // reference is not the last one.
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
    VG_CHECK(!SlotTestAccess::ownsThreads(*slot)); // joined on the removing thread, before the map let the Slot go
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
    // The admission stays registered (rule info keeps reporting the Slot) while the manager's next poll takes the
    // state to ERROR. Over no device the Slot is never READY, so what the mark refuses and cancels is pinned by the
    // next case.
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
    // The Slot is never enabled, so no thread of its own moves its state, and is made READY by hand over an existing
    // directory. Premise: the temporary filesystem has more than 532 MiB free, one Photo's critical mark, since the
    // admission probes the mount path's free space.
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
    // The manager's state sync, run by hand over a Slot made READY as in the previous case. Rotation turned off while
    // the Slot is saturated cancels nothing (a kept edge: the task waits until a person acts), nor does READY
    // returning. Premise: as above, more than 532 MiB free.
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

VG_TEST_CASE(a_publication_that_fails_removes_its_file_and_a_moved_thumbnail_and_says_so)
{
    // Over a Slot that was never enabled, its mount path a directory of the case: publication reads no Slot state.
    // Each clip is a cache file with its thumbnail, as a video writer leaves them.
    FileEventRecorder::count = 0;
    Fixture f(FixtureOptions { .on_file_event_cb = &FileEventRecorder::callback });
    Slot slot(*f.ctx, f.dev, f.root + "/publish-mnt", /*removable*/ true, /*internal*/ false, "DCIM");
    const auto root = slot.getRecordRootPath();
    const auto cache = SlotTestAccess::recordCachePath(slot);
    std::filesystem::create_directories(cache);
    const auto write = [](const std::filesystem::path &path) {
        std::ofstream out(path);
        out << "data";
        VG_REQUIRE(static_cast<bool>(out));
    };
    const auto clip = [&](const std::string &name, const std::string &day) {
        CacheFileInfo info(std::make_shared<WriteEvent>(std::chrono::steady_clock::now(), 0, uint64_t { 0 }));
        info.event->status.store(WriteEvent::Status::Completed);
        info.path = cache / name;
        info.first_level_dir_name = day;
        info.thumbnail_path = cache / (name + ".jpg");
        write(info.path);
        write(info.thumbnail_path);
        return info;
    };
    const auto thumbnail = [&](const std::string &day, const std::string &name) {
        return index_manager::ThumbnailPathFrom(root / day / name);
    };

    // Published: the file and its thumbnail move under the day's directory, and the file callback reports it.
    VG_CHECK(SlotTestAccess::publish(slot, clip("a.mp4", "day-a")));
    VG_CHECK(std::filesystem::is_regular_file(root / "day-a" / "a.mp4"));
    VG_CHECK(std::filesystem::is_regular_file(thumbnail("day-a", "a.mp4")));
    VG_CHECK_EQ(FileEventRecorder::count.load(), 1);

    // A file in the way of the day's directory: nothing moves, and the cache file and its thumbnail are removed.
    write(root / "day-b");
    VG_CHECK(!SlotTestAccess::publish(slot, clip("b.mp4", "day-b")));
    VG_CHECK(!std::filesystem::exists(cache / "b.mp4") && !std::filesystem::exists(cache / "b.mp4.jpg"));

    // The cache file is gone, as a create callback that wrote elsewhere leaves it: the size fails after the thumbnail
    // moved, and the moved thumbnail is removed again.
    auto missing = clip("c.mp4", "day-a");
    std::filesystem::remove(missing.path);
    VG_CHECK(!SlotTestAccess::publish(slot, std::move(missing)));
    VG_CHECK(!std::filesystem::exists(thumbnail("day-a", "c.mp4")) && !std::filesystem::exists(cache / "c.mp4.jpg"));

    // The rename fails, here because the cache file is on another filesystem: the file and the moved thumbnail are
    // removed. Skipped where /dev/shm is not a filesystem of its own.
    struct stat shm {};
    struct stat tmp {};
    std::string other = "/dev/shm/vigil-test-XXXXXX";
    if (::stat("/dev/shm", &shm) == 0 && ::stat(f.root.c_str(), &tmp) == 0 && shm.st_dev != tmp.st_dev
        && ::mkdtemp(other.data()) != nullptr)
    {
        struct RemovedOnExit
        {
            const std::string &path;

            ~RemovedOnExit()
            {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } const removed { other };
        auto elsewhere = clip("d.mp4", "day-a");
        const auto moved = std::filesystem::path(other) / "d.mp4";
        std::filesystem::copy_file(elsewhere.path, moved);
        std::filesystem::remove(elsewhere.path);
        elsewhere.path = moved;
        VG_CHECK(!SlotTestAccess::publish(slot, std::move(elsewhere)));
        VG_CHECK(!std::filesystem::exists(moved) && !std::filesystem::exists(thumbnail("day-a", "d.mp4")));
        VG_CHECK(!std::filesystem::exists(root / "day-a" / "d.mp4"));
    }

    VG_CHECK_EQ(FileEventRecorder::count.load(), 1); // the failures reported nothing
    VG_CHECK(std::filesystem::is_regular_file(root / "day-a" / "a.mp4"));
}

VG_TEST_CASE(a_selector_that_matches_several_slots_enables_one_and_keeps_the_enabled_one)
{
    // A UUID shared by cards cloned from one image, set by hand, since a Slot over no device has none. Each Slot is
    // enabled by path before a UUID selection, so in one of the two rounds the enabled Slot comes first in map order,
    // whichever that order is.
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
    // Over no device the unmount itself fails, since nothing is mounted, so the callback is what the case observes.
    UnmountRecorder::clear();
    LogRecorder::clear();
    Fixture f(FixtureOptions { .log_cb = &LogRecorder::callback, .on_unmount_cb = &UnmountRecorder::callback });
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
    LogRecorder::clear();
    VG_CHECK_EQ(next->drop(), 0);    // a removal that raced the eject: dropped already, nothing runs again
    VG_CHECK_EQ(next->disable(), 0); // a switch's copy of the map: nothing is synced at a path a replacement may hold
    VG_CHECK(!LogRecorder::contains("for syncing"));
    VG_CHECK_EQ(UnmountRecorder::count(), 2);
    next.reset();
    VG_CHECK_EQ(UnmountRecorder::count(), 2);
}

VG_TEST_CASE(an_eject_erases_only_the_slot_it_dropped)
{
    // By the time an eject erases its entry, a removal may have dropped the Slot and the same pass built a
    // replacement under the key. A test cannot order two drops on one mutex, so it reaches that state from the other
    // side: the eject's own drop is held inside the unmount callback, with the Slot's locks taken, while the test
    // replaces the map entry.
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
    // Over a directory in the temporary root the disable logs no sync failure. Over the fixture's nonexistent mount
    // path the open fails, which shows the record catches the line: the disable and the drop's unmount each write it,
    // checked before the Slot's destructor, whose unmount of an undropped Slot syncs as well.
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
        LogRecorder::clear();
        VG_CHECK_EQ(slot.disable(), 0);
        VG_CHECK(LogRecorder::contains("for syncing: 2")); // ENOENT, from the disable
        LogRecorder::clear();
        VG_CHECK_EQ(slot.drop(), 0);
        VG_CHECK(LogRecorder::contains("for syncing: 2")); // from the drop's unmount
    }
}

VG_TEST_CASE(an_enabled_slot_that_waits_for_a_format_request_leaves_the_processor_idle)
{
    // A zero-filled file, on which blkid finds nothing, stands for an unsupported card. Past the wait's first
    // interval, the case bounds the process's CPU time over a second: a wait leaves it almost empty (a few
    // milliseconds, sanitized or not, and load cannot raise it), and a spin exceeds the bound while it gets a tenth of
    // a core.
    Fixture f;
    const auto dev = f.root + "/zero-dev";
    {
        std::ofstream out(dev, std::ios::binary);
        out << std::string(std::size_t { 1 } << 20, '\0');
    }
    auto slot = std::make_shared<Slot>(*f.ctx, dev, f.mount, /*removable*/ true, /*internal*/ false, "DCIM");
    VG_REQUIRE(slot->getStatus().state == SlotState::NotFormattedOrFormatUnsupported);
    VG_REQUIRE(slot->enable() == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // past the first interval of the manager's wait
    const auto before = ProcessCpuTime();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    VG_CHECK(ProcessCpuTime() - before < std::chrono::milliseconds(100));
    VG_CHECK(slot->getStatus().state == SlotState::NotFormattedOrFormatUnsupported);
}

VG_TEST_CASE(a_device_that_bounces_within_one_pass_gets_a_new_slot_and_one_added_and_removed_within_it_gets_none)
{
    // A remove discards an add queued before it and leaves one after it. The devices named here do not exist, so the
    // pass's Slots are not removable and never mount; a whole disk's add names no partition and is ignored.
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
    // Over a nonexistent device, with the scan marked done, the first add stands for the scan's Slot.
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

VG_TEST_CASE(a_child_the_host_reaps_is_reported_as_a_failure)
{
    // The Slot's commands run through Execute. With SIGCHLD ignored the kernel reaps the child, so its exit status is
    // unknowable: an error, where the default disposition reports the status itself.
    std::atomic<int> sig = 0;
    VG_CHECK_EQ(utils::Execute("sh", { "-c", "exit 3" })(&sig), 3);
    VG_CHECK_EQ(utils::Execute("sh", { "-c", "exit 3" })(nullptr), 3);
    struct sigaction ignore {};
    struct sigaction previous {};
    ignore.sa_handler = SIG_IGN;
    VG_REQUIRE(::sigaction(SIGCHLD, &ignore, &previous) == 0);
    const auto polled = utils::Execute("sh", { "-c", "exit 0" })(&sig);
    const auto waited = utils::Execute("sh", { "-c", "exit 0" })(nullptr);
    VG_REQUIRE(::sigaction(SIGCHLD, &previous, nullptr) == 0);
    VG_CHECK_EQ(polled, -ECHILD);
    VG_CHECK_EQ(waited, -ECHILD);
}

VG_TEST_CASE(a_failed_automatic_task_leaves_no_file_in_the_cache)
{
    // Over a never-enabled Slot whose gates a triggered task passes (READY and registered, free space, rules enabled,
    // the channel revision unchanged): a video sink that fails at the first frame and an image capture that fails
    // after writing each fail their task with no publication, and the file the host created goes with the writer.
    Fixture f;
    auto cfg = MakeFakeConfig(f.root.c_str());
    cfg.media.video_file_create_cb = &FailingSink::Create;
    cfg.media.video_file_finish_cb = &FailingSink::Finish;
    cfg.media.video_file_write_video_frame_cb = &FailingSink::WriteVideo;
    cfg.media.video_file_write_audio_frame_cb = &FailingSink::WriteAudio;
    cfg.media.image_capture_cb = &FailingSink::Capture;
    Context ctx(cfg);
    Slot slot(ctx, f.dev, f.root + "/failed-mnt", /*removable*/ true, /*internal*/ false, "DCIM");
    std::filesystem::create_directories(SlotTestAccess::recordCachePath(slot));
    SlotTestAccess::openReady(slot);
    SlotTestAccess::setStats(slot, SlotStats(FileSize { 1 } << 40, FileSize { 1 } << 40));
    ctx.record_rule_enabled.store(true);

    const auto settings = [](CaptureType type, const char *format) {
        return FileWriterSettings::create(type, format, std::chrono::milliseconds(5000),
            constants::kRecordWriterPrefixDefault, constants::kVideoClipLengthDefault,
            WaterMarks::from(format, 1, nullptr));
    };
    const auto event = [] {
        return std::make_shared<WriteEvent>(std::chrono::steady_clock::now(), int64_t { 1000 }, uint64_t { 42 });
    };
    const auto run = [](auto writer) {
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Failed);
        VG_CHECK(!writer->takePublication());
        VG_CHECK(std::filesystem::is_regular_file(FailingSink::created_path)); // the host's file, until the writer goes
        writer.reset();
        VG_CHECK(!std::filesystem::exists(FailingSink::created_path));
    };

    uint8_t data[4] = { 1, 2, 3, 4 };
    const vg_video_frame_t frame { .pts = 1, .data = data, .bytes_len = sizeof data, .key_frame = true };
    ctx.slot_media_buffer.enqueue(MediaFrame(frame));
    run(std::make_unique<writers::VideoWriter<writers::SlotAdaptedWriter<Slot>>>(std::ref(slot),
        ctx.action_settings.channelRevision(CaptureType::Video), FileTask::Kind::Triggered, std::ref(ctx),
        settings(CaptureType::Video, "MP4"), event()));
    run(std::make_unique<writers::ImageWriter<writers::SlotAdaptedWriter<Slot>>>(std::ref(slot),
        ctx.action_settings.channelRevision(CaptureType::Image), FileTask::Kind::Triggered, std::ref(ctx),
        settings(CaptureType::Image, "JPG"), event()));
}

VG_TEST_MAIN()
