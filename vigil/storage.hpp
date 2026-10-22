#pragma once
#ifndef VIGIL_STORAGE_HPP
#define VIGIL_STORAGE_HPP

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "configs.hpp"
#include "constants.hpp"
#include "context.hpp"
#include "log.hpp"
#include "slot.hpp"
#include "status.hpp"

namespace vigil::storage {

using namespace vigil::status;
using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::slot;

class Storage
{
public:
    // `run_manager_thread == false` is a test seam: no thread; tests drive the Slot map through StorageTestAccess.
    Storage(const StorageConfig &cfg, Context &ctx, bool run_manager_thread = true)
        : _ctx(ctx), _data_dir_path(kDataDirDefaultPath), _config_mutex(), _config(cfg), _stop_token(false),
          _manager_thread(nullptr), _ejection_in_progress(false), _slots_mutex(), _slots(), _netlink_fd(-1),
          _initial_scan_done(false)
    {
        // Check data directory name validity (non-empty, not slash or dot)
        if (_ctx.c_api.storage.data_directory_path)
        {
            try
            {
                auto data_dir_path = std::filesystem::path(_ctx.c_api.storage.data_directory_path);
                if (data_dir_path.empty() || data_dir_path.is_absolute())
                    throw std::invalid_argument("Data directory path cannot be empty, or absolute");
                _data_dir_path = std::move(data_dir_path);
            }
            catch (const std::exception &e)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Invalid data directory path ",
                    _ctx.c_api.storage.data_directory_path, ": ", e.what(), ", using default path ",
                    kDataDirDefaultPath);
            }
        }

        // Clean relay directory
        if (_ctx.c_api.storage.relay_directory_path)
        {
            std::error_code ec;
            std::filesystem::remove_all(_ctx.c_api.storage.relay_directory_path, ec);
            if (ec)
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to clean relay directory ",
                    _ctx.c_api.storage.relay_directory_path, ": ", ec.message());
        }

        // Start manager thread
        if (run_manager_thread)
            _manager_thread = std::make_unique<std::thread>(&Storage::managerThreadTask, this);
    }

    ~Storage() noexcept
    {
        if (_manager_thread)
        {
            _stop_token.store(true, std::memory_order_release);
            _manager_thread->join();
            _manager_thread.reset();
        }
        if (_netlink_fd >= 0)
        {
            ::close(_netlink_fd);
            _netlink_fd = -1;
        }
    }

    StorageStatus getStatus() const
    {
        auto status = StorageStatus(_data_dir_path);
        const auto current_cfg = getConfig();
        std::unordered_map<std::string, std::shared_ptr<Slot>> slots;
        {
            const std::shared_lock lock(_slots_mutex);
            slots = _slots;
        }
        for (const auto &[_, slot]: slots)
        {
            if (!slot)
                continue;
            auto sta = slot->getStatus();
            sta.selected = isSelectedSlot(slot, current_cfg);
            status.slots_status.emplace_back(std::move(sta));
        }
        return status;
    }

    vg_sta_e ejectSlot(const std::string &slot_dev_path)
    {
        std::shared_ptr<Slot> slot;
        const FlagGuard ejection(_ejection_in_progress);
        {
            const std::shared_lock lock2(_slots_mutex);
            if (const auto it = _slots.find(slot_dev_path); it != _slots.end())
                slot = it->second;
        }
        if (!slot || slot->isInternal())
            return ENOTSUP;
        // The answer follows the unmount, which can only detach while a request still uses the filesystem
        if (const auto ret = slot->drop(); ret != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to disable storage slot with device path ", slot->devPath(),
                ", code: ", ret);
            return ret;
        }
        {
            // Its own entry only: a removal may have dropped this Slot meanwhile and the same netlink pass built a
            // replacement under the key, which an erase would destroy undropped, unmounting the present device
            const std::unique_lock slots_lock(_slots_mutex);
            if (const auto it = _slots.find(slot->devPath().string()); it != _slots.end() && it->second == slot)
                _slots.erase(it);
        }
        return 0;
    }

    vg_sta_e formatSlot(const std::string &slot_dev_path)
    {
        std::shared_ptr<Slot> slot;
        {
            const std::shared_lock lock(_slots_mutex);
            if (const auto it = _slots.find(slot_dev_path); it != _slots.end())
                slot = it->second;
        }
        return slot ? slot->formatFileSystem() : EPERM;
    }

    vg_sta_e removeFilesOrDirectories(const std::string &slot_dev_path,
        const std::vector<std::filesystem::path> &relative_paths, SlotFileOpsProgress *progress = nullptr,
        const CancellationCheck &should_cancel = {}, const SlotFileOpsProgressCallback &on_progress = {})
    {
        std::shared_ptr<Slot> slot;
        {
            const std::shared_lock lock(_slots_mutex);
            if (const auto it = _slots.find(slot_dev_path); it != _slots.end())
                slot = it->second;
        }
        if (!slot)
        {
            if (progress)
            {
                *progress = SlotFileOpsProgress(SlotFileOpsProgress::Type::Remove);
                progress->requested_count = relative_paths.size();
                progress->processed_count = relative_paths.size();
                progress->failed_count = relative_paths.size();
                if (on_progress)
                    on_progress(*progress);
            }
            return EPERM;
        }

        return slot->removeFilesOrDirectories(relative_paths, progress, should_cancel, on_progress);
    }

    vg_sta_e freeUpSlot(const std::string &slot_dev_path, SlotFileOpsProgress *progress = nullptr,
        const CancellationCheck &should_cancel = {}, const SlotFileOpsProgressCallback &on_progress = {})
    {
        std::shared_ptr<Slot> slot;
        {
            const std::shared_lock lock(_slots_mutex);
            if (const auto it = _slots.find(slot_dev_path); it != _slots.end())
                slot = it->second;
        }
        if (!slot)
        {
            if (progress)
            {
                *progress = SlotFileOpsProgress(SlotFileOpsProgress::Type::FreeUp);
                if (on_progress)
                    on_progress(*progress);
            }
            return EPERM;
        }
        return slot->freeUpUsedSpace(progress, should_cancel, on_progress);
    }

    vg_sta_e setConfig(const StorageConfig &cfg) noexcept
    {
        const std::unique_lock lock(_config_mutex);
        if (_config != cfg)
            _config = cfg;
        return 0;
    }

    StorageConfig getConfig() const noexcept
    {
        const std::shared_lock lock(_config_mutex);
        return _config;
    }

    vg_sta_e setSlotConfig(const std::string &slot_dev_path, const SlotConfig &cfg)
    {
        vg_sta_e ret = 0;
        std::shared_ptr<Slot> slot;
        {
            const std::shared_lock lock(_slots_mutex);
            if (const auto it = _slots.find(slot_dev_path); it != _slots.end())
                slot = it->second;
        }
        if (!slot)
        {
            ret = EPERM;
            goto l_set_slot_config_and_return;
        }
        if (ret = slot->setQuotaLimitBytes(cfg.quota_limit_bytes); ret != 0)
            goto l_set_slot_config_and_return;
        if (ret = slot->setQuotaRotate(cfg.quota_rotate); ret != 0)
            goto l_set_slot_config_and_return;

    l_set_slot_config_and_return:
        return ret;
    }

    vg_sta_e enableOrHeartbeatNginxRelay(const std::string &slot_dev_path, RelayStatus &status)
    {
        std::shared_ptr<Slot> slot;
        {
            const std::shared_lock lock(_slots_mutex);
            if (const auto it = _slots.find(slot_dev_path); it != _slots.end())
                slot = it->second;
        }
        if (!slot)
            return EPERM;
        return slot->enableOrHeartbeatNginxRelay() | slot->statusOfNginxRelay(status);
    }

    vg_sta_e statusOfNginxRelay(const std::string &slot_dev_path, RelayStatus &status)
    {
        std::shared_ptr<Slot> slot;
        {
            const std::shared_lock lock(_slots_mutex);
            if (auto it = _slots.find(slot_dev_path); it != _slots.end())
                slot = it->second;
        }
        return slot ? slot->statusOfNginxRelay(status) : EPERM;
    }

    vg_sta_e disableNginxRelay(const std::string &slot_dev_path) noexcept
    {
        std::shared_ptr<Slot> slot;
        {
            const std::shared_lock lock(_slots_mutex);
            if (auto it = _slots.find(slot_dev_path); it != _slots.end())
                slot = it->second;
        }
        if (!slot)
            return EPERM;
        slot->disableNginxRelay();
        return 0;
    }

private:
    static bool isValidDevName(std::string_view name)
    {
        if (name.empty() || name == "." || name == "..")
            return false;
        return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    }

    void managerThreadTask()
    {
        while (!_stop_token.load(std::memory_order_acquire))
        {
            const auto start = std::chrono::steady_clock::now();
            try
            {
                syncAndUpdateSlotsWithNetlink();
                syncEnabledSlotWithConfig();
            }
            catch (const std::exception &e)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Exception occurred in storage manager thread: ", e.what());
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < kManagerLoopInterval)
                std::this_thread::sleep_for(kManagerLoopInterval - elapsed);
        }
    }

    bool isDevHidden(const std::string &dev_name) const
    {
        if (_ctx.c_api.storage.is_hidden_device_cb)
            return _ctx.c_api.storage.is_hidden_device_cb(dev_name.c_str());
        return false;
    }

    bool isDevRemovable(const std::string &dev_name) const
    {
        const auto sys_path = std::filesystem::path("/sys/class/block") / dev_name;
        auto check_removable = [](const std::filesystem::path &sys_path) -> bool {
            std::ifstream ifs(sys_path / "removable");
            int r;
            return ifs >> r && r == 1;
        };
        if (std::error_code ec; std::filesystem::exists(sys_path / "partition", ec))
        {
            if (const auto target = std::filesystem::canonical(sys_path, ec); !ec)
            {
                if (check_removable(target.parent_path()))
                    return true;
            }
        }
        return check_removable(sys_path);
    }

    std::filesystem::path getMountPath(const std::string &dev_name) const
    {
        return std::filesystem::path(kStorageMountBasePath) / ("rc_" + dev_name);
    }

    void syncAndUpdateSlotsWithNetlink()
    {
        if (_netlink_fd < 0)
        {
            _netlink_fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
            if (_netlink_fd < 0)
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create netlink socket: ", static_cast<int>(errno));
            else
            {
                struct sockaddr_nl sa = {};
                sa.nl_family = AF_NETLINK;
                sa.nl_groups = 1;
                if (::bind(_netlink_fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to bind netlink socket: ", static_cast<int>(errno));
                    ::close(_netlink_fd);
                    _netlink_fd = -1;
                }
                else
                {
                    int flags = ::fcntl(_netlink_fd, F_GETFL, 0);
                    ::fcntl(_netlink_fd, F_SETFL, flags | O_NONBLOCK);
                }
            }
        }

        struct SlotInfo
        {
            std::string mount_point;
            bool removable;
            bool internal;
        };
        std::unordered_map<std::string, SlotInfo> dev_2_slot_info_vec;

        // Built by the one-time scan and again by an add of the same node, so a bounced internal device stays internal
        const char *const internal_dev_path = _ctx.c_api.storage.internal_device_path;
        const auto internal_slot_info = [this, internal_dev_path]() {
            std::string mount_point;
            if (_ctx.c_api.storage.internal_mount_point)
                mount_point = _ctx.c_api.storage.internal_mount_point;
            else
                mount_point = getMountPath(std::filesystem::path(internal_dev_path).filename().string());
            return SlotInfo { std::move(mount_point), false, true };
        };

        if (!_initial_scan_done)
        {
            if (internal_dev_path)
                dev_2_slot_info_vec.emplace(internal_dev_path, internal_slot_info());

            try
            {
                for (const auto &entry: std::filesystem::directory_iterator("/sys/class/block"))
                {
                    const auto dev_name = entry.path().filename().string();
                    if (dev_name.starts_with("loop") || dev_name.starts_with("ram") || dev_name.starts_with("sr"))
                        continue;
                    if (!isValidDevName(dev_name))
                        continue;
                    if (std::error_code ec2; std::filesystem::exists(entry.path() / "partition", ec2))
                    {
                        if (isDevHidden(dev_name))
                            continue;
                        dev_2_slot_info_vec.emplace("/dev/" + dev_name,
                            SlotInfo { getMountPath(dev_name), isDevRemovable(dev_name), false });
                    }
                }
                _initial_scan_done = true;
            }
            catch (const std::exception &e)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Exception occurred while scanning block devices: ", e.what());
            }
        }

        std::list<std::string> dev_2_remove;
        if (_netlink_fd >= 0)
        {
            char buffer[kStorageNetlinkBufferSize];
            while (true)
            {
                ssize_t len = ::recv(_netlink_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
                if (len < 0)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Netlink recv error: ", static_cast<int>(errno));
                    break;
                }
                std::string action, devname, subsystem, devtype;
                const char *ptr = buffer;
                const auto end = ptr + len;
                while (ptr < end)
                {
                    const size_t remaining = end - ptr;
                    const size_t s_len = ::strnlen(ptr, remaining);
                    if (s_len == remaining)
                        break; // No null-terminator found
                    const std::string_view s(ptr, s_len);
                    if (s.starts_with(kStorageNetlinkAction))
                        action = s.substr(kStorageNetlinkAction.length());
                    else if (s.starts_with(kStorageNetlinkDevName))
                        devname = s.substr(kStorageNetlinkDevName.length());
                    else if (s.starts_with(kStorageNetlinkSubsystem))
                        subsystem = s.substr(kStorageNetlinkSubsystem.length());
                    else if (s.starts_with(kStorageNetlinkDevType))
                        devtype = s.substr(kStorageNetlinkDevType.length());
                    ptr += s_len + 1;
                }

                if (subsystem == "block" && !devname.empty() && isValidDevName(devname) && !isDevHidden(devname))
                {
                    const std::string dev_path = "/dev/" + devname;
                    if (action == "add")
                    {
                        if (internal_dev_path && dev_path == internal_dev_path)
                            dev_2_slot_info_vec.emplace(dev_path, internal_slot_info());
                        else if (devtype == "partition")
                            dev_2_slot_info_vec.emplace(dev_path,
                                SlotInfo { getMountPath(devname), isDevRemovable(devname), false });
                    }
                    else if (action == "remove")
                    {
                        // Arrival order decides: an add queued before the remove is discarded, one after it stands
                        dev_2_slot_info_vec.erase(dev_path);
                        dev_2_remove.emplace_back(dev_path);
                    }
                }
            }
        }

        for (const auto &dev_path: dev_2_remove)
            removeSlot(dev_path);

        {
            const std::shared_lock lock(_slots_mutex);
            for (const auto &[dev_path, _]: _slots)
            {
                if (dev_2_slot_info_vec.contains(dev_path))
                    dev_2_slot_info_vec.erase(dev_path);
            }
        }

        for (const auto &[dev_path, info]: dev_2_slot_info_vec)
        {
            auto slot = std::make_shared<Slot>(_ctx, dev_path, info.mount_point, info.removable, info.internal,
                _data_dir_path);
            {
                const std::unique_lock lock(_slots_mutex);
                if (!_slots.contains(dev_path)) [[likely]]
                {
                    _slots.emplace(dev_path, std::move(slot));
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Added storage slot for device ", dev_path);
                }
            }
        }
    }

    // Drops the Slot before the map lets it go, as ejectSlot does: its threads are joined first, so the last reference
    // is never released on one of them (a status copy in a FILE callback), and it is unmounted, unless internal, before
    // the pass below may build a new Slot at the same mount path. The device is gone, so the entry goes either way, and
    // only this thread inserts, so the erase cannot hit a replacement.
    void removeSlot(const std::string &dev_path) noexcept
    {
        std::shared_ptr<Slot> slot;
        {
            const std::shared_lock lock(_slots_mutex);
            if (const auto it = _slots.find(dev_path); it != _slots.end())
                slot = it->second;
        }
        if (!slot)
            return;
        if (const auto ret = slot->drop(); ret != 0)
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to disable storage slot with device path ", dev_path,
                ", code: ", ret);
        {
            const std::unique_lock lock(_slots_mutex);
            _slots.erase(dev_path);
        }
        slot.reset();
        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Removed storage slot for device ", dev_path);
    }

    bool isSelectedSlot(const std::shared_ptr<Slot> &slot, const StorageConfig &config) const noexcept
    {
        if (!slot) [[unlikely]]
            return false;
        if (!config.enabled)
            return false;
        bool dev_path_match = config.target_enabled_slot_dev_path.empty()
                                  ? true
                                  : slot->devPath() == config.target_enabled_slot_dev_path;
        bool uuid_match
            = config.target_enabled_slot_uuid.empty() ? true : slot->getUUID() == config.target_enabled_slot_uuid;
        return dev_path_match && uuid_match;
    }

    // Enables the selected Slot only after every other enabled Slot was disabled, which joins its threads and cancels
    // its pending tasks, so a task never moves to another device. Of several matching Slots (cloned cards sharing a
    // UUID), the one already enabled stays the target.
    vg_sta_e syncEnabledSlotWithConfig()
    {
        const auto current_cfg = getConfig();
        std::vector<std::shared_ptr<Slot>> enabled_slots_without_target;
        std::shared_ptr<Slot> target_slot;
#ifdef VG_RETURN_ON_EJECTION
#error "VG_RETURN_ON_EJECTION is already defined"
#endif
#define VG_RETURN_ON_EJECTION()                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (_ejection_in_progress.load(std::memory_order_acquire))                                                     \
            return 0;                                                                                                  \
    }                                                                                                                  \
    while (0)
        VG_RETURN_ON_EJECTION();
        {
            const std::shared_lock lock2(_slots_mutex);
            for (const auto &[_, s]: _slots)
            {
                if (isSelectedSlot(s, current_cfg) && !(target_slot && target_slot->isEnabled()))
                    target_slot = s;
                else if (s->isEnabled())
                    enabled_slots_without_target.emplace_back(s);
            }
        }
        for (const auto &slot: enabled_slots_without_target)
        {
            if (const auto ret = slot->disable(); ret != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to disable storage slot with device path ", slot->devPath(),
                    ", code: ", ret);
                return ret;
            }
        }
        if (!target_slot)
            return 0;
        VG_RETURN_ON_EJECTION();
        if (!target_slot->isEnabled())
        {
            VG_RETURN_ON_EJECTION();
            if (const auto ret = target_slot->enable(); ret != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to enable storage slot with device path ",
                    target_slot->devPath(), ", code: ", ret);
                return ret;
            }
        }
        return 0;
    }

private:
    static inline constexpr const std::string_view kDataDirDefaultPath = "DCIM";
    static inline constexpr const std::chrono::milliseconds kManagerLoopInterval = std::chrono::milliseconds(100);

private:
    friend struct StorageTestAccess; // defined by the module tests only

    Context &_ctx;

    std::filesystem::path _data_dir_path;

    mutable std::shared_mutex _config_mutex;
    StorageConfig _config;

    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _stop_token;
    std::unique_ptr<std::thread> _manager_thread;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _ejection_in_progress;

    mutable std::shared_mutex _slots_mutex;
    std::unordered_map<std::string, std::shared_ptr<Slot>> _slots;

    int _netlink_fd;
    bool _initial_scan_done;
};

} // namespace vigil::storage

#endif
