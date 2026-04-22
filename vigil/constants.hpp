#pragma once
#ifndef VIGIL_CONSTANTS_HPP
#define VIGIL_CONSTANTS_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace vigil::constants {

static inline constexpr const int kEndpointErrorBase = 30000;

#if defined(__cpp_lib_hardware_interference_size) && __cpp_lib_hardware_interference_size >= 201703L
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
static inline constexpr const size_t kHardwareDestructiveInterferenceSize = std::hardware_destructive_interference_size;
#pragma GCC diagnostic pop
#else
static inline constexpr const size_t kHardwareDestructiveInterferenceSize = 64;
#endif

static inline constexpr const std::string_view kRecordWriterPrefixDefault = "";
static inline constexpr const char *kRecordWriterDefaultFormat = "MP4";
static inline constexpr const std::chrono::milliseconds kRecordWriterDefaultIntervalMs
    = std::chrono::milliseconds(5000);
static inline constexpr const std::string_view kCaptureWriterPrefixDefault = "Capture ";

static inline constexpr const size_t kMaskWidth = 128;
static inline constexpr const size_t kMaskHeight = 128;

static inline constexpr const size_t kInferenceQueueSize = 60 * 1;

static inline constexpr const std::chrono::seconds kVideoClipLengthDefault = std::chrono::seconds(60);
static inline constexpr const size_t kSlotMediaMuxerBufferPeriod = 10;
static inline constexpr const size_t kCaptureMediaMuxerBufferPeriod = 10;

static inline constexpr const int kStorageDefaultUid = 1000;
static inline constexpr const int kStorageDefaultGid = 1000;
static inline constexpr const std::string_view kStorageDefaultDevPath = "/dev/mmcblk0p8";
static inline constexpr const std::string_view kStorageMountBasePath = "/mnt";
static inline constexpr const char *kStorageFirstLevelDirNameTimeFormat = "%Y-%m-%d";
static inline constexpr const char *kStorageFileNamePrefixTimeFormat = "%H.%M.%S";
static inline constexpr const std::string_view kStorageThumbnailDirName = ".thumb";
static inline constexpr const std::string kStorageThumbnailFileType = "JPG";
static inline constexpr const std::string_view kStorageThumbnailFileExt = ".thumb.jpg";
static inline constexpr const int kStorageThumbnailMaxWH = 1024;
static inline constexpr const size_t kStorageNetlinkBufferSize = 4096;
static inline constexpr const std::string_view kStorageNetlinkAction = "ACTION=";
static inline constexpr const std::string_view kStorageNetlinkDevName = "DEVNAME=";
static inline constexpr const std::string_view kStorageNetlinkSubsystem = "SUBSYSTEM=";
static inline constexpr const std::string_view kStorageNetlinkDevType = "DEVTYPE=";
static inline constexpr const size_t kStorageMinSupportedFileSystemBytes = 2048UL * 1024 * 1024;    // 2GB
static inline constexpr const size_t kStorageMinFileSystemFreeBytes = 512 * 1024 * 1024;            // 512MB
static inline constexpr const size_t kStorageMinFileSystemFreeBytesInternal = 4096UL * 1024 * 1024; // 4GB
static inline constexpr const size_t kStorageHighWaterMarkFactor = 5;
static inline constexpr const size_t kStorageCriticalWaterMarkFactor = 1;
static inline constexpr const size_t kStorageRecycleFactor = 10;
static inline constexpr const std::string_view kStoragePreservedProbeFilePath = "/userdata/update_ota.tar";

static inline constexpr const std::array<const char *, 7> kTMDayNamesMap = {
    "Sun",
    "Mon",
    "Tue",
    "Wed",
    "Thu",
    "Fri",
    "Sat",
};

} // namespace vigil::constants

#endif
