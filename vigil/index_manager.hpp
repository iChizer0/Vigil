#pragma once
#ifndef VIGIL_INDEX_MANAGER_HPP
#define VIGIL_INDEX_MANAGER_HPP

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "constants.hpp"
#include "context.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil::index_manager {

using namespace vigil::types;
using namespace vigil::constants;

class IndexManager final
{
public:
    IndexManager(Context &ctx, const std::filesystem::path &root_path)
        : _ctx(ctx), _root_path(root_path), _fs_mutex(), _fs_sync_revision(0), _1st_level_mutex(), _1st_level_stats(),
          _last_index_time()
    {
    }

    ~IndexManager() = default;

    std::chrono::steady_clock::time_point lastIndexTime() const noexcept
    {
        return _last_index_time.load(std::memory_order_relaxed);
    }

    FileSize usedBytes() const noexcept
    {
        FileSize total = 0;
        const std::shared_lock lock(_1st_level_mutex);
        for (const auto &[_, size_atomic]: _1st_level_stats)
            total += size_atomic.load(std::memory_order_relaxed);
        return total;
    }

    std::vector<std::pair<FileIndex, FileSize>> getFirstLevelStats() const
    {
        std::vector<std::pair<FileIndex, FileSize>> stats;
        const std::shared_lock lock(_1st_level_mutex);
        for (const auto &[index, size_atomic]: _1st_level_stats)
            stats.emplace_back(index, size_atomic.load(std::memory_order_relaxed));
        return stats;
    }

    void reset() noexcept
    {
        const std::unique_lock lock(_1st_level_mutex);
        _1st_level_stats.clear();
        _last_index_time.store(std::chrono::steady_clock::now(), std::memory_order_release);
    }

    bool trySync() noexcept
    {
        FileSize new_used_bytes = 0;
        const auto last_fs_sync_rev = _fs_sync_revision.load(std::memory_order_acquire);
        std::vector<std::pair<FileIndex, FileSize>> new_1st_level_stats;
        try
        {
            for (const auto &entry: std::filesystem::directory_iterator(_root_path,
                     std::filesystem::directory_options::skip_permission_denied))
            {
                try
                {
                    const auto &file_name = entry.path().filename().string();
                    if (file_name.starts_with('.'))
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG,
                            "Skipping hidden file or directory during indexing: ", entry.path());
                        continue; // Skip hidden files and dirs
                    }
                    {
                        const std::shared_lock lock(_fs_mutex);
                        const auto entry_size = doStatsSizeUnsafe(entry.path(), entry.status());
                        if (!entry_size)
                        {
                            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN,
                                "Failed to get size for entry during indexing: ", entry.path());
                            return false;
                        }
                        new_used_bytes += *entry_size;
                        new_1st_level_stats.emplace_back(FileIndex(entry.path(), kStorageFirstLevelDirNameTimeFormat),
                            *entry_size);
                        if (last_fs_sync_rev != _fs_sync_revision.load(std::memory_order_acquire))
                        {
                            VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG,
                                "Filesystem changed during indexing, restarting index sync");
                            return false;
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to index entry ", entry.path(), ": ", e.what());
                    continue;
                }
            }
            // Sort, newer first (faster insert to map)
            std::sort(new_1st_level_stats.begin(), new_1st_level_stats.end(),
                [](const auto &a, const auto &b) { return a > b; });
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to index first-level entries in ", _root_path, ": ", e.what());
            return false;
        }
        {
            const std::unique_lock lock(_1st_level_mutex);
            const std::shared_lock lock2(_fs_mutex);
            if (last_fs_sync_rev != _fs_sync_revision.load(std::memory_order_relaxed))
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Filesystem changed during swapping index, restarting index sync");
                return false;
            }
            _1st_level_stats.clear();
            for (const auto &pair: new_1st_level_stats)
                _1st_level_stats.insert(pair);
        }
        _last_index_time.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
        return true;
    }

    std::optional<FileSize> moveAndAddCacheFile(const CacheFileInfo &cache_file_info) noexcept
    {
        if (cache_file_info.cache_file_path.empty() || cache_file_info.target_dir_name.empty())
            return std::nullopt;
        std::error_code ec;
        // Ensure target directory exists
        const auto target_dir_path = _root_path / cache_file_info.target_dir_name;
        const auto target_file_path = target_dir_path / cache_file_info.cache_file_path.filename();
        {
            const std::unique_lock lock(_1st_level_mutex);
            auto it = std::find_if(_1st_level_stats.begin(), _1st_level_stats.end(),
                [&cache_file_info](const auto &pair) { return pair.first.name == cache_file_info.target_dir_name; });
            {
                const std::unique_lock lock(_fs_mutex);
                if (it == _1st_level_stats.end())
                {
                    if (!ensureDirectoryExistUnsafe(target_dir_path))
                        return std::nullopt;
                    const auto dir_status = std::filesystem::status(target_dir_path, ec);
                    if (ec)
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to get file status for ", target_dir_path, ": ",
                            ec.message());
                        return std::nullopt;
                    }
                    const auto dir_size = doStatsSizeUnsafe(target_dir_path, dir_status);
                    if (!dir_size)
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to get size for new directory ", target_dir_path,
                            ", aborting adding cache file");
                        return std::nullopt;
                    }
                    bool success = false;
                    std::tie(it, success) = _1st_level_stats.insert_or_assign(
                        FileIndex(cache_file_info.target_dir_name, cache_file_info.creation_time), *dir_size);
                    if (it == _1st_level_stats.end() || !success)
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                            "Failed to add new directory entry to index: ", cache_file_info.target_dir_name);
                        return std::nullopt;
                    }
                }
                std::filesystem::rename(cache_file_info.cache_file_path, target_file_path, ec);
                if (ec)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to move file from ", cache_file_info.cache_file_path,
                        " to ", target_file_path, ": ", ec.message());
                    return std::nullopt;
                }
                _fs_sync_revision.fetch_add(1, std::memory_order_release);
                // Get file size
                const auto file_size = std::filesystem::file_size(target_file_path, ec);
                if (ec)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to get file size for ", target_file_path, ": ",
                        ec.message());
                    return std::nullopt;
                }
                it->second.fetch_add(file_size, std::memory_order_relaxed);
                return file_size;
            }
        }
    }

    vg_sta_e removeFileOrDirectory(const std::filesystem::path &relative_path)
    {
        const auto normalized = relative_path.lexically_normal();
        if (normalized.begin() == normalized.end())
            return EINVAL; // Skip empty path
        if (normalized.is_absolute())
            return EINVAL; // Skip invalid paths, e.g. absolute or hidden paths
        if (!isUnderRoot(normalized))
            return EACCES; // Skip paths outside root
        std::error_code ec;
        const auto first_part = normalized.begin()->string();
        if (first_part.empty() || first_part.starts_with('.'))
            return EACCES; // Skip invalid paths, e.g. hidden files/dirs
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Removing file or directory from index: ", normalized);
        if (FileSize removed_size = 0;
            std::next(normalized.begin()) != normalized.end()) // If the path is file/directory under first-level
        {
            const std::shared_lock lock(_1st_level_mutex);
            auto it = std::find_if(_1st_level_stats.begin(), _1st_level_stats.end(),
                [&first_part](const auto &pair) { return pair.first.name == first_part; });
            if (it == _1st_level_stats.end())
                return removeUntrackedFileOrDirectoryUnsafe(_root_path / normalized); // Not found in index
            {
                const std::unique_lock lock2(_fs_mutex);
                const auto full_path = _root_path / normalized; // Treat as already canonicalized
                const auto status = std::filesystem::status(full_path, ec);
                if (ec)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to get file status for ", full_path, ": ",
                        ec.message());
                    return ENOENT;
                }
                if (auto size = doStatsSizeUnsafe(full_path, status); !size)
                    return EIO;
                else
                    removed_size = *size;
                if (auto removed = std::filesystem::remove_all(full_path, ec); ec)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove file or directory ", full_path, ": ",
                        ec.message());
                    return EIO;
                }
                else if (removed > 0)
                    _fs_sync_revision.fetch_add(1, std::memory_order_acq_rel);
            }
            FileSize observed = it->second.load(std::memory_order_acquire);
            FileSize desired = observed;
            const bool reindex_needed = observed < removed_size;
            if (reindex_needed)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Removed size ", removed_size, " exceeds tracked size ", observed,
                    " for directory ", (_root_path / it->first.name));
                const auto full_path = _root_path / it->first.name;
                do
                {
                    const auto fs_sync_rev_before = _fs_sync_revision.load(std::memory_order_acquire);
                    const std::unique_lock lock2(_fs_mutex);
                    if (auto status = std::filesystem::status(full_path, ec); ec)
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to get file status for ", full_path, ": ",
                            ec.message());
                        desired = 0;
                    }
                    else
                    {
                        if (auto size_now = doStatsSizeUnsafe(full_path, status); size_now)
                            desired = *size_now;
                        else
                            desired = 0;
                    }
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Resetting tracked size to ", desired, " for directory ",
                        full_path);
                    if (fs_sync_rev_before == _fs_sync_revision.load(std::memory_order_acquire))
                    {
                        it->second.store(desired, std::memory_order_release);
                        break;
                    }
                    std::this_thread::yield();
                }
                while (true);
            }
            else
                do
                {
                    desired = observed - removed_size;
                }
                while (!it->second.compare_exchange_strong(observed, desired, std::memory_order_acq_rel,
                    std::memory_order_acquire));
            _fs_sync_revision.fetch_add(1, std::memory_order_acq_rel);
            return 0;
        }
        else // If the path is a first-level file/directory
        {
            const std::unique_lock lock(_1st_level_mutex);
            auto it = std::find_if(_1st_level_stats.begin(), _1st_level_stats.end(),
                [&first_part](const auto &pair) { return pair.first.name == first_part; });
            if (it == _1st_level_stats.end())
                return removeUntrackedFileOrDirectoryUnsafe(_root_path / first_part); // Not found in index
            return removeFirstLevelUnsafe(it).has_value() ? 0 : EIO;
        }
    }

    FileSize freeUpStaledFiles(FileSize required_to_free)
    {
        if (required_to_free <= 0)
            return 0;
        else if (auto used = usedBytes(); required_to_free > used)
            required_to_free = used;
        FileSize bytes_free = 0;
        while (bytes_free < required_to_free)
        {
            const std::unique_lock lock(_1st_level_mutex);
            auto it = _1st_level_stats.begin();
            if (it == _1st_level_stats.end())
                break; // No more files to remove
            const auto removed_size = removeFirstLevelUnsafe(it);
            if (!removed_size.has_value())
                break; // Failed to remove file
            bytes_free += *removed_size > 0 ? *removed_size : 0;
        }
        return bytes_free;
    }

    static bool ensureDirectoryExist(const std::filesystem::path &dir_path, Context &ctx) noexcept
    {
        std::error_code ec;
        if (std::filesystem::is_directory(dir_path, ec))
            return true;
        else
            std::filesystem::remove_all(dir_path, ec);
        if (std::filesystem::create_directories(dir_path, ec); ec)
        {
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create directory ", dir_path, ": ", ec.message());
            return false;
        }
        return true;
    }

private:
    bool isUnderRoot(const std::filesystem::path &relative_path) const
    {
        const auto target_path = (_root_path / relative_path).lexically_normal();
        return canonicalPathUnderRoot(target_path).has_value();
    }

    std::optional<std::filesystem::path> canonicalPathUnderRoot(const std::filesystem::path &path) const
    {
        std::error_code ec;
        const auto root_canonical = std::filesystem::weakly_canonical(_root_path, ec);
        if (ec)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to resolve canonical root path ", _root_path, ": ",
                ec.message());
            return std::nullopt;
        }
        const auto target_canonical = std::filesystem::weakly_canonical(path, ec);
        if (ec)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to resolve canonical path for ", path, ": ", ec.message());
            return std::nullopt;
        }
        auto root_it = root_canonical.begin();
        auto candidate_it = target_canonical.begin();
        for (; root_it != root_canonical.end(); ++root_it, ++candidate_it)
        {
            if (candidate_it == target_canonical.end() || *root_it != *candidate_it)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Canonical path ", target_canonical, " escapes storage root ",
                    root_canonical);
                return std::nullopt;
            }
        }
        return target_canonical;
    }

    bool ensureDirectoryExistUnsafe(const std::filesystem::path &dir_path) noexcept
    {
        std::error_code ec;
        if (std::filesystem::is_directory(dir_path, ec))
            return true;
        else if (std::filesystem::remove_all(dir_path, ec))
            _fs_sync_revision.fetch_add(1, std::memory_order_release);
        if (std::filesystem::create_directories(dir_path, ec))
            _fs_sync_revision.fetch_add(1, std::memory_order_release);
        if (ec)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create directory ", dir_path, ": ", ec.message());
            return false;
        }
        return true;
    }

    vg_sta_e removeUntrackedFileOrDirectoryUnsafe(const std::filesystem::path &path) noexcept
    {
        std::error_code ec;
        _fs_sync_revision.fetch_add(1, std::memory_order_relaxed);
        std::filesystem::remove_all(path, ec);
        if (ec)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove untracked file or directory ", path, ": ",
                ec.message());
            return EIO;
        }
        _fs_sync_revision.fetch_add(1, std::memory_order_release);
        return 0;
    }

    std::optional<FileSize> removeFirstLevelUnsafe(auto &it)
    {
        std::error_code ec;
        _fs_sync_revision.fetch_add(1, std::memory_order_relaxed);
        {
            const std::unique_lock lock2(_fs_mutex);
            const auto target_path = _root_path / it->first.name; // Should be already normalized
            if (auto removed = std::filesystem::remove_all(target_path, ec); ec)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove file or directory ", target_path, ": ",
                    ec.message());
                return std::nullopt;
            }
            else if (removed > 0)
                _fs_sync_revision.fetch_add(1, std::memory_order_relaxed);
        }
        const auto removed_size = it->second.load(std::memory_order_relaxed);
        it = _1st_level_stats.erase(it);
        return removed_size;
    }

    std::optional<FileSize> doStatsSizeUnsafe(const std::filesystem::path &path,
        const std::filesystem::file_status &status) const noexcept
    {
        std::error_code ec;
        switch (status.type())
        {
            case std::filesystem::file_type::regular:
                return std::filesystem::file_size(path, ec);

            case std::filesystem::file_type::directory: {
                auto it = std::filesystem::recursive_directory_iterator(path,
                    std::filesystem::directory_options::skip_permission_denied, ec);
                if (ec)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to open directory iterator for ", path, ": ",
                        ec.message());
                    return std::nullopt;
                }
                FileSize total_size = 0;
                const auto end = std::filesystem::recursive_directory_iterator();
                while (it != end)
                {
                    const auto &entry = *it;
                    if (entry.is_regular_file(ec))
                        total_size += entry.file_size(ec);
                    else
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Skipping non-regular file ", entry.path(),
                            " in size calculation");
                    if (ec)
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to get file size for ", entry.path(), ": ",
                            ec.message());
                    if (it.increment(ec); ec)
                    {
                        VG_LOG(_ctx.log, VG_LOG_LEVEL_WARN, "Failed to increment directory iterator for ", path, ": ",
                            ec.message());
                        break;
                    }
                }
                return total_size;
            }

            default:
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Skipping non-regular and non-directory file ", path,
                    " in size calculation");
                return std::nullopt;
        }
    }

private:
    Context &_ctx;
    std::filesystem::path _root_path;
    mutable std::shared_mutex _fs_mutex;

    alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> _fs_sync_revision;
    mutable std::shared_mutex _1st_level_mutex;
    std::map<FileIndex, std::atomic<FileSize>> _1st_level_stats;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<std::chrono::steady_clock::time_point> _last_index_time;
};

} // namespace vigil::index_manager

#endif
