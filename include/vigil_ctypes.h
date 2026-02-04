#ifndef VIGIL_CTYPES_H
#define VIGIL_CTYPES_H

#ifdef VG_CTYPES_VERSION
#error "Duplicate definition of VG_CTYPES_VERSION"
#else
#define VG_CTYPES_VERSION "2025.11.25"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// - MARK: Common Enums and Types

typedef int vg_sta_e; /**< Standard status code type. 0 indicates success, otherwise errors. */

typedef enum vg_log_level_e {
    VG_LOG_LEVEL_DEBUG = 0,
    VG_LOG_LEVEL_INFO = 1,
    VG_LOG_LEVEL_WARN = 2,
    VG_LOG_LEVEL_ERROR = 3,
    VG_LOG_LEVEL_FATAL = 4
} vg_log_level_e;

// - MARK: Media Enums and Data Structures

typedef struct vg_video_frame_t
{
    int64_t pts;
    void *data;
    size_t bytes_len;
    int key_frame; /**< Non-zero when the frame is a keyframe (IDR). */
} vg_video_frame_t;

typedef struct vg_audio_frame_t
{
    int64_t pts;
    void *data;
    size_t bytes_len;
} vg_audio_frame_t;

/**
 * @brief Callback surface for media storage and capture operations.
 *
 * Allows the host application to provide file lifecycle hooks and image
 * capture handling for video/audio recording initiated by Vigil.
 */
typedef struct vg_config_media_t
{
    void *user_data; /**< User data pointer passed to all media callbacks. */
    vg_sta_e (*video_file_create_cb)(int *fd, const char *filepath, const char *format,
        void *user_data); /**< Creates or opens a media sink and outputs a file descriptor (parent folder already
                             exists). */
    vg_sta_e (
        *video_file_finish_cb)(int fd, void *user_data); /**< Completes any pending work and closes the media sink. */
    vg_sta_e (*video_file_write_video_frame_cb)(int fd, const vg_video_frame_t *frame,
        void *user_data); /**< Writes a video frame into the active recording. */
    vg_sta_e (*video_file_write_audio_frame_cb)(int fd, const vg_audio_frame_t *frame,
        void *user_data); /**< Writes an audio frame into the active recording. */
    vg_sta_e (*image_capture_cb)(const char *filepath, const char *format,
        void *user_data); /**< Handles on-demand still image capture for a given source, and write to file (parent
                             folder already exists). */
    vg_sta_e (*thumbnail_generate_cb)(const void *src_data, size_t src_data_len, const char *src_format,
        const char *thumb_filepath, const char *thumb_format, int max_wh, void *user_data); /**< Generates a thumbnail
                         image from the source image data and writes to file */

} vg_config_media_t;

// - MARK: Config Enums and Types

typedef enum vg_rule_type_e {
    VG_RULE_TYPE_UNKNOWN = 0,
    VG_RULE_TYPE_INFERENCE = 1,
    VG_RULE_TYPE_INFERENCE_SET = 2,
    VG_RULE_TYPE_TIMER = 3,
    VG_RULE_TYPE_GPIO = 4,
    VG_RULE_TYPE_TTY = 5,
    VG_RULE_TYPE_SCHEDULE = 6,
    VG_RULE_TYPE_HTTP = 7,
} vg_rule_type_e;

typedef struct vg_rule_evt_t
{
    vg_rule_type_e type; /**< Type of the last evaluated rule. */
    const char *id;      /**< Internal managed string, null terminated, do not free. */
    int64_t timestamp;   /**< Timestamp of the last evaluation. */
} vg_rule_evt_t;

typedef enum vg_file_evt_op_type_e {
    VG_FILE_EVT_OP_ADDED = 1,
} vg_file_evt_op_type_e;

typedef struct vg_file_evt_t
{
    vg_file_evt_op_type_e op_type; /**< Type of file event operation. */
    const char *path;              /**< Internal managed string, null terminated, do not free. */
    size_t size_bytes;             /**< Size of the file in bytes. */
    const char *attrs;             /**< Internal managed string, null terminated, do not free. */
    int64_t timestamp; /**< Timestamp of the file event (can be corresponding to the timestamp of rule event). */
} vg_file_evt_t;

/**
 * @brief Basic runtime configuration for the Vigil library.
 *
 * Supplies the configuration root, default log verbosity, and an optional
 * callback for redirecting log messages to an external logger.
 */
typedef struct vg_config_basic_t
{
    vg_log_level_e log_level; /**< Minimum log severity that should be emitted. */
    vg_sta_e (*log_cb)(vg_log_level_e level, const char *msg, size_t bytes_len); /**< Optional logging callback. */
    void (*on_rule_event_cb)(const vg_rule_evt_t *evt); /**< Optional callback invoked when a rule is activated. */
    void (*on_file_event_cb)(const vg_file_evt_t *evt); /**< Optional callback invoked on file events. */
    const char *config_directory;                       /**< Filesystem path to the configuration root. */
} vg_config_basic_t;

typedef enum vg_gpio_pin_mod_e {
    VG_GPIO_PIN_MODE_DISABLED = 0,
    VG_GPIO_PIN_MODE_INP_FLOATING = 1 << 1,
    VG_GPIO_PIN_MODE_INP_PULLUP = 1 << 2,
    VG_GPIO_PIN_MODE_INP_PULLDOWN = 1 << 3,
} vg_gpio_pin_mod_e;

typedef enum vg_gpio_pin_lvl_e {
    VG_GPIO_PIN_LVL_NONE = -1,
    VG_GPIO_PIN_LVL_LOW = 0,
    VG_GPIO_PIN_LVL_HIGH = 1,
} vg_gpio_pin_lvl_e;

typedef enum vg_gpio_pin_evt_e {
    VG_GPIO_PIN_EVT_NONE = 0,
    VG_GPIO_PIN_EVT_FALLING = 1,
    VG_GPIO_PIN_EVT_RISING = 2
} vg_gpio_pin_evt_e;

#define VG_GPIO_PIN_NAME_MAX_LEN 64

/**
 * @brief Configuration for an individual GPIO pin.
 *
 * Describes how Vigil should address and interact with a single GPIO.
 */
typedef struct vg_config_gpio_pin_t
{
    int num;                             /**< Hardware pin identifier or logical number. */
    char name[VG_GPIO_PIN_NAME_MAX_LEN]; /**< Human-readable label, null-terminated. */
    vg_gpio_pin_mod_e state;             /**< Current state for the GPIO pin. */
    vg_gpio_pin_mod_e capabilities;      /**< Supported modes for the GPIO pin. */
} vg_config_gpio_pin_t;

/**
 * @brief Collection of GPIO pin configurations.
 *
 * Used to return a list of available GPIO pins from the host application.
 */
typedef struct vg_config_gpio_pins_t
{
    void (*free_cb)(void *pins);
    vg_config_gpio_pin_t *pins;
    size_t len;
} vg_config_gpio_pins_t;

typedef struct vg_config_gpios_t
{
    vg_sta_e (*available_cb)(vg_config_gpio_pins_t *pins); /**< Callback to query available GPIO pins. */
    vg_sta_e (
        *acquire_cb)(int pin, vg_gpio_pin_mod_e state, size_t debounce_ms); /**< Callback to init a GPIO pin for use. */
    void (*release_cb)(int pin);                 /**< Callback to release a previously used GPIO pin. */
    vg_gpio_pin_lvl_e (*sync_level_cb)(int pin); /**< Callback to read the current level of a GPIO pin. */
    vg_gpio_pin_evt_e (*sync_event_cb)(int pin); /**< Callback to sync the last event of a GPIO pin. */
} vg_config_gpios_t;

/**
 * @brief Configuration for a single TTY endpoint.
 *
 * Maps a logical name to the Unix domain socket used for communication.
 */
typedef struct vg_config_tty_t
{
    const char *name;        /**< Identifier displayed in logs and diagnostics. */
    const char *socket_path; /**< Filesystem path to the Unix socket. */
    size_t buffer_size;      /**< Size of the internal read/write buffer for the TTY. */
} vg_config_tty_t;

typedef struct vg_config_ttys_t
{
    vg_config_tty_t *ttys;
    size_t len;
} vg_config_ttys_t;

typedef struct vg_config_storage_t
{
    void (*on_mount_cb)(const char *dev, const char *mount_path);   /**< Callback invoked when storage is mounted. */
    void (*on_unmount_cb)(const char *dev, const char *mount_path); /**< Callback invoked when storage is unmounted. */
    bool (*is_hidden_device_cb)(const char *dev); /**< Callback to determine if a block device should be hidden
                                                    from storage management. */
    const char *internal_device_path; /**< Device path to the internal storage device. e.g. /dev/mmcblk0p1 */
    const char *internal_mount_point; /**< Filesystem path to the internal storage mount point. */
    const char *data_directory_name;  /**< Filesystem name to the data storage root. e.g. DCIM, don't start with '/' */
    const char *relay_directory_path; /**< Filesystem path to the nginx relay directory for Vigil relay data. */
} vg_config_storage_t;

typedef struct vg_config_t
{
    vg_config_basic_t basic;
    vg_config_gpios_t gpios;
    vg_config_ttys_t ttys;
    vg_config_media_t media;
    vg_config_storage_t storage;
} vg_config_t;

// - MARK: HTTP API Data Structures

typedef struct vg_http_api_request_t
{
    void (*free_cb)(void *payload);
    void *payload;
    size_t payload_len;
} vg_http_api_request_t;

#define VG_HTTP_API_SAFE_RELEASE_REQUEST(request)                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((request) != NULL && (request)->free_cb != NULL && (request)->payload != NULL)                             \
        {                                                                                                              \
            (request)->free_cb((request)->payload);                                                                    \
            (request)->payload = NULL;                                                                                 \
            (request)->payload_len = 0;                                                                                \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

typedef struct vg_http_api_response_t
{
    void (*free_cb)(void *body);
    void *body;
    size_t body_len;
} vg_http_api_response_t;

#define VG_HTTP_API_SAFE_RELEASE_RESPONSE(response)                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((response) != NULL && (response)->free_cb != NULL && (response)->body != NULL)                             \
        {                                                                                                              \
            (response)->free_cb((response)->body);                                                                     \
            (response)->body = NULL;                                                                                   \
            (response)->body_len = 0;                                                                                  \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

#ifdef __cplusplus
}
#endif

#endif
