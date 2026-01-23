#pragma once
#ifndef VIGIL_INDEX_MANAGER_HPP
#define VIGIL_INDEX_MANAGER_HPP

#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "constants.hpp"
#include "context.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil::index_manager {

using namespace vigil::types;
using namespace vigil::constants;

namespace fs = std::filesystem;

inline bool IsPathUnderRoot(const fs::path &root, const fs::path &path) noexcept
{
    try
    {
        if (root.empty() || path.empty() || root == path || root.is_relative())
            return false;
        if (path.is_relative())
            return IsPathUnderRoot(root, (root / path).lexically_normal());

        std::error_code ec;
        const auto root_canonical = std::filesystem::weakly_canonical(root, ec);
        if (ec)
            return false;
        const auto path_canonical = std::filesystem::weakly_canonical(path, ec);
        if (ec)
            return false;
        auto root_it = root_canonical.begin();
        auto path_it = path_canonical.begin();
        for (; root_it != root_canonical.end() && path_it != path_canonical.end(); ++root_it, ++path_it)
        {
            if (*root_it != *path_it)
                return false;
        }
        return root_it == root_canonical.end();
    }
    catch (...)
    {
        return false;
    }
}

inline bool EnsureDirectoryExists(const fs::path &path, Context *ctx = nullptr) noexcept
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        return true;
    else
        std::filesystem::remove_all(path, ec);
    if (std::filesystem::create_directories(path, ec); ec)
    {
        if (ctx)
        {
            VG_LOG(ctx->log, VG_LOG_LEVEL_ERROR, "Failed to create directory ", path, ": ", ec.message());
        }
        return false;
    }
    return true;
}

class IndexManager
{
public:
    enum class IndexStatus {
        CANCELED,
        PARTIAL,
        SUCCESS,
    };

    struct IndexResult
    {
        bool valid;
        FileSize size;
    };

    enum class EventResult {
        SKIPPED,
        INCREMENTAL,
        REINDEX,
        REINDEX_IMMEDIATELY,
    };

    class EntryIndex
    {
    private:
        enum Status : int64_t { INVALIDATED = -1 };
        struct Index
        {
            int64_t size;
            size_t revision;
        };
        alignas(kHardwareDestructiveInterferenceSize) std::atomic<Index> _index;

    public:
        EntryIndex() noexcept : _index({ Status::INVALIDATED, 0 }) { }

        EntryIndex(const EntryIndex &) = delete;
        EntryIndex &operator=(const EntryIndex &) = delete;

        EntryIndex(EntryIndex &&) = delete;
        EntryIndex &operator=(EntryIndex &&) = delete;

        inline size_t size() const noexcept
        {
            const auto size = _index.load(std::memory_order_acquire).size;
            return size >= 0 ? static_cast<size_t>(size) : 0;
        }

        inline bool isValid() const noexcept
        {
            return _index.load(std::memory_order_acquire).size >= 0;
        }

        inline auto revision() const noexcept
        {
            return _index.load(std::memory_order_acquire).revision;
        }

        bool tryUpdateSize(size_t new_size, size_t revision) noexcept
        {
            auto current = _index.load(std::memory_order_acquire);
            // Invalidate if revision has changed
            if (revision != current.revision)
            {
                internalMarkInvalidated(current);
                return false;
            }
            // Try to update size
            return _index.compare_exchange_strong(current, Index { static_cast<int64_t>(new_size), revision + 1 },
                std::memory_order_acq_rel);
        }

        void markInvalidated() noexcept
        {
            auto current = _index.load(std::memory_order_acquire);
            internalMarkInvalidated(current);
        }

    private:
        inline void internalMarkInvalidated(Index &current) noexcept
        {
            while (current.size != Status::INVALIDATED)
            { // While not invalidated
                if (_index.compare_exchange_strong(current, Index { Status::INVALIDATED, current.revision + 1 },
                        std::memory_order_acq_rel))
                    break;
            }
        }
    };

    explicit IndexManager(Context &ctx, const std::string &watch_path)
        : _ctx(ctx), _watch_path(fs::absolute(watch_path)), _mount_point(detectMountPoint(_watch_path)),
          _shutdown(false), _indexable(true), _indexing(false), _fanotify_fd(-1), _mount_fd(-1), _generation_current(0),
          _generation_target(1)
    {
        if (!fs::exists(_watch_path))
        {
            throw std::runtime_error("Watch directory does not exist: " + _watch_path.string());
        }
        if (!fs::is_directory(_watch_path))
        {
            throw std::runtime_error("Watch path is not a directory: " + _watch_path.string());
        }
        // Open fd to mount point for use with open_by_handle_at
        if (_mount_fd = ::open(_mount_point.c_str(), O_RDONLY); _mount_fd < 0)
        {
            throw std::runtime_error(
                "Failed to open mount point: " + _mount_point.string() + ", errno: " + std::to_string(errno));
        }

        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "IndexManager watching path: ", _watch_path,
            ", mount point: ", _mount_point);

        initFanotify();

        // Start worker threads
        _watcher_thread = std::thread(&IndexManager::watcherLoop, this);
        _indexer_thread = std::thread(&IndexManager::indexerLoop, this);
    }

    ~IndexManager()
    {
        shutdown();
    }

    IndexManager(const IndexManager &) = delete;
    IndexManager &operator=(const IndexManager &) = delete;

    IndexManager(IndexManager &&) = delete;
    IndexManager &operator=(IndexManager &&) = delete;

    const fs::path &mountPoint() const
    {
        return _mount_point;
    }

    bool indexing() const
    {
        return _indexing.load(std::memory_order_acquire);
    }

    bool indexed() const
    {
        return _generation_current.load(std::memory_order_acquire)
               == _generation_target.load(std::memory_order_acquire);
    }

    IndexResult size() const noexcept
    {
        if (!_indexable.load(std::memory_order_acquire))
            return { false, 0 };
        const auto generation = _generation_current.load(std::memory_order_acquire);
        if (generation != _generation_target.load(std::memory_order_acquire))
            return { false, 0 };
        FileSize total_size = 0;
        {
            const std::shared_lock entries_lock(_entries_mutex);
            for (const auto &[_, entry]: _entries)
            {
                if (!entry->isValid())
                    return { false, 0 };
                total_size += entry->size();
            }
        }
        return { _indexable.load(std::memory_order_acquire)
                     && generation == _generation_target.load(std::memory_order_acquire),
            total_size };
    }

    size_t removeEntries(size_t target_size_bytes) noexcept
    {
        size_t removed_size = 0;
        {
            const std::shared_lock entries_lock(_entries_mutex);
            for (auto it = _entries.begin(); it != _entries.end() && removed_size < target_size_bytes;)
            {
                if (!it->second->isValid())
                {
                    ++it;
                    continue;
                }
                try
                {
                    const fs::path entry_path = _watch_path / it->first;
                    const size_t entry_size = it->second->size();
                    fs::remove_all(entry_path);
                    removed_size += entry_size;
                }
                catch (const std::exception &e)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to remove entry ", it->first, ": ", e.what());
                    ++it;
                }
            }
        }
        return removed_size;
    }

    void requestReindex(bool invalidate_all_entries = false) noexcept
    {
        if (invalidate_all_entries)
        {
            std::shared_lock entries_lock(_entries_mutex);
            for (auto &[_, entry]: _entries)
                entry->markInvalidated();
        }
        _indexable.store(true, std::memory_order_release);
        _generation_target.fetch_add(1, std::memory_order_release);
        _indexer_cv.notify_one();
    }

private:
    static fs::path detectMountPoint(const fs::path &path)
    {
        const fs::path abs_path = fs::absolute(path);
        struct stat path_stat;
        if (::stat(abs_path.c_str(), &path_stat) != 0)
        {
            throw std::runtime_error("Failed to stat path: " + abs_path.string() + ", errno: " + std::to_string(errno));
        }

        const dev_t target_dev = path_stat.st_dev;
        fs::path current = abs_path;
        fs::path mount_point = current;

        // Traverse up until we hit root or find a different device
        while (current.has_parent_path() && current != current.parent_path())
        {
            const fs::path parent = current.parent_path();
            struct stat parent_stat;
            if (::stat(parent.c_str(), &parent_stat) != 0)
                break; // Can't stat parent, use current as mount point

            if (parent_stat.st_dev != target_dev)
            {
                mount_point = current;
                break; // Parent is on different device - current is the mount point
            }

            // Same device, continue up
            mount_point = parent;
            current = parent;
        }

        return mount_point;
    }

    // Reference to context
    Context &_ctx;

    // Path management
    const fs::path _watch_path;  // Directory to index and watch
    const fs::path _mount_point; // Auto-detected mount point for fanotify

    // Shutdown control
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _shutdown;

    // Indexing state
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _indexable;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _indexing;

    // Indexed entries
    mutable std::shared_mutex _entries_mutex;
    std::map<std::string, std::shared_ptr<EntryIndex>> _entries;

    // Kernel fanotify
    int _fanotify_fd;
    int _mount_fd; // fd to root dir, used for open_by_handle_at

    // Thread synchronization
    std::mutex _indexer_mutex;
    std::condition_variable _indexer_cv;

    // Threads
    std::thread _watcher_thread;
    std::thread _indexer_thread;

    // Generation counter for detecting changes during indexing
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> _generation_current;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> _generation_target;

    void shutdown() noexcept
    {
        if (bool expected = false; !_shutdown.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return; // Already shut down

        _indexer_cv.notify_all();

        try
        {
            if (_watcher_thread.joinable())
                _watcher_thread.join();
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Exception occurred while shutting down watcher thread: ", e.what());
        }
        try
        {
            if (_indexer_thread.joinable())
                _indexer_thread.join();
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Exception occurred while shutting down indexer thread: ", e.what());
        }

        // Close fanotify fd to unblock poll()
        if (_fanotify_fd >= 0)
        {
            ::close(_fanotify_fd);
            _fanotify_fd = -1;
        }
        if (_mount_fd >= 0)
        {
            ::close(_mount_fd);
            _mount_fd = -1;
        }
    }

    void initFanotify()
    {
        _fanotify_fd = ::fanotify_init(FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME | FAN_UNLIMITED_QUEUE,
            O_RDONLY | O_LARGEFILE);
        if (_fanotify_fd < 0)
        {
            throw std::runtime_error("Failed to initialize fanotify: " + std::to_string(errno));
        }

        auto try_mark = [this](uint64_t mark_flag) {
            const uint64_t event_mask = FAN_CLOSE_WRITE | FAN_CREATE | FAN_DELETE | FAN_MOVE | FAN_MODIFY | FAN_ONDIR
                                        | FAN_DELETE_SELF | FAN_MOVE_SELF;
            return ::fanotify_mark(_fanotify_fd, FAN_MARK_ADD | mark_flag, event_mask, AT_FDCWD, _mount_point.c_str());
        };

        // Prefer a mount mark, but fall back to a filesystem mark if the
        // kernel/filesystem rejects FAN_MARK_MOUNT (common on some removable or
        // network filesystems).
        if (int mount_rc = try_mark(FAN_MARK_MOUNT); mount_rc < 0)
        {
            const int mount_err = errno;
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "FAN_MARK_MOUNT failed: ", std::strerror(mount_err));

            // Some filesystems (e.g., FAT/exFAT or certain network mounts) don't
            // support FAN_MARK_MOUNT. In those cases, retry with FAN_MARK_FILESYSTEM
            // to keep a single O(1) mark over the whole tree.
            if (int fs_rc = try_mark(FAN_MARK_FILESYSTEM); fs_rc < 0)
            {
                const int fs_err = errno;
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "FAN_MARK_FILESYSTEM failed: ", std::strerror(fs_err));

                ::close(_fanotify_fd);
                _fanotify_fd = -1;

                throw std::runtime_error(
                    "Failed to mark target for fanotify (mount: " + std::string(std::strerror(mount_err))
                    + ", filesystem: " + std::string(std::strerror(fs_err)) + ")");
            }
        }
    }

    bool isUnderWatchPath(const fs::path &path) const noexcept
    {
        // Check if path starts with watch path
        const auto &watch_str = _watch_path.native();
        const auto &path_str = path.native();
        // Allow exact match (for watch path itself) or proper subpath
        if (auto ord = path_str.length() <=> watch_str.length(); ord == std::strong_ordering::less)
        {
            return false;
        }
        else if (ord == std::strong_ordering::equal)
        {
            return path_str == watch_str;
        }
        return path_str.compare(0, watch_str.length(), watch_str) == 0 && (path_str[watch_str.length()] == '/');
    }

    bool containsHiddenComponent(const fs::path &path) const noexcept
    {
        fs::path rel;
        try
        {
            rel = fs::relative(path, _watch_path);
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to compute relative path for: ", path, ", error: ", e.what());
            return true; // Treat errors as hidden components
        }
        for (const auto &component: rel)
        {
            if (component.native().starts_with('.'))
            {
                return true;
            }
        }
        return false;
    }

    void indexerLoop() noexcept
    {
        while (!_shutdown.load(std::memory_order_acquire))
        {
            {
                std::unique_lock lock(_indexer_mutex);
                _indexer_cv.wait(lock, [this] {
                    if (_shutdown.load(std::memory_order_acquire)) [[unlikely]]
                    {
                        return true;
                    }
                    if (!_indexable.load(std::memory_order_acquire)) [[unlikely]]
                    {
                        return false;
                    }
                    if (_generation_current.load(std::memory_order_acquire)
                        != _generation_target.load(std::memory_order_acquire))
                    {
                        return true;
                    }
                    {
                        const std::shared_lock entries_lock(_entries_mutex);
                        for (const auto &[_, entry]: _entries)
                        {
                            if (!entry->isValid())
                            {
                                return true;
                            }
                        }
                    }
                    return false;
                });
            }
            if (_shutdown.load(std::memory_order_acquire))
            {
                break;
            }
            performIndex();
        }
    }

    void performIndex() noexcept
    {
        _indexing.store(true, std::memory_order_release);

        do
        {
            const auto generation = _generation_target.load(std::memory_order_acquire);
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Performing index on generation: ", generation);

            // Build new index
            if (DIR *dir = ::opendir(_watch_path.c_str()); !dir)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to open directory for indexing: ", _watch_path,
                    ", errno: ", std::to_string(errno));
                handleRootRemoved();
            }
            else
            {
                try
                {
                    if (indexRootEntries(*dir, _watch_path))
                    {
                        _generation_current.store(generation, std::memory_order_release);
                    }
                }
                catch (const std::exception &e)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Exception occurred during indexing: ", e.what());
                }
                ::closedir(dir);
            }

            // Check if changes occurred during indexing
            if (_generation_current.load(std::memory_order_acquire)
                != _generation_target.load(std::memory_order_acquire))
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Changes detected during indexing, indexing again...");
                std::this_thread::yield();
                continue;
            }

            VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Indexing complete for generation: ", generation);
            break;
        }
        while (!_shutdown.load(std::memory_order_acquire) && _indexable.load(std::memory_order_acquire));

        _indexing.store(false, std::memory_order_release);
    }

    bool indexRootEntries(DIR &dir, const fs::path &dir_path)
    {
        std::unordered_set<std::string> seen_entries;
        bool partial_index = false;
        for (struct dirent *entry; (entry = ::readdir(&dir)) != nullptr;)
        {
            const std::string name = entry->d_name;
            if (name.starts_with('.'))
            {
                continue; // Skip ., .. and hidden files and directories
            }

            seen_entries.insert(name);

            // Handle new entries (skip shared lock)
            std::shared_ptr<EntryIndex> entry_index;
            {
                const std::unique_lock lock(_entries_mutex);
                if (auto it = _entries.find(name); it == _entries.end())
                {
                    it = _entries.emplace(name, std::make_shared<EntryIndex>()).first;
                    entry_index = it->second;
                }
                else if (it->second->isValid())
                {
                    continue; // Already indexed and valid
                }
                else
                {
                    entry_index = it->second;
                }
            }

            const fs::path full_path = dir_path / name;
            size_t size = 0;
            const auto revision = entry_index->revision();
            switch (indexFileOrSubdirectory(full_path, size))
            {
                case IndexStatus::SUCCESS:
                    if (!entry_index->tryUpdateSize(size, revision))
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Partial index detected for entry: ", std::quoted(name));
                        partial_index = true;
                    }
                    continue;
                case IndexStatus::PARTIAL:
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Invalidating entry due to partial index: ", std::quoted(name));
                    entry_index->markInvalidated();
                    partial_index = true;
                    continue;
                case IndexStatus::CANCELED:
                    return false;
                default:
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Invalidating entry due to unknown error: ", std::quoted(name));
                    entry_index->markInvalidated();
                    partial_index = true;
            }
        }

        // Remove missing entries if not seen
        {
            const std::unique_lock entries_lock(_entries_mutex);
            for (auto it = _entries.begin(); it != _entries.end();)
            {
                if (!seen_entries.contains(it->first))
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "Removing missing entry from index: ", std::quoted(it->first));
                    it->second->markInvalidated();
                    it = _entries.erase(it);
                    partial_index = true;
                }
                else
                {
                    ++it;
                }
            }
        }

        return !partial_index;
    }

    IndexStatus indexSubdirectory(const fs::path &dir_path, size_t &total) const noexcept
    {
        if (_shutdown.load(std::memory_order_acquire))
        {
            return IndexStatus::CANCELED;
        }

        DIR *dir = ::opendir(dir_path.c_str());
        if (!dir)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Opening subdirectory failed: ", dir_path,
                ", errno: ", std::to_string(errno));
            return IndexStatus::PARTIAL;
        }

        IndexStatus status = IndexStatus::SUCCESS;
        for (struct dirent *entry; status == IndexStatus::SUCCESS && (entry = ::readdir(dir)) != nullptr;)
        {
            const std::string_view name = entry->d_name;
            if (name.starts_with('.'))
            {
                continue; // Skip ., .. and hidden files and directories
            }
            const fs::path full_path = dir_path / name;
            status = indexFileOrSubdirectory(full_path, total);
        }

        ::closedir(dir);
        return status;
    }

    IndexStatus indexFileOrSubdirectory(const fs::path &path, size_t &total) const noexcept
    {
        if (_shutdown.load(std::memory_order_acquire))
        {
            return IndexStatus::CANCELED;
        }

        struct stat st;
        if (::lstat(path.c_str(), &st) != 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to stat path: ", path, ", errno: ", std::to_string(errno));
            return IndexStatus::PARTIAL;
        }

        // Fallback to st_size if st_blocks is zero (should not happen on normal
        // files)
        total += st.st_blocks > 0 ? (st.st_blocks << 9) : st.st_size;
        // Recurse into subdirectory
        if (S_ISDIR(st.st_mode))
        {
            if (auto sta = indexSubdirectory(path, total); sta != IndexStatus::SUCCESS)
            {
                return sta;
            }
        }

        return IndexStatus::SUCCESS;
    }

    static constexpr inline const size_t kFanotifyBufferSize = 8192;
    static constexpr inline const size_t kFanotifyPoolTimeoutMs = 500;
    static constexpr inline const size_t kFanotifyPoolDelayMs = 1000;

    void watcherLoop() noexcept
    {
        alignas(struct fanotify_event_metadata) char buffer[kFanotifyBufferSize];
        while (!_shutdown.load(std::memory_order_acquire))
        {
            struct pollfd pfd = { _fanotify_fd, POLLIN, 0 };
            if (int ret = ::poll(&pfd, 1, kFanotifyPoolTimeoutMs); ret < 0)
            {
                if (int err = errno; err == EINTR)
                {
                    continue;
                }
                else
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Poll error in watcher loop: ", std::strerror(err));
                    requestReindex(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(kFanotifyPoolDelayMs));
                    continue;
                }
            }
            else if (ret == 0)
            {
                continue; // Timeout
            }

            if (!(pfd.revents & POLLIN))
            {
                continue;
            }

            const ssize_t len = ::read(_fanotify_fd, buffer, sizeof(buffer));
            if (len <= 0)
            { // 0 on EOF
                if (int err = errno; err == EINTR || err == EAGAIN)
                {
                    continue;
                }
                else
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Read error in watcher loop: ", std::strerror(err));
                    requestReindex(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(kFanotifyPoolDelayMs));
                    continue;
                }
            }
            processFanotifyEvents(buffer, len);
        }
    }

    void processFanotifyEvents(char *buffer, ssize_t len) noexcept
    {
        bool pending_changes = false;
        for (struct fanotify_event_metadata *metadata = reinterpret_cast<struct fanotify_event_metadata *>(buffer);
            !_shutdown.load(std::memory_order_acquire) && FAN_EVENT_OK(metadata, len);)
        {
            if (metadata->vers != FANOTIFY_METADATA_VERSION) [[unlikely]]
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                    "Fanotify metadata version mismatch: ", static_cast<int>(metadata->vers),
                    " != ", FANOTIFY_METADATA_VERSION);
                break;
            }

            EventResult result = EventResult::SKIPPED;

            try
            {
                // Handle FID events (directory change notifications)
                if (metadata->mask
                    & (FAN_CREATE | FAN_DELETE | FAN_MOVE | FAN_MODIFY | FAN_DELETE_SELF | FAN_MOVE_SELF))
                {
                    // For FID-based events, extract the path
                    if (metadata->event_len > sizeof(struct fanotify_event_metadata))
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Processing FID event, mask: ", VG_HEX(metadata->mask, 8));
                        result = handleFidEvent(metadata);
                    }
                    else
                    {
                        // Non-FID event with fd
                        if (metadata->fd >= 0) [[likely]]
                        {
                            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG,
                                "Processing FD event, mask: ", VG_HEX(metadata->mask, 8));
                            result = handleFdEvent(metadata);
                        }
                        else [[unlikely]]
                        {
                            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN,
                                "Skipped event with no FID or FD, mask: ", VG_HEX(metadata->mask, 8));
                        }
                    }
                }
                else
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Skipped unhandled event, mask: ", VG_HEX(metadata->mask, 8));
                }
            }
            catch (const std::exception &e)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                    "Invalidate all entries and reindex requested due to exception: ", e.what());
                requestReindex(true);
                pending_changes = false;
            }

            // Close fd if present
            if (metadata->fd >= 0)
            {
                ::close(metadata->fd);
            }

            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Event processed with result: ", static_cast<int>(result));
            switch (result)
            {
                case EventResult::SKIPPED:
                    break;
                case EventResult::INCREMENTAL:
                    break;
                case EventResult::REINDEX:
                    pending_changes = true;
                    break;
                case EventResult::REINDEX_IMMEDIATELY:
                    requestReindex(false);
                    pending_changes = false;
                    break;
                default:
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN,
                        "Unknown EventResult from event handler: ", static_cast<int>(result));
            }

            metadata = FAN_EVENT_NEXT(metadata, len);
        }
        if (pending_changes)
        {
            requestReindex(false);
        }
    }

    EventResult handleFidEvent(struct fanotify_event_metadata *metadata)
    {
        struct fanotify_event_info_fid *fid = reinterpret_cast<struct fanotify_event_info_fid *>(metadata + 1);
        if (fid->hdr.info_type != FAN_EVENT_INFO_TYPE_DFID_NAME) [[unlikely]]
        {
            throw std::runtime_error("Unexpected fanotify FID event info type: " + std::to_string(fid->hdr.info_type));
        }

        struct file_handle *file_handle = reinterpret_cast<struct file_handle *>(fid->handle);
        // Filename follows the file handle data (f_handle array)
        const char *filename = reinterpret_cast<const char *>(file_handle->f_handle + file_handle->handle_bytes);
        // Skip empty name or hidden files
        if (auto pc = filename; !pc || *pc == '\0' || *pc == '.')
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Skipped FID event with empty or hidden filename");
            return EventResult::SKIPPED;
        }

        // Get the directory path from the file handle
        int dir_fd = ::open_by_handle_at(_mount_fd, file_handle, O_RDONLY | O_DIRECTORY);
        if (dir_fd < 0)
        {
            throw std::runtime_error("FID open_by_handle_at failed: " + std::to_string(errno));
        }

        fs::path full_path;

        {
            char proc_path[64] { "/proc/self/fd/" };
            std::to_chars(proc_path + std::char_traits<char>::length(proc_path), proc_path + sizeof(proc_path), dir_fd)
                .ptr[0]
                = '\0';

            char dir_path[PATH_MAX];
            const ssize_t path_len = ::readlink(proc_path, dir_path, sizeof(dir_path) - 1);

            ::close(dir_fd);

            if (path_len < 0)
            {
                throw std::runtime_error("FID readlink failed: " + std::to_string(errno));
            }
            dir_path[path_len] = '\0';
            full_path = fs::path(dir_path) / filename;
        }

        return handleEvent(metadata->mask, full_path);
    }

    EventResult handleFdEvent(struct fanotify_event_metadata *metadata)
    {
        fs::path full_path;

        {
            char proc_path[64] { "/proc/self/fd/" };
            std::to_chars(proc_path + std::char_traits<char>::length(proc_path), proc_path + sizeof(proc_path),
                metadata->fd)
                .ptr[0]
                = '\0';

            char file_path[PATH_MAX];
            const ssize_t path_len = ::readlink(proc_path, file_path, sizeof(file_path) - 1);
            if (path_len < 0)
            {
                throw std::runtime_error("FD readlink failed: " + std::to_string(errno));
            }
            file_path[path_len] = '\0';
            full_path = fs::path(file_path);
        }

        return handleEvent(metadata->mask, full_path);
    }

    EventResult handleEvent(uint64_t mask, const fs::path &path) noexcept
    {
        if (path == _watch_path)
        {
            return EventResult::REINDEX_IMMEDIATELY;
        }
        if (!isUnderWatchPath(path) || containsHiddenComponent(path))
        {
            return EventResult::SKIPPED;
        }
        return tryIncrementalUpdate(mask, path);
    }

    EventResult tryIncrementalUpdate(uint64_t mask, const fs::path &path) noexcept
    {
        // Handle root removal incrementally
        if (mask & FAN_DELETE_SELF)
        {
            if (path == _watch_path)
            {
                handleRootRemoved();
                return EventResult::SKIPPED;
            }
        }

        // Get relative path from watch path
        fs::path rel_path;
        try
        {
            rel_path = fs::relative(path, _watch_path);
        }
        catch (...)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to compute relative path for incremental update: ", path);
            return EventResult::REINDEX;
        }
        if (rel_path.empty() || rel_path == ".")
        {
            return EventResult::REINDEX; // Path is the watch path itself
        }

        // Get the first component (entry name) and check if it's nested
        auto it = rel_path.begin();
        const std::string &entry_name = it->native();
        const bool is_nested = std::next(it, 1) != rel_path.end();
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Attempting incremental update for entry: ", std::quoted(entry_name),
            ", nested: ", is_nested, ", mask: ", VG_HEX(mask, 8));

        // Handle deletion
        if (mask & (FAN_DELETE | FAN_MOVED_FROM))
        {
            return handleIncrementalRemove(entry_name, is_nested, path);
        }

        // Handle creation
        if (mask & (FAN_CREATE | FAN_MOVED_TO))
        {
            return handleIncrementalAdd(entry_name, is_nested, path);
        }

        // Handle modification
        if (mask & FAN_MODIFY)
        {
            return handleIncrementalModify(entry_name, is_nested, path);
        }

        // Other cases - fall back to reindex
        return EventResult::REINDEX;
    }

    EventResult handleIncrementalRemove(const std::string &entry_name, bool is_nested, const fs::path &path) noexcept
    {
        if (!is_nested)
        {
            const std::unique_lock entries_lock(_entries_mutex);
            if (auto it = _entries.find(entry_name); it == _entries.end())
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Untracked entry removed from: ", path);
                return EventResult::INCREMENTAL;
            }
            else
            {
                // Erase untracked entry immediately
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Erasing untracked entry from index: ", std::quoted(entry_name));
                it->second->markInvalidated(); // Mark as invalidated
                _entries.erase(it);
                return EventResult::INCREMENTAL; // Treated as successful incremental
                                                 // remove
            }
        }
        else
        {
            const std::shared_lock entries_lock(_entries_mutex);
            if (auto it = _entries.find(entry_name); it == _entries.end())
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Untracked entry removed from: ", path);
                return EventResult::INCREMENTAL;
            }
            else
            {
                // If under an entry - lazy reindex it
                it->second->markInvalidated();
                return EventResult::REINDEX;
            }
        }
    }

    EventResult handleIncrementalAdd(const std::string &entry_name, bool is_nested, const fs::path &path) noexcept
    {
        (void)is_nested;
        const std::shared_lock entries_lock(_entries_mutex);
        if (auto it = _entries.find(entry_name); it == _entries.end())
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Untracked entry add to: ", path);
            return EventResult::REINDEX;
        }
        else
        { // Lazy reindex it
            it->second->markInvalidated();
            return EventResult::REINDEX;
        }
    }

    EventResult handleIncrementalModify(const std::string &entry_name, bool is_nested, const fs::path &path) noexcept
    {
        (void)is_nested;
        const std::shared_lock entries_lock(_entries_mutex);

        if (auto it = _entries.find(entry_name); it == _entries.end())
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Untracked entry modified at: ", path);
            return EventResult::REINDEX;
        }
        else
        { // Lazy reindex it
            it->second->markInvalidated();
            return EventResult::REINDEX;
        }
    }

    void handleRootRemoved() noexcept
    {
        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN,
            "Watch directory has been removed or is no longer accessible: ", _watch_path);
        _indexable.store(false, std::memory_order_release);
        {
            std::shared_lock entries_lock(_entries_mutex);
            for (auto &[_, entry]: _entries)
            {
                entry->markInvalidated();
            }
        }
        _generation_target.fetch_add(1, std::memory_order_release);
        _indexer_cv.notify_one();
    }
};

} // namespace vigil::index_manager

#endif
