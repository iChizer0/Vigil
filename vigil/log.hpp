#pragma once
#ifndef VIGIL_LOG_HPP
#define VIGIL_LOG_HPP

#include <chrono>

#ifndef VG_LOG_LEVEL
#define VG_LOG_LEVEL (vg_log_level_e::VG_LOG_LEVEL_DEBUG)
#endif

#ifndef VG_LOG_TRACE_ENABLED
#define VG_LOG_TRACE_ENABLED 0
#endif

#ifndef VG_LOG_INTERNAL_ENABLED
#define VG_LOG_INTERNAL_ENABLED 0
#endif

#ifdef VG_LOG
#error "Duplicate definition of VG_LOG"
#endif

namespace {
inline decltype(auto) getCurrentTimeStamp() noexcept
{
    auto current_time = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(current_time).count();
    return ms;
}
} // namespace

#if VG_LOG_TRACE_ENABLED
#define VG_LOG(logf, lvl, ...)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if constexpr (lvl >= VG_LOG_LEVEL)                                                                             \
            logf(lvl, "[", getCurrentTimeStamp(), "] ", __FILE__, ":", __LINE__, ": ", __VA_ARGS__);                   \
    }                                                                                                                  \
    while (0)
#else
#define VG_LOG(logf, lvl, ...)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (lvl >= VG_LOG_LEVEL)                                                                                       \
            logf(lvl, __VA_ARGS__);                                                                                    \
    }                                                                                                                  \
    while (0)
#endif

#endif
