#pragma once
#ifndef VIGIL_CONSTANTS_HPP
#define VIGIL_CONSTANTS_HPP

#include <chrono>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>

namespace vigil::constants {

static inline constexpr const int kEndpointErrorBase = 30000;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
static inline constexpr const size_t kHardwareDestructiveInterferenceSize = std::hardware_destructive_interference_size;
#pragma GCC diagnostic pop

static inline constexpr const std::string_view kRecordWriterPrefixDefault = "";
static inline constexpr const std::string kRecordWriterDefaultFormat = "mp4";
static inline constexpr const std::chrono::milliseconds kRecordWriterDefaultIntervalMs = std::chrono::milliseconds(0);
static inline constexpr const std::string_view kCaptureWriterPrefixDefault = "Capture ";

static inline constexpr const size_t kMaskWidth = 128;
static inline constexpr const size_t kMaskHeight = 128;

static inline constexpr const size_t kInferenceQueueSize = 60 * 1;

static inline constexpr const std::chrono::seconds kVideoClipLengthDefault = std::chrono::seconds(60);
static inline constexpr const size_t kVideoFrameBufferSize = 30 * 5;
static inline constexpr const size_t kAudioFrameBufferSize = 50 * 5;
static inline constexpr const size_t kMediaMuxerBufferSize = kVideoFrameBufferSize + kAudioFrameBufferSize;

static inline constexpr const char *kStorageFirstLevelDirNameTimeFormat = "%Y-%m-%d";
static inline constexpr const size_t kStorageNetlinkBufferSize = 4096;
static inline constexpr const std::string_view kStorageNetlinkAction = "ACTION=";
static inline constexpr const std::string_view kStorageNetlinkDevName = "DEVNAME=";
static inline constexpr const std::string_view kStorageNetlinkSubsystem = "SUBSYSTEM=";
static inline constexpr const std::string_view kStorageNetlinkDevType = "DEVTYPE=";

} // namespace vigil::constants

#endif
