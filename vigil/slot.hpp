#pragma once
#ifndef VIGIL_SLOT_HPP
#define VIGIL_SLOT_HPP

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <blkid/blkid.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "constants.hpp"
#include "context.hpp"
#include "index_manager.hpp"
#include "log.hpp"
#include "relay.hpp"
#include "status.hpp"
#include "types.hpp"
#include "utils.hpp"
#include "write_stats.hpp"
#include "writers.hpp"

namespace vigil::slot {

using namespace vigil::status;
using namespace vigil::types;
using namespace vigil::constants;
using namespace vigil::index_manager;
using namespace vigil::write_stats;
using namespace vigil::relay;
using namespace vigil::writers;
using namespace vigil::utils;

using CancellationCheck = std::function<bool()>;

static inline size_t SystemPreservedUsageProbe() noexcept
{
    static const std::chrono::duration<size_t, std::milli> probe_interval = std::chrono::milliseconds(50);
    static auto last_probe_time = std::chrono::steady_clock::now() - probe_interval;
    static size_t last_usage = 0;

    if (const auto now = std::chrono::steady_clock::now(); now - last_probe_time >= probe_interval)
    {
        struct ::stat st;
        if (::stat(kStoragePreservedProbeFilePath.data(), &st) == 0)
            last_usage = st.st_blocks > 0 ? st.st_blocks * 512 : 0; // st_blocks is in 512-byte units
        else
            last_usage = 0;
        last_probe_time = now;
    }
    return last_usage;
}

static inline size_t GetInternalPreservedLimit() noexcept
{
    const auto system_usage = SystemPreservedUsageProbe();
    return system_usage >= kStorageMinFileSystemFreeBytesInternal
               ? system_usage
               : kStorageMinFileSystemFreeBytesInternal - system_usage;
}

class Slot final
{
public:
    Slot(Context &ctx, std::filesystem::path dev_path, std::filesystem::path mount_path, bool removable, bool internal,
        std::string_view data_dir_name)
        : _ctx(ctx), _dev_path(std::move(dev_path)), _mount_path(std::move(mount_path)), _removable(removable),
          _internal(internal), _data_dir_name(data_dir_name), _info_mutex(), _label(), _uuid(), _type(), _mutex(),
          _op_mutex(), _stop_token(false), _manager_thread(nullptr), _writer_thread(nullptr), _state(SlotState::Error),
          _stats(), _quota_limit_bytes(-1), _quota_rotate(true), _index_manager(nullptr), _writer_task_count(0),
          _rotating(false), _write_stats(), _relay(ctx, _mount_path / _data_dir_name)
    {
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Creating storage slot for device path ", _dev_path, " at mount path ",
            _mount_path, ", removable: ", _removable ? "true" : "false");
        if (ensureStateResetToInitial())
        {
            ensureStateMounted();
            ensureStateResetToInitial(); // Reset to initial state after mounting
        }
    }

    ~Slot() noexcept
    {
        disable();
        ensureStateForceUnmounted();

        // Detach slot check callback (if current attached)
        {
            const std::unique_lock lock2(_ctx.slot_check_mutex);
            if (_ctx.slot_check.first == static_cast<const void *>(this))
                _ctx.slot_check = std::make_pair(nullptr, nullptr);
        }
    }

    const std::filesystem::path &devPath() const noexcept
    {
        return _dev_path;
    }

    const std::filesystem::path &mountPath() const noexcept
    {
        return _mount_path;
    }

    inline bool isRemovable() const noexcept
    {
        return _removable;
    }

    inline bool isInternal() const noexcept
    {
        return _internal;
    }

    inline bool isEnabled() const noexcept
    {
        const std::shared_lock lock(_mutex);
        return _manager_thread && _writer_thread;
    }

    inline bool isReadyToWrite() const noexcept
    {
        return !_stop_token.load(std::memory_order_acquire)
               && _state.load(std::memory_order_acquire) == SlotState::Ready;
    }

    inline bool isWriting() const noexcept
    {
        return _writer_task_count.load(std::memory_order_acquire) > 0;
    }

    inline bool isRotating() const noexcept
    {
        return _rotating.load(std::memory_order_acquire);
    }

    std::optional<std::string> getUUID() const noexcept
    {
        std::shared_lock lock(_info_mutex);
        return _uuid;
    }

    std::shared_ptr<IndexManager> getIndexManager() const noexcept
    {
        return loadIndexManager();
    }

    SlotStatus getStatus() const noexcept
    {
        const auto is_enabled = isEnabled();
        auto state = _state.load(std::memory_order_acquire);
        if (!is_enabled && state != SlotState::Formatting)
            state = probeInitialStatusAndGetState();
        std::optional<std::string> label, uuid, format;
        {
            std::shared_lock lock(_info_mutex);
            label = _label;
            uuid = _uuid;
            format = _type;
        }
        const auto idx_mgr = loadIndexManager();
        return SlotStatus {
            .dev_path = _dev_path,
            .mount_path = _mount_path,
            .removable = isRemovable(),
            .internal = isInternal(),
            .label = std::move(label),
            .uuid = std::move(uuid),
            .type = std::move(format),
            .selected = false,
            .enabled = is_enabled,
            .syncing = idx_mgr ? idx_mgr->indexing() : false,
            .writing = isWriting(),
            .rotating = isRotating(),
            .state = state,
            .stats = _stats.load(std::memory_order_acquire),
            .quota_minimum_recommend_bytes = [this]() -> FileSize {
                const auto watermarks = _ctx.shared_writer_settings.getWaterMarks();
                return std::max(watermarks.high_remain_bytes + watermarks.recycle_bytes,
                    static_cast<FileSize>(kStorageMinSupportedFileSystemBytes));
            }(),
            .quota_preserved_bytes = isInternal() ? static_cast<FileSize>(kStorageMinFileSystemFreeBytesInternal) : 0,
            .quota_used_bytes = [&idx_mgr]() -> std::optional<FileSize> {
                if (idx_mgr)
                    return idx_mgr->size().size;
                return std::nullopt;
            }(),
            .quota_limit_bytes = _quota_limit_bytes.load(std::memory_order_acquire),
            .quota_rotate = _quota_rotate.load(std::memory_order_acquire),
        };
    }

    vg_sta_e enable() noexcept
    {
        const std::unique_lock lock(_mutex);
        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Enabling storage slot for device path ", _dev_path);

        if (_manager_thread && _writer_thread)
            return 0;

        if (ensureStateResetToInitial())
        {
            ensureStateMounted();
            ensureStateConfigured();
            ensureStateResetToInitial(); // Reset to initial state after mounting
        }

        // Reset write stats and cache
        resetWriteStatsUnsafe();
        resetWriteCacheUnsafe();

        _stop_token.store(false, std::memory_order_release);
        try
        {
            // Launch manager thread
            if (!_manager_thread)
                _manager_thread = std::make_unique<std::thread>(&Slot::managerThreadTask, this);
            // Launch writer thread
            if (!_writer_thread)
                _writer_thread = std::make_unique<std::thread>(&Slot::writerThreadTask, this);
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create threads for storage slot for device path ",
                _dev_path, ": ", e.what());
            return EFAULT;
        }

        // Attach slot check callback (single slot only)
        {
            const std::unique_lock lock2(_ctx.slot_check_mutex);
            _ctx.slot_check = std::make_pair(static_cast<const void *>(this), [](const void *p) -> bool {
                if (!p)
                    return false;
                const Slot *sp = static_cast<const Slot *>(p);
                if (!sp->_mutex.try_lock())
                    return false; // Treat as not ready to write if cannot acquire lock
                const auto ready = sp->isReadyToWrite();
                sp->_mutex.unlock();
                return ready;
            });
        }

        return 0;
    }

    void disable() noexcept
    {
        const std::unique_lock lock(_mutex);
        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Disabling storage slot for device path ", _dev_path);

        // Disable relay
        disableNginxRelay();

        _stop_token.store(true, std::memory_order_release);
        try
        {
            if (_writer_thread)
            {
                _writer_thread->join();
                _writer_thread.reset();
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to safely shutdown writer thread for device path ", _dev_path,
                ": ", e.what());
        }
        try
        {
            if (_manager_thread)
            {
                // Stop manager thread
                _manager_thread->join();
                _manager_thread.reset();
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to safely shutdown manager thread for device path ", _dev_path,
                ": ", e.what());
        }

        // Reset index manager
        storeIndexManager(nullptr);

        // Reset rotating status
        _rotating.store(false, std::memory_order_release);
    }

    vg_sta_e setQuotaLimitBytes(FileSize bytes)
    {
        if (bytes < -1 || bytes > _stats.load(std::memory_order_acquire).size_bytes)
            return EINVAL;
        const std::unique_lock lock(_mutex);
        if (_state.load(std::memory_order_acquire) == SlotState::Mounted)
            ensureStateConfigured();
        if (_state.load(std::memory_order_acquire) < SlotState::Configured)
            return EPERM;
        if (!writeQuotaLimitToFile(bytes))
            return EIO;
        _quota_limit_bytes.store(bytes, std::memory_order_release);
        return 0;
    }

    vg_sta_e setQuotaRotate(bool rotate)
    {
        const std::unique_lock lock(_mutex);
        if (_state.load(std::memory_order_acquire) == SlotState::Mounted)
            ensureStateConfigured();
        if (_state.load(std::memory_order_acquire) < SlotState::Configured)
            return EPERM;
        if (!writeQuotaRotateToFile(rotate))
            return EIO;
        _quota_rotate.store(rotate, std::memory_order_release);
        return 0;
    }

    vg_sta_e removeFileOrDirectory(const std::filesystem::path &relative_path)
    {
        if (auto state = _state.load(std::memory_order_acquire); state < SlotState::Configured)
            return EPERM;

        if (auto p = getRecordRootPath(); IsPathUnderRoot(p, relative_path))
        {
            if (!RemoveWithThumbnailCheck(p / relative_path))
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove file or directory ", relative_path,
                    " on slot with device path ", _dev_path);
                return EIO;
            }
        }
        else
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Path ", relative_path, " is outside record root ", p,
                ", cannot remove");
            return EACCES;
        }

        return 0;
    }

    vg_sta_e removeFilesOrDirectories(const std::vector<std::filesystem::path> &relative_paths,
        SlotFileOpsProgress *progress = nullptr, const CancellationCheck &should_cancel = {},
        const SlotFileOpsProgressCallback &on_progress = {})
    {
        if (auto state = _state.load(std::memory_order_acquire); state < SlotState::Configured)
            return EPERM;

        auto local_progress = SlotFileOpsProgress(SlotFileOpsProgress::Type::Remove);
        auto &active_progress = progress ? *progress : local_progress;
        active_progress = SlotFileOpsProgress(SlotFileOpsProgress::Type::Remove);
        active_progress.requested_count = relative_paths.size();
        vg_sta_e ret = 0;

        auto update_progress = [&]() {
            if (on_progress)
                on_progress(active_progress);
        };

        update_progress();

        for (const auto &relative_path: relative_paths)
        {
            if (should_cancel && should_cancel())
                return ECANCELED;

            try
            {
                if (const auto path_ret = removeFileOrDirectory(relative_path); path_ret == 0)
                    ++active_progress.success_count;
                else
                {
                    if (ret == 0)
                        ret = path_ret;
                    ++active_progress.failed_count;
                }
            }
            catch (const std::exception &e)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Exception occurred while removing file or directory ",
                    relative_path, " on slot with device path ", _dev_path, ": ", e.what());
                if (ret == 0)
                    ret = EIO;
                ++active_progress.failed_count;
            }

            ++active_progress.processed_count;
            update_progress();
        }

        return ret;
    }

    vg_sta_e freeUpUsedSpace(SlotFileOpsProgress *progress = nullptr, const CancellationCheck &should_cancel = {},
        const SlotFileOpsProgressCallback &on_progress = {})
    {
        if (auto state = _state.load(std::memory_order_acquire); state < SlotState::Configured)
            return EPERM;

        vg_sta_e ret = 0;
        auto local_progress = SlotFileOpsProgress(SlotFileOpsProgress::Type::FreeUp);
        auto &active_progress = progress ? *progress : local_progress;
        active_progress = SlotFileOpsProgress(SlotFileOpsProgress::Type::FreeUp);

        auto update_progress = [&]() {
            if (on_progress)
                on_progress(active_progress);
        };

        try
        {
            const auto record_root_path = getRecordRootPath();
            const auto is_hidden_entry = [](const std::filesystem::directory_entry &entry) {
                const auto file_name = entry.path().filename();
                const auto &native_name = file_name.native();
                return !native_name.empty()
                       && native_name.front() == static_cast<std::filesystem::path::value_type>('.');
            };

            update_progress();

            for (const auto &entry: std::filesystem::directory_iterator(record_root_path,
                     std::filesystem::directory_options::skip_permission_denied))
            {
                if (should_cancel && should_cancel())
                {
                    ret = ECANCELED;
                    break;
                }
                if (is_hidden_entry(entry))
                    continue; // Skip hidden files and dirs
                ++active_progress.requested_count;

                const auto entry_path = entry.path();
                std::error_code ec;
                std::filesystem::remove_all(entry_path, ec);
                if (ec)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to remove file or directory: ", entry_path,
                        ", error: ", ec.message());
                    if (ret == 0)
                        ret = EIO;
                    ++active_progress.failed_count;
                }
                else
                    ++active_progress.success_count;

                ++active_progress.processed_count;
                update_progress();
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to iterate record root path for freeing up space: ", e.what());
            ret = EIO;
        }
        if (auto idx_mgr = loadIndexManager(); idx_mgr)
            idx_mgr->requestReindex(true);

        return ret;
    }

    inline std::filesystem::path getRecordRootPath() const noexcept
    {
        return _mount_path / _data_dir_name;
    }

    vg_sta_e formatFileSystem() noexcept
    {
        if (_internal)
            return EPERM;
        const std::unique_lock lock(_mutex);
        if (const auto state = _state.load(std::memory_order_acquire);
            state != SlotState::NotFormattedOrFormatUnsupported)
            return EPERM;
        if (!ensureStateFormatted())
            return EIO;
        return 0;
    }

    vg_sta_e enableOrHeartbeatNginxRelay()
    {
        return _relay.enableOrHeartbeatNginxRelay();
    }

    vg_sta_e statusOfNginxRelay(RelayStatus &status) noexcept
    {
        status = _relay.getStatus();
        return 0;
    }

    void disableNginxRelay() noexcept
    {
        _relay.disableNginxRelay();
    }

    // - MARK: Slot Protected Methods

protected:
    friend SlotAdaptedWriter<Slot>;

    inline Context &ctxRef() const
    {
        return _ctx;
    }

    inline std::atomic<size_t> &writerCountRef()
    {
        return _writer_task_count;
    }

    void moveAndAddCacheFile(const CacheFileInfo &cache_file_info) noexcept
    {
        if (cache_file_info.path.empty())
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                "Cache file info has empty path, first-level dir name: ", cache_file_info.first_level_dir_name);
            return;
        }

        const auto target_dir_path = getRecordRootPath() / cache_file_info.first_level_dir_name;
        const auto target_file_path = target_dir_path / cache_file_info.path.filename();
        const auto thumbnail_path = ThumbnailPathFrom(target_file_path);

        if (cache_file_info.thumbnail && _ctx.c_api.media.thumbnail_generate_cb)
        {
            if (!EnsureDirectoryExists(thumbnail_path.parent_path(), true, false, &_ctx))
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create thumbnail directory at ",
                    thumbnail_path.parent_path());
            }
            else if (auto ret = _ctx.c_api.media.thumbnail_generate_cb(cache_file_info.thumbnail.data,
                         cache_file_info.thumbnail.size, cache_file_info.thumbnail.format, thumbnail_path.c_str(),
                         kStorageThumbnailFileType, kStorageThumbnailMaxWH, _ctx.c_api.media.user_data);
                     ret != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to generate thumbnail for cache file ",
                    cache_file_info.path, ", code: ", static_cast<int>(ret));
            }
            else
                EnsureOwnerAndPermissions(thumbnail_path, kStorageDefaultUid, kStorageDefaultGid,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::group_write
                        | fs::perms::others_read,
                    &this->ctxRef());
        }

        size_t size = 0;
        std::error_code ec;
        if (size = std::filesystem::file_size(cache_file_info.path, ec); ec)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to get file size for cache file ", cache_file_info.path, ": ",
                ec.message());
            goto l_fail_add_cache_file;
        }
        _write_stats.addNewWriteStat(size);
        if (!EnsureDirectoryExists(target_dir_path, true, false, &_ctx))
            goto l_fail_add_cache_file;

        EnsureOwnerAndPermissions(cache_file_info.path, kStorageDefaultUid, kStorageDefaultGid,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::group_write
                | fs::perms::others_read,
            &this->ctxRef());

        if (std::filesystem::rename(cache_file_info.path, target_file_path, ec); ec)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to move file from ", cache_file_info.path, " to ",
                target_file_path, ": ", ec.message());
            goto l_fail_add_cache_file;
        }

        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Created new record file at ", target_file_path, ", size: ", size,
            " bytes");

        if (_ctx.c_api.basic.on_file_event_cb)
        {
            const auto path_str = target_file_path.string();
            const auto attributes_str
                = cache_file_info.event ? WriteEvent::StatusString(cache_file_info.event->status) : "";
            const auto file_evt = vg_file_evt_t {
                .op_type = VG_FILE_EVT_OP_ADDED,
                .path = path_str.c_str(),
                .size_bytes = size,
                .attributes = attributes_str,
                .event_uid = cache_file_info.event ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         cache_file_info.event->timepoint.time_since_epoch())
                                                         .count()
                                                   : 0,
                .timestamp = _ctx.shared_time.unixTime(),
            };
            _ctx.c_api.basic.on_file_event_cb(&file_evt);
        }
        return;

    l_fail_add_cache_file:
        std::filesystem::remove_all(thumbnail_path, ec);
        std::filesystem::remove_all(cache_file_info.path, ec);
        return;
    }

    std::filesystem::path getRecordQuotaLimitPath() const
    {
        return getRecordRootPath() / kRecordQuotaLimitFileName;
    }

    std::filesystem::path getRecordQuotaNoRotatePath() const
    {
        return getRecordRootPath() / kRecordQuotaNoRotateFileName;
    }

    std::filesystem::path getRecordCachePath() const
    {
        return getRecordRootPath() / kRecordCacheDirName;
    }

    // - MARK: Slot Private Methods

private:
    std::shared_ptr<IndexManager> loadIndexManager(std::memory_order order = std::memory_order_acquire) const noexcept
    {
        return std::atomic_load_explicit(&_index_manager, order);
    }

    void storeIndexManager(std::shared_ptr<IndexManager> index_manager,
        std::memory_order order = std::memory_order_release) const noexcept
    {
        std::atomic_store_explicit(&_index_manager, index_manager, order);
    }

    void resetWriteStatsUnsafe()
    {
        _write_stats.reset();
    }

    void resetWriteCacheUnsafe()
    {
        if (_state.load(std::memory_order_acquire) < SlotState::Configured) // Requires the root folder to be ready
            return;                                                         // No cache to clear if not mounted
        const auto cache_path = getRecordCachePath();
        std::error_code ec;
        std::filesystem::remove_all(cache_path, ec);
        if (ec)
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to clear record cache at ", cache_path, ": ", ec.message());
    }

    bool probeStateHealth() const noexcept
    {
        return probeDeviceOnline() && probeCorrectlyMounted() && probeCorrectlyConfigured();
    }

    void doRotateStaledFilesAndSync() const noexcept
    {
        // Check if we need to free up space
        if (!_quota_rotate.load(std::memory_order_acquire))
        {
            _rotating.store(false, std::memory_order_release);
            return;
        }

        // Get index manager
        auto idx_mgr = loadIndexManager();
        if (!idx_mgr)
        {
            _rotating.store(false, std::memory_order_release);
            return;
        }

        const auto limit = _quota_limit_bytes.load(std::memory_order_acquire);
        const auto watermarks = _ctx.shared_writer_settings.getWaterMarks();

        // Check if quota limit is exceeded
        if (limit >= 0)
        {
            if (auto index_result = idx_mgr->size(); !index_result.valid)
                return;
            else if (index_result.size + watermarks.high_remain_bytes > limit)
                goto l_free_up_space;
        }

        // Check if free space is sufficient
        if (doStats();
            _stats.load(std::memory_order_acquire).free_bytes
            < (isInternal() ? static_cast<FileSize>(watermarks.high_remain_bytes + GetInternalPreservedLimit())
                            : watermarks.high_remain_bytes))
            goto l_free_up_space;

        _rotating.store(false, std::memory_order_release);
        return;

    l_free_up_space:
        _rotating.store(true, std::memory_order_release);

        const auto estimated_write_bytes_in_period
            = _write_stats.getEstimatedWriteBytesIn(kManagerEstimatedWriteBytesPeriod);
        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Storage slot for device path ", _dev_path,
            " is low on free space, quota limit: ", limit, " bytes, estimated write bytes in next ",
            kManagerEstimatedWriteBytesPeriod.count(), " minutes: ", estimated_write_bytes_in_period,
            " bytes, will free up ", watermarks.recycle_bytes, " bytes");

        const auto freed = idx_mgr->tryReleaseSpace(watermarks.recycle_bytes);
        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Freed up ", freed, " bytes on storage slot for device path", _dev_path);
    }

    void managerThreadTask() noexcept
    {
        while (!_stop_token.load(std::memory_order_acquire))
        {
            const auto start = std::chrono::steady_clock::now();
            // Check and recover state if needed
            if (!probeStateHealth())
            {
                if (!ensureStateResetToInitial())
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to reset storage slot for device path ", _dev_path,
                        " to initial state, will retry");
                    goto l_wait_and_continue;
                }
            }
            goto l_check_state_loop;

        l_check_state_loop_delayed:
            if (auto elapsed = std::chrono::steady_clock::now() - start; elapsed < kManagerProbeInterval)
                std::this_thread::sleep_for(kManagerProbeInterval - elapsed);

        l_check_state_loop:
            if (_stop_token.load(std::memory_order_acquire))
                return;
            switch (_state.load(std::memory_order_acquire))
            {
                case SlotState::Error: {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Storage slot for device path ", _dev_path,
                        " is in error state, will retry mounting");
                    // Error state requires a longer delay before retrying
                    const auto s = std::chrono::steady_clock::now();
                    do
                    {
                        if (_stop_token.load(std::memory_order_acquire))
                            return;
                        std::this_thread::sleep_for(kManagerProbeInterval);
                    }
                    while (std::chrono::steady_clock::now() - s < kManagerRecoverDelay);
                    continue; // Probe and reset again
                }

                case SlotState::NotFormattedOrFormatUnsupported:
                    // Wait for external format request
                    goto l_check_state_loop_delayed;

                case SlotState::Formatting:
                    // Wait for formatting to complete
                    goto l_check_state_loop_delayed;

                case SlotState::NotMounted:
                    if (!ensureStateMounted())
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to mount storage slot for device path ", _dev_path,
                            ", will retry");
                        goto l_wait_and_continue;
                    }
                    goto l_check_state_loop;

                case SlotState::Mounted:
                    // Wait for external configuration request
                    goto l_check_state_loop_delayed;

                case SlotState::Configured:
                    if (!ensureStateReady())
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN,
                            "Failed to make configured storage slot ready for device path ", _dev_path, ", will retry");
                        goto l_wait_and_continue;
                    }
                    goto l_check_state_loop;

                case SlotState::Indexing:
                    // Wait for indexing to complete
                    goto l_check_state_loop_delayed;

                case SlotState::Ready:
                    // All good, rotate staled files and sync stats
                    doRotateStaledFilesAndSync();
                    goto l_wait_and_continue;

                default:
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Storage slot for device path ", _dev_path,
                        " is in unknown state, will reset");
                    goto l_wait_and_continue;
            }

        l_wait_and_continue:
            if (auto elapsed = std::chrono::steady_clock::now() - start; elapsed < kManagerProbeInterval)
                std::this_thread::sleep_for(kManagerProbeInterval - elapsed);
        }
    }

    void writerThreadTask() noexcept
    {
        auto handle_write = [this](SlotAdaptedWriter<Slot> &writer) -> bool {
            const auto result = writer.operator()();
            const auto end = writer.endTime();
            if (result)
            {
                if (result->status != WriteEvent::Status::Interrupted)
                {
                    do
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        if (_stop_token.load(std::memory_order_acquire))
                        {
                            _ctx.shared_rule_event.reset(result->status);
                            return false; // Stop requested
                        }
                    }
                    while (std::chrono::steady_clock::now() < end);
                }
                _ctx.shared_rule_event.reset(result->status);
            }
            return true; // Continue
        };

        _ctx.shared_rule_event.reset(WriteEvent::Status::Canceled); // Clear any existing rule event at start
        while (!_stop_token.load(std::memory_order_acquire))
        {
            std::shared_ptr<WriteEvent> event = nullptr;
            const auto start = std::chrono::steady_clock::now();
            const auto [rev, settings] = _ctx.shared_writer_settings.get();
            if (!settings)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                    "FileWriterSettings is null in storage slot writer thread for device path ", _dev_path,
                    ", cannot start new writer task");
                goto l_continue_evt;
            }

            {
                const auto watermarks = settings->watermarks;

                // Check if slot status is ready to write
                if (!isReadyToWrite())
                    goto l_continue_evt;

                // Check if quota limit is exceeded
                if (auto limit = _quota_limit_bytes.load(std::memory_order_acquire); limit >= 0)
                {
                    if (auto idx_mgr = loadIndexManager(); !idx_mgr)
                        goto l_continue_evt;
                    else if (auto index_result = idx_mgr->size(); !index_result.valid)
                        goto l_continue_evt;
                    else if (index_result.size + watermarks.critical_remain_bytes > limit)
                        goto l_continue_log;
                }

                // Check if free space is sufficient
                if (doStats(); _stats.load(std::memory_order_acquire).free_bytes
                               <= (isInternal() ? static_cast<FileSize>(
                                                      watermarks.critical_remain_bytes + GetInternalPreservedLimit())
                                                : watermarks.critical_remain_bytes))
                    goto l_continue_log;

                // Check if media is paused
                if (_ctx.media_paused.load(std::memory_order_acquire))
                    goto l_continue_evt;

                // Check for new write event
                if (event = _ctx.shared_rule_event.getCurrent(); !event)
                    goto l_continue_evt;

                // Start new writer task

                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Starting new writer task, dev path ", _dev_path);
                try
                {
                    switch (settings->type)
                    {
                        case FileWriterSettings::CaptureType::Image:
                            if (auto writer = std::make_unique<ImageWriter<SlotAdaptedWriter<Slot>>>(std::ref(*this),
                                    rev, std::ref(_ctx), std::move(settings), std::move(event));
                                writer && !handle_write(*writer))
                                return;
                            break;
                        case FileWriterSettings::CaptureType::Video:
                            if (auto writer = std::make_unique<VideoWriter<SlotAdaptedWriter<Slot>>>(std::ref(*this),
                                    rev, std::ref(_ctx), std::move(settings), std::move(event));
                                writer && !handle_write(*writer))
                                return;
                            break;
                        default: {
                            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                                "Unsupported writer type for storage slot with device path ", _dev_path);
                            _ctx.shared_rule_event.reset(WriteEvent::Status::Failed); // Clear processed event
                        }
                        break;
                    }
                }
                catch (const std::exception &e)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                        "Failed to create new writer task for storage slot with device "
                        "path ",
                        _dev_path, ": ", e.what());
                }
                continue;
            }

        l_continue_log:
            if (_quota_rotate.load())
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Writer thread for ", _dev_path,
                    " reached critical watermark, skipping new writer task start");
            }

        l_continue_evt:
            if (event)
                _ctx.shared_rule_event.reset(WriteEvent::Status::Canceled); // Clear processed event
            if (auto elapsed = std::chrono::steady_clock::now() - start; elapsed < kWriterProbeInterval)
                std::this_thread::sleep_for(kWriterProbeInterval - elapsed);
            continue;
        }
    }

    // - MARK: Slot Private Methods

private:
    bool probeDeviceOnline() const noexcept
    {
        std::error_code ec;
        if (std::filesystem::exists(_dev_path, ec))
            return true;
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Device path ", _dev_path,
            " does not exist or is inaccessible: ", ec.message());
        return false;
    }

    bool doUpdatePartionInfo() const noexcept
    {
        const std::shared_lock lock(_op_mutex);
        blkid_probe pr = blkid_new_probe_from_filename(_dev_path.c_str());
        if (!pr)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create blkid probe for device ", _dev_path);
            return false;
        }
        blkid_probe_enable_superblocks(pr, 1);
        blkid_probe_set_superblocks_flags(pr,
            BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID | BLKID_SUBLKS_TYPE | BLKID_SUBLKS_USAGE);
        if (auto ret = blkid_do_probe(pr); ret != 0)
        {
            if (ret == 1)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "No recognizable filesystem found on device ", _dev_path);
                const std::unique_lock lock2(_info_mutex);
                _label.reset();
                _uuid.reset();
                _type.reset();
            }
            else
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to probe device ", _dev_path,
                    " for device info, code: ", ret, ", errno: ", static_cast<int>(errno));
            blkid_free_probe(pr);
            return ret == 1;
        }
        {
            const char *val = nullptr;
            const std::unique_lock lock2(_info_mutex);
            if (blkid_probe_lookup_value(pr, "LABEL", &val, nullptr) == 0 && val)
                _label = std::string(val);
            else
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "No LABEL found for device ", _dev_path);
            if (blkid_probe_lookup_value(pr, "UUID", &val, nullptr) == 0 && val)
                _uuid = std::string(val);
            else
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "No UUID found for device ", _dev_path);
            if (blkid_probe_lookup_value(pr, "TYPE", &val, nullptr) == 0 && val)
                _type = std::string(val);
            else
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "No TYPE found for device ", _dev_path);
        }
        blkid_free_probe(pr);
        return true;
    }

    bool probeDeviceFileSystemSupported() const noexcept
    {
        const std::shared_lock lock(_info_mutex);
        return _uuid.has_value() && _type.has_value()
               && kSupportedFileSystemTypes.find(*_type) != kSupportedFileSystemTypes.end();
    }

    SlotState probeInitialStatusAndGetState() const noexcept
    {
        if (!probeDeviceOnline())
            return SlotState::Error;
        if (!doUpdatePartionInfo())
            return SlotState::Error;
        if (!probeDeviceFileSystemSupported())
            return SlotState::NotFormattedOrFormatUnsupported;
        if (!probeCorrectlyMounted())
            return SlotState::NotMounted;
        if (!doStats())
            return SlotState::Error;
        if (!probeCorrectlyConfigured())
            return SlotState::Mounted;
        if (!doSyncQuotaLimit())
            return SlotState::Mounted;
        if (!doSyncQuotaRotate())
            return SlotState::Mounted;
        return SlotState::Configured;
    }

    bool ensureStateResetToInitial() noexcept
    {
        auto state = _state.load(std::memory_order_acquire);
        return _state.compare_exchange_strong(state, probeInitialStatusAndGetState(), std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool doFormat() const noexcept
    {
        if (_internal)
            return false;
        const std::unique_lock lock(_op_mutex);
        try
        {
            int ret = Execute("umount", { _dev_path.string() })();
            ret = Execute("mkfs.vfat", { "-F", "32", "-I", _dev_path.string() })();
            if (ret != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to format storage slot device ", _dev_path,
                    " with mkfs.vfat, return code: ", ret);
                return false;
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Exception occurred while formatting storage slot device ", _dev_path,
                ": ", e.what());
            return false;
        }
        return true;
    }

    bool ensureStateFormatted() noexcept // Never auto-format, must be called explicitly
    {
        auto state = _state.load(std::memory_order_acquire);
        if (state != SlotState::NotFormattedOrFormatUnsupported || state == SlotState::Formatting)
            return false;
        if (!_state.compare_exchange_strong(state, SlotState::Formatting, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return false;              // Early return if state changed, formatting should only be done by one thread
        state = SlotState::Formatting; // Update state variable
        if (!doFormat())
            goto l_format_error;
        if (!doUpdatePartionInfo())
            goto l_format_error;
        return _state.compare_exchange_strong(state, SlotState::NotMounted, std::memory_order_acq_rel,
            std::memory_order_acquire);

    l_format_error:
        while (!_state.compare_exchange_strong(state, SlotState::Error, std::memory_order_acq_rel,
            std::memory_order_acquire))
            std::this_thread::yield();
        return false;
    }

    bool probeCorrectlyMounted() const noexcept
    {
        struct stat dev_sb
        {
        };
        struct stat mnt_sb
        {
        };
        bool mount_stats_valid = false;
        bool mount_path_missing = false;

        {
            const std::shared_lock lock(_op_mutex);
            if (::stat(_dev_path.c_str(), &dev_sb) != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to stat device path ", _dev_path, ": ",
                    static_cast<int>(errno));
                return false;
            }

            if (::stat(_mount_path.c_str(), &mnt_sb) == 0)
            {
                mount_stats_valid = true;
            }
            else if (errno == ENOENT)
            {
                mount_path_missing = true;
            }
            else
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Failed to stat mount path ", _mount_path, ": ",
                    static_cast<int>(errno));
                return false;
            }
        }

        if (mount_path_missing)
            return false;

        if (!mount_stats_valid)
        {
            const std::shared_lock lock(_op_mutex);
            if (::stat(_mount_path.c_str(), &mnt_sb) != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Failed to stat mount path ", _mount_path, ": ",
                    static_cast<int>(errno));
                return false;
            }
        }

        if (dev_sb.st_rdev != mnt_sb.st_dev)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Device path ", _dev_path, " is not correctly mounted at mount path ",
                _mount_path);
            return false;
        }
        return true;
    }

    bool doUnmount() const noexcept
    {
        const std::unique_lock lock(_op_mutex);
        // Sync the filesystem before unmounting (syncfs)
        if (auto fd = ::open(_mount_path.c_str(), O_RDONLY); fd < 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to open mount path ", _mount_path,
                " for syncing before unmounting: ", static_cast<int>(errno));
        }
        else if (::syncfs(fd) != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to sync filesystem at mount path ", _mount_path,
                " before unmounting: ", static_cast<int>(errno));
            ::close(fd);
        }
        else
            ::close(fd);
        // Unmount the device
        if (::umount2(_mount_path.c_str(), MNT_DETACH) != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to unmount storage slot at path ", _mount_path, ": ",
                static_cast<int>(errno));
            return false;
        }
        {
            std::error_code ec;
            std::filesystem::remove_all(_mount_path, ec);
            if (ec)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove mount path ", _mount_path,
                    " after unmounting: ", ec.message());
            }
        }
        if (_ctx.c_api.storage.on_unmount_cb)
            _ctx.c_api.storage.on_unmount_cb(_dev_path.c_str(), _mount_path.c_str());
        return true;
    }

    bool doMount() const noexcept
    {
        const std::unique_lock lock(_op_mutex);
        {
            std::error_code ec;
            std::filesystem::remove_all(_mount_path, ec);
            if (ec)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove existing mount path ", _mount_path,
                    " before mounting: ", ec.message());
                return false;
            }
            if (std::filesystem::exists(_mount_path, ec))
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Mount path ", _mount_path,
                    " already exists and cannot be removed before mounting: ", ec.message());
                return false;
            }
        }
        if (!EnsureDirectoryExists(_mount_path, true, true, &_ctx))
            return false;
        bool is_native_fs = false;
        {
            const std::shared_lock lock(_info_mutex);
            if (_type.has_value())
                is_native_fs = kNativeFileSystemTypes.contains(*_type);
        }
        try
        {
            std::vector<std::string> args;
            if (!is_native_fs)
            {
                args.push_back("-o");
                std::string options = "uid=" + std::to_string(kStorageDefaultUid)
                                      + ",gid=" + std::to_string(kStorageDefaultGid) + ",umask=000";
                args.push_back(std::move(options));
            }
            args.push_back(_dev_path.string());
            args.push_back(_mount_path.string());
            if (auto ret = Execute("mount", std::move(args))(); ret != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to mount storage slot device ", _dev_path, " to ",
                    _mount_path, ", code: ", ret);
                return false;
            }
            EnsureOwnerAndPermissions(_mount_path, kStorageDefaultUid, kStorageDefaultGid,
                fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all);
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Exception occurred while mounting storage slot device ", _dev_path,
                " to ", _mount_path, ": ", e.what());
            return false;
        }
        if (_ctx.c_api.storage.on_mount_cb)
            _ctx.c_api.storage.on_mount_cb(_dev_path.c_str(), _mount_path.c_str());
        return true;
    }

    bool doStats() const noexcept
    {
        struct statvfs vfs
        {
        };
        const std::shared_lock lock(_op_mutex);
        if (::statvfs(_mount_path.c_str(), &vfs) != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to get filesystem stats for mount path ", _mount_path, ": ",
                static_cast<int>(errno));
            return false;
        }
        const auto new_stats = SlotStats(static_cast<FileSize>(vfs.f_blocks * vfs.f_frsize),
            static_cast<FileSize>(vfs.f_bfree * vfs.f_frsize));
        while (true)
        {
            SlotStats current_stats = _stats.load(std::memory_order_acquire);
            if (_stats.compare_exchange_strong(current_stats, new_stats, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }
        return true;
    }

    bool ensureStateMounted() noexcept
    {
        // Only mount if not mounted
        auto state = _state.load(std::memory_order_acquire);
        if (state != SlotState::NotMounted)
            return false;
        if (!doMount())
            goto l_mount_error;
        // Update stats
        if (!doStats())
            goto l_mount_error;
        return _state.compare_exchange_strong(state, SlotState::Mounted, std::memory_order_acq_rel,
            std::memory_order_acquire);

    l_mount_error:
        while (!_state.compare_exchange_strong(state, SlotState::Error, std::memory_order_acq_rel,
            std::memory_order_acquire))
            std::this_thread::yield();
        return false;
    }

    void ensureStateForceUnmounted() noexcept
    {
        if (!isInternal())
            _state.store(doUnmount() ? SlotState::NotMounted : SlotState::Error, std::memory_order_release);
    }

    FileSize readQuotaLimitFromFile() const noexcept
    {
        std::error_code ec;
        if (auto quota_file = getRecordQuotaLimitPath(); !std::filesystem::is_regular_file(quota_file, ec))
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "No valid quota file found at ", quota_file);
            return -1;
        }
        else
        {
            std::ifstream ifs(quota_file, std::ios::in);
            if (!ifs)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to open quota file ", quota_file, ": ", ec.message());
                return -1;
            }
            std::string line;
            if (std::getline(ifs, line))
            {
                try
                {
                    const FileSize quota_limit = std::stoll(line);
                    const auto stats = _stats.load(std::memory_order_acquire);
                    if (quota_limit < -1 || quota_limit >= stats.size_bytes)
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Quota limit ", quota_limit,
                            " bytes exceeds -1 or total size ", stats.size_bytes, " bytes, ignoring quota limit");
                        return -1;
                    }
                    return quota_limit;
                }
                catch (const std::exception &e)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to parse quota limit from file ", quota_file, ": ",
                        e.what());
                    return -1;
                }
            }
            ifs.close();
        }
        return -1;
    }

    bool writeQuotaLimitToFile(FileSize quota_limit) const noexcept
    {
        const auto quota_limit_path = getRecordQuotaLimitPath();
        if (std::error_code ec;
            !std::filesystem::is_regular_file(quota_limit_path, ec)) // If not regular file, remove it first
        {
            std::filesystem::remove_all(quota_limit_path, ec);
            if (ec)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove non-regular file at ", quota_limit_path, ": ",
                    ec.message());
                return false;
            }
        }
        std::ofstream quota_file(quota_limit_path, std::ios::trunc);
        if (!quota_file.is_open())
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to open quota limit file at ", quota_limit_path,
                " for writing: ", static_cast<int>(errno));
            return false;
        }
        quota_file << std::to_string(quota_limit) << std::endl;
        if (quota_file.fail())
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to write quota limit to file at ", quota_limit_path, ": ",
                static_cast<int>(errno));
            return false;
        }
        quota_file.close();
        return true;
    }

    bool writeQuotaRotateToFile(bool rotate) const noexcept
    {
        const auto quota_no_rotate_path = getRecordQuotaNoRotatePath();
        // If false, create an empty file; else, remove the file if exists
        std::error_code ec;
        if (!rotate)
        {
            if (std::filesystem::is_regular_file(quota_no_rotate_path, ec))
                return true; // Already exists
            std::filesystem::remove_all(quota_no_rotate_path, ec);
            std::ofstream quota_file(quota_no_rotate_path, std::ios::trunc);
            if (!quota_file.is_open())
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to open quota no rotate file at ", quota_no_rotate_path,
                    " for writing: ", static_cast<int>(errno));
                return false;
            }
            quota_file.close();
        }
        else
        {
            std::filesystem::remove_all(quota_no_rotate_path, ec);
            if (ec)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove quota no rotate file at ", quota_no_rotate_path,
                    ": ", ec.message());
                return false;
            }
        }
        return true;
    }

    bool probeCorrectlyConfigured() const noexcept
    {
        std::error_code ec;
        if (auto record_root_dir = getRecordRootPath(); !std::filesystem::is_directory(record_root_dir, ec))
            return false;
        return true;
    }

    bool doConfigureDirectories() const noexcept
    {
        if (!EnsureDirectoryExists(getRecordRootPath(), true, true, &_ctx))
            return false;
        if (!EnsureDirectoryExists(getRecordCachePath(), false, false, &_ctx))
            return false;
        return true;
    }

    bool doSyncQuotaLimit() const noexcept
    {
        _quota_limit_bytes.store(readQuotaLimitFromFile(), std::memory_order_release);
        return true;
    }

    bool doSyncQuotaRotate() const noexcept
    {
        std::error_code ec;
        _quota_rotate.store(!std::filesystem::is_regular_file(getRecordQuotaNoRotatePath(), ec),
            std::memory_order_release);
        return true;
    }

    bool ensureStateConfigured() noexcept
    {
        auto state = _state.load(std::memory_order_acquire);
        if (state != SlotState::Mounted)
            return false;
        if (!doConfigureDirectories())
            goto l_configure_error;
        if (!doSyncQuotaLimit())
            goto l_configure_error;
        if (!doSyncQuotaRotate())
            goto l_configure_error;
        return _state.compare_exchange_strong(state, SlotState::Configured, std::memory_order_acq_rel,
            std::memory_order_acquire);

    l_configure_error:
        while (!_state.compare_exchange_strong(state, SlotState::Error, std::memory_order_acq_rel,
            std::memory_order_acquire))
            std::this_thread::yield();
        return false;
    }

    bool doCreateIndexManager() noexcept
    {
        if (!loadIndexManager())
        {
            try
            {
                storeIndexManager(std::make_shared<IndexManager>(std::ref(_ctx), getRecordRootPath()));
            }
            catch (const std::exception &e)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create IndexManager for storage slot with device path ",
                    _dev_path, ": ", e.what());
                return false;
            }
        }
        return true;
    }

    bool doSyncIndex() noexcept
    {
        auto idx_mgr = loadIndexManager();
        if (!idx_mgr)
            return false;
        idx_mgr->requestReindex();
        while (!_stop_token.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(kIndexSyncInterval);
            if (idx_mgr->indexed())
                return true;
        }
        return false;
    }

    bool ensureStateReady() noexcept
    {
        auto state = _state.load(std::memory_order_acquire);
        if (state != SlotState::Configured || state == SlotState::Indexing)
            return false;
        if (!_state.compare_exchange_strong(state, SlotState::Indexing, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return false;            // Early return if state changed, indexing should only be done by one thread
        state = SlotState::Indexing; // Update expected state
        if (!doCreateIndexManager())
            goto l_index_again;
        if (!doSyncIndex())
            goto l_index_again;
        return _state.compare_exchange_strong(state, SlotState::Ready, std::memory_order_acq_rel,
            std::memory_order_acquire);

    l_index_again:
        while (!_state.compare_exchange_strong(state, SlotState::Configured, std::memory_order_acq_rel,
            std::memory_order_acquire))
            std::this_thread::yield();
        return false;
    }

    // - MARK: Slot Constants

private:
    static inline const std::unordered_set<std::string_view> kNativeFileSystemTypes = {
        "ext2",
        "ext3",
        "ext4",
        "xfs",
        "btrfs",
    };
    static inline const std::unordered_set<std::string_view> kSupportedFileSystemTypes = {
        "ext2",
        "ext3",
        "ext4",
        "fat",
        "vfat",
    };
    static inline constexpr const std::chrono::milliseconds kManagerProbeInterval = std::chrono::milliseconds(500);
    static inline constexpr const std::chrono::seconds kManagerRecoverDelay = std::chrono::seconds(3);
    static inline constexpr const std::chrono::minutes kManagerEstimatedWriteBytesPeriod = std::chrono::minutes(30);
    static inline constexpr const std::chrono::milliseconds kWriterProbeInterval = std::chrono::milliseconds(50);
    static inline constexpr const std::chrono::milliseconds kWriterRotatePullDelay = std::chrono::milliseconds(50);
    static inline constexpr const std::chrono::milliseconds kIndexSyncInterval = std::chrono::milliseconds(100);
    static inline constexpr const std::string_view kRecordCacheDirName = ".cache";
    static inline constexpr const std::string_view kRecordQuotaLimitFileName = ".quota_limit";
    static inline constexpr const std::string_view kRecordQuotaNoRotateFileName = ".quota_no_rotate";

    // - MARK: Slot Private Members

private:
    // External context, e.g., logging, frame buffers
    Context &_ctx;

    // Constant properties from construction
    const std::filesystem::path _dev_path;
    const std::filesystem::path _mount_path;
    const bool _removable;
    const bool _internal;
    const std::string_view _data_dir_name;

    // Mutable info properties
    mutable std::shared_mutex _info_mutex;
    mutable std::optional<std::string> _label;
    mutable std::optional<std::string> _uuid;
    mutable std::optional<std::string> _type;

    // Mutex for public methods
    mutable std::shared_mutex _mutex;

    // Mutex for device operations
    mutable std::shared_mutex _op_mutex;

    // Stop token for manager and writer threads
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _stop_token;

    // Manager and writer threads
    std::unique_ptr<std::thread> _manager_thread;
    std::unique_ptr<std::thread> _writer_thread;

    // Shared slot state machine
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<SlotState> _state;

    // Partition and quota stats
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<SlotStats> _stats;
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<FileSize> _quota_limit_bytes;
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<bool> _quota_rotate;

    // Index manager, e.g., 'YYYY-MM-DD' directories
    alignas(kHardwareDestructiveInterferenceSize) mutable std::shared_ptr<IndexManager> _index_manager;

    // Writer task count
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> _writer_task_count;

    // Rotating status
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<bool> _rotating;

    // Write stats
    mutable WriteStats _write_stats;

    // Relay
    Relay _relay;
};

} // namespace vigil::slot

#endif
