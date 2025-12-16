#pragma once
#ifndef VIGIL_WRITE_STATS_HPP
#define VIGIL_WRITE_STATS_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

#include "constants.hpp"
#include "context.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil::write_stats {

using namespace vigil::types;
using namespace vigil::constants;

class WriteStats final
{
public:
    WriteStats()
        : _write_stats_mutex(), _write_stats_time_base(std::chrono::steady_clock::now()),
          _write_stats_time_last_sync(std::chrono::minutes(0)), _hourly_write_bytes_per_minutes {}
    {
        for (auto &bytes_written: _hourly_write_bytes_per_minutes)
            bytes_written = 0;
    }

    ~WriteStats() = default;

    void addNewWriteStat(FileSize bytes)
    {
        if (bytes <= 0)
            return;
        const std::unique_lock lock(_write_stats_mutex);
        const auto now = std::chrono::steady_clock::now();
        const auto now_since_start = std::chrono::duration_cast<std::chrono::minutes>(now - _write_stats_time_base);
        syncWriteStatsAndGetDurationUnsafe(now_since_start);
        const auto index = now_since_start.count() % std::chrono::minutes::period::num;
        _hourly_write_bytes_per_minutes[index] += bytes;
    }

    FileSize getEstimatedWriteBytesIn(const std::chrono::minutes &period)
    {
        const std::unique_lock lock(_write_stats_mutex);
        const auto now_since_start = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - _write_stats_time_base);
        const auto elapsed_minutes = syncWriteStatsAndGetDurationUnsafe(now_since_start).count();
        if (elapsed_minutes <= 0)
            return 0;
        const auto total_bytes = std::accumulate(_hourly_write_bytes_per_minutes.begin(),
            _hourly_write_bytes_per_minutes.end(), static_cast<FileSize>(0));
        return static_cast<FileSize>(std::round(static_cast<float>(total_bytes * period.count()) / elapsed_minutes));
    }

    void reset()
    {
        const std::unique_lock lock(_write_stats_mutex);
        for (auto &bytes_written: _hourly_write_bytes_per_minutes)
            bytes_written = 0; // Reset the counter for this minute
        _write_stats_time_last_sync = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - _write_stats_time_base);
    }

private:
    std::chrono::minutes syncWriteStatsAndGetDurationUnsafe(const std::chrono::minutes &now_since_start)
    {
        const auto cur = now_since_start.count();
        const auto prv = _write_stats_time_last_sync.count();
        const auto elapsed = cur - prv;
        if (elapsed > std::chrono::minutes::period::num)
        {
            const auto staled = elapsed - std::chrono::minutes::period::num;
            const auto start = prv;
            const auto end = prv + staled;
            for (auto i = start; i < end; ++i)
                _hourly_write_bytes_per_minutes[i % std::chrono::minutes::period::num]
                    = 0; // Reset the counter for staled minutes
            _write_stats_time_last_sync = now_since_start - std::chrono::minutes(std::chrono::minutes::period::num);
        }
        return std::chrono::minutes(elapsed);
    }

private:
    mutable std::mutex _write_stats_mutex;
    const std::chrono::steady_clock::time_point _write_stats_time_base;
    std::chrono::minutes _write_stats_time_last_sync;
    std::array<FileSize, std::chrono::minutes::period::num> _hourly_write_bytes_per_minutes;
};

} // namespace vigil::write_stats

#endif
