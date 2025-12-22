#pragma once
#ifndef VIGIL_RELAY_HPP
#define VIGIL_RELAY_HPP

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>

#include "constants.hpp"
#include "context.hpp"
#include "log.hpp"
#include "status.hpp"

namespace vigil::relay {

using namespace vigil::status;
using namespace vigil::constants;

class Relay final
{
public:
    Relay(Context &ctx, const std::filesystem::path &target_dir)
        : _ctx(ctx), _target_dir_path(target_dir), _relay_dir_path(_ctx.c_api.storage.relay_directory_path), _mutex(),
          _relay_dir_mutex(), _relay_dir_name(), _relay_thread(nullptr), _last_active_time()
    {
    }

    ~Relay() noexcept
    {
        disableRelayUnsafe();
    }

    RelayStatus getStatus() const noexcept
    {
        RelayStatus status;
        status.relay_directory = getRelayDirName();
        if (status.relay_directory.empty())
            status.relay_timeout_remain = std::chrono::seconds(0);
        else if (auto duration = std::chrono::steady_clock::now() - _last_active_time.load(std::memory_order_acquire);
            duration < kRelayTimeout)
            status.relay_timeout_remain = std::chrono::duration_cast<std::chrono::seconds>(kRelayTimeout - duration);
        status.relay_timeout = kRelayTimeout;
        return status;
    }

    vg_sta_e enableOrHeartbeatNginxRelay()
    {
        const std::unique_lock api_lock(_mutex);
        {
            if (isRelayEnabled())
            {
                _last_active_time.store(std::chrono::steady_clock::now(), std::memory_order_release);
                return 0;
            }
        }

        // Create unique relay directory
        try
        {
            std::error_code ec;
            // Check if target directory exists
            if (!std::filesystem::is_directory(_target_dir_path, ec))
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR,
                    "Target directory for nginx relay does not exist: ", _target_dir_path);
                return ENOENT;
            }

            // Ensure relay root directory exists
            std::filesystem::create_directories(_relay_dir_path, ec);
            if (ec)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create relay root directory ", _relay_dir_path, ": ",
                    ec.message());
                return EIO;
            }

            auto relay_dir_name = generateUUIDv4();
            for (;; relay_dir_name = generateUUIDv4())
            {
                const auto relay_path = _relay_dir_path / relay_dir_name;
                std::filesystem::create_symlink(_target_dir_path, relay_path, ec);
                if (!ec)
                    break;
                if (ec == std::errc::file_exists)
                    continue;
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create relay symlink ", relay_path, ": ", ec.message());
                relay_dir_name.clear();
                break;
            }
            if (relay_dir_name.empty())
                return EIO;

            // Update the relay directory name to stop any existing relay thread
            if (_relay_thread)
            {
                _relay_thread->join();
                _relay_thread.reset();
            }
            {
                const std::unique_lock lock(_relay_dir_mutex);
                _relay_dir_name = relay_dir_name;
            }
            _last_active_time.store(std::chrono::steady_clock::now(), std::memory_order_release);
            _relay_thread = std::make_unique<std::thread>(&Relay::relayThreadTask, this);
            if (!_relay_thread)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create relay thread, resetting");
                stopRelay();
                return EFAULT;
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Unexpected exception on enabling nginx relay: ", e.what());
            return EFAULT;
        }

        return 0;
    }

    vg_sta_e disableNginxRelay() noexcept
    {
        const std::unique_lock api_lock(_mutex);
        disableRelayUnsafe();
        return 0;
    }

private:
    static inline const constexpr auto kRelayCheckInterval = std::chrono::milliseconds(100);
    static inline const constexpr auto kRelayTimeout = std::chrono::seconds(300);

private:
    inline bool isRelayEnabled() const noexcept
    {
        const std::shared_lock lock(_relay_dir_mutex);
        return !_relay_dir_name.empty();
    }

    std::string getRelayDirName() const noexcept
    {
        const std::shared_lock lock(_relay_dir_mutex);
        return _relay_dir_name;
    }

    void relayThreadTask()
    {
        do
        {
            const auto duration = std::chrono::steady_clock::now() - _last_active_time.load(std::memory_order_acquire);
            if (duration >= kRelayTimeout)
            {
                VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Relay timeout reached, stopping relay");
                stopRelay();
                return;
            }
            std::this_thread::sleep_for(kRelayCheckInterval);
        }
        while (isRelayEnabled());
    }

    void disableRelayUnsafe() noexcept
    {
        stopRelay();
        if (_relay_thread)
        {
            _relay_thread->join();
            _relay_thread.reset();
        }
    }

    void stopRelay() noexcept
    {
        const std::unique_lock lock(_relay_dir_mutex);
        if (_relay_dir_name.empty())
            return;
        const auto relay_path = _relay_dir_path / _relay_dir_name;
        std::error_code ec;
        std::filesystem::remove_all(relay_path, ec);
        if (ec)
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to remove relay directory ", relay_path, ": ", ec.message());
        _relay_dir_name.clear();
    }

    static std::string generateUUIDv4()
    {
        static const char *hex_chars = "0123456789abcdef";
        std::array<uint8_t, 16> uuid_bytes;

        static thread_local std::random_device rd;
        static thread_local std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        for (auto &byte: uuid_bytes)
            byte = static_cast<uint8_t>(dis(gen));

        // Set version and variant bits
        uuid_bytes[6] = (uuid_bytes[6] & 0x0F) | 0x40; // Version 4
        uuid_bytes[8] = (uuid_bytes[8] & 0x3F) | 0x80; // Variant 1
        // Convert to string
        std::string uuid_str;
        uuid_str.reserve(36);
        for (size_t i = 0; i < uuid_bytes.size(); ++i)
        {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                uuid_str += '-';
            uuid_str += hex_chars[(uuid_bytes[i] >> 4) & 0x0F];
            uuid_str += hex_chars[uuid_bytes[i] & 0x0F];
        }
        return uuid_str;
    }

private:
    Context &_ctx;

    const std::filesystem::path _target_dir_path;
    const std::filesystem::path _relay_dir_path;

    mutable std::mutex _mutex;

    mutable std::shared_mutex _relay_dir_mutex;
    std::string _relay_dir_name;
    std::unique_ptr<std::thread> _relay_thread;

    alignas(kHardwareDestructiveInterferenceSize) std::atomic<std::chrono::steady_clock::time_point> _last_active_time;
};

} // namespace vigil::relay

#endif
