#pragma once
#ifndef VIGIL_SLOT_HPP
#define VIGIL_SLOT_HPP

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

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

class Slot final
{
public:
    Slot(Context &ctx, std::filesystem::path dev_path, std::filesystem::path mount_path, bool removable, bool internal,
        std::string_view data_dir_name)
        : _ctx(ctx), _dev_path(std::move(dev_path)), _mount_path(std::move(mount_path)), _removable(removable),
          _internal(internal), _data_dir_name(data_dir_name), _info_mutex(), _label(), _uuid(), _type(), _mutex(),
          _op_mutex(), _stop_token(false), _manager_thread(nullptr), _writer_thread(nullptr), _state(SlotState::Error),
          _stats(), _quota_limit_bytes(0), _quota_rotate(false), _index_manager(_ctx, _mount_path / _data_dir_name),
          _write_stats(), _writer_task(nullptr),
          _writer_task_stop_token(_ctx.media_paused.load(std::memory_order_acquire)), _writer_task_count(0),
          _relay(ctx, _mount_path / _data_dir_name)
    {
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Creating storage slot for device path ", _dev_path, " at mount path ",
            _mount_path, ", removable: ", _removable ? "true" : "false");
        if (ensureStateResetToInitial())
        {
            ensureStateMounted();
            ensureStateConfigured();
            ensureStateResetToInitial(); // Reset to initial state after mounting
        }
    }

    ~Slot() noexcept
    {
        while (disable() != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Retrying to disable storage slot for device path ", _dev_path);
            std::this_thread::yield();
        }
    }

    std::filesystem::path devPath() const
    {
        return _dev_path;
    }

    std::filesystem::path mountPath() const
    {
        return _mount_path;
    }

    bool isWriting() const
    {
        return _writer_task_count.load(std::memory_order_acquire) > 0;
    }

    inline bool isEnabled() const
    {
        const std::shared_lock lock(_mutex);
        return _manager_thread && _writer_thread;
    }

    inline bool isReadyToWrite() const
    {
        return !_writer_task_stop_token.load(std::memory_order_acquire)
               && _state.load(std::memory_order_acquire) == SlotState::Ready;
    }

    inline bool isRemovable() const
    {
        return _removable;
    }

    inline bool isInternal() const
    {
        return _internal;
    }

    SlotStatus getStatus() const
    {
        const auto is_enabled = isEnabled();
        auto state = _state.load(std::memory_order_acquire);
        std::optional<std::string> label, uuid, format;
        std::optional<FileSize> quota_used;
        if (!is_enabled) // Passively update stats when not enabled
            state = probeInitialStatusAndGetState();
        else if (state >= SlotState::Ready)
            quota_used = _index_manager.usedBytes();
        {
            std::shared_lock lock(_info_mutex);
            label = _label;
            uuid = _uuid;
            format = _type;
        }
        return SlotStatus {
            .dev_path = _dev_path,
            .mount_path = _mount_path,
            .removable = isRemovable(),
            .internal = isInternal(),
            .label = std::move(label),
            .uuid = std::move(uuid),
            .type = std::move(format),
            .enabled = is_enabled,
            .writing = isWriting(),
            .state = state,
            .stats = _stats.load(std::memory_order_acquire),
            .quota_used_bytes = quota_used,
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
        _writer_task_stop_token.store(false, std::memory_order_release);
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

        // Resume write operations (skip status check)
        resumeWrite(true);

        return 0;
    }

    vg_sta_e disable() noexcept
    {
        const std::unique_lock lock(_mutex);
        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Disabling storage slot for device path ", _dev_path);
        vg_sta_e ret = 0;

        // Disable relay
        if (ret = disableNginxRelay(); ret != 0)
            goto l_disable_fail;

        // Remove all write handlers
        if (ret = suspendWrite(true); ret != 0)
            goto l_disable_fail;

        _writer_task_stop_token.store(true, std::memory_order_release);
        _stop_token.store(true, std::memory_order_release);

        try
        {
            if (_writer_thread)
            {
                // Stop writer thread
                _writer_thread->join();
                _writer_thread.reset();
            }
            if (_manager_thread)
            {
                // Stop manager thread
                _manager_thread->join();
                _manager_thread.reset();
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to safely shutdown threads for storage slot for device path ",
                _dev_path, ": ", e.what());
            ret = EFAULT; // Should not goto since there are remaining cleanup steps
        }

        // Reset index manager
        _index_manager.reset();

        // Unmount and set state to unmounted
        if (!ensureStateForceUnmounted())
        {
            ret = EIO;
            goto l_disable_fail;
        }

        return ret;

    l_disable_fail:
        VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to disable storage slot for device path ", _dev_path,
            ", code: ", ret);
        return ret;
    }

    vg_sta_e suspendWrite(bool wait = true) noexcept
    {
        auto stop_requested = _writer_task_stop_token.load(std::memory_order_acquire);
        while (!stop_requested)
        {
            if (_writer_task_stop_token.compare_exchange_weak(stop_requested, true, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }
        if (wait)
        {
            while (_writer_task_count.load(std::memory_order_acquire) > 0)
                std::this_thread::yield(); // Wait for writers to finish
            return 0;
        }

        return _writer_task_count.load(std::memory_order_acquire) > 0 ? EAGAIN : 0;
    }

    vg_sta_e resumeWrite(bool reset_media_queue = true) noexcept
    {
        if (reset_media_queue)
            _ctx.clearMediaBuffer();
        auto stop_requested = _writer_task_stop_token.load(std::memory_order_acquire);
        while (stop_requested)
        {
            if (_writer_task_stop_token.compare_exchange_weak(stop_requested, false, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }
        return 0;
    }

    vg_sta_e setQuotaLimitBytes(FileSize bytes)
    {
        if (bytes < 0 || bytes > _stats.load(std::memory_order_acquire).size_bytes)
            return EINVAL;
        const std::unique_lock lock(_mutex);
        if (_state.load(std::memory_order_acquire) == SlotState::Mounted && !ensureStateConfigured())
            return EIO;
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
        if (_state.load(std::memory_order_acquire) == SlotState::Mounted && !ensureStateConfigured())
            return EIO;
        if (_state.load(std::memory_order_acquire) < SlotState::Configured)
            return EPERM;
        if (!writeQuotaRotateToFile(rotate))
            return EIO;
        _quota_rotate.store(rotate, std::memory_order_release);
        return 0;
    }

    vg_sta_e removeFileOrDirectory(const std::filesystem::path &relative_path)
    {
        if (auto state = _state.load(std::memory_order_acquire);
            state < SlotState::Configured || state == SlotState::Indexing)
            return EPERM;
        const auto ret = _index_manager.removeFileOrDirectory(relative_path);
        if (ret != 0)
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove file or directory at relative path ", relative_path,
                ": ", ret);
        return ret;
    }

    vg_sta_e freeUpUsedSpace()
    {
        if (auto state = _state.load(std::memory_order_acquire);
            state < SlotState::Configured || state == SlotState::Indexing)
            return EPERM;
        const auto used = _index_manager.usedBytes();
        if (used <= 0)
            return 0;
        const auto freed = _index_manager.freeUpStaledFiles(used);
        return freed > 0 ? 0 : EIO; // At least some space should be freed
    }

    inline std::filesystem::path getRecordRootPath() const
    {
        return _mount_path / _data_dir_name;
    }

    vg_sta_e formatFileSystem()
    {
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

    vg_sta_e statusOfNginxRelay(RelayStatus &status)
    {
        status = _relay.getStatus();
        return 0;
    }

    vg_sta_e disableNginxRelay() noexcept
    {
        return _relay.disableNginxRelay();
    }

    // - MARK: Slot Protected Methods

protected:
    friend SlotAdaptedWriter<Slot>;

    inline Context &ctxRef() const
    {
        return _ctx;
    }

    inline std::atomic<int> &writerCountRef()
    {
        return _writer_task_count;
    }

    void addToCacheFileSyncQueue(const CacheFileInfo &cache_file_info) noexcept
    {
        const std::unique_lock lock(_cache_file_sync_mutex);
        _cache_file_sync_queue.push(cache_file_info);
    }

    std::filesystem::path getRecordQuotaLimitPath() const
    {
        return getRecordRootPath() / kRecordQuotaLimitFileName;
    }

    std::filesystem::path getRecordQuotaRotatePath() const
    {
        return getRecordRootPath() / kRecordQuotaRotateFileName;
    }

    std::filesystem::path getRecordCachePath() const
    {
        return getRecordRootPath() / kRecordCacheDirName;
    }

    // - MARK: Slot Private Methods

private:
    void resetWriteStatsUnsafe()
    {
        _write_stats.reset();
    }

    void resetWriteCacheUnsafe()
    {
        if (_state.load(std::memory_order_acquire) < SlotState::Configured) // Requires the root folder to be ready
            return;                                                         // No cache to clear if not mounted
        // There's no activate writer at this moment
        {
            const std::unique_lock lock(_cache_file_sync_mutex);
            while (!_cache_file_sync_queue.empty())
                _cache_file_sync_queue.pop();
        }
        const auto cache_path = getRecordCachePath();
        std::error_code ec;
        std::filesystem::remove_all(cache_path, ec);
        if (ec)
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to clear record cache at ", cache_path, ": ", ec.message());
    }

    bool probeStateHealth() const
    {
        return probeDeviceOnline() && probeCorrectlyMounted() && probeCorrectlyConfigured();
    }

    void doRotateStaledFilesAndSync() const
    {
        const auto quota_limit = _quota_limit_bytes.load(std::memory_order_acquire);
        const auto estimated_write_bytes_in_period = _write_stats.getEstimatedWriteBytesIn(
            kManagerEstimatedWriteBytesPeriod); // Should before quota calculation since it updates quota used

        FileSize bytes_to_free = 0;
        // First check if we are already over quota if quota limit is set
        if (quota_limit > 0)
        {
            const auto quota_used = _index_manager.usedBytes();
            const auto quota_free = quota_limit - quota_used;
            if (estimated_write_bytes_in_period > quota_free)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Quota used bytes ", quota_used,
                    " bytes is over limit for storage slot for device path ", _dev_path,
                    ", need to free up space for estimated write bytes in ", kManagerEstimatedWriteBytesPeriod.count(),
                    " minutes: ", estimated_write_bytes_in_period, " bytes");
                bytes_to_free = estimated_write_bytes_in_period - quota_free;
                goto l_free_up_space; // Free up space directly
            }
        }
        // Next check if we need to free up space based on the free space of this slot
        {
            // Sync stats to check if we need to free more space
            if (!doStats())
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to sync stats for storage slot ", _dev_path);
                return;
            }
            const auto slot_stats = _stats.load(std::memory_order_acquire);
            const auto free_bytes = slot_stats.free_bytes;
            if (estimated_write_bytes_in_period <= free_bytes)
                goto l_try_sync_index_and_cache; // No need to free up space
            bytes_to_free = estimated_write_bytes_in_period - free_bytes;
            VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Free space of storage slot for device path ", _dev_path,
                " is lower than quota threshold, need to free up ", bytes_to_free, " bytes");
        }

    l_try_sync_index_and_cache:
        // Sync the index first
        if (auto now = std::chrono::steady_clock::now(); now - _index_manager.lastIndexTime() >= kIndexSyncInterval)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Syncing index for storage slot for device path ", _dev_path);
            if (!_index_manager.trySync())
                VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Trying sync index for storage slot for device path ", _dev_path,
                    " again");
            VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Resynced index took ",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - now).count(),
                " ms for storage slot for device path ", _dev_path);
        }
        // Sync the cache files
        try
        {
            for (;;)
            {
                std::optional<CacheFileInfo> cache_file_info;
                {
                    // Lazy sync the cache files
                    const std::unique_lock lock(_cache_file_sync_mutex);
                    if (!_cache_file_sync_queue.empty())
                    {
                        cache_file_info = std::move(_cache_file_sync_queue.front());
                        _cache_file_sync_queue.pop();
                    }
                }
                if (!cache_file_info) // If no cache file info is available, break the loop
                    break;
                if (auto result = _index_manager.moveAndAddCacheFile(*cache_file_info); result)
                {
                    if (result->second > 0)
                        _write_stats.addNewWriteStat(result->second);
                    if (_ctx.c_api.basic.on_file_event_cb)
                    {
                        const auto path_str = result->first.string();
                        const auto attrs_str
                            = cache_file_info->event ? WriteEvent::StatusString(cache_file_info->event->status) : "";
                        const auto file_evt = vg_file_evt_t {
                            .op_type = VG_FILE_EVT_OP_ADDED,
                            .path = path_str.c_str(),
                            .size_bytes = result->second > 0 ? static_cast<size_t>(result->second) : 0,
                            .attrs = attrs_str,
                            .timestamp = _ctx.unix_time,
                        };
                        _ctx.c_api.basic.on_file_event_cb(&file_evt);
                    }
                }
                else
                {
                    std::error_code ec;
                    if (std::filesystem::exists(cache_file_info->path, ec))
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Deferring cache file sync due to move failure for ",
                            cache_file_info->path);
                        const std::unique_lock lock(_cache_file_sync_mutex);
                        _cache_file_sync_queue.push(std::move(*cache_file_info));
                    }
                    else if (ec)
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Unable to check cache file existence for ",
                            cache_file_info->path, ": ", ec.message());
                    }
                    else
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Cache file ", cache_file_info->path,
                            " missing after move failure, skipping resync");
                }
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                "Unexpected error during syncing cache files for storage slot for device path ", _dev_path, ": ",
                e.what());
        }
        return;

    l_free_up_space:
        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Storage slot for device path ", _dev_path,
            " is low on free space, quota limit: ", quota_limit, " bytes, estimated write bytes in ",
            kManagerEstimatedWriteBytesPeriod.count(), " minutes: ", estimated_write_bytes_in_period,
            " bytes, will free up ", bytes_to_free, " bytes");
        bytes_to_free = _index_manager.freeUpStaledFiles(bytes_to_free);
        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Freed up ", bytes_to_free, " bytes on storage slot for device path ",
            _dev_path);
    }

    void managerThreadTask()
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

    void writerThreadTask()
    {
        _ctx.clearRuleEvent(); // Clear any existing rule event at start
        while (!_stop_token.load(std::memory_order_acquire))
        {
            // If already has a writer, check if it is finished
            if (_writer_task)
            {
                const auto result = _writer_task->operator()();
                const auto end = _writer_task->endTime();
                _writer_task.reset();
                if (result->status != WriteEvent::Status::Interrupted)
                {
                    do
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        std::this_thread::yield();
                        if (_stop_token.load(std::memory_order_acquire))
                            return; // Stop requested or storage is ejected
                    }
                    while (std::chrono::steady_clock::now() < end);
                }
            }

            const auto start = std::chrono::steady_clock::now();
            bool start_new_writer = false;
            std::shared_ptr<WriteEvent> evaluation_result;

            // Check if we are asked to stop writing (suspend)
            if (start_new_writer = isReadyToWrite(); !start_new_writer)
                goto l_continue;

            // Check if could create a new writer
            if (!_quota_rotate.load(std::memory_order_acquire))
            {
                if (auto limit = _quota_limit_bytes.load(std::memory_order_acquire); limit > 0)
                {
                    if (start_new_writer = _index_manager.usedBytes() < limit; !start_new_writer)
                        goto l_continue;
                }
                else if (start_new_writer = _stats.load(std::memory_order_acquire).free_bytes > kMinFileSystemFreeBytes;
                    !start_new_writer)
                    goto l_continue;
            }

            // If no writer is active, check the rule evaluation status if should create one
            {
                const std::unique_lock lock(_ctx.rule_event_mutex);
                if (_ctx.rule_event)
                {
                    start_new_writer = true;
                    evaluation_result = _ctx.rule_event;
                }
            }
            if (!start_new_writer)
                goto l_continue;

            // If media is paused, do not start new writer
            if (start_new_writer = !_ctx.media_paused.load(std::memory_order_acquire); !start_new_writer)
                goto l_continue;

            goto l_start_new;

        l_continue: {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < kWriterProbeInterval)
                std::this_thread::sleep_for(kWriterProbeInterval - elapsed);
            continue;
        }

        l_start_new:
            try
            {
                auto writer_settings = _ctx.getWriterSettings();
                switch (writer_settings.second.type)
                {
                    case FileWriterSettings::CaptureType::Image:
                        _writer_task = std::make_unique<ImageWriter<SlotAdaptedWriter<Slot>>>(std::ref(*this),
                            std::ref(_ctx), std::move(writer_settings), evaluation_result);
                        break;
                    case FileWriterSettings::CaptureType::Video:
                        _writer_task = std::make_unique<VideoWriter<SlotAdaptedWriter<Slot>>>(std::ref(*this),
                            std::ref(_ctx), std::move(writer_settings), evaluation_result);
                        break;
                    default: {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                            "Unsupported writer type for storage slot with device path ", _dev_path);
                        const std::unique_lock lock(_ctx.rule_event_mutex);
                        _ctx.rule_event.reset(); // Clear invalid evaluation result
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
        }
    }

    // - MARK: Slot Private Methods

private:
    bool probeDeviceOnline() const
    {
        std::error_code ec;
        if (std::filesystem::exists(_dev_path, ec))
            return true;
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Device path ", _dev_path,
            " does not exist or is inaccessible: ", ec.message());
        return false;
    }

    bool doUpdatePartionInfo() const
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
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to probe device ", _dev_path, " for device info, code: ", ret,
                ", errno: ", std::strerror(errno));
            blkid_free_probe(pr);
            return false;
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

    bool probeDeviceFileSystemSupported() const
    {
        const std::shared_lock lock(_info_mutex);
        return _type.has_value() && kSupportedFileSystemTypes.find(*_type) != kSupportedFileSystemTypes.end();
    }

    SlotState probeInitialStatusAndGetState() const
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

    bool ensureStateResetToInitial()
    {
        auto state = _state.load(std::memory_order_acquire);
        return _state.compare_exchange_strong(state, probeInitialStatusAndGetState(), std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool doFormat() const
    {
        const std::unique_lock lock(_op_mutex);
        const auto cmd
            = "mkfs.vfat -F 32 -I '" + _dev_path.string() + "' > /dev/null 2>&1"; // Format the device using mkfs.vfat
        if (int ret = std::system(cmd.c_str()); ret != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to format storage slot device ", _dev_path,
                " with mkfs.vfat, return code: ", ret);
            return false;
        }
        return true;
    }

    bool ensureStateFormatted() // Never auto-format, must be called explicitly
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

    bool probeCorrectlyMounted() const
    {
        struct stat dev_sb {};
        struct stat mnt_sb {};
        bool mount_stats_valid = false;
        bool mount_path_missing = false;

        {
            const std::shared_lock lock(_op_mutex);
            if (::stat(_dev_path.c_str(), &dev_sb) != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to stat device path ", _dev_path, ": ",
                    std::strerror(errno));
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
                    std::strerror(errno));
                return false;
            }
        }

        if (mount_path_missing)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Mount path ", _mount_path,
                " does not exist, recreating before mount probe");
            const std::unique_lock lock(_op_mutex);
            if (!IndexManager::ensureDirectoryExist(_mount_path, _ctx))
                return false;
        }

        if (!mount_stats_valid)
        {
            const std::shared_lock lock(_op_mutex);
            if (::stat(_mount_path.c_str(), &mnt_sb) != 0)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Failed to stat mount path ", _mount_path, ": ",
                    std::strerror(errno));
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

    bool doUnmount() const
    {
        const std::unique_lock lock(_op_mutex);
        // Sync the filesystem before unmounting (syncfs)
        if (auto fd = ::open(_mount_path.c_str(), O_RDONLY); fd < 0)
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to open mount path ", _mount_path,
                " for syncing before unmounting: ", std::strerror(errno));
        else if (::syncfs(fd) != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to sync filesystem at mount path ", _mount_path,
                " before unmounting: ", std::strerror(errno));
            ::close(fd);
        }
        else
            ::close(fd);
        // Unmount the device
        if (::umount2(_mount_path.c_str(), MNT_DETACH) != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to unmount storage slot at path ", _mount_path, ": ",
                std::strerror(errno));
            return false;
        }
        if (_ctx.c_api.storage.on_unmount_cb)
            _ctx.c_api.storage.on_unmount_cb(_dev_path.c_str(), _mount_path.c_str());
        return true;
    }

    bool doMount() const
    {
        const std::unique_lock lock(_op_mutex);
        if (!IndexManager::ensureDirectoryExist(_mount_path, _ctx))
            return false;
        const auto cmd = "mount '" + _dev_path.string() + "' '" + _mount_path.string() + "' > /dev/null 2>&1";
        if (auto ret = std::system(cmd.c_str()); ret != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to mount storage slot device ", _dev_path, " to ", _mount_path,
                ", code: ", ret);
            return false;
        }
        if (_ctx.c_api.storage.on_mount_cb)
            _ctx.c_api.storage.on_mount_cb(_dev_path.c_str(), _mount_path.c_str());
        return true;
    }

    bool doStats() const
    {
        struct statvfs vfs {};
        const std::shared_lock lock(_op_mutex);
        if (::statvfs(_mount_path.c_str(), &vfs) != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to get filesystem stats for mount path ", _mount_path, ": ",
                std::strerror(errno));
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

    bool ensureStateMounted()
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

    bool ensureStateForceUnmounted()
    {
        auto state = _state.load(std::memory_order_acquire);
        if (isRemovable() && !isInternal())
        {
            // No need to check state, unmount anyway
            if (probeDeviceOnline() && probeCorrectlyMounted() && !doUnmount())
                goto l_unmount_error;
            _state.store(SlotState::NotMounted, std::memory_order_release);
        }
        else
            _state.store(SlotState::Mounted, std::memory_order_release);
        return true;

    l_unmount_error:
        while (!_state.compare_exchange_strong(state, SlotState::Error, std::memory_order_acq_rel,
            std::memory_order_acquire))
            std::this_thread::yield();
        return false;
    }

    FileSize readQuotaLimitFromFile() const
    {
        std::error_code ec;
        if (auto quota_file = getRecordQuotaLimitPath(); !std::filesystem::is_regular_file(quota_file, ec))
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "No valid quota file found at ", quota_file);
            return 0;
        }
        else
        {
            std::ifstream ifs(quota_file, std::ios::in);
            if (!ifs)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to open quota file ", quota_file, ": ", ec.message());
                return 0;
            }
            std::string line;
            if (std::getline(ifs, line))
            {
                try
                {
                    const FileSize quota_limit = std::stoll(line);
                    const auto stats = _stats.load(std::memory_order_acquire);
                    if (quota_limit <= 0 || quota_limit >= stats.size_bytes)
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Quota limit ", quota_limit,
                            " bytes exceeds or equals 0 or total size ", stats.size_bytes,
                            " bytes, ignoring quota limit");
                        return 0;
                    }
                    return quota_limit;
                }
                catch (const std::exception &e)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to parse quota limit from file ", quota_file, ": ",
                        e.what());
                    return 0;
                }
            }
            ifs.close();
        }
        return 0;
    }

    bool writeQuotaLimitToFile(FileSize quota_limit) const
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
                " for writing: ", std::strerror(errno));
            return false;
        }
        quota_file << std::to_string(quota_limit) << std::endl;
        if (quota_file.fail())
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to write quota limit to file at ", quota_limit_path, ": ",
                std::strerror(errno));
            return false;
        }
        quota_file.close();
        return true;
    }

    bool writeQuotaRotateToFile(bool rotate) const
    {
        const auto quota_rotate_path = getRecordQuotaRotatePath();
        // If true, create an empty file; else, remove the file if exists
        std::error_code ec;
        if (rotate)
        {
            if (std::filesystem::is_regular_file(quota_rotate_path, ec))
                return true; // Already exists
            std::filesystem::remove_all(quota_rotate_path, ec);
            std::ofstream quota_file(quota_rotate_path, std::ios::trunc);
            if (!quota_file.is_open())
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to open quota rotate file at ", quota_rotate_path,
                    " for writing: ", std::strerror(errno));
                return false;
            }
            quota_file.close();
        }
        else
        {
            std::filesystem::remove_all(quota_rotate_path, ec);
            if (ec)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove quota rotate file at ", quota_rotate_path, ": ",
                    ec.message());
                return false;
            }
        }
        return true;
    }

    bool probeCorrectlyConfigured() const
    {
        std::error_code ec;
        if (auto record_root_dir = getRecordRootPath(); !std::filesystem::is_directory(record_root_dir, ec))
            return false;
        return true;
    }

    bool doConfigureDirectories() const
    {
        if (!IndexManager::ensureDirectoryExist(getRecordRootPath(), _ctx))
            return false;
        if (!IndexManager::ensureDirectoryExist(getRecordCachePath(), _ctx))
            return false;
        return true;
    }

    bool doSyncQuotaLimit() const
    {
        if (auto quota_limit = readQuotaLimitFromFile(); quota_limit > 0)
            _quota_limit_bytes.store(quota_limit, std::memory_order_release);
        return true;
    }

    bool doSyncQuotaRotate() const
    {
        std::error_code ec;
        _quota_rotate.store(std::filesystem::is_regular_file(getRecordQuotaRotatePath(), ec),
            std::memory_order_release);
        return true;
    }

    bool ensureStateConfigured()
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

    bool ensureStateReady()
    {
        auto state = _state.load(std::memory_order_acquire);
        if (state != SlotState::Configured || state == SlotState::Indexing)
            return false;
        if (!_state.compare_exchange_strong(state, SlotState::Indexing, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return false;            // Early return if state changed, indexing should only be done by one thread
        state = SlotState::Indexing; // Update expected state
        if (!_index_manager.trySync())
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
    static inline const std::unordered_set<std::string_view> kSupportedFileSystemTypes = { "fat", "vfat", "exfat" };
    static inline constexpr const std::chrono::milliseconds kManagerProbeInterval = std::chrono::milliseconds(500);
    static inline constexpr const std::chrono::seconds kManagerRecoverDelay = std::chrono::seconds(3);
    static inline constexpr const std::chrono::minutes kManagerEstimatedWriteBytesPeriod = std::chrono::minutes(30);
    static inline constexpr const std::chrono::milliseconds kWriterProbeInterval = std::chrono::milliseconds(10);
    static inline constexpr const std::chrono::milliseconds kWriterRotatePullDelay = std::chrono::milliseconds(50);
    static inline constexpr const std::chrono::days kIndexSyncInterval = std::chrono::days(7);
    static inline constexpr const std::string_view kRecordCacheDirName = ".cache";
    static inline constexpr const std::string_view kRecordQuotaLimitFileName = ".quota_limit";
    static inline constexpr const std::string_view kRecordQuotaRotateFileName = ".quota_rotate";
    static inline constexpr const FileSize kMinFileSystemFreeBytes = 512 * 1024 * 1024; // 512 MB

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
    mutable IndexManager _index_manager;

    // Write stats
    mutable WriteStats _write_stats;

    // Writer task
    std::unique_ptr<SlotAdaptedWriter<Slot>> _writer_task;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<
        bool> _writer_task_stop_token; // Controls whether new writes are allowed and current writes should stop
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<int> _writer_task_count;

    // Cache to sync queue
    mutable std::mutex _cache_file_sync_mutex;
    mutable std::queue<CacheFileInfo> _cache_file_sync_queue;

    // Relay
    Relay _relay;
};

} // namespace vigil::slot

#endif
