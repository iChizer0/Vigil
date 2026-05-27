#pragma once
#ifndef VIGIL_CONFIGS_HPP
#define VIGIL_CONFIGS_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "context.hpp"
#include "json.hpp" // IWYU pragma: keep
#include "log.hpp"
#include "types.hpp"

namespace vigil::configs {

using namespace vigil::types;

struct InferenceRuleConfig
{
    std::string id;
    size_t debounce_times;
    Range<float> confidence_filter;
    std::unordered_set<std::string> class_filter;
    std::vector<ROI<float>> region_filter;

    InferenceRuleConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        j.at("sID").get_to(id);
        j.at("iDebounceTimes").get_to(debounce_times);
        {
            auto &conf_rg = j.at("lConfidenceFilter");
            const auto lb = conf_rg.at(0).get<float>();
            const auto ub = conf_rg.at(1).get<float>();
            if (lb > ub || lb < 0.0f || ub > 1.0f)
                throw Exception("Invalid confidence filter range [" + std::to_string(lb) + ", " + std::to_string(ub)
                                + "] while parsing inference rule");
            confidence_filter = Range<float>(lb, ub);
        }
        {
            auto &cls_set = j.at("lClassFilter");
            if (!cls_set.is_array())
                throw Exception("Class filter must be an array while parsing inference rule");
            class_filter.reserve(cls_set.size());
            for (const auto &cls_j: cls_set)
                class_filter.emplace(cls_j.get<std::string>());
        }
        if (j.contains("lRegionFilter"))
        {
            auto &roi_vec = j.at("lRegionFilter");
            region_filter.reserve(roi_vec.size());
            for (const auto &roi_j: roi_vec)
            {
                auto &polygon_j = roi_j.at("lPolygon");
                ROI<float> roi;
                roi.polygon.points.reserve(polygon_j.size());
                for (const auto &pt_j: polygon_j)
                {
                    roi.polygon.points.push_back(Point<float> {
                        .x = pt_j.at(0).get<float>(),
                        .y = pt_j.at(1).get<float>(),
                    });
                }
                if (roi.polygon.points.size() < 3)
                    throw Exception("ROI polygon must have at least 3 points");
                region_filter.push_back(std::move(roi));
            }
        }
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["sID"] = id;
        j["iDebounceTimes"] = debounce_times;
        j["lConfidenceFilter"] = { confidence_filter.lb(), confidence_filter.ub() };
        j["lClassFilter"] = nlohmann::json(class_filter);
        if (!region_filter.empty())
        {
            nlohmann::json roi_vec = nlohmann::json::array();
            for (const auto &roi: region_filter)
            {
                nlohmann::json roi_j;
                nlohmann::json polygon_j = nlohmann::json::array();
                for (const auto &pt: roi.polygon.points)
                {
                    polygon_j.push_back({ pt.x, pt.y });
                }
                roi_j["lPolygon"] = polygon_j;
                roi_vec.push_back(roi_j);
            }
            j["lRegionFilter"] = roi_vec;
        }
    }

    bool operator==(const InferenceRuleConfig &other) const noexcept
    {
        return id == other.id && debounce_times == other.debounce_times && confidence_filter == other.confidence_filter
               && class_filter == other.class_filter && region_filter == other.region_filter;
    }
};

struct InferenceRuleSetConfig
{
    std::vector<InferenceRuleConfig> rules;

    InferenceRuleSetConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        rules.reserve(j.size());
        for (const auto &rule_j: j)
        {
            rules.emplace_back(rule_j, ctx);
        }
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        nlohmann::json rules_vec = nlohmann::json::array();
        for (const auto &rule: rules)
        {
            nlohmann::json rule_j;
            rule.to(rule_j, ctx);
            rules_vec.push_back(rule_j);
        }
        j = rules_vec;
    }

    bool operator==(const InferenceRuleSetConfig &other) const noexcept
    {
        return rules == other.rules;
    }
};

struct TimerRuleConfig
{
    int interval_seconds;

    TimerRuleConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        j.at("iIntervalSeconds").get_to(interval_seconds);
        if (interval_seconds < 0)
            throw Exception(
                "Invalid interval seconds " + std::to_string(interval_seconds) + " while parsing timer rule");
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["iIntervalSeconds"] = interval_seconds;
    }

    bool operator==(const TimerRuleConfig &other) const noexcept
    {
        return interval_seconds == other.interval_seconds;
    }
};

struct GPIOPinConfig
{
    std::unique_ptr<vg_config_gpio_pin_t> pin;

    GPIOPinConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]]) : pin(nullptr)
    {
        if (ctx.c_api.gpios.available_cb == nullptr)
            throw Exception("C API GPIO available callback is null while parsing GPIO pin config");

        vg_config_gpio_pins_t available_pins;
        if (ctx.c_api.gpios.available_cb(&available_pins) != 0)
            throw Exception("Failed to get available GPIO pins from C API while parsing GPIO pin config");

        if (available_pins.pins && available_pins.len > 0)
        {
            if (j.contains("iNum"))
            {
                const auto pin_number = j.at("iNum").get<int>();
                for (size_t i = 0; i < available_pins.len; ++i)
                {
                    if (available_pins.pins[i].num == pin_number)
                    {
                        pin = std::make_unique<vg_config_gpio_pin_t>();
                        std::memcpy(pin.get(), &available_pins.pins[i], sizeof(vg_config_gpio_pin_t));
                        break;
                    }
                }
            }
            else if (j.contains("sName"))
            {
                const auto pin_name = j.at("sName").get<std::string>();
                for (size_t i = 0; i < available_pins.len; ++i)
                {
                    if (pin_name == available_pins.pins[i].name)
                    {
                        pin = std::make_unique<vg_config_gpio_pin_t>();
                        std::memcpy(pin.get(), &available_pins.pins[i], sizeof(vg_config_gpio_pin_t));
                        break;
                    }
                }
            }
        }

        if (available_pins.free_cb)
            available_pins.free_cb(available_pins.pins);
        else
            VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "No free_cb provided for available GPIO pins");

        if (!pin)
            throw Exception("Failed to find GPIO pin in C API while parsing GPIO pin config");
    }

    ~GPIOPinConfig() = default;
};

struct GPIORuleConfig
{
    std::string pin_name;
    int pin_number;
    vg_gpio_pin_mod_e state;
    vg_gpio_pin_lvl_e level;
    vg_gpio_pin_evt_e event;
    int debounce_durations_ms;

    GPIORuleConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        {
            const auto gpio_pin_cfg = GPIOPinConfig(j, ctx);
            if (!gpio_pin_cfg.pin)
                throw Exception("Failed to find GPIO in C API while parsing GPIO rule");
            pin_name = std::string(gpio_pin_cfg.pin->name);
            pin_number = gpio_pin_cfg.pin->num;
        }
        {
            const auto state_str = j.at("sState").get<std::string>();
            if (auto state_opt = GPIOHelpers::StringToState(state_str); state_opt.has_value())
                state = state_opt.value();
            else
                throw Exception("Invalid state string '" + state_str + "' while parsing GPIO rule");
            if (state == VG_GPIO_PIN_MODE_DISABLED)
                throw Exception("GPIO state cannot be DISABLED while parsing GPIO rule");
        }
        {
            const auto signal_str = j.at("sSignal").get<std::string>();
            if (auto level_opt = GPIOHelpers::StringToLevel(signal_str); level_opt.has_value())
            {
                level = level_opt.value();
                event = VG_GPIO_PIN_EVT_NONE;
            }
            else if (auto event_opt = GPIOHelpers::StringToEvent(signal_str); event_opt.has_value())
            {
                level = VG_GPIO_PIN_LVL_NONE;
                event = event_opt.value();
            }
            else
                throw Exception("Invalid signal string '" + signal_str + "' while parsing GPIO rule");
        }
        {
            j.at("iDebounceDurationMs").get_to(debounce_durations_ms);
            if (debounce_durations_ms < 0)
                throw Exception(
                    "Invalid debounce duration " + std::to_string(debounce_durations_ms) + " while parsing GPIO rule");
        }
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["sName"] = pin_name;
        {
            j["sState"] = GPIOHelpers::StateToString(state);
            j["sSignal"] = [&]() -> std::optional<std::string_view> {
                if (auto level_str = GPIOHelpers::LevelToString(level); level_str.has_value())
                    return level_str;
                else if (auto event_str = GPIOHelpers::EventToString(event); event_str.has_value())
                    return event_str;
                else
                    return std::nullopt;
            }();
        }
        j["iDebounceDurationMs"] = debounce_durations_ms;
    }

    bool operator==(const GPIORuleConfig &other) const noexcept
    {
        return pin_name == other.pin_name && pin_number == other.pin_number && state == other.state
               && level == other.level && event == other.event && debounce_durations_ms == other.debounce_durations_ms;
    }
};

struct TTYRuleConfig
{
    std::string name;
    std::string socket_path;
    std::string cmd;
    size_t buffer_size;

    TTYRuleConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        j.at("sName").get_to(name);
        {
            for (size_t i = 0; i < ctx.c_api.ttys.len; ++i)
            {
                if (ctx.c_api.ttys.ttys[i].name && name == ctx.c_api.ttys.ttys[i].name)
                {
                    socket_path = ctx.c_api.ttys.ttys[i].socket_path;
                    buffer_size = ctx.c_api.ttys.ttys[i].buffer_size;
                    break;
                }
            }
            if (socket_path.empty())
                throw Exception("Failed to find TTY with name '" + name + "' in C API while parsing TTY rule");
            if (buffer_size == 0)
                throw Exception("TTY '" + name + "' has zero buffer size in C API while parsing TTY rule");
        }
        {
            j.at("sCommand").get_to(cmd);
            if (cmd.empty())
                throw Exception("TTY rule for '" + name + "' has empty command string");
            if (cmd.length() > buffer_size)
                throw Exception("TTY rule command length exceeds buffer size for TTY '" + name + "'");
        }
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["sName"] = name;
        j["sCommand"] = cmd;
    }

    bool operator==(const TTYRuleConfig &other) const noexcept
    {
        return name == other.name && socket_path == other.socket_path && cmd == other.cmd
               && buffer_size == other.buffer_size;
    }
};

struct HTTPRuleConfig
{
    HTTPRuleConfig() = default;

    ~HTTPRuleConfig() = default;

    bool operator==(HTTPRuleConfig &) const noexcept
    {
        return true;
    }
};

struct AlwaysOnRuleConfig
{
    AlwaysOnRuleConfig() noexcept = default;

    ~AlwaysOnRuleConfig() noexcept = default;

    bool operator==(const AlwaysOnRuleConfig &) const noexcept
    {
        return true;
    }
};

struct ScheduleRuleConfig
{
    bool enabled;
    std::vector<TimeRange> active_weekdays;

    ScheduleRuleConfig() : enabled(false), active_weekdays()
    {
        // When enabled, default to a full week schedule
        active_weekdays.emplace_back(LocalTime(1, 0, 0, 0), LocalTime(0, 24, 0, 0));
    }

    ScheduleRuleConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        {
            enabled = j.at("bEnabled").get<bool>();
        }
        {
            auto &weekdays_vec = j.at("lActiveWeekdays");
            active_weekdays.reserve(weekdays_vec.size());
            for (const auto &tr_j: weekdays_vec)
            {
                const auto s_str = tr_j.at("sStart").get<std::string>();
                const auto e_str = tr_j.at("sEnd").get<std::string>();
                auto start = LocalTimeFromWeekday(s_str);
                auto end = LocalTimeFromWeekday(e_str);
                if (start == end)
                    throw Exception("Start time equals end time while parsing schedule rule");
                active_weekdays.emplace_back(start, end);
            }
        }
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        {
            j["bEnabled"] = enabled;
        }
        {
            nlohmann::json weekdays_vec = nlohmann::json::array();
            for (const auto &tr: active_weekdays)
            {
                nlohmann::json tr_j;
                tr_j["sStart"] = LocalTimeToWeekday(tr.start());
                tr_j["sEnd"] = LocalTimeToWeekday(tr.end());
                weekdays_vec.push_back(tr_j);
            }
            j["lActiveWeekdays"] = weekdays_vec;
        }
    }

    bool operator==(const ScheduleRuleConfig &other) const noexcept
    {
        return enabled == other.enabled && active_weekdays == other.active_weekdays;
    }

private:
    static inline LocalTime LocalTimeFromWeekday(std::string_view time_str)
    {
        // Parse weekday + ISO 8601 time string: "Day HH:MM:SS"
        char day_buf[8] = {};
        int h, m, s;
        if (std::sscanf(time_str.data(), "%7s %d:%d:%d", day_buf, &h, &m, &s) != 4)
            throw Exception("Invalid time format '" + std::string(time_str)
                            + "' while parsing schedule rule, expected 'Day HH:MM:SS'");

        int d = -1;
        std::string_view day_sv(day_buf);
        for (size_t i = 0; i < kTMDayNamesMap.size(); ++i)
        {
            if (day_sv == kTMDayNamesMap[i])
            {
                d = static_cast<int>(i);
                break;
            }
        }
        if (d < 0)
            throw Exception("Invalid day name '" + std::string(day_buf) + "' while parsing schedule rule");

        if (h < 0 || h > 24)
            throw Exception("Invalid hour " + std::to_string(h) + " while parsing schedule rule");
        if (m < 0 || m > 59)
            throw Exception("Invalid minute " + std::to_string(m) + " while parsing schedule rule");
        if (s < 0 || s > 59)
            throw Exception("Invalid second " + std::to_string(s) + " while parsing schedule rule");
        // 24:00:00 is valid (end of day), but 24:01:00 etc. is not
        if (h == 24 && (m != 0 || s != 0))
            throw Exception("Hour 24 is only valid with 00:00 (end-of-day) while parsing schedule rule");

        return LocalTime(d, h, m, s);
    }

    static inline std::string LocalTimeToWeekday(const LocalTime &lt)
    {
        // Format as "Day HH:MM:SS" string
        auto tm = lt.tm();
        const size_t day_idx
            = (tm.tm_wday >= 0 && tm.tm_wday < static_cast<int>(kTMDayNamesMap.size())) ? tm.tm_wday : 0;
        char buf[32] = { 0 };
        std::snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d", kTMDayNamesMap[day_idx], tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    }
};

struct RecordConfig
{
    bool enabled;
    size_t settings_rev;
    std::shared_ptr<FileWriterSettings> settings;

    RecordConfig(bool e, const std::shared_ptr<FileWriterSettings> &s) : enabled(e), settings(s)
    {
        if (!settings)
            throw Exception("FileWriterSettings is null while constructing RecordConfig");
    }

    RecordConfig(const nlohmann::json &j, const Context &ctx)
    {
        enabled = j.at("bRuleEnabled").get<bool>();

        {
            const auto settings_j = j.at("dWriterConfig");
            const auto itv_ms = settings_j.at("iIntervalMs").get<long>();
            if (itv_ms <= 0)
                throw Exception(
                    "Invalid recording interval " + std::to_string(itv_ms) + "ms while parsing RecordConfig");
            settings_rev = ctx.shared_writer_settings.get().first;
            const auto fmt = settings_j.at("sFormat").get<std::string>();
            if (settings = FileWriterSettings::create(fmt, std::chrono::milliseconds(itv_ms),
                    kRecordWriterPrefixDefault, kVideoClipLengthDefault,
                    WaterMarks::from(fmt,
                        CaptureTypeFrom(fmt) == CaptureType::Video ? kVideoClipLengthDefault.count() : 1,
                        &ctx.c_api.media));
                !settings)
                throw Exception("Failed to create FileWriterSettings while parsing RecordConfig");
        }
    }

    static RecordConfig from(const Context &ctx)
    {
        return RecordConfig(ctx.record_rule_enabled.load(std::memory_order_acquire),
            ctx.shared_writer_settings.get().second);
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["bRuleEnabled"] = enabled;
        if (auto settings_j = nlohmann::json::object(); settings)
        {
            settings_j["sFormat"] = settings->format;
            settings_j["iIntervalMs"] = static_cast<long>(settings->interval.count());
            j["dWriterConfig"] = settings_j;
        }
    }

    vg_sta_e sync(Context &ctx) const noexcept
    {
        ctx.record_rule_enabled.store(enabled, std::memory_order_release);
        if (ctx.shared_writer_settings.update(settings_rev, settings))
            return 0;
        else
            return EAGAIN;
    }
};

struct SlotConfig
{
    FileSize quota_limit_bytes;
    bool quota_rotate;

    SlotConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        {
            j.at("iQuotaLimitBytes").get_to(quota_limit_bytes);
            if (quota_limit_bytes < -1)
                throw Exception("Invalid quota limit bytes " + std::to_string(quota_limit_bytes));
        }
        j.at("bQuotaRotate").get_to(quota_rotate);
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["iQuotaLimitBytes"] = quota_limit_bytes;
        j["bQuotaRotate"] = quota_rotate;
    }

    bool operator==(const SlotConfig &other) const noexcept
    {
        return quota_limit_bytes == other.quota_limit_bytes && quota_rotate == other.quota_rotate;
    }
};

struct StorageConfig
{
    bool enabled;
    std::string target_enabled_slot_dev_path;
    std::string target_enabled_slot_uuid;

    StorageConfig() noexcept : enabled(false), target_enabled_slot_dev_path(), target_enabled_slot_uuid() { }

    StorageConfig(std::string &&dev_path, std::string &&uuid)
        : enabled(false), target_enabled_slot_dev_path(std::move(dev_path)), target_enabled_slot_uuid(std::move(uuid))
    {
        enabled = !target_enabled_slot_dev_path.empty() || !target_enabled_slot_uuid.empty();
    }

    StorageConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]]) : enabled(false)
    {
        auto selector_j = j.at("dSelectSlotToEnable");
        if (enabled = !selector_j.is_null(); enabled)
        {
            if (auto vj = selector_j.at("sByDevPath"); !vj.is_null())
                vj.get_to(target_enabled_slot_dev_path);
            if (auto vj = selector_j.at("sByUUID"); !vj.is_null())
                vj.get_to(target_enabled_slot_uuid);
            if (target_enabled_slot_dev_path.empty() && target_enabled_slot_uuid.empty())
                throw Exception("At least one of target enabled slot device path or UUID must be specified");
        }
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        if (enabled)
        {
            nlohmann::json selector_j;
            selector_j["sByDevPath"] = target_enabled_slot_dev_path;
            selector_j["sByUUID"] = target_enabled_slot_uuid;
            j["dSelectSlotToEnable"] = selector_j;
        }
        else
            j["dSelectSlotToEnable"] = nullptr;
    }

    bool operator==(const StorageConfig &other) const noexcept
    {
        return enabled == other.enabled && target_enabled_slot_dev_path == other.target_enabled_slot_dev_path
               && target_enabled_slot_uuid == other.target_enabled_slot_uuid;
    }
};

struct StorageControlConfig
{
    enum class TaskType : int32_t {
        Sync,
        AsyncSubmit,
        AsyncStatus,
        AsyncCancel,
    };

    enum class ActionType : int32_t {
        Unknown,
        ConfigSlot,
        RelaySlot,
        RelayStatusSlot,
        UnrelaySlot,
        FormatSlot,
        FreeUpSlot,
        EjectSlot,
        RemoveFilesOrDirectories,
    };

    TaskType task = TaskType::Sync;
    ActionType action = ActionType::Unknown;
    std::string slot_dev_path;
    std::optional<SlotConfig> slot_config;
    std::optional<std::vector<std::filesystem::path>> files_or_directories_to_remove;

    static constexpr std::string_view TaskTypeString(const TaskType task) noexcept
    {
        switch (task)
        {
            case TaskType::Sync:
                return "SYNC";
            case TaskType::AsyncSubmit:
                return "ASYNC_SUBMIT";
            case TaskType::AsyncStatus:
                return "ASYNC_STATUS";
            case TaskType::AsyncCancel:
                return "ASYNC_CANCEL";
            default:
                return "UNKNOWN";
        }
    }

    static constexpr std::optional<TaskType> TaskTypeFromString(const std::string_view task_str) noexcept
    {
        if (task_str == "SYNC")
            return TaskType::Sync;
        if (task_str == "ASYNC_SUBMIT")
            return TaskType::AsyncSubmit;
        if (task_str == "ASYNC_STATUS")
            return TaskType::AsyncStatus;
        if (task_str == "ASYNC_CANCEL")
            return TaskType::AsyncCancel;
        return std::nullopt;
    }

    static constexpr std::string_view ActionString(const ActionType action) noexcept
    {
        switch (action)
        {
            case ActionType::ConfigSlot:
                return "CONFIG";
            case ActionType::RelaySlot:
                return "RELAY";
            case ActionType::RelayStatusSlot:
                return "RELAY_STATUS";
            case ActionType::UnrelaySlot:
                return "UNRELAY";
            case ActionType::FormatSlot:
                return "FORMAT";
            case ActionType::FreeUpSlot:
                return "FREE_UP";
            case ActionType::EjectSlot:
                return "EJECT";
            case ActionType::RemoveFilesOrDirectories:
                return "REMOVE_FILES_OR_DIRECTORIES";
            default:
                return "UNKNOWN";
        }
    }

    static constexpr std::optional<ActionType> ActionTypeFromString(const std::string_view action_str) noexcept
    {
        if (action_str == "FORMAT")
            return ActionType::FormatSlot;
        if (action_str == "FREE_UP")
            return ActionType::FreeUpSlot;
        if (action_str == "EJECT")
            return ActionType::EjectSlot;
        if (action_str == "CONFIG")
            return ActionType::ConfigSlot;
        if (action_str == "RELAY")
            return ActionType::RelaySlot;
        if (action_str == "RELAY_STATUS")
            return ActionType::RelayStatusSlot;
        if (action_str == "UNRELAY")
            return ActionType::UnrelaySlot;
        if (action_str == "REMOVE_FILES_OR_DIRECTORIES")
            return ActionType::RemoveFilesOrDirectories;
        return std::nullopt;
    }

    static constexpr bool ActionSupportsAsync(const ActionType action) noexcept
    {
        switch (action)
        {
            case ActionType::FormatSlot:
            case ActionType::FreeUpSlot:
            case ActionType::EjectSlot:
            case ActionType::RemoveFilesOrDirectories:
                return true;
            default:
                return false;
        }
    }

    static constexpr bool ActionIsRunningCancelable(const ActionType action) noexcept
    {
        return action == ActionType::FreeUpSlot || action == ActionType::RemoveFilesOrDirectories;
    }

    constexpr bool isSyncTask() const noexcept
    {
        return task == TaskType::Sync;
    }

    constexpr bool isAsyncTask() const noexcept
    {
        return task != TaskType::Sync;
    }

    constexpr bool isAsyncSubmitTask() const noexcept
    {
        return task == TaskType::AsyncSubmit;
    }

    constexpr bool actionSupportsAsync() const noexcept
    {
        return ActionSupportsAsync(action);
    }

    constexpr bool requiresSlotConfigPayload() const noexcept
    {
        return isSyncTask() && action == ActionType::ConfigSlot;
    }

    constexpr bool requiresRemovePathsPayload() const noexcept
    {
        return action == ActionType::RemoveFilesOrDirectories && (isSyncTask() || isAsyncSubmitTask());
    }

    StorageControlConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        using namespace std::string_literals;

        {
            task = TaskType::Sync;
            if (j.contains("sTask"))
            {
                const auto task_str = j.at("sTask").get<std::string>();
                if (const auto parsed_task_type = TaskTypeFromString(task_str); parsed_task_type.has_value())
                    task = *parsed_task_type;
                else
                    throw Exception("Invalid storage control task string '"s + task_str + "'"s);
            }
        }

        {
            const auto action_str = j.at("sAction").get<std::string>();
            if (const auto parsed_action = ActionTypeFromString(action_str); parsed_action.has_value())
                action = *parsed_action;
            else
                throw Exception("Invalid storage action string '"s + action_str + "'"s);
        }

        if (isAsyncTask() && !actionSupportsAsync())
            throw Exception(
                "Storage control action '"s + std::string(ActionString(action)) + "' does not support async tasks");

        j.at("sSlotDevPath").get_to(slot_dev_path);

        {
            if (requiresSlotConfigPayload())
            {
                slot_config = SlotConfig(j.at("dSlotConfig"), ctx);
            }
            else if (requiresRemovePathsPayload())
            {
                auto &files_vec = j.at("lFilesOrDirectoriesToRemove");
                std::vector<std::filesystem::path> to_remove;
                to_remove.reserve(files_vec.size());
                for (const auto &file_j: files_vec)
                    to_remove.emplace_back(file_j.get<std::string>());
                files_or_directories_to_remove = std::move(to_remove);
            }
        }

        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Parsed storage control config: task=", TaskTypeString(task),
            ", action=", ActionString(action), ", slot_dev_path=", slot_dev_path);
    }
};

struct CaptureConfig
{
    std::filesystem::path output;
    std::shared_ptr<FileWriterSettings> settings;

    CaptureConfig(const nlohmann::json &j, const Context &ctx)
    {
        j.at("sOutput").get_to(output);
        {
            std::chrono::seconds video_length = std::chrono::seconds::max();
            if (j.contains("iVideoLengthSeconds"))
            {
                if (const auto l = j.at("iVideoLengthSeconds").get<int>(); l > 0)
                    video_length = std::chrono::seconds(l);
                else
                    throw Exception(
                        "Invalid video length seconds " + std::to_string(l) + " while parsing CaptureConfig");
            }
            const auto fmt = j.at("sFormat").get<std::string>();
            settings = FileWriterSettings::create(fmt, std::chrono::milliseconds(0), kCaptureWriterPrefixDefault,
                video_length,
                WaterMarks::from(fmt, CaptureTypeFrom(fmt) == CaptureType::Video ? kCaptureWaterMarkBatch : 1,
                    &ctx.c_api.media));
            if (!settings)
                throw Exception("Failed to create FileWriterSettings while parsing CaptureConfig");
        }
    }
};

} // namespace vigil::configs

#endif
