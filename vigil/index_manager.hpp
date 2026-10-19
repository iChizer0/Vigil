#pragma once
#ifndef VIGIL_INDEX_MANAGER_HPP
#define VIGIL_INDEX_MANAGER_HPP

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

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

#ifndef VG_INDEX_MGR_TRACE_DEBUG
#define VG_INDEX_MGR_TRACE_DEBUG 0
#endif

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

inline bool EnsureOwnerAndPermissions(const fs::path &path, uid_t owner_uid, gid_t owner_gid, fs::perms permissions,
    Context *ctx = nullptr) noexcept
{
    std::error_code ec;
    std::filesystem::permissions(path, permissions, ec);
    if (ec)
    {
        if (ctx)
        {
            VG_LOG(ctx->log, VG_LOG_LEVEL_ERROR, "Failed to set permissions for ", path, ": ", ec.message());
        }
        return false;
    }
    if (::chown(path.c_str(), owner_uid, owner_gid) != 0)
    {
        if (ctx)
        {
            VG_LOG(ctx->log, VG_LOG_LEVEL_ERROR, "Failed to set owner for ", path, ": ", static_cast<int>(errno));
        }
        return false;
    }
    return true;
}

// Idempotent and non-destructive (docs/PLAN.md section 9): an existing directory is accepted, and whatever else
// is in the way makes this fail instead of being removed. Two publishers may create the same date directory at
// the same time; create_directories tolerates the other one winning.
inline bool EnsureDirectoryExists(const fs::path &path, bool update_permissions = false,
    bool override_permissions = false, Context *ctx = nullptr) noexcept
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
    {
        if (override_permissions)
            EnsureOwnerAndPermissions(path, kStorageDefaultUid, kStorageDefaultGid,
                fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all, ctx);
        return true;
    }
    if (std::filesystem::create_directories(path, ec); ec)
    {
        if (ctx)
        {
            VG_LOG(ctx->log, VG_LOG_LEVEL_ERROR, "Failed to create directory ", path, ": ", ec.message());
        }
        return false;
    }
    if (update_permissions)
    {
        EnsureOwnerAndPermissions(path, kStorageDefaultUid, kStorageDefaultGid,
            fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all, ctx);
    }
    return true;
}

inline fs::path ThumbnailPathFrom(const fs::path &path) noexcept
{
    return path.parent_path() / kStorageThumbnailDirName
           / (path.filename().string() + std::string(kStorageThumbnailFileExt));
}

// Where a file is published: `directory / file_name` when nothing exists there, otherwise the first free
// "<stem> (n)<extension>" with n counting from 1, so a published file never replaces an existing one. Automatic
// names carry their start time, so a taken name means a repeated clock (a restart before time synchronization);
// concurrent publishers never share a name, which is why an existence check is enough here.
inline fs::path UniquePublicationPath(const fs::path &directory, const fs::path &file_name) noexcept
{
    std::error_code ec;
    auto path = directory / file_name;
    const auto stem = file_name.stem().string();
    const auto extension = file_name.extension().string();
    for (size_t n = 1; std::filesystem::exists(path, ec); ++n)
        path = directory / (stem + " (" + std::to_string(n) + ")" + extension);
    return path;
}

inline bool RemoveWithThumbnailCheck(const fs::path &path) noexcept
{
    static const std::unordered_set<std::string_view> kThumbnailAssociatedExtensions = { ".mp4" };
    std::error_code ec;
    if (auto ext = path.extension().string(); kThumbnailAssociatedExtensions.contains(ext))
        fs::remove_all(ThumbnailPathFrom(path), ec);
    fs::remove_all(path, ec);
    return !ec;
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
    public:
        enum Status : int64_t { INVALIDATED = -1 };

        struct Index
        {
            int64_t size;
            size_t revision;

            inline bool isValid() const noexcept
            {
                return size >= 0;
            }
        };

    private:
        alignas(kHardwareDestructiveInterferenceSize) std::atomic<Index> _index;

    public:
        EntryIndex() noexcept : _index({ Status::INVALIDATED, 0 }) { }

        EntryIndex(const EntryIndex &) = delete;
        EntryIndex &operator=(const EntryIndex &) = delete;

        EntryIndex(EntryIndex &&) = delete;
        EntryIndex &operator=(EntryIndex &&) = delete;

        inline Index current() const noexcept
        {
            return _index.load(std::memory_order_acquire);
        }

        inline size_t size() const noexcept
        {
            const auto size = _index.load(std::memory_order_acquire).size;
            return size >= 0 ? static_cast<size_t>(size) : 0;
        }

        inline bool isValid() const noexcept
        {
            return _index.load(std::memory_order_acquire).isValid();
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

    explicit IndexManager(Context &ctx, const fs::path &mount_point, const fs::path &watch_path)
        : _ctx(ctx), _mount_point(mount_point), _watch_path(fs::absolute(watch_path).lexically_normal()),
          _shutdown(false), _indexable(true), _indexing(false), _mount_fd(-1), _fanotify_fd(-1), _generation_current(0),
          _generation_target(1)
    {
        if (!fs::exists(_watch_path))
            throw std::runtime_error("Watch directory does not exist: " + _watch_path.string());
        if (!fs::is_directory(_watch_path))
            throw std::runtime_error("Watch path is not a directory: " + _watch_path.string());
        if (!IsPathUnderRoot(_mount_point, _watch_path))
            throw std::runtime_error(
                "Watch path " + _watch_path.string() + " is not under mount point " + _mount_point.string());

        try
        {
            // Open fd to mount point for use with open_by_handle_at
            if (_mount_fd = ::open(_mount_point.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); _mount_fd < 0)
                throw std::runtime_error(
                    "Failed to open mount point: " + _mount_point.string() + ", errno: " + std::to_string(errno));

            if (_fanotify_fd
                = ::fanotify_init(FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME | FAN_UNLIMITED_QUEUE,
                    O_RDONLY | O_LARGEFILE | O_CLOEXEC);
                _fanotify_fd < 0)
                throw std::runtime_error("Failed to initialize fanotify: " + std::to_string(errno));

            constexpr static const uint64_t kEventMask
                = FAN_CLOSE_WRITE | FAN_CREATE | FAN_DELETE | FAN_MOVE | FAN_ONDIR | FAN_DELETE_SELF | FAN_MOVE_SELF;
            if (const int ret = ::fanotify_mark(_fanotify_fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, kEventMask, AT_FDCWD,
                    _mount_point.c_str());
                ret != 0)
                throw std::runtime_error("Failed to mark target for fanotify: " + std::to_string(errno));

            // Start worker threads
            _watcher_thread = std::thread(&IndexManager::watcherLoop, this);
            _indexer_thread = std::thread(&IndexManager::indexerLoop, this);
        }
        catch (...)
        {
            shutdown();
            throw;
        }

        VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO, "IndexManager initialized, watching path: ", _watch_path,
            ", mount point: ", _mount_point);
    }

    ~IndexManager() noexcept
    {
        shutdown();
    }

    IndexManager(const IndexManager &) = delete;
    IndexManager &operator=(const IndexManager &) = delete;

    IndexManager(IndexManager &&) = delete;
    IndexManager &operator=(IndexManager &&) = delete;

    bool indexing() const noexcept
    {
        return _indexing.load(std::memory_order_acquire);
    }

    bool indexed() const noexcept
    {
        return _generation_current.load(std::memory_order_acquire)
               == _generation_target.load(std::memory_order_acquire);
    }

    class IndexSuspendGuard final: public NonCopyable<IndexSuspendGuard>
    {
    public:
        IndexSuspendGuard(IndexManager &manager) noexcept : _manager(manager)
        {
            _manager._indexer_mutex.lock();
        }

        ~IndexSuspendGuard() noexcept
        {
            _manager._indexer_mutex.unlock();
            _manager._indexer_cv.notify_one();
        }

    private:
        IndexManager &_manager;
    };

    IndexSuspendGuard suspend() noexcept
    {
        return { *this };
    }

    IndexResult size() const noexcept
    {
        if (!_indexable.load(std::memory_order_acquire))
            return { false, 0 };
        const auto generation = _generation_current.load(std::memory_order_acquire);
        bool valid = true;
        FileSize total_size = 0;
        {
            const std::shared_lock entries_lock(_entries_mutex);
            for (const auto &[_, entry]: _entries)
            {
                valid = valid && entry->isValid();
                total_size += entry->size();
            }
        }
        return { valid && _indexable.load(std::memory_order_acquire)
                     && generation == _generation_target.load(std::memory_order_acquire),
            total_size };
    }

    size_t tryReleaseSpace(size_t target_size_bytes) noexcept
    {
        if (target_size_bytes == 0)
            return 0;
        size_t removed_size = 0;
        const std::unique_lock lock(_indexer_mutex);
        {
            const std::shared_lock entries_lock(_entries_mutex);
            for (auto it = _entries.begin();
                 it != _entries.end() && removed_size < target_size_bytes && !_shutdown.load(std::memory_order_acquire);
                 ++it)
            {
                const auto &entry_name = it->first;
                const auto &entry_index = it->second;
                if (!entry_index->isValid())
                    continue; // Skip invalid entries
                const fs::path entry_path = _watch_path / entry_name;
                const size_t entry_size = entry_index->size();
                const auto size_to_remove = target_size_bytes - removed_size;
                bool remove_entry = false;
                size_t removed_size_under_entry = 0;
                if (entry_size > size_to_remove)
                    std::tie(remove_entry, removed_size_under_entry) = removeInsideEntry(entry_path, size_to_remove);
                else
                    remove_entry = true;

                std::error_code ec;
                if (remove_entry)
                {
                    fs::remove_all(entry_path, ec);
                    if (!ec) [[likely]]
                        removed_size += entry_size;
                    entry_index->markInvalidated();
                }
                else if (removed_size_under_entry > 0)
                {
                    removed_size += removed_size_under_entry;
                    entry_index->markInvalidated();
                }
                else
                    ec.assign(EBADF, std::generic_category()); // No space removed, treat as failure to remove
                if (ec) [[unlikely]]
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to remove space under entry: ", entry_path,
                        ", code:", ec.value());
                }
            }
            if (removed_size)
                _generation_target.fetch_add(1, std::memory_order_release);
        }
        if (removed_size)
            _indexer_cv.notify_one();
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
    std::pair<bool, size_t> removeInsideEntry(const fs::path &entry_path, size_t target_size_bytes) const noexcept
    {
        {
            struct stat st;
            if (::lstat(entry_path.c_str(), &st) != 0)
                return { false, 0 }; // Failed to stat, treat as no space removed but entry should not be removed
            if (!S_ISDIR(st.st_mode))
                return { true, 0 }; // Not a directory, caller should remove the whole entry
        }

        std::vector<std::string> entry_to_remove;
        size_t removed_size = 0;
        size_t remove_count_success = 0;
        size_t remove_count_fail_or_skip = 0;
        std::error_code ec;

        try
        {
            for (const auto &entry:
                fs::directory_iterator(entry_path, fs::directory_options::skip_permission_denied, ec))
            {
                if (_shutdown.load(std::memory_order_acquire)) [[unlikely]]
                    return { false, removed_size };
                if (ec)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to iterate entry ", entry.path(), ": ", ec.message());
                    ec.clear(); // Best effort, try to remove what we can
                    ++remove_count_fail_or_skip;
                    continue;
                }
                entry_to_remove.emplace_back(entry.path().filename().string());
            }
            // Sort paths to ensure deterministic order of removal, A-Z, hidden files (starting with .) will be removed
            // last
            std::sort(entry_to_remove.begin(), entry_to_remove.end());
            for (auto it = entry_to_remove.begin(); it != entry_to_remove.end() && removed_size < target_size_bytes;
                 ++it)
            {
                if (it->starts_with('.'))
                    continue; // Skip hidden files until the end
                const auto p = entry_path / *it;
                size_t sz = 0;
                if (const auto sta = indexFileOrSubdirectory(p, sz); sta == IndexStatus::SUCCESS) [[likely]]
                {
                    if (RemoveWithThumbnailCheck(p)) [[likely]]
                    {
                        removed_size += sz;
                        ++remove_count_success;
                    }
                    else
                        ++remove_count_fail_or_skip;
                    it->clear();
                }
                else if (sta == IndexStatus::PARTIAL)
                {
                    it->clear();
                    continue;
                }
                else if (sta == IndexStatus::CANCELED)
                    break;
            }
            for (auto it = entry_to_remove.cbegin(); it != entry_to_remove.cend() && removed_size < target_size_bytes;
                 ++it)
            {
                if (it->empty())
                    continue;
                const auto p = entry_path / *it;
                size_t sz = 0;
                if (const auto sta = indexFileOrSubdirectory(p, sz); sta == IndexStatus::SUCCESS) [[likely]]
                {
                    if (RemoveWithThumbnailCheck(p)) [[likely]]
                    {
                        removed_size += sz;
                        ++remove_count_success;
                    }
                    else
                        ++remove_count_fail_or_skip;
                }
                else if (sta == IndexStatus::PARTIAL)
                    continue;
                else if (sta == IndexStatus::CANCELED)
                    break;
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to remove inside entry ", entry_path, ": ", e.what());
            ++remove_count_fail_or_skip;
        }
        // If all entries to remove are successfully removed under the entry, remove the entry itself
        return { (entry_to_remove.size() == remove_count_success) && (remove_count_fail_or_skip == 0), removed_size };
    }

    // Reference to context
    Context &_ctx;

    // Path management
    const fs::path _mount_point; // Auto-detected mount point for fanotify
    const fs::path _watch_path;  // Directory to index and watch

    // Shutdown control
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _shutdown;

    // Indexing state
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _indexable;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _indexing;

    // Indexed entries
    mutable std::shared_mutex _entries_mutex;
    std::map<std::string, std::shared_ptr<EntryIndex>> _entries;

    // Kernel fanotify
    int _mount_fd; // fd to root dir, used for open_by_handle_at
    int _fanotify_fd;

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

        _indexable.store(false, std::memory_order_release);

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

    bool makeRelativeToWatchPath(const fs::path &path, fs::path &rel_path) const noexcept
    {
        try
        {
            const fs::path normalized_path
                = path.is_absolute() ? path.lexically_normal() : (_watch_path / path).lexically_normal();

            if (normalized_path == _watch_path)
            {
                rel_path.clear();
                return true;
            }

            const auto &watch_str = _watch_path.native();
            const auto &path_str = normalized_path.native();

            if (watch_str == "/")
            {
                if (path_str.empty() || path_str.front() != '/')
                    return false;
                rel_path = path_str.size() > 1 ? fs::path(path_str.substr(1)) : fs::path();
                return true;
            }

            const auto watch_len = watch_str.length();
            if (path_str.length() <= watch_len || path_str.compare(0, watch_len, watch_str) != 0
                || path_str[watch_len] != '/')
                return false;

            rel_path = fs::path(path_str.substr(watch_len + 1));
            return true;
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Failed to compute lexical relative path for: ", path,
                ", error: ", e.what());
            return false;
        }
    }

    void indexerLoop() noexcept
    {
        while (!_shutdown.load(std::memory_order_acquire))
        {
            {
                std::unique_lock lock(_indexer_mutex);
                _indexer_cv.wait(lock, [this] {
                    if (_shutdown.load(std::memory_order_acquire)) [[unlikely]]
                        return true;
                    if (!_indexable.load(std::memory_order_acquire)) [[unlikely]]
                        return false;
                    if (_generation_current.load(std::memory_order_acquire)
                        != _generation_target.load(std::memory_order_acquire))
                        return true;
                    {
                        const std::shared_lock entries_lock(_entries_mutex);
                        for (const auto &[_, entry]: _entries)
                        {
                            if (!entry->isValid())
                                return true;
                        }
                    }
                    return false;
                });
            }
            if (_shutdown.load(std::memory_order_acquire))
                break;
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
                        _generation_current.store(generation, std::memory_order_release);
                    else
                        std::this_thread::sleep_for(kStorageIndexFailRetryDelay);
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
                std::this_thread::sleep_for(kStorageIndexRetryDelay);
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
        {
            const std::shared_lock entries_lock(_entries_mutex);
            seen_entries.reserve(_entries.size());
        }
        bool partial_index = false;
        for (struct dirent *entry; (entry = ::readdir(&dir)) != nullptr;)
        {
            const std::string_view name_view = entry->d_name;
            if (name_view.starts_with('.'))
                continue; // Skip ., .. and hidden files and directories

            const std::string name(name_view);
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
                    continue; // Already indexed and valid
                else
                    entry_index = it->second;
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
                    ++it;
            }
        }

        return !partial_index;
    }

    IndexStatus indexSubdirectory(const fs::path &dir_path, size_t &total) const noexcept
    {
        if (_shutdown.load(std::memory_order_acquire))
            return IndexStatus::CANCELED;

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
            if (name == "." || name == "..")
                continue; // Skip ., .. special entries

            const fs::path full_path = dir_path / name;
            status = indexFileOrSubdirectory(full_path, total);
        }

        ::closedir(dir);
        return status;
    }

    IndexStatus indexFileOrSubdirectory(const fs::path &path, size_t &total) const noexcept
    {
        if (_shutdown.load(std::memory_order_acquire))
            return IndexStatus::CANCELED;

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
                return sta;
        }

        return IndexStatus::SUCCESS;
    }

    static constexpr inline const size_t kFanotifyBufferSize = 65535;
    static constexpr inline const size_t kFanotifyPollTimeoutMs = 500;
    static constexpr inline const size_t kFanotifyPollDelayMs = 1000;

    void watcherLoop() noexcept
    {
        void *buffer
            = ::operator new(kFanotifyBufferSize, std::align_val_t { alignof(struct fanotify_event_metadata) });
        while (!_shutdown.load(std::memory_order_acquire))
        {
            struct pollfd pfd = { _fanotify_fd, POLLIN, 0 };
            if (const int ret = ::poll(&pfd, 1, kFanotifyPollTimeoutMs); ret < 0)
            {
                if (const int err = errno; err == EINTR)
                    continue;
                else
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Poll error in watcher loop: ", std::strerror(err));
                    requestReindex(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(kFanotifyPollDelayMs));
                    continue;
                }
            }
            else if (ret == 0)
                continue; // Timeout

            if (!(pfd.revents & POLLIN))
                continue;

            const ssize_t len = ::read(pfd.fd, buffer, kFanotifyBufferSize);
            if (len <= 0)
            {
                if (const int err = errno; err == EINTR || err == EAGAIN)
                    continue; // 0 on EOF
                else
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Read error in watcher loop: ", std::strerror(err));
                    requestReindex(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(kFanotifyPollDelayMs));
                    continue;
                }
            }
            processFanotifyEvents(buffer, len);
        }
        ::operator delete(buffer, std::align_val_t { alignof(struct fanotify_event_metadata) });
    }

    void processFanotifyEvents(void *buffer, ssize_t len) noexcept
    {
        bool pending_changes = false;
        for (struct fanotify_event_metadata *metadata = reinterpret_cast<struct fanotify_event_metadata *>(buffer);
             FAN_EVENT_OK(metadata, len) && !_shutdown.load(std::memory_order_acquire);
             metadata = FAN_EVENT_NEXT(metadata, len))
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
                    & (FAN_CLOSE_WRITE | FAN_CREATE | FAN_DELETE | FAN_MOVE | FAN_DELETE_SELF | FAN_MOVE_SELF))
                {
                    // For FID-based events, extract the path
                    if (metadata->event_len > metadata->metadata_len)
                    {
#if VG_INDEX_MGR_TRACE_DEBUG
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Processing FID event, mask: ", VG_HEX(metadata->mask, 8));
#endif
                        result = handleFidEvent(metadata);
                    }
                    else if (metadata->fd >= 0)
                    {
#if VG_INDEX_MGR_TRACE_DEBUG
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Processing FD event, mask: ", VG_HEX(metadata->mask, 8));
#endif
                        result = handleFdEvent(metadata);
                    }
                    else
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN,
                            "Skipped event with no FID or FD, mask: ", VG_HEX(metadata->mask, 8));
                    }
                }
                else
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Skipped unhandled event, mask: ", VG_HEX(metadata->mask, 8));
                }
            }
            catch (const std::exception &e)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG,
                    "Invalidate all entries and reindex requested due to exception: ", e.what());
                requestReindex(true);
                pending_changes = false;
            }

            // Close fd if present
            if (metadata->fd >= 0)
                ::close(metadata->fd);

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
        }
        if (pending_changes)
            requestReindex(false);
    }

    EventResult handleFidEvent(struct fanotify_event_metadata *metadata)
    {
        struct fanotify_event_info_header *hdr = reinterpret_cast<struct fanotify_event_info_header *>(
            reinterpret_cast<char *>(metadata) + metadata->metadata_len);
        if (hdr->len == 0) [[unlikely]]
            throw std::runtime_error("FID event with zero-length info");
        if (hdr->info_type != FAN_EVENT_INFO_TYPE_DFID_NAME) [[unlikely]]
            throw std::runtime_error("FID event with unexpected info type: " + std::to_string(hdr->info_type));
        struct fanotify_event_info_fid *fid = reinterpret_cast<struct fanotify_event_info_fid *>(hdr);
        struct file_handle *file_handle = reinterpret_cast<struct file_handle *>(fid->handle);

        // Filename follows the file handle data (f_handle array)
        const char *filename = reinterpret_cast<const char *>(file_handle->f_handle + file_handle->handle_bytes);
        // Skip empty name or hidden files
        if (!filename || *filename == '\0' || std::strcmp(filename, ".") == 0 || std::strcmp(filename, "..") == 0)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Skipped FID event with empty or special filename");
            return EventResult::SKIPPED;
        }

        // Get the directory path from the file handle
        const int fd = ::open_by_handle_at(_mount_fd, file_handle, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0)
            throw std::runtime_error("FID open_by_handle_at failed: " + std::to_string(errno));

        fs::path full_path;

        {
            char proc_path[64] { "/proc/self/fd/" };
            const auto res = std::to_chars(proc_path + std::char_traits<char>::length(proc_path),
                proc_path + sizeof(proc_path) - 1, fd);
            if (res.ec != std::errc()) [[unlikely]]
            {
                ::close(fd);
                throw std::runtime_error("FID proc path formatting failed");
            }
            *res.ptr = '\0';
#if VG_INDEX_MGR_TRACE_DEBUG
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "FID event filename: \"", filename, "\", proc path: ", proc_path);
#endif

            char path[PATH_MAX];
            const ssize_t path_len = ::readlink(proc_path, path, sizeof(path) - 1);

            ::close(fd);
            if (path_len < 0)
                throw std::runtime_error("FID readlink failed: " + std::to_string(errno));

            path[path_len] = '\0';
            full_path = fs::path(path) / filename;
        }

        return handleEvent(metadata->mask, full_path);
    }

    EventResult handleFdEvent(struct fanotify_event_metadata *metadata)
    {
        fs::path full_path;

        {
            char proc_path[64] { "/proc/self/fd/" };
            const auto res = std::to_chars(proc_path + std::char_traits<char>::length(proc_path),
                proc_path + sizeof(proc_path) - 1, metadata->fd);
            if (res.ec != std::errc()) [[unlikely]]
                throw std::runtime_error("FD proc path formatting failed");
            *res.ptr = '\0';

            char file_path[PATH_MAX];
            const ssize_t path_len = ::readlink(proc_path, file_path, sizeof(file_path) - 1);
            if (path_len < 0)
                throw std::runtime_error("FD readlink failed: " + std::to_string(errno));

            file_path[path_len] = '\0';
            full_path = fs::path(file_path);
        }

        return handleEvent(metadata->mask, full_path);
    }

    EventResult handleEvent(uint64_t mask, const fs::path &path) noexcept
    {
        fs::path rel_path;
#if VG_INDEX_MGR_TRACE_DEBUG
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Handling event for path: ", path, ", mask: ", VG_HEX(mask, 8));
#endif
        if (!makeRelativeToWatchPath(path, rel_path))
            return EventResult::SKIPPED;
        if (rel_path.empty())
        {
            if (mask & (FAN_DELETE_SELF | FAN_MOVE_SELF))
            {
                handleRootRemoved();
                return EventResult::SKIPPED;
            }
            return EventResult::REINDEX_IMMEDIATELY;
        }
        if (const auto it = rel_path.begin(); it->native().starts_with('.'))
            return EventResult::SKIPPED; // Skip hidden files and directories at root level
        return tryIncrementalUpdate(mask, path, rel_path);
    }

    EventResult tryIncrementalUpdate(uint64_t mask, const fs::path &path, const fs::path &rel_path) noexcept
    {
        if (rel_path.empty() || rel_path == ".")
            return EventResult::REINDEX; // Path is the watch path itself

        // Get the first component (entry name) and check if it's nested
        auto it = rel_path.begin();
        const std::string entry_name = it->native();
        const bool is_nested = std::next(it, 1) != rel_path.end();
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Attempting incremental update for entry: ", std::quoted(entry_name),
            ", nested: ", is_nested, ", mask: ", VG_HEX(mask, 8));

        // Handle deletion
        if (mask & (FAN_DELETE | FAN_MOVED_FROM))
            return handleIncrementalRemove(entry_name, is_nested, path);

        // Handle creation
        if (mask & (FAN_CREATE | FAN_MOVED_TO))
            return handleIncrementalAdd(entry_name, is_nested, path);

        // Handle modification
        if (mask & FAN_CLOSE_WRITE)
            return handleIncrementalModify(entry_name, is_nested, path);

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
        const std::shared_lock entries_lock(_entries_mutex);
        if (auto it = _entries.find(entry_name); it == _entries.end())
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Untracked entry add to: ", path);
            return EventResult::REINDEX;
        }
        else
        {
            if (!is_nested)
            {
                it->second->markInvalidated();
                return EventResult::REINDEX;
            }
            bool incremental = false;
            if (const auto cur = it->second->current(); cur.isValid())
            {
                size_t size = 0;
                if (indexFileOrSubdirectory(path, size) == IndexStatus::SUCCESS)
                    incremental = it->second->tryUpdateSize(cur.size + size, cur.revision);
            }
            if (incremental)
                return EventResult::INCREMENTAL;
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
                entry->markInvalidated();
        }
        _generation_target.fetch_add(1, std::memory_order_release);
        _indexer_cv.notify_one();
    }
};

} // namespace vigil::index_manager

#endif
