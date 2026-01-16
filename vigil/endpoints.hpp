#pragma once
#ifndef VIGIL_ENDPOINTS_HPP
#define VIGIL_ENDPOINTS_HPP

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "configs.hpp"
#include "context.hpp"
#include "log.hpp"
#include "rules.hpp"
#include "status.hpp"
#include "storage.hpp"
#include "types.hpp"
#include "writers.hpp"

namespace vigil::endpoints {

using namespace vigil::rules;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::storage;
using namespace vigil::status;
using namespace vigil::constants;
using namespace vigil::writers;

// - MARK: Config Store

class ConfigStore final
{
public:
    ConfigStore(Context &ctx, std::string_view name)
        : _ctx(ctx), _name(name), _dir_path(std::filesystem::path(ctx.c_api.basic.config_directory))
    {
        // Ensure directory exists
        std::error_code ec;
        std::filesystem::create_directories(_dir_path, ec);
        if (ec)
            throw Exception("Failed to create config directory '" + _dir_path.string() + "': " + ec.message());
    }

    ~ConfigStore() = default;

    vg_sta_e loadFromFile(nlohmann::json &j) const noexcept
    {
        std::error_code ec;
        const auto file_path = _dir_path / _name;
        if (!std::filesystem::is_regular_file(file_path, ec))
            return ENOENT;

        std::ifstream file(file_path);
        if (!file.is_open())
            return EIO;
        try
        {
            file >> j;
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to load JSON from file '", file_path.string(),
                "': ", e.what());
            return EINVAL;
        }

        return 0;
    }

    vg_sta_e saveToFile(const nlohmann::json &j) noexcept
    {
        std::error_code ec;
        const auto tmp_path = _dir_path / (std::string(".") + std::string(_name));

        const std::unique_lock lock(_mutex);
        std::ofstream file(tmp_path, std::ios::trunc);
        if (!file.is_open())
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to open temporary file '", tmp_path.string(),
                "' for writing: ", std::strerror(errno));
            return EIO;
        }
        try
        {
            file << j.dump(4);
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to write JSON to temporary file '", tmp_path.string(),
                "': ", e.what());
            return EINVAL;
        }

        std::filesystem::rename(tmp_path, _dir_path / _name, ec);
        if (ec)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to rename temporary file '", tmp_path.string(), "' to '",
                (_dir_path / _name).string(), "': ", ec.message());
            std::filesystem::remove(tmp_path, ec);
            return EIO;
        }

        return 0;
    }

    template<typename T>
    std::unique_ptr<T> loadToConfig() noexcept
    {
        nlohmann::json j;
        vg_sta_e ret = loadFromFile(j);
        if (ret != 0)
            return {};

        try
        {
            auto obj = std::make_unique<T>(j, _ctx);
            return obj;
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to create config from file '", (_dir_path / _name).string(),
                "': ", e.what());
            return {};
        }

        return {};
    }

    template<typename T>
    bool saveFromConfig(const T &config) noexcept
    {
        nlohmann::json j;
        try
        {
            config.to(j, _ctx);
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Failed to serialize config to JSON for file '",
                (_dir_path / _name).string(), "': ", e.what());
            return false;
        }

        vg_sta_e ret = saveToFile(j);
        if (ret != 0)
            return false;

        return true;
    }

private:
    Context &_ctx;
    std::string_view _name;
    std::filesystem::path _dir_path;
    mutable std::mutex _mutex;
};

// - MARK: Rule Endpoint

class RuleEndpoint final
{
private:
    static inline constexpr const std::string_view kRuleConfigName = "rule_config.json";
    static inline constexpr const std::string_view kScheduleRuleConfigName = "schedule_rule_config.json";
    static inline constexpr const std::string_view kRecordRuleConfigName = "record_rule_config.json";
    static inline constexpr const std::chrono::milliseconds kRuleEvaluationInterval = std::chrono::milliseconds(10);

public:
    RuleEndpoint(std::shared_ptr<Context> ctx) : _ctx(std::move(ctx)), _rule_stop_token(false)
    {
        if (!_ctx)
            throw Exception("Context pointer cannot be null in RuleEndpoint");

        {
            _rule_config_store = std::make_unique<ConfigStore>(*_ctx, kRuleConfigName);
            if (nlohmann::json j; _rule_config_store->loadFromFile(j) == 0)
                updateRuleConfig(j, false);
        }
        {
            _schedule_rule_config = std::make_unique<ScheduleRuleConfig>();
            _schedule_rule_config_store = std::make_unique<ConfigStore>(*_ctx, kScheduleRuleConfigName);
            if (nlohmann::json j; _schedule_rule_config_store->loadFromFile(j) == 0)
                updateScheduleRuleConfig(j, false);
        }
        {
            _record_rule_config_store = std::make_unique<ConfigStore>(*_ctx, kRecordRuleConfigName);
            if (nlohmann::json j; _record_rule_config_store->loadFromFile(j) == 0)
                updateRecordRuleConfig(j, false);
        }

        _rule_thread = std::make_unique<std::thread>(&RuleEndpoint::ruleThreadTask, this);
        if (!_rule_thread)
            throw Exception("Failed to create rule evaluation thread in RuleEndpoint");
    }

    ~RuleEndpoint() noexcept
    {
        try
        {
            if (_rule_thread)
            {
                _rule_stop_token.store(true, std::memory_order_release);
                _rule_thread->join();
                _rule_thread.reset();
            }
        }
        catch (...)
        {
        }
    }

    nlohmann::json updateRuleConfig(const nlohmann::json &j, bool store = true)
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const auto rule_enabled = j.at("bRuleEnabled").get<bool>();
            const auto writer_config = FileWriterConfig(j.at("dWriterConfig"), *_ctx);

            if (store) // Store first
            {
                if (ret = _rule_config_store->saveToFile(j); ret != 0)
                    throw Exception("Failed to store rule config to file");
            }

            writer_config.sync(*_ctx); // Before updating rule enabled state
            _ctx->rule_enabled.store(rule_enabled, std::memory_order_relaxed);
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : EINVAL;
            res_j["message"] = e.what();
        }
        res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));

        return res_j;
    }

    nlohmann::json getRuleConfig() const
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            res_j["bRuleEnabled"] = _ctx->rule_enabled.load(std::memory_order_relaxed);
            {
                nlohmann::json writer_cfg_j;
                const auto writer_cfg = FileWriterConfig::from(*_ctx);
                writer_cfg.to(writer_cfg_j, *_ctx);
                res_j["dWriterConfig"] = writer_cfg_j;
            }
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : ENOENT;
            res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));
            res_j["message"] = e.what();
        }

        return res_j;
    }

    nlohmann::json updateScheduleRuleConfig(const nlohmann::json &j, bool store = true)
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            auto schedule_rule_config = std::make_unique<ScheduleRuleConfig>(j, *_ctx);
            const std::unique_lock lock(_schedule_rule_mutex);

            if (!_schedule_rule_config || *schedule_rule_config != *_schedule_rule_config)
            {
                _schedule_rule_config = std::move(schedule_rule_config);
                _schedule_rule.reset(); // If fail in the following steps, the previous rule is changed
                _schedule_rule = std::make_unique<ScheduleRule>(*_schedule_rule_config, *_ctx);

                if (store)
                {
                    if (ret = _schedule_rule_config_store->saveToFile(j); ret != 0)
                        throw Exception("Failed to store schedule rule config to file");
                }
            }
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : EINVAL;
            res_j["message"] = e.what();
        }
        res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));

        return res_j;
    }

    nlohmann::json getScheduleRuleConfig() const
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const std::shared_lock lock(_schedule_rule_mutex);
            if (_schedule_rule_config)
                _schedule_rule_config->to(res_j, *_ctx);
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : ENOENT;
            res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));
            res_j["message"] = e.what();
        }

        return res_j;
    }

    nlohmann::json updateRecordRuleConfig(const nlohmann::json &j, bool store = true)
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const auto rule_type_str = j.at("sCurrentSelected").get<std::string>();
            const auto rule_type = BaseRule::TypeFromString(rule_type_str);
            const std::unique_lock lock(_record_rule_mutex);
            if (rule_type == VG_RULE_TYPE_INFERENCE_SET)
                swapTypedRecordRule<InferenceRuleSet>(InferenceRuleSetConfig(j.at("lInferenceSet"), *_ctx));
            else if (rule_type == VG_RULE_TYPE_TIMER)
                swapTypedRecordRule<TimerRule>(TimerRuleConfig(j.at("dTimer"), *_ctx));
            else if (rule_type == VG_RULE_TYPE_GPIO)
                swapTypedRecordRule<GPIORule>(GPIORuleConfig(j.at("dGPIO"), *_ctx));
            else if (rule_type == VG_RULE_TYPE_TTY)
                swapTypedRecordRule<TTYRule>(TTYRuleConfig(j.at("dTTY"), *_ctx));
            else if (rule_type == VG_RULE_TYPE_HTTP)
                swapTypedRecordRule<HTTPRule>(HTTPRuleConfig());
            else
                throw Exception("Invalid rule type while updating record rule config '" + rule_type_str + "'");

            _record_rule_config = j;

            if (store)
            {
                // Entire JSON object
                if (ret = _record_rule_config_store->saveToFile(j); ret != 0)
                    throw Exception("Failed to store record rule config to file");
            }
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : EINVAL;
            res_j["message"] = e.what();
        }
        res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));

        return res_j;
    }

    nlohmann::json getRecordRuleConfig() const
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const std::shared_lock lock(_record_rule_mutex);
            res_j = _record_rule_config;
            std::visit(
                [&](auto &cfg) {
                    using ConfigType = std::decay_t<decltype(cfg)>;
                    if constexpr (std::is_same_v<ConfigType, std::monostate>)
                        return;
                    if constexpr (std::is_same_v<ConfigType, InferenceRuleSetConfig>)
                    {
                        auto arr = nlohmann::json::array();
                        cfg.to(arr, *_ctx);
                        res_j["lInferenceSet"] = arr;
                    }
                    else if constexpr (std::is_same_v<ConfigType, TimerRuleConfig>)
                    {
                        auto obj = nlohmann::json::object();
                        cfg.to(obj, *_ctx);
                        res_j["dTimer"] = obj;
                    }
                    else if constexpr (std::is_same_v<ConfigType, GPIORuleConfig>)
                    {
                        auto obj = nlohmann::json::object();
                        cfg.to(obj, *_ctx);
                        res_j["dGPIO"] = obj;
                    }
                    else if constexpr (std::is_same_v<ConfigType, TTYRuleConfig>)
                    {
                        auto obj = nlohmann::json::object();
                        cfg.to(obj, *_ctx);
                        res_j["dTTY"] = obj;
                    }
                },
                _record_rule_config_variant);
            res_j["sCurrentSelected"] = _record_rule ? BaseRule::TypeString(_record_rule->type()) : "";
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : ENOENT;
            res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));
            res_j["message"] = e.what();
        }

        return res_j;
    }

    nlohmann::json getRuleInfo() const
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            {
                auto [last_evt, current_evt, current_owner] = _ctx->shared_rule_event.get();

                res_j["dLastRuleEvent"] = [&]() -> std::optional<nlohmann::json> {
                    if (!last_evt)
                        return std::nullopt;

                    const auto &evt = last_evt;
                    auto obj = nlohmann::json::object();
                    obj["sStatus"] = WriteEvent::StatusString(evt->status.load(std::memory_order_relaxed));
                    obj["iTimestamp"] = evt->timestamp;
                    return obj;
                }();

                res_j["dCurrentRuleEvent"] = [&]() -> std::optional<nlohmann::json> {
                    if (!current_evt)
                        return std::nullopt;

                    const auto &evt = current_evt;
                    auto obj = nlohmann::json::object();
                    obj["sStatus"] = WriteEvent::StatusString(evt->status.load(std::memory_order_relaxed));
                    obj["iTimestamp"] = evt->timestamp;
                    return obj;
                }();

                res_j["dCurrentRuleEventOwner"] = [&]() -> std::optional<nlohmann::json> {
                    const auto &owner = current_owner;
                    if (!owner.has_value())
                        return std::nullopt;

                    auto obj = nlohmann::json::object();
                    obj["sRuleType"] = BaseRule::TypeString(owner->type);
                    obj["sRuleID"] = owner->id;
                    obj["iTimestamp"] = owner->timestamp;
                    return obj;
                }();
            }
            {
                auto gpios_obj = nlohmann::json::object();
                if (_ctx->c_api.gpios.available_cb)
                {
                    vg_config_gpio_pins_t available_pins;
                    if (_ctx->c_api.gpios.available_cb(&available_pins) == 0 && available_pins.pins
                        && available_pins.len > 0)
                    {
                        for (size_t i = 0; i < available_pins.len; ++i)
                        {
                            const auto &pin = available_pins.pins[i];
                            GPIOPinStatus gpio_status(pin);
                            gpio_status.to(gpios_obj[gpio_status.pin.name], *_ctx);
                        }
                    }
                    if (available_pins.free_cb)
                        available_pins.free_cb(available_pins.pins);
                    else
                        VG_LOG(_ctx->log, VG_LOG_LEVEL_DEBUG, "No free_cb provided for available GPIO pins");
                }
                res_j["dAvailableGPIOs"] = gpios_obj;
            }
            {
                auto ttys_obj = nlohmann::json::object();
                if (_ctx->c_api.ttys.ttys && _ctx->c_api.ttys.len > 0)
                {
                    for (size_t i = 0; i < _ctx->c_api.ttys.len; ++i)
                    {
                        const auto &tty = _ctx->c_api.ttys.ttys[i];
                        ttys_obj[tty.name ? std::string(tty.name) : std::to_string(i)] = {
                            { "sSocketPath", tty.socket_path ? std::string(tty.socket_path) : "" },
                            { "iBufferSize", static_cast<int>(tty.buffer_size) },
                        };
                    }
                }
                res_j["dAvailableTTYs"] = ttys_obj;
            }
            {
                const bool media_paused = _ctx->media_paused.load(std::memory_order_relaxed);
                res_j["bMediaPaused"] = media_paused;
            }
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : ENOENT;
            res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));
            res_j["message"] = e.what();
        }

        return res_j;
    }

    nlohmann::json doHTTPRuleActivate(const nlohmann::json &)
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            // Pre-checks
            {
                if (!_ctx->rule_enabled.load(std::memory_order_relaxed))
                {
                    ret = EPERM;
                    throw Exception("Cannot activate HTTP rule when rule evaluation is disabled");
                }
            }
            {
                const std::shared_lock lock(_schedule_rule_mutex);
                if (_schedule_rule && _schedule_rule->type() == VG_RULE_TYPE_SCHEDULE)
                {
                    if (!static_cast<ScheduleRule *>(_schedule_rule.get())->isActive(LocalTime::now()))
                    {
                        ret = EPERM;
                        throw Exception(
                            "Activating HTTP rule required schedule rule to be ready in current configuration");
                    }
                }
            }
            {
                const std::shared_lock lock(_ctx->slot_check_mutex);
                if (_ctx->slot_check.second)
                {
                    if (!(_ctx->slot_check.second)(_ctx->slot_check.first))
                    {
                        ret = EBUSY;
                        throw Exception("Cannot activate HTTP rule when storage slot is not ready to write");
                    }
                }
            }
            // Try to activate
            {
                const std::shared_lock lock(_record_rule_mutex);
                if (!_record_rule || _record_rule->type() != VG_RULE_TYPE_HTTP)
                {
                    ret = EPERM;
                    throw Exception("Current record rule is not HTTP rule, cannot activate");
                }
                if (ret = static_cast<HTTPRule *>(_record_rule.get())->tryActivate(); ret != 0)
                    throw Exception("Failed to activate HTTP rule, error code: " + std::to_string(ret));
            }
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : EFAULT;
            res_j["message"] = e.what();
        }
        res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));

        return res_j;
    }

private:
    void ruleThreadTask() noexcept
    {
        while (!_rule_stop_token.load(std::memory_order_relaxed))
        {
            const auto start = std::chrono::steady_clock::now();
            ruleTaskLoop();
            if (auto duration = std::chrono::steady_clock::now() - start; duration < kRuleEvaluationInterval)
                std::this_thread::sleep_for(kRuleEvaluationInterval - duration);
        }
    }

    inline void ruleTaskLoop() noexcept
    {
        if (!_ctx->tick())
            return; // No new data to evaluate or disabled

        vg_rule_type_e type = VG_RULE_TYPE_UNKNOWN;
        std::string id;
        {
            const std::shared_lock lock(_schedule_rule_mutex);
            if (_schedule_rule && _schedule_rule->operator()(*_ctx))
            {
                type = _schedule_rule->type();
                id = _schedule_rule->id();
            }
            else
                return;
        }
        {
            const std::shared_lock lock_record(_record_rule_mutex);
            if (_record_rule && _record_rule->operator()(*_ctx))
            {
                type = _record_rule->type();
                id = _record_rule->id();
            }
            else
                return;
        }

        _ctx->tryUpdateRuleEvent(type, id);
    }

private:
    template<typename T, typename C>
    void swapTypedRecordRule(const C &new_config)
    {
        try
        {
            if (_record_rule)
                _record_rule.reset();
            _record_rule = std::make_unique<T>(new_config, *_ctx);
            _record_rule_config_variant = new_config;
        }
        catch (std::exception &e)
        {
            VG_LOG(_ctx->log, VG_LOG_LEVEL_ERROR, "Failed to create record rule from config: ", e.what());
            rollbackRecordRule();
        }
    }

    void rollbackRecordRule()
    {
        _record_rule = std::visit(
            [&](auto &cfg) -> std::unique_ptr<BaseRule> {
                using ConfigType = std::decay_t<decltype(cfg)>;
                if constexpr (std::is_same_v<ConfigType, InferenceRuleSetConfig>)
                    return std::make_unique<InferenceRuleSet>(cfg, *_ctx);
                else if constexpr (std::is_same_v<ConfigType, TimerRuleConfig>)
                    return std::make_unique<TimerRule>(cfg, *_ctx);
                else if constexpr (std::is_same_v<ConfigType, GPIORuleConfig>)
                    return std::make_unique<GPIORule>(cfg, *_ctx);
                else if constexpr (std::is_same_v<ConfigType, TTYRuleConfig>)
                    return std::make_unique<TTYRule>(cfg, *_ctx);
                else if constexpr (std::is_same_v<ConfigType, HTTPRuleConfig>)
                    return std::make_unique<HTTPRule>(cfg, *_ctx);
                else
                    return nullptr;
            },
            _record_rule_config_variant);
    }

private:
    mutable std::shared_ptr<Context> _ctx;

    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _rule_stop_token;
    std::unique_ptr<std::thread> _rule_thread;

    mutable std::shared_mutex _schedule_rule_mutex;
    std::unique_ptr<ScheduleRuleConfig> _schedule_rule_config;
    std::unique_ptr<BaseRule> _schedule_rule;

    mutable std::shared_mutex _record_rule_mutex;
    nlohmann::json _record_rule_config;
    std::variant<std::monostate, InferenceRuleSetConfig, TimerRuleConfig, GPIORuleConfig, TTYRuleConfig, HTTPRuleConfig>
        _record_rule_config_variant;
    std::unique_ptr<BaseRule> _record_rule;

    // Config store
    std::unique_ptr<ConfigStore> _rule_config_store;
    std::unique_ptr<ConfigStore> _schedule_rule_config_store;
    std::unique_ptr<ConfigStore> _record_rule_config_store;
};

// - MARK: Storage Endpoint

class StorageEndpoint final
{
private:
    static inline constexpr const std::string_view kStorageConfigName = "storage_config.json";

public:
    StorageEndpoint(std::shared_ptr<Context> ctx)
        : _ctx(std::move(ctx)), _storage(nullptr), _storage_config_store(nullptr)
    {
        if (!_ctx)
            throw Exception("Context pointer cannot be null in StorageEndpoint");

        _storage = std::make_unique<Storage>(StorageConfig {}, *_ctx);
        {
            _storage_config_store = std::make_unique<ConfigStore>(*_ctx, kStorageConfigName);
            if (nlohmann::json j; _storage_config_store->loadFromFile(j) == 0)
                updateStorageConfig(j, false);
        }
    }

    ~StorageEndpoint() = default;

    nlohmann::json updateStorageConfig(const nlohmann::json &j, bool store = true)
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const auto config = StorageConfig(j, *_ctx); // Check validity

            if (store)
            {
                if (ret = _storage_config_store->saveToFile(j); ret != 0)
                    throw Exception("Failed to store storage config to file");
            }

            if (ret = _storage->setConfig(config); ret != 0)
                throw Exception("Failed to apply storage configuration");
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : EINVAL;
            res_j["message"] = e.what();
        }
        res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));

        return res_j;
    }

    nlohmann::json getStorageConfig() const
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const auto config = _storage->getConfig();
            config.to(res_j, *_ctx);
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : ENOENT;
            res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));
            res_j["message"] = e.what();
        }

        return res_j;
    }

    nlohmann::json getStorageStatus() const
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const auto status = _storage->getStatus();
            status.to(res_j, *_ctx);
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : ENOENT;
            res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));
            res_j["message"] = e.what();
        }

        return res_j;
    }

    nlohmann::json doStorageControl(const nlohmann::json &j)
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const auto control_config = StorageControlConfig(j, *_ctx);
            switch (control_config.action)
            {
                case StorageControlConfig::ActionType::FormatSlot:

                    if (ret = _storage->formatSlot(control_config.slot_dev_path); ret != 0)
                        throw Exception("Failed to format storage slot");
                    break;

                case StorageControlConfig::ActionType::FreeUpSlot:
                    if (ret = _storage->freeUpSlot(control_config.slot_dev_path); ret != 0)
                        throw Exception("Failed to free up storage slot");
                    break;

                case StorageControlConfig::ActionType::EjectSlot:
                    if (ret = _storage->ejectSlot(control_config.slot_dev_path); ret != 0)
                        throw Exception("Failed to eject storage slot");
                    break;

                case StorageControlConfig::ActionType::ConfigSlot:
                    if (!control_config.slot_config)
                        throw Exception("Missing slot configuration for ConfigSlot action");
                    if (ret = _storage->setSlotConfig(control_config.slot_dev_path, *control_config.slot_config);
                        ret != 0)
                        throw Exception("Failed to configure storage slot");
                    break;

                case StorageControlConfig::ActionType::RelaySlot: {
                    RelayStatus relay_status;
                    if (ret = _storage->enableOrHeartbeatNginxRelay(control_config.slot_dev_path, relay_status);
                        ret != 0)
                        throw Exception("Failed to enable nginx relay on storage slot");
                    res_j["dRelayStatus"] = nlohmann::json::object();
                    relay_status.to(res_j["dRelayStatus"], *_ctx);
                }
                break;

                case StorageControlConfig::ActionType::RelayStatusSlot: {
                    RelayStatus relay_status;
                    if (ret = _storage->statusOfNginxRelay(control_config.slot_dev_path, relay_status); ret != 0)
                        throw Exception("Failed to enable nginx relay on storage slot");
                    res_j["dRelayStatus"] = nlohmann::json::object();
                    relay_status.to(res_j["dRelayStatus"], *_ctx);
                }
                break;

                case StorageControlConfig::ActionType::UnrelaySlot:
                    if (ret = _storage->disableNginxRelay(control_config.slot_dev_path); ret != 0)
                        throw Exception("Failed to disable nginx relay on storage slot");
                    break;

                case StorageControlConfig::ActionType::RemoveFilesOrDirectories:
                    if (!control_config.files_or_directories_to_remove)
                        throw Exception("Missing files or directories to remove for RemoveFilesOrDirectories action");
                    {
                        auto to_remove = *control_config.files_or_directories_to_remove;
                        if (ret = _storage->removeFilesOrDirectories(control_config.slot_dev_path, to_remove); ret != 0)
                            throw Exception("Failed to remove files or directories on storage slot");
                        res_j["lRemovedFilesOrDirectories"] = nlohmann::json::array();
                        for (const auto &path: to_remove)
                            res_j["lRemovedFilesOrDirectories"].emplace_back(path.string());
                    }
                    break;

                default:
                    throw Exception("Unknown storage action type");
            }
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : EINVAL;
            res_j["message"] = e.what();
        }
        res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));

        return res_j;
    }

private:
    mutable std::shared_ptr<Context> _ctx;

    // Storage manager (should be singleton later), thread-safe
    std::unique_ptr<Storage> _storage;

    // Config store
    std::unique_ptr<ConfigStore> _storage_config_store;
};

// - MARK: Capture Endpoint

class CaptureEndpoint final
{
public:
    CaptureEndpoint(std::shared_ptr<Context> ctx) : _ctx(std::move(ctx))
    {
        if (!_ctx)
            throw Exception("Context pointer cannot be null in CaptureEndpoint");

        _capture_thread = std::make_unique<std::thread>(&CaptureEndpoint::captureThreadTask, this);
        if (!_capture_thread)
            throw Exception("Failed to create capture thread in CaptureEndpoint");
    }

    ~CaptureEndpoint() noexcept
    {
        _finish_current_capture.store(true);

        try
        {
            if (_capture_thread)
            {
                _capture_stop_token.store(true, std::memory_order_release);
                _capture_thread->join();
                _capture_thread.reset();
            }
        }
        catch (...)
        {
        }
    }

    nlohmann::json getCaptureStatus() const
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        auto get_capture_status = [&]() -> CaptureStatus {
            const std::shared_lock lock(_event_mutex);
            return { _event_active, _event_last };
        };

        try
        {
            auto evt = get_capture_status();
            evt.to(res_j, *_ctx);
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : ENOENT;
            res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));
            res_j["message"] = e.what();
        }

        return res_j;
    }

    nlohmann::json doCaptureStart(const nlohmann::json &j)
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const std::unique_lock lock(_event_mutex);
            if (_event_active)
            {
                ret = EBUSY;
                throw Exception("Capture is already in progress");
            }

            const auto config = CaptureConfig(j, *_ctx);
            _event_active = getCaptureEvent(config);
            if (!_event_active)
            {
                ret = EFAULT;
                throw Exception("Failed to create capture event from configuration");
            }
            res_j["dCapture"] = CaptureStatus::formatEvent(_event_active);
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : EINVAL;
            res_j["message"] = e.what();
        }
        res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));

        return res_j;
    }

    nlohmann::json doCaptureStop(const nlohmann::json &)
    {
        auto res_j = nlohmann::json::object();
        vg_sta_e ret = 0;

        try
        {
            const std::unique_lock lock(_event_mutex);
            if (!_event_active)
            {
                ret = ENOENT;
                throw Exception("No ongoing capture to stop");
            }
            if (auto exp = false;
                !_finish_current_capture.compare_exchange_strong(exp, true, std::memory_order_acq_rel))
            {
                ret = EALREADY;
                throw Exception("Capture stopping is already in progress");
            }
        }
        catch (const std::exception &e)
        {
            res_j.clear();
            ret = ret != 0 ? ret : EINVAL;
            res_j["message"] = e.what();
        }
        res_j["code"] = static_cast<int>(ret == 0 ? 0 : (kEndpointErrorBase + std::abs(ret)));

        return res_j;
    }

private:
    static inline constexpr const std::chrono::milliseconds kWriterProbeInterval = std::chrono::milliseconds(50);

private:
    std::shared_ptr<CaptureEvent> getCaptureEvent(const CaptureConfig &config) const
    {
        if (!config.output.is_absolute())
            throw Exception("Capture output must be an absolute path");

        const auto output_str = config.output.string();
        if (!(output_str.starts_with(kStorageMountBasePath)
                || (_ctx->c_api.storage.internal_mount_point
                    && output_str.starts_with(_ctx->c_api.storage.internal_mount_point))))
            throw Exception("Capture output must be under storage mount point");

        if (!config.settings)
            throw Exception("Capture settings cannot be null");

        const auto steady_now = std::chrono::steady_clock::now();
        const auto system_now = std::chrono::system_clock::now();

        auto write_event = std::make_shared<WriteEvent>(steady_now,
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(system_now.time_since_epoch()).count()));

        auto fmt = config.settings->format;
        std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return std::tolower(c); });
        fmt = '.' + fmt;

        std::filesystem::path output_directory;
        std::string file_name;
        if (!output_str.ends_with(fmt))
        {
            output_directory = config.output;

            std::stringstream ss;
            ss << std::string(config.settings->file_name_prefix);

            const auto local_time = LocalTime(system_now);
            auto tm = local_time.tm();
            ss << std::put_time(&tm, kStorageFirstLevelDirNameTimeFormat);

            const auto duration = steady_now.time_since_epoch();
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);
            ss << ' ' << std::put_time(&tm, kStorageFileNamePrefixTimeFormat) << ' ' << micros.count() << fmt;

            file_name = ss.str();
        }
        else
        {
            output_directory = config.output.parent_path();
            file_name = config.output.filename().string();
        }

        return std::make_shared<CaptureEvent>(std::move(output_directory), config.settings, std::move(write_event),
            std::move(file_name));
    }

    void captureThreadTask() noexcept
    {
        while (!_capture_stop_token.load(std::memory_order_relaxed))
        {
            const auto start = std::chrono::steady_clock::now();
            captureTaskLoop();
            if (auto duration = std::chrono::steady_clock::now() - start; duration < kWriterProbeInterval)
                std::this_thread::sleep_for(kWriterProbeInterval - duration);
        }
    }

    void captureTaskLoop() noexcept
    {
        if (_ctx->media_paused.load(std::memory_order_relaxed))
            return;
        auto event = getCaptureEvent();
        try
        {
            if (auto writer = getWriterFromEvent(event); writer)
                writer->operator()();
        }
        catch (std::exception &e)
        {
            VG_LOG(_ctx->log, VG_LOG_LEVEL_ERROR, "Exception in capture task loop: ", e.what());
            handleCompletedCaptureEvent(event);
        }
    }

    std::shared_ptr<CaptureEvent> getCaptureEvent() const noexcept
    {
        const std::shared_lock lock(_event_mutex);
        return _event_active;
    }

    std::unique_ptr<CaptureAdaptedWriter<CaptureEndpoint>> getWriterFromEvent(std::shared_ptr<CaptureEvent> event)
    {
        if (!event || !event->settings)
            return nullptr;
        switch (event->settings->type)
        {
            case FileWriterSettings::CaptureType::Image:
                _ctx->capture_media_buffer.clearToLastKeyFrame();
                return std::make_unique<ImageWriter<CaptureAdaptedWriter<CaptureEndpoint>>>(std::ref(*this), event,
                    std::ref(*_ctx), event->settings, event->event);
            case FileWriterSettings::CaptureType::Video:
                _ctx->capture_media_buffer.clearToLastKeyFrame();
                return std::make_unique<VideoWriter<CaptureAdaptedWriter<CaptureEndpoint>>>(std::ref(*this), event,
                    std::ref(*_ctx), event->settings, event->event);
            default:
                return nullptr;
        }
    }

protected:
    friend CaptureAdaptedWriter<CaptureEndpoint>;

    void handleCompletedCaptureEvent(std::shared_ptr<CaptureEvent> event) noexcept
    {
        const std::unique_lock lock(_event_mutex);
        if (event != _event_active)
            VG_LOG(_ctx->log, VG_LOG_LEVEL_WARN, "Completed capture event does not match current event pointer");
        _event_last = std::move(event);
        _event_active.reset();
        _finish_current_capture.store(false, std::memory_order_release);
    }

    inline bool shouldFinishWriting() const noexcept
    {
        return _finish_current_capture.load(std::memory_order_relaxed);
    }

    inline bool shouldContinueWriting() const noexcept
    {
        return !_capture_stop_token.load(std::memory_order_relaxed);
    }

private:
    mutable std::shared_ptr<Context> _ctx;

    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _capture_stop_token = false;
    std::unique_ptr<std::thread> _capture_thread;

    mutable std::shared_mutex _event_mutex;
    std::shared_ptr<CaptureEvent> _event_active;
    std::shared_ptr<CaptureEvent> _event_last;

    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _finish_current_capture = false;
};

} // namespace vigil::endpoints

#endif
