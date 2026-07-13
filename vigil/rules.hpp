#pragma once
#ifndef VIGIL_RULES_HPP
#define VIGIL_RULES_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "configs.hpp"
#include "constants.hpp"
#include "context.hpp"
#include "envelope.pb.h"
#include "log.hpp"
#include "types.hpp"

namespace vigil::rules {

using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::constants;

class BaseRule
{
public:
    BaseRule(vg_rule_type_e type, int precedence) noexcept : _type(type), _precedence(precedence) { }

    virtual ~BaseRule() = default;

    static inline const char *TypeString(vg_rule_type_e type) noexcept
    {
        switch (type)
        {
            case VG_RULE_TYPE_INFERENCE:
                return "INFERENCE";
            case VG_RULE_TYPE_INFERENCE_SET:
                return "INFERENCE_SET";
            case VG_RULE_TYPE_TIMER:
                return "TIMER";
            case VG_RULE_TYPE_GPIO:
                return "GPIO";
            case VG_RULE_TYPE_TTY:
                return "TTY";
            case VG_RULE_TYPE_SCHEDULE:
                return "SCHEDULE";
            case VG_RULE_TYPE_HTTP:
                return "HTTP";
            case VG_RULE_TYPE_ALWAYS_ON:
                return "ALWAYS_ON";
            case VG_RULE_TYPE_SED:
                return "SED";
            default:
                return "UNKNOWN";
        }
    }

    static vg_rule_type_e TypeFromString(const std::string &type_str) noexcept
    {
        if (type_str == "INFERENCE")
            return VG_RULE_TYPE_INFERENCE;
        else if (type_str == "INFERENCE_SET")
            return VG_RULE_TYPE_INFERENCE_SET;
        else if (type_str == "TIMER")
            return VG_RULE_TYPE_TIMER;
        else if (type_str == "GPIO")
            return VG_RULE_TYPE_GPIO;
        else if (type_str == "TTY")
            return VG_RULE_TYPE_TTY;
        else if (type_str == "SCHEDULE")
            return VG_RULE_TYPE_SCHEDULE;
        else if (type_str == "HTTP")
            return VG_RULE_TYPE_HTTP;
        else if (type_str == "ALWAYS_ON")
            return VG_RULE_TYPE_ALWAYS_ON;
        else if (type_str == "SED")
            return VG_RULE_TYPE_SED;
        else
            return VG_RULE_TYPE_UNKNOWN;
    }

    vg_rule_type_e type() const noexcept
    {
        return _type;
    }

    virtual std::string id() const noexcept
    {
        return {};
    }

    virtual bool operator()(const Context &ctx) const noexcept = 0;

    std::strong_ordering operator<=>(const BaseRule &other) const noexcept
    {
        if (_precedence < other._precedence)
            return std::strong_ordering::less;
        else if (_precedence > other._precedence)
            return std::strong_ordering::greater;
        else
            return std::strong_ordering::equal;
    }

private:
    vg_rule_type_e _type;
    const int _precedence;
};

class SoundEventRule final: public BaseRule
{
public:
    SoundEventRule(const SoundEventRuleConfig &cfg, const Context &ctx)
        : BaseRule(VG_RULE_TYPE_SED, 0), _id(cfg.id), _consecutive_window(cfg.consecutive_window),
          _conf_rg(cfg.confidence_filter), _cls_set(cfg.class_filter.begin(), cfg.class_filter.end()),
          _last_active(false), _stop_token(false)
    {
        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Initialized sound event rule: '", _id,
            "', consecutive_window: ", _consecutive_window.count(), "ms, confidence_filter: [", _conf_rg.lb(), ", ",
            _conf_rg.ub(), "], class_filter size: ", _cls_set.size());

        _poll_thread = std::thread(&SoundEventRule::pollThreadTask, this, std::cref(ctx));
    }

    ~SoundEventRule() noexcept
    {
        _stop_token.store(true, std::memory_order_release);
        try
        {
            if (_poll_thread.joinable())
                _poll_thread.join();
        }
        catch (...)
        {
        }
    }

    std::string id() const noexcept override
    {
        return _id;
    }

    bool operator()(const Context &ctx [[maybe_unused]]) const noexcept override
    {
        return _last_active.exchange(false, std::memory_order_acq_rel);
    }

private:
    enum class DecodeStatus { Ignored, Inactive, Active, Malformed };
    enum class WaitStatus { Ready, Timeout, Stopped, Error };

    static inline constexpr size_t kSoundEventMaxFrameBytes = 64 * 1024;
    static inline constexpr std::chrono::milliseconds kSoundEventPollInterval { 100 };
    static inline constexpr std::chrono::milliseconds kSoundEventConnectTimeout { 1000 };
    static inline constexpr std::chrono::milliseconds kSoundEventFrameReadTimeout { 5000 };
    static inline constexpr std::chrono::milliseconds kSoundEventInitialReconnectBackoff { 100 };
    static inline constexpr std::chrono::milliseconds kSoundEventMaxReconnectBackoff { 5000 };

    void pollThreadTask(const Context &ctx) const noexcept
    {
        auto backoff = kSoundEventInitialReconnectBackoff;
        while (!_stop_token.load(std::memory_order_acquire))
        {
            bool connection_healthy = false;
            try
            {
                const auto fd = openSocket(ctx);
                if (fd) [[likely]]
                {
                    VG_LOG(ctx.log, VG_LOG_LEVEL_INFO, "Connected to sound event UDS at '", kRuleSoundEventUDSPath,
                        "' with fd ", fd.get());
                    processConnection(ctx, fd, connection_healthy);
                }
            }
            catch (const std::exception &e)
            {
                VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Sound event UDS connection failed: ", e.what());
            }
            catch (...)
            {
                VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Sound event UDS connection failed with unknown error");
            }

            if (_stop_token.load(std::memory_order_acquire))
                break;

            if (connection_healthy)
                backoff = kSoundEventInitialReconnectBackoff;
            sleepOrStop(backoff);
            backoff = std::min(backoff * 2, kSoundEventMaxReconnectBackoff);
        }
    }

    AutoCloseFD openSocket(const Context &ctx) const noexcept
    {
        auto fd = AutoCloseFD(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (!fd) [[unlikely]]
        {
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create UDS for sound event rule: ", errno);
            return fd;
        }

        ::sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (std::strlen(kRuleSoundEventUDSPath) >= sizeof(addr.sun_path)) [[unlikely]]
        {
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Sound event UDS path too long: '", kRuleSoundEventUDSPath, "'");
            return {};
        }
        std::strncpy(addr.sun_path, kRuleSoundEventUDSPath, sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) [[likely]]
            return fd;

        {
            const auto err = errno;
            if (err != EINPROGRESS && err != EAGAIN && err != EWOULDBLOCK)
            {
                if (err == ENOENT || err == ECONNREFUSED)
                    VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Sound event UDS at '", kRuleSoundEventUDSPath,
                        "' is not ready: ", err);
                else
                    VG_LOG(ctx.log, VG_LOG_LEVEL_WARN, "Failed to connect sound event UDS at '", kRuleSoundEventUDSPath,
                        "': ", err);
                return {};
            }

            if (waitForFd(fd, POLLOUT, kSoundEventConnectTimeout, ctx) != WaitStatus::Ready)
            {
                if (!_stop_token.load(std::memory_order_acquire))
                    VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Timed out connecting sound event UDS at '",
                        kRuleSoundEventUDSPath, "'");
                return {};
            }
        }

        {
            int so_error = 0;
            ::socklen_t so_error_len = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0 || so_error != 0)
            {
                const auto final_err = so_error != 0 ? so_error : errno;
                if (final_err == ENOENT || final_err == ECONNREFUSED)
                    VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Sound event UDS at '", kRuleSoundEventUDSPath,
                        "' is not ready: ", strerror(final_err));
                else
                    VG_LOG(ctx.log, VG_LOG_LEVEL_WARN, "Failed to connect sound event UDS at '", kRuleSoundEventUDSPath,
                        "': ", strerror(final_err));
                return {};
            }
        }

        return fd;
    }

    void processConnection(const Context &ctx, const AutoCloseFD &fd, bool &connection_healthy) const
    {
        std::array<uint8_t, 4> len_buf {};
        std::vector<uint8_t> frame;
        std::optional<std::chrono::steady_clock::time_point> active_since;

        while (!_stop_token.load(std::memory_order_acquire))
        {
            if (!readExact(fd, len_buf.data(), len_buf.size(), ctx, kSoundEventFrameReadTimeout, false,
                    "sound event frame prefix"))
                break;

            const auto frame_len = readLe32(len_buf.data());
            if (frame_len == 0 || frame_len > kSoundEventMaxFrameBytes)
            {
                VG_LOG(ctx.log, VG_LOG_LEVEL_WARN, "Invalid sound event UDS frame length ", frame_len, " from '",
                    kRuleSoundEventUDSPath, "', closing connection");
                break;
            }

            frame.resize(frame_len);
            if (!readExact(fd, frame.data(), frame.size(), ctx, kSoundEventFrameReadTimeout, true,
                    "sound event frame payload"))
                break;

            const auto status = filterEnvelope(frame.data(), frame.size());
            if (status == DecodeStatus::Malformed)
            {
                VG_LOG(ctx.log, VG_LOG_LEVEL_WARN, "Malformed sound event protobuf frame from '",
                    kRuleSoundEventUDSPath, "', closing connection");
                break;
            }

            connection_healthy = true;
            if (status == DecodeStatus::Ignored)
                continue;

            if (finalActive(status == DecodeStatus::Active, active_since, std::chrono::steady_clock::now()))
                _last_active.store(true, std::memory_order_release);
        }
    }

    bool finalActive(bool active, std::optional<std::chrono::steady_clock::time_point> &active_since,
        std::chrono::steady_clock::time_point now) const noexcept
    {
        if (!active)
        {
            active_since.reset();
            return false;
        }
        if (!active_since)
            active_since = now;
        return now - *active_since >= _consecutive_window;
    }

    bool readExact(const AutoCloseFD &fd, uint8_t *dst, size_t len, const Context &ctx,
        std::chrono::milliseconds timeout, bool start_timeout_immediately, std::string_view what) const noexcept
    {
        size_t offset = 0;
        std::optional<std::chrono::steady_clock::time_point> deadline;

        if (start_timeout_immediately)
            deadline = std::chrono::steady_clock::now() + timeout;

        while (offset < len && !_stop_token.load(std::memory_order_acquire))
        {
            const ssize_t n = ::recv(fd.get(), dst + offset, len - offset, 0);
            if (n > 0)
            {
                offset += static_cast<size_t>(n);
                if (!deadline && offset > 0)
                    deadline = std::chrono::steady_clock::now() + timeout;
                continue;
            }
            else if (n == 0)
            {
                if (!_stop_token.load(std::memory_order_acquire))
                    VG_LOG(ctx.log, VG_LOG_LEVEL_INFO, "Sound event UDS at '", kRuleSoundEventUDSPath,
                        "' closed by peer while reading ", what);
                return false;
            }
            if (const auto err = errno; err == EINTR)
                continue;
            else if (err != EAGAIN && err != EWOULDBLOCK)
            {
                if (!_stop_token.load(std::memory_order_acquire))
                    VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Failed to read ", what, " from sound event UDS at '",
                        kRuleSoundEventUDSPath, "': ", err);
                return false;
            }

            auto wait_time = kSoundEventPollInterval;
            if (deadline)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= *deadline)
                {
                    VG_LOG(ctx.log, VG_LOG_LEVEL_WARN, "Timed out reading ", what, " from sound event UDS at '",
                        kRuleSoundEventUDSPath, "'");
                    return false;
                }
                wait_time = std::min(wait_time, std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now));
                if (wait_time.count() <= 0)
                    wait_time = std::chrono::milliseconds(1);
            }

            if (const auto wait_status = waitForFd(fd, POLLIN, wait_time, ctx); wait_status == WaitStatus::Timeout)
                continue;
            else if (wait_status != WaitStatus::Ready)
                return false;
        }

        return offset == len;
    }

    WaitStatus waitForFd(const AutoCloseFD &fd, short events, std::chrono::milliseconds timeout,
        const Context &ctx) const noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!_stop_token.load(std::memory_order_acquire))
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return WaitStatus::Timeout;

            auto wait_time = std::min(kSoundEventPollInterval,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            if (wait_time.count() <= 0)
                wait_time = std::chrono::milliseconds(1);

            ::pollfd pfd { .fd = fd.get(), .events = events, .revents = 0 };
            const int ret = ::poll(&pfd, 1,
                static_cast<int>(std::min<int64_t>(wait_time.count(), std::numeric_limits<int>::max())));
            if (ret > 0)
            {
                if (pfd.revents & POLLNVAL)
                {
                    VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Invalid sound event UDS fd ", fd.get());
                    return WaitStatus::Error;
                }
                if (pfd.revents & (events | POLLERR | POLLHUP))
                    return WaitStatus::Ready;
                continue;
            }
            else if (ret == 0)
                continue;
            if (errno == EINTR)
                continue;
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Poll failed for sound event UDS at '", kRuleSoundEventUDSPath,
                "': ", errno);
            return WaitStatus::Error;
        }
        return WaitStatus::Stopped;
    }

    bool sleepOrStop(std::chrono::milliseconds duration) const noexcept
    {
        while (duration.count() > 0 && !_stop_token.load(std::memory_order_acquire))
        {
            const auto step = std::min(duration, kSoundEventPollInterval);
            std::this_thread::sleep_for(step);
            duration -= step;
        }
        return _stop_token.load(std::memory_order_acquire);
    }

    DecodeStatus filterEnvelope(const uint8_t *data, size_t size) const noexcept
    {
        try
        {
            acoustics::Envelope envelope;
            if (size > static_cast<size_t>(std::numeric_limits<int>::max())
                || !envelope.ParseFromArray(data, static_cast<int>(size)))
                return DecodeStatus::Malformed;
            if (!envelope.has_inference())
                return DecodeStatus::Ignored;

            const auto &inference = envelope.inference();
            if (!_id.empty() && (!inference.has_head_id() || inference.head_id() != _id))
                return DecodeStatus::Inactive;

            for (const auto &top_k: inference.top_k())
            {
                if (!std::isfinite(top_k.prob()) || !(_conf_rg & top_k.prob()))
                    continue;
                if (!_cls_set.contains(top_k.label()))
                    continue;

                return DecodeStatus::Active;
            }
            return DecodeStatus::Inactive;
        }
        catch (...)
        {
            return DecodeStatus::Malformed;
        }
    }

    static inline uint32_t readLe32(const uint8_t *ptr) noexcept
    {
        return static_cast<uint32_t>(ptr[0]) | (static_cast<uint32_t>(ptr[1]) << 8)
               | (static_cast<uint32_t>(ptr[2]) << 16) | (static_cast<uint32_t>(ptr[3]) << 24);
    }

private:
    std::string _id;
    std::chrono::milliseconds _consecutive_window;
    Range<float> _conf_rg;
    std::unordered_set<std::string> _cls_set;

    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<bool> _last_active;
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<bool> _stop_token;
    mutable std::thread _poll_thread;
};

class InferenceRule final: public BaseRule
{
public:
    InferenceRule(const InferenceRuleConfig &cfg, const Context &ctx) noexcept
        : BaseRule(VG_RULE_TYPE_INFERENCE, 0), _sub_ref(ctx.inference_subscribers), _id(cfg.id), _active_times(0),
          _active_times_min(cfg.debounce_times), _conf_rg(cfg.confidence_filter),
          _cls_set(cfg.class_filter.begin(), cfg.class_filter.end())
    {
        updateMaskRegion(cfg.region_filter);

        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Initialized inference rule: '", _id,
            "' with active_times_min: ", _active_times_min, ", confidence_filter: [", _conf_rg.lb(), ", ",
            _conf_rg.ub(), "], class_filter size: ", _cls_set.size(),
            ", region_filter size: ", cfg.region_filter.size(), ", mask initialized: ", (_msk_rng ? "yes" : "no"));
        if constexpr (VG_LOG_LEVEL == vg_log_level_e::VG_LOG_LEVEL_DEBUG)
        {
            VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Inference rule '", _id,
                "' region mask: ", _msk_rng ? _msk_rng->encoded() : std::string("none"));
        }

        _sub_ref.fetch_add(1, std::memory_order_relaxed);
    }

    ~InferenceRule() noexcept
    {
        _sub_ref.fetch_sub(1, std::memory_order_release);
    }

    std::string id() const noexcept override
    {
        return _id;
    }

    bool operator()(const Context &ctx) const noexcept override
    {
        return finalActive(filterDetection(ctx) || filterClassification(ctx));
    }

private:
    bool finalActive(bool active) const noexcept
    {
        if (active)
            _active_times += _active_times < _active_times_min;
        else if (_active_times > 0)
            --_active_times;
        return _active_times >= _active_times_min;
    }

    void updateMaskRegion(const std::vector<ROI<float>> &region_filter) noexcept
    {
        if (region_filter.empty())
        {
            _msk_rng.reset();
            return;
        }

        auto p = Mask<kMaskWidth, kMaskHeight>::from(false);
        if (!p)
            return;

        for (const auto &roi: region_filter)
        {
            auto msk = Mask<kMaskWidth, kMaskHeight>::from(roi.polygon);
            if (msk)
                *p |= *msk;
        }

        _msk_rng = std::move(p);
    }

    bool filterDetection(const Context &ctx) const noexcept
    {
        static_assert(std::is_unsigned_v<decltype(ctx.shared_inference_result.detections)::value_type::value_type>,
            "Value type of detection result should be unsigned.");

        bool active = false;
        for (const auto &det: ctx.shared_inference_result.detections)
        {
            if (!(_conf_rg & det.confidence))
                continue;
            if (!_cls_set.empty() && !_cls_set.contains(det.class_name))
                continue;
            if (_msk_rng
                && !_msk_rng->at(static_cast<size_t>((det.object.top_left.x + det.object.bottom_right.x) >> 1),
                    static_cast<size_t>((det.object.top_left.y + det.object.bottom_right.y) >> 1)))
                continue;

            active = true;
            break;
        }

        return active;
    }

    bool filterClassification(const Context &ctx) const noexcept
    {
        bool active = false;
        for (const auto &cls: ctx.shared_inference_result.classifications)
        {
            if (!(_conf_rg & cls.confidence))
                continue;
            if (!_cls_set.empty() && std::find(_cls_set.begin(), _cls_set.end(), cls.class_name) == _cls_set.end())
                continue;

            active = true;
            break;
        }

        return active;
    }

private:
    std::atomic<int> &_sub_ref;

    std::string _id;
    mutable int _active_times;
    int _active_times_min;

    Range<float> _conf_rg;
    std::unordered_set<std::string> _cls_set;
    std::unique_ptr<Mask<kMaskWidth, kMaskHeight>> _msk_rng;
};

class InferenceRuleSet final: public BaseRule
{
public:
    InferenceRuleSet(const InferenceRuleSetConfig &cfg, const Context &ctx) noexcept
        : BaseRule(VG_RULE_TYPE_INFERENCE_SET, 0)
    {
        for (const auto &rule_cfg: cfg.rules)
        {
            _rules.emplace_back(std::make_unique<InferenceRule>(rule_cfg, ctx));
        }

        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Initialized inference rule set with ", _rules.size(), " rules");
    }

    ~InferenceRuleSet() = default;

    std::string id() const noexcept override
    {
        return _last_activated_id;
    }

    bool operator()(const Context &ctx) const noexcept override
    {
        _last_activated_id.clear();
        for (const auto &rule: _rules)
        {
            if (rule && (*rule)(ctx))
            {
                _last_activated_id = rule->id();
                return true;
            }
        }
        return false;
    }

private:
    std::unordered_set<std::string> _activated_rule_ids;
    std::vector<std::unique_ptr<InferenceRule>> _rules;
    mutable std::string _last_activated_id;
};

class TimerRule final: public BaseRule
{
public:
    TimerRule(const TimerRuleConfig &cfg, const Context &ctx) noexcept
        : BaseRule(VG_RULE_TYPE_TIMER, 20), _interval(std::chrono::seconds(cfg.interval_seconds)),
          _last_trigger_time(std::chrono::steady_clock::now() - _interval)
    {
        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Initialized timer rule with interval: ", cfg.interval_seconds);
    }

    ~TimerRule() = default;

    bool operator()(const Context &ctx) const noexcept override
    {
        const auto now = ctx.shared_time.steadyTime();
        if (now - _last_trigger_time >= _interval)
        {
            _last_trigger_time = now;
            return true;
        }
        return false;
    }

private:
    std::chrono::seconds _interval;
    mutable std::chrono::steady_clock::time_point _last_trigger_time;
};

class GPIORule final: public BaseRule
{
public:
    GPIORule(const GPIORuleConfig &cfg, const Context &ctx)
        : BaseRule(VG_RULE_TYPE_GPIO, 10), _pin_number(cfg.pin_number), _state(cfg.state), _level(cfg.level),
          _event(cfg.event), _acquire_cb(ctx.c_api.gpios.acquire_cb), _release_cb(ctx.c_api.gpios.release_cb)
    {
        if (!_acquire_cb)
            throw Exception("No GPIO acquire callback provided in C API while initializing GPIO rule");
        if (!_release_cb)
            throw Exception("No GPIO release callback provided in C API while initializing GPIO rule");
        if (!ctx.c_api.gpios.sync_level_cb && !ctx.c_api.gpios.sync_event_cb)
            throw Exception("No GPIO sync level or last event callback provided in C API while initializing GPIO rule");

        if (auto ret = _acquire_cb(_pin_number, _state, static_cast<size_t>(cfg.debounce_durations_ms)); ret != 0)
            throw Exception("Failed to acquire GPIO pin number " + std::to_string(_pin_number)
                            + " while initializing GPIO rule, error code: " + std::to_string(ret));

        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Initialized GPIO rule for pin number: ", _pin_number);
    }

    ~GPIORule() noexcept
    {
        _release_cb(_pin_number);
    }

    bool operator()(const Context &ctx) const noexcept override
    {
        if (_event != VG_GPIO_PIN_EVT_NONE)
            return ctx.c_api.gpios.sync_event_cb(_pin_number) == _event;
        else if (_level != VG_GPIO_PIN_LVL_NONE)
            return ctx.c_api.gpios.sync_level_cb(_pin_number) == _level;
        return false;
    }

private:
    int _pin_number;
    vg_gpio_pin_mod_e _state;
    vg_gpio_pin_lvl_e _level;
    vg_gpio_pin_evt_e _event;

    vg_sta_e (*_acquire_cb)(int pin, vg_gpio_pin_mod_e mode, size_t debounce_ms);
    void (*_release_cb)(int pin);
};

class TTYRule final: public BaseRule
{
public:
    TTYRule(const TTYRuleConfig &cfg, const Context &ctx)
        : BaseRule(VG_RULE_TYPE_TTY, 0), _socket_path(cfg.socket_path), _cmd(cfg.cmd), _cmd_pos(0),
          _buffer_size(cfg.buffer_size), _buffer(new char[_buffer_size]), _fd(-1), _found(0)
    {
        if (_cmd.empty())
            throw Exception("TTY rule command cannot be empty while initializing TTY rule");

        openSocket(ctx);

        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Initialized TTY rule with command: '", cfg.cmd, "', fd: ", _fd,
            ", buffer_size: ", cfg.buffer_size);
    }

    ~TTYRule()
    {
        closeSocket();
    }

    bool operator()(const Context &ctx) const noexcept override
    {
        if (!isSocketOpen())
            openSocket(ctx);
        while (isSocketOpen())
        {
            const ssize_t n = ::recv(_fd, _buffer, _buffer_size, 0);
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break; // No more data available
                VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Failed to read from Unix socket at '", _socket_path,
                    "': ", strerror(errno));
                closeSocket();
                break;
            }
            if (n == 0)
            {
                VG_LOG(ctx.log, VG_LOG_LEVEL_INFO, "Unix socket at '", _socket_path, "' closed by peer");
                closeSocket();
                break;
            }

            VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Read ", n, " bytes from Unix socket at '", _socket_path, "'");

            for (ssize_t i = 0; i < n; ++i)
            {
                if (_buffer[i] == _cmd[_cmd_pos])
                {
                    ++_cmd_pos;
                    if (_cmd_pos == _cmd.size())
                    {
                        ++_found;
                        _cmd_pos = 0;
                    }
                }
                else if (_cmd_pos > 0)
                    _cmd_pos = (_buffer[i] == _cmd[0]) ? 1 : 0;
            }
        }

        const bool found = _found > 0;
        _found -= found;
        return found;
    }

private:
    inline bool isSocketOpen() const noexcept
    {
        return _fd >= 0;
    }

    void openSocket(const Context &ctx) const noexcept
    {
        _fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
        {
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create Unix socket for TTY '", _socket_path,
                "': ", strerror(errno));
            return;
        }

        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (_socket_path.size() >= sizeof(addr.sun_path))
        {
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Unix socket path too long for TTY: '", _socket_path, "'");
            closeSocket();
            return;
        }
        std::strncpy(addr.sun_path, _socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            const auto err = errno;
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Failed to connect Unix socket at '", _socket_path,
                "': ", strerror(err));
            closeSocket();
            return;
        }

        auto flags = fcntl(_fd, F_GETFL, 0);
        auto ret = fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
        if (ret < 0)
        {
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Failed to set non-blocking flags for Unix socket at '", _socket_path,
                "': ", strerror(errno), ", code: ", ret);
            closeSocket();
            return;
        }

        VG_LOG(ctx.log, VG_LOG_LEVEL_INFO, "Connected to Unix socket at '", _socket_path, "' with fd ", _fd,
            ", flags set to:", flags);

        _cmd_pos = 0;
        std::memset(_buffer, 0, _buffer_size);
        _found = 0;
    }

    void closeSocket() const noexcept
    {
        if (_fd >= 0)
        {
            ::close(_fd);
            _fd = -1;
        }
    }

private:
    const std::string _socket_path;
    const std::string _cmd;
    mutable size_t _cmd_pos;
    const size_t _buffer_size;
    mutable char *_buffer;
    mutable int _fd;
    mutable int _found;
};

class HTTPRule: public BaseRule
{
public:
    HTTPRule(const HTTPRuleConfig &, const Context &ctx) noexcept : BaseRule(VG_RULE_TYPE_HTTP, 0), _ctx(ctx) { }

    ~HTTPRule() = default;

    vg_sta_e tryActivate() noexcept
    {
        return _ctx.tryUpdateRuleEvent(type(), id()) ? 0 : EBUSY;
    }

private:
    bool operator()(const Context &ctx [[maybe_unused]]) const noexcept override
    {
        return false;
    }

private:
    const Context &_ctx;
};

class AlwaysOnRule: public BaseRule
{
public:
    AlwaysOnRule(const AlwaysOnRuleConfig &, const Context &ctx) noexcept
        : BaseRule(VG_RULE_TYPE_ALWAYS_ON, 0), _last_trigger_time(ctx.shared_time.steadyTime() - currentInterval(ctx))
    {
        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Initialized always-on rule with interval: ",
            std::chrono::duration_cast<std::chrono::milliseconds>(currentInterval(ctx)).count(), "ms");
    }

    ~AlwaysOnRule() = default;

    bool operator()(const Context &ctx) const noexcept override
    {
        if (ctx.media_paused.load(std::memory_order_relaxed)
            || ctx.media_writers_active.load(std::memory_order_relaxed) > 0)
            return false;

        const auto now = ctx.shared_time.steadyTime();
        if (const auto interval = currentInterval(ctx); now - _last_trigger_time >= interval)
        {
            if (ctx.shared_rule_event.getCurrent())
                return false;

            _last_trigger_time = now;
            return true;
        }
        return false;
    }

private:
    static std::chrono::steady_clock::duration currentInterval(const Context &ctx) noexcept
    {
        const auto [_, settings] = ctx.shared_writer_settings.get();
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            settings ? settings->interval : kRecordWriterDefaultIntervalMs);
    }

    mutable std::chrono::steady_clock::time_point _last_trigger_time;
};

class ScheduleRule: public BaseRule
{
public:
    ScheduleRule(const ScheduleRuleConfig &cfg, const Context &ctx) noexcept
        : BaseRule(VG_RULE_TYPE_SCHEDULE, 30), _enabled(cfg.enabled), _active_weekdays(cfg.active_weekdays)
    {
        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG,
            "Initialized schedule rule with active weekdays: ", cfg.active_weekdays.size());
    }

    ~ScheduleRule() = default;

    inline bool isActive(const LocalTime &t) const noexcept
    {
        if (!_enabled) // Treat disabled rule as active
            return true;
        for (const auto &tr: _active_weekdays)
        {
            if (tr & t)
                return true;
        }
        return false;
    }

private:
    bool operator()(const Context &ctx) const noexcept override
    {
        return isActive(ctx.shared_time.localTime());
    }

private:
    bool _enabled;
    std::vector<TimeRange> _active_weekdays;
};

} // namespace vigil::rules

#endif
