#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#include "vigil_capi.h"

static std::mutex log_mutex;

// Dummy callbacks
vg_sta_e log_cb(vg_log_level_e level, const char *msg, size_t bytes_len)
{
    const std::unique_lock lock(log_mutex);
    switch (level)
    {
        case VG_LOG_LEVEL_DEBUG:
            std::cout << "[D] ";
            break;
        case VG_LOG_LEVEL_INFO:
            std::cout << "[I] ";
            break;
        case VG_LOG_LEVEL_WARN:
            std::cout << "[W] ";
            break;
        case VG_LOG_LEVEL_ERROR:
            std::cout << "[E] ";
            break;
        case VG_LOG_LEVEL_FATAL:
            std::cout << "[F] ";
            break;
        default:
            break;
    }
    std::cout.write(msg, bytes_len);
    std::cout << std::endl;
    return 0;
}

void on_rule_event_cb(const vg_rule_evt_t *evt)
{
    std::cout << "Rule activated: " << (evt->id ? evt->id : "unknown") << std::endl;
}

vg_sta_e acquire_cb(int pin, vg_gpio_pin_mod_e mode, size_t debounce_time_ms)
{
    std::cout << "Acquire pin " << pin << " with mode " << mode << " and debounce " << debounce_time_ms << "ms"
              << std::endl;
    return 0;
}

vg_sta_e available_gpios_cb(vg_config_gpio_pins_t *pins)
{
    if (pins == nullptr)
    {
        return -1;
    }

    pins->free_cb = nullptr;
    pins->pins = nullptr;
    pins->len = 0;
    return 0;
}

void release_cb(int pin)
{
    std::cout << "Release pin " << pin << std::endl;
}

vg_gpio_pin_lvl_e sync_level_cb(int pin)
{
    (void)pin;
    return VG_GPIO_PIN_LVL_LOW;
}

vg_gpio_pin_evt_e sync_event_cb(int pin)
{
    (void)pin;
    return VG_GPIO_PIN_EVT_NONE;
}

vg_sta_e video_file_create_cb(int *fd, const char *filepath, const char *format, void *user_data)
{
    (void)user_data;
    (void)format;
    std::cout << "Create video file: " << filepath << std::endl;
    *fd = 100; // Dummy FD
    return 0;
}

vg_sta_e video_file_finish_cb(int fd, void *user_data)
{
    (void)fd;
    (void)user_data;
    std::cout << "Finish video file FD: " << fd << std::endl;
    return 0;
}

vg_sta_e video_file_write_video_frame_cb(int fd, const vg_video_frame_t *frame, void *user_data)
{
    (void)fd;
    (void)frame;
    (void)user_data;
    // std::cout << "Write video frame to FD: " << fd << std::endl;
    return 0;
}

vg_sta_e video_file_write_audio_frame_cb(int fd, const vg_audio_frame_t *frame, void *user_data)
{
    (void)fd;
    (void)frame;
    (void)user_data;
    // std::cout << "Write audio frame to FD: " << fd << std::endl;
    return 0;
}

vg_sta_e image_capture_cb(const char *filepath, const char *format, void *user_data)
{
    (void)user_data;
    std::cout << "Image capture requested for file: " << filepath << " with format: " << format << std::endl;
    return 0;
}

void on_mount_cb(const char *dev, const char *mount_path)
{
    std::cout << "Mounted " << dev << " at " << mount_path << std::endl;
}

void on_unmount_cb(const char *dev, const char *mount_path)
{
    std::cout << "Unmounted " << dev << " at " << mount_path << std::endl;
}

bool is_hidden_device_cb(const char *dev)
{
    (void)dev;
    return false;
}

int main()
{
    vg_config_t config = {};

    // Basic config
    config.basic.config_directory = "./config";
    config.basic.log_level = VG_LOG_LEVEL_DEBUG;
    config.basic.log_cb = log_cb;
    config.basic.on_rule_event_cb = on_rule_event_cb;

    // GPIOs
    config.gpios.available_cb = available_gpios_cb;
    config.gpios.acquire_cb = acquire_cb;
    config.gpios.release_cb = release_cb;
    config.gpios.sync_level_cb = sync_level_cb;
    config.gpios.sync_event_cb = sync_event_cb;

    // TTYs
    config.ttys.len = 0;
    config.ttys.ttys = nullptr;

    // Media
    config.media.user_data = nullptr;
    config.media.video_file_create_cb = video_file_create_cb;
    config.media.video_file_finish_cb = video_file_finish_cb;
    config.media.video_file_write_video_frame_cb = video_file_write_video_frame_cb;
    config.media.video_file_write_audio_frame_cb = video_file_write_audio_frame_cb;
    config.media.image_capture_cb = image_capture_cb;

    // Storage
    config.storage.internal_device_path = "/dev/null";
    config.storage.internal_mount_point = "/tmp";
    config.storage.data_directory_name = "data";
    config.storage.relay_directory_path = "relay";
    config.storage.on_mount_cb = on_mount_cb;
    config.storage.on_unmount_cb = on_unmount_cb;
    config.storage.is_hidden_device_cb = is_hidden_device_cb;

    std::cout << "Initializing Vigil Service..." << std::endl;
    vg_sta_e status = vg_service_init(&config);
    if (status != 0)
    {
        std::cerr << "Failed to initialize Vigil Service: " << status << std::endl;
        return 1;
    }
    std::cout << "Vigil Service Initialized." << std::endl;

    std::cout << "Running for 5 seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::cout << "Deinitializing Vigil Service..." << std::endl;
    status = vg_service_deinit();
    if (status != 0)
    {
        std::cerr << "Failed to deinitialize Vigil Service: " << status << std::endl;
        return 1;
    }
    std::cout << "Vigil Service Deinitialized." << std::endl;

    return 0;
}
