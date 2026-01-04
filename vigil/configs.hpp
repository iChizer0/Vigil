#pragma once
#ifndef VIGIL_CONFIGS_HPP
#define VIGIL_CONFIGS_HPP

#include <chrono>
#include <new>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "context.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil::configs {

using namespace vigil::types;

struct InferenceRuleConfig
{
    std::string id;
    size_t debounce_times;
    Range<float> confidence_filter;
    std::unordered_set<int> class_filter;
    std::vector<ROI<float>> region_filter;

    InferenceRuleConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        j.at("sID").get_to(id);
        j.at("iDebounceTimes").get_to(debounce_times);
        {
            auto &conf_rg = j.at("lConfidenceFilter");
            confidence_filter = Range<float>(conf_rg.at(0).get<float>(), conf_rg.at(1).get<float>());
        }
        {
            auto &cls_set = j.at("lClassFilter");
            class_filter = cls_set.get<std::unordered_set<int>>();
        }
        if (j.contains("lRegionFilter"))
        {
            auto &roi_vec = j.at("lRegionFilter");
            for (const auto &roi_j: roi_vec)
            {
                auto &polygon_j = roi_j.at("lPolygon");
                ROI<float> roi;
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
    const vg_config_gpio_pin_t *pin;

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
                        pin = new (std::nothrow) vg_config_gpio_pin_t { available_pins.pins[i] };
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
                        pin = new (std::nothrow) vg_config_gpio_pin_t { available_pins.pins[i] };
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

    ~GPIOPinConfig() noexcept
    {
        if (pin)
        {
            delete pin;
            pin = nullptr;
        }
    }
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
                throw Exception("Failed to find GPIO with name '" + pin_name + "' in C API while parsing GPIO rule");
            pin_number = gpio_pin_cfg.pin->num;
        }
        {
            const auto state_str = j.at("sState").get<std::string>();
            if (auto state_opt = GPIOHelpers::StringToState(state_str); state_opt.has_value())
                state = state_opt.value();
            else
                throw Exception("Invalid state string '" + state_str + "' while parsing GPIO rule");
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
};

struct ScheduleRuleConfig
{
    bool enabled;
    std::vector<TimeRange> active_weekdays;

    ScheduleRuleConfig() : active_weekdays() { }

    ScheduleRuleConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        {
            enabled = j.at("bEnabled").get<bool>();
        }
        {
            auto &weekdays_vec = j.at("lActiveWeekdays");
            auto getWeekdayLocalTime = [&](const nlohmann::json &j, std::string_view k) -> LocalTime {
                int d, h, m, s;
                auto &rg_j = j.at(k);

                rg_j.at("iDay").get_to(d);
                if (d < 0 || d > 6)
                    throw Exception("Invalid day of week " + std::to_string(d) + " while parsing schedule rule");
                rg_j.at("iHour").get_to(h);
                if (h < 0 || h > 23)
                    throw Exception("Invalid hour " + std::to_string(h) + " while parsing schedule rule");
                rg_j.at("iMinute").get_to(m);
                if (m < 0 || m > 59)
                    throw Exception("Invalid minute " + std::to_string(m) + " while parsing schedule rule");
                rg_j.at("iSecond").get_to(s);
                if (s < 0 || s > 59)
                    throw Exception("Invalid second " + std::to_string(s) + " while parsing schedule rule");

                return LocalTime(d, h, m, s);
            };
            for (const auto &tr_j: weekdays_vec)
                active_weekdays.emplace_back(getWeekdayLocalTime(tr_j, "dStart"), getWeekdayLocalTime(tr_j, "dEnd"));
        }
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        {
            j["bEnabled"] = enabled;
        }
        {
            nlohmann::json weekdays_vec = nlohmann::json::array();
            auto reflectLocalTime = [&](const LocalTime &lt) -> nlohmann::json {
                auto tm = lt.tm();
                nlohmann::json lt_j;
                lt_j["iDay"] = tm.tm_wday;
                lt_j["iHour"] = tm.tm_hour;
                lt_j["iMinute"] = tm.tm_min;
                lt_j["iSecond"] = tm.tm_sec;
                return lt_j;
            };
            for (const auto &tr: active_weekdays)
            {
                nlohmann::json tr_j;
                tr_j["dStart"] = reflectLocalTime(tr.start());
                tr_j["dEnd"] = reflectLocalTime(tr.end());
                weekdays_vec.push_back(tr_j);
            }
            j["lActiveWeekdays"] = weekdays_vec;
        }
    }

    bool operator==(const ScheduleRuleConfig &other) const noexcept
    {
        return enabled == other.enabled && active_weekdays == other.active_weekdays;
    }
};

struct FileWriterConfig
{
    FileWriterSettings settings;

    FileWriterConfig(const FileWriterSettings &s) : settings(s) { }

    FileWriterConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        settings = FileWriterSettings(j.at("sFormat").get<std::string>(),
            std::chrono::milliseconds(j.at("iIntervalMs").get<int>()));
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["sFormat"] = settings.format;
        j["iIntervalMs"] = settings.interval.count();
    }

    bool operator==(const FileWriterConfig &other) const noexcept
    {
        return settings == other.settings;
    }

    void sync(Context &ctx) const
    {
        std::unique_lock lock(ctx.writer_settings_mutex);
        if (settings != ctx.writer_settings)
        {
            ctx.writer_settings = settings;
            ctx.writer_settings_revision.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static FileWriterConfig fromContext(const Context &ctx)
    {
        std::shared_lock lock(ctx.writer_settings_mutex);
        return FileWriterConfig(ctx.writer_settings);
    }
};

struct SlotConfig
{
    FileSize quota_limit_bytes;
    bool quota_rotate;

    SlotConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        j.at("iQuotaLimitBytes").get_to(quota_limit_bytes);
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
    std::string target_enabled_slot_dev_path;

    StorageConfig() noexcept : target_enabled_slot_dev_path() { }

    StorageConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        j.at("sTargetEnabledSlotDevPath").get_to(target_enabled_slot_dev_path);
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["sTargetEnabledSlotDevPath"] = target_enabled_slot_dev_path;
    }

    bool operator==(const StorageConfig &other) const noexcept
    {
        return target_enabled_slot_dev_path == other.target_enabled_slot_dev_path;
    }
};

struct StorageControlConfig
{
    enum ActionType {
        FormatSlot,
        FreeUpSlot,
        EjectSlot,
        ConfigSlot,
        RelaySlot,
        RelayStatusSlot,
        UnrelaySlot,
        RemoveFilesOrDirectories,
    };
    ActionType action;
    std::string slot_dev_path;
    std::optional<SlotConfig> slot_config;
    std::optional<std::vector<std::filesystem::path>> files_or_directories_to_remove;

    StorageControlConfig(const nlohmann::json &j, const Context &ctx [[maybe_unused]])
    {
        using namespace std::string_literals;
        if (auto action_str = j.at("sAction").get<std::string>(); action_str == "FORMAT")
            action = ActionType::FormatSlot;
        else if (action_str == "FREE_UP")
            action = ActionType::FreeUpSlot;
        else if (action_str == "EJECT")
            action = ActionType::EjectSlot;
        else if (action_str == "CONFIG")
            action = ActionType::ConfigSlot;
        else if (action_str == "RELAY")
            action = ActionType::RelaySlot;
        else if (action_str == "RELAY_STATUS")
            action = ActionType::RelayStatusSlot;
        else if (action_str == "UNRELAY")
            action = ActionType::UnrelaySlot;
        else if (action_str == "REMOVE_FILES_OR_DIRECTORIES")
            action = ActionType::RemoveFilesOrDirectories;
        else
            throw Exception("Invalid storage action string '"s + action_str + "'"s);
        j.at("sSlotDevPath").get_to(slot_dev_path);
        if (action == ActionType::ConfigSlot)
            slot_config = SlotConfig(j.at("dSlotConfig"), ctx);
        else if (action == ActionType::RemoveFilesOrDirectories)
        {
            auto &files_vec = j.at("lFilesOrDirectoriesToRemove");
            std::vector<std::filesystem::path> to_remove;
            for (const auto &file_j: files_vec)
                to_remove.emplace_back(file_j.get<std::string>());
            files_or_directories_to_remove = to_remove;
        }

        VG_LOG(ctx.log, VG_LOG_LEVEL_DEBUG, "Parsed storage control config: action=", static_cast<int>(action),
            ", slot_dev_path=", slot_dev_path);
    }
};

} // namespace vigil::configs

#endif
