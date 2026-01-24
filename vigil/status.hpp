#pragma once
#ifndef VIGIL_STATUS_HPP
#define VIGIL_STATUS_HPP

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context.hpp"
#include "types.hpp"

namespace vigil::status {

using namespace vigil::types;

struct RelayStatus
{
    std::string relay_directory;
    std::chrono::seconds relay_timeout_remain;
    std::chrono::seconds relay_timeout;

    RelayStatus() : relay_directory(), relay_timeout_remain(0), relay_timeout(0) { }

    ~RelayStatus() = default;

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["iRelayTimeoutRemain"] = relay_timeout_remain.count();
        j["iRelayTimeout"] = relay_timeout.count();
        j["sRelayDirectory"] = relay_directory;
    }
};

struct SlotStatus
{
    std::filesystem::path dev_path;
    std::filesystem::path mount_path;
    bool removable;
    bool internal;
    std::optional<std::string> label;
    std::optional<std::string> uuid;
    std::optional<std::string> type;
    bool enabled;
    bool syncing;
    bool writing;
    SlotState state;
    SlotStats stats;
    FileSize quota_minimum_recommend_bytes;
    FileSize quota_preserved_bytes;
    std::optional<FileSize> quota_used_bytes;
    FileSize quota_limit_bytes;
    bool quota_rotate;

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["sDevPath"] = dev_path;
        j["sMountPath"] = mount_path;
        j["bRemovable"] = removable;
        j["bInternal"] = internal;
        j["sLabel"] = label;
        j["sUUID"] = uuid;
        j["sType"] = type;
        j["bEnabled"] = enabled;
        j["bSyncing"] = syncing;
        j["bWriting"] = writing;
        j["eState"] = static_cast<int>(state);
        j["sState"] = SlotStateString(state);
        j["iStatsSizeBytes"] = stats.size_bytes;
        j["iStatsFreeBytes"] = stats.free_bytes;
        j["iQuotaMinimumRecommendBytes"] = quota_minimum_recommend_bytes;
        j["iQuotaPreservedBytes"] = quota_preserved_bytes;
        j["iQuotaUsedBytes"] = quota_used_bytes;
        j["iQuotaLimitBytes"] = quota_limit_bytes;
        j["bQuotaRotate"] = quota_rotate;
    }
};

struct StorageStatus
{
    std::vector<SlotStatus> slots_status;
    std::string_view data_dir_name;
    std::string current_enabled_slot_dev_path;
    std::string target_enabled_slot_dev_path;

    StorageStatus(std::string_view ddn)
        : slots_status(), data_dir_name(ddn), current_enabled_slot_dev_path(), target_enabled_slot_dev_path()
    {
    }

    ~StorageStatus() = default;

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        nlohmann::json slots_dict = nlohmann::json::array();
        for (const auto &slot_status: slots_status)
        {
            nlohmann::json slot_j;
            slot_status.to(slot_j, ctx);
            slots_dict.push_back(slot_j);
        }
        j["lSlots"] = slots_dict;
        j["sDataDirName"] = data_dir_name;
        j["sCurrentEnabledSlotDevPath"] = current_enabled_slot_dev_path;
        j["sTargetEnabledSlotDevPath"] = target_enabled_slot_dev_path;
    }
};

struct GPIOPinStatus
{
    vg_config_gpio_pin_t pin;

    GPIOPinStatus(const vg_config_gpio_pin_t &p) : pin(p) { }

    void to(nlohmann::json &j, const Context &ctx) const
    {
        j["iNum"] = pin.num;
        j["sState"] = GPIOHelpers::StateToString(pin.state);
        {
            auto capabilities_arr = nlohmann::json::array();
            if (pin.capabilities & VG_GPIO_PIN_MODE_INP_FLOATING)
                capabilities_arr.push_back("FLOATING");
            if (pin.capabilities & VG_GPIO_PIN_MODE_INP_PULLUP)
                capabilities_arr.push_back("PULL_UP");
            if (pin.capabilities & VG_GPIO_PIN_MODE_INP_PULLDOWN)
                capabilities_arr.push_back("PULL_DOWN");
            j["lCapabilities"] = capabilities_arr;
        }
        j["sLevel"] = ((pin.state & ~VG_GPIO_PIN_MODE_DISABLED) && ctx.c_api.gpios.sync_level_cb)
                          ? GPIOHelpers::LevelToString(ctx.c_api.gpios.sync_level_cb(pin.num))
                          : std::optional<std::string_view>();
    }
};

struct CaptureStatus
{
    std::shared_ptr<CaptureEvent> active;
    std::shared_ptr<CaptureEvent> last;
    bool stop_requested;

    CaptureStatus(std::shared_ptr<CaptureEvent> active_evt, std::shared_ptr<CaptureEvent> last_evt, bool sr)
        : active(std::move(active_evt)), last(std::move(last_evt)), stop_requested(sr)
    {
    }

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["dActiveCapture"] = formatEvent(active);
        j["dLastCapture"] = formatEvent(last);
        j["bStopRequested"] = stop_requested;
    }

    static std::optional<nlohmann::json> formatEvent(const std::shared_ptr<CaptureEvent> &evt)
    {
        if (!evt || !evt->settings)
            return std::nullopt;

        auto oj = nlohmann::json::object();

        oj["sID"] = evt->id();
        oj["sOutputDirectory"] = evt->output_directory;
        {
            oj["sFormat"] = evt->settings->format;
            if (evt->settings->type == FileWriterSettings::CaptureType::Video
                && evt->settings->video_clip_length != std::chrono::seconds::max())
                oj["iVideoLengthSeconds"] = evt->settings->video_clip_length.count();
        }
        if (evt->event)
        {
            oj["sStatus"] = WriteEvent::StatusString(evt->event->status.load(std::memory_order_relaxed));
            oj["sTimepoint"]
                = std::chrono::duration_cast<std::chrono::milliseconds>(evt->event->timepoint.time_since_epoch())
                      .count();
            oj["iTimestamp"] = evt->event->timestamp;
        }
        oj["sFileName"] = evt->file_name;

        return oj;
    }
};

} // namespace vigil::status

#endif // VIGIL_STATUS_HPP
