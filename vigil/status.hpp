#pragma once
#ifndef VIGIL_STATUS_HPP
#define VIGIL_STATUS_HPP

#include <filesystem>
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
    bool writing;
    SlotState state;
    SlotStats stats;
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
        j["bWriting"] = writing;
        j["eState"] = static_cast<int>(state);
        j["sState"] = SlotStateString(state);
        j["iStatsSizeBytes"] = stats.size_bytes;
        j["iStatsFreeBytes"] = stats.free_bytes;
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
    std::filesystem::path output_directory;
    FileWriterSettings settings;
    std::shared_ptr<WriteEvent> event;
    std::string file_name;
    bool ready_to_capture_new;

    void to(nlohmann::json &j, const Context &ctx [[maybe_unused]]) const
    {
        j["sOutputDirectory"] = output_directory;
        {
            j["sFormat"] = settings.format;
            if (settings.type == FileWriterSettings::CaptureType::Video
                && settings.video_clip_length != std::chrono::seconds::max())
                j["iVideoClipLengthSeconds"] = settings.video_clip_length.count();
        }
        if (event)
        {
            j["sStatus"] = WriteEvent::StatusString(event->status.load(std::memory_order_relaxed));
            j["sTimepoint"] = event->timepoint.time_since_epoch().count();
            j["iTimestamp"] = event->timestamp;
        }
        j["sFileName"] = file_name;
        j["bReadyToCaptureNew"] = ready_to_capture_new;
    }
};

} // namespace vigil::status

#endif // VIGIL_STATUS_HPP
