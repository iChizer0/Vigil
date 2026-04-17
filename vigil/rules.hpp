#pragma once
#ifndef VIGIL_RULES_HPP
#define VIGIL_RULES_HPP

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "configs.hpp"
#include "constants.hpp"
#include "context.hpp"
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
            if (!_cls_set.empty() && std::find(_cls_set.begin(), _cls_set.end(), det.class_name) == _cls_set.end())
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
    std::vector<std::string> _cls_set;
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
        _fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
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
        const auto now = ctx.shared_time.steadyTime();
        if (const auto interval = currentInterval(ctx); now - _last_trigger_time >= interval)
        {
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
