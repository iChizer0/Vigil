#ifndef VIGIL_CTYPES_H
#define VIGIL_CTYPES_H

#ifdef VG_CTYPES_VERSION
#error "Duplicate definition of VG_CTYPES_VERSION"
#else
#define VG_CTYPES_VERSION "2026.10.29"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// - MARK: Common Enums and Types

typedef int vg_sta_e; /**< 0, or an errno value. */

typedef enum vg_log_level_e {
    VG_LOG_LEVEL_DEBUG = 0,
    VG_LOG_LEVEL_INFO = 1,
    VG_LOG_LEVEL_WARN = 2,
    VG_LOG_LEVEL_ERROR = 3,
    VG_LOG_LEVEL_FATAL = 4
} vg_log_level_e;

// - MARK: Media Enums and Data Structures

/**
 * One encoded video frame. Enqueued, data and bytes_len must describe a payload, which Vigil copies; in a write
 * callback they describe Vigil's copy, read-only and valid for the call. pts is passed through. A key frame is expected
 * at least every 10 seconds, the buffer period: with a longer GOP, clips are canceled or end without one.
 */
typedef struct vg_video_frame_t
{
    int64_t pts;
    void *data;
    size_t bytes_len;
    int key_frame; /**< 1 for a key frame (IDR); any other value is not one. */
} vg_video_frame_t;

/**
 * One encoded audio frame, with the payload contract of vg_video_frame_t.
 */
typedef struct vg_audio_frame_t
{
    int64_t pts;
    void *data;
    size_t bytes_len;
} vg_audio_frame_t;

/**
 * The media callbacks. A nonzero status is a failure, logged and never retried; a failed file task counts toward its
 * channel's consecutive failure limit, which stops the recording Slot. Arguments are borrowed for the call.
 *
 * The file, capture, and thumbnail callbacks run on the Photo and Video workers and the capture thread, sound_play_cb
 * on the sound worker, all outside Vigil's locks: they may call any C API function but vg_service_init,
 * vg_service_deinit, the scoped suspension functions and, on a Photo or Video worker, the storage calls that eject,
 * format, or configure that worker's Slot.
 * max_bytes_estimate_cb runs on the threads of vg_service_init, vg_http_api_rule_config_post,
 * vg_http_api_capture_start_post, and vg_media_scoped_resume_write, possibly under a Vigil lock: keep it pure, with no
 * C API call.
 */
typedef struct vg_config_media_t
{
    void *user_data; /**< Passed to every media callback. */
    /** Optional. The bytes `batch` seconds of `format` video, or one `format` image (batch 1), may take; 0 or less
     * uses Vigil's table. */
    int64_t (*max_bytes_estimate_cb)(const char *format, size_t batch, void *user_data);
    /** Required for video recording and capture. Creates the sink at filepath (its directory exists) and returns 0
     * with a non-negative descriptor in *fd; anything else fails the task, and no descriptor may stay open then. Vigil
     * never closes it: it goes to video_file_finish_cb exactly once. */
    vg_sta_e (*video_file_create_cb)(int *fd, const char *filepath, const char *format, void *user_data);
    /** Required for video recording and capture. Completes the pending work and closes the sink; once per created
     * file, also after a failed write. */
    vg_sta_e (*video_file_finish_cb)(int fd, void *user_data);
    /** Required for video recording and capture. */
    vg_sta_e (*video_file_write_video_frame_cb)(int fd, const vg_video_frame_t *frame, void *user_data);
    /** Required for video recording and capture, even when no audio is ever enqueued. */
    vg_sta_e (*video_file_write_audio_frame_cb)(int fd, const vg_audio_frame_t *frame, void *user_data);
    /** Required for the Photo action and manual image capture. Captures a still of `format` into filepath (its
     * directory exists). */
    vg_sta_e (*image_capture_cb)(const char *filepath, const char *format, void *user_data);
    /** Optional. Writes a `thumb_format` image of at most max_wh pixels a side to thumb_filepath, from src_data, the
     * payload of the clip's first key frame as the host enqueued it, with src_format the clip's format (MP4); without
     * it, or when it fails, the video is published without a thumbnail. */
    vg_sta_e (*thumbnail_generate_cb)(const void *src_data, size_t src_data_len, const char *src_format,
        const char *thumb_filepath, const char *thumb_format, int max_wh, void *user_data);
    /** Optional; without it the Sound action is unavailable. Plays the configured file (an absolute path) or, with a
     * null filepath, the host's own sound, once, and returns when playback is finished; Vigil makes one call at a
     * time. It must complete in bounded time, also during shutdown: Vigil cannot interrupt it and waits for it. */
    vg_sta_e (*sound_play_cb)(const char *filepath, void *user_data);
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
    VG_RULE_TYPE_ALWAYS_ON = 8,
    VG_RULE_TYPE_SED = 9
} vg_rule_type_e;

/**
 * One dispatched rule event, borrowed for the duration of on_rule_event_cb; its strings are never null.
 */
typedef struct vg_rule_evt_t
{
    vg_rule_type_e type;         /**< The owning rule's type; UNKNOWN for a direct EVENT input. */
    const char *id;              /**< The condition: the inference member ID, the GPIO signal, the TTY command, or the
                                    SED rule ID; empty otherwise. */
    int64_t timestamp;           /**< Event time in Unix milliseconds: the input's timestamp for FRAME and direct EVENT
                                    owners, otherwise when the condition was observed. */
    uint64_t uid;                /**< Unique per rule event; the files it produces carry it as event_uid. */
    const char *source_id;       /**< The inference source for FRAME and direct EVENT owners, the pin name for GPIO,
                                    the TTY name for TTY; empty otherwise. */
    const char *source_delivery; /**< "FRAME" for inference frame owners, "EVENT" for every other owner. */
    bool writer_created;         /**< At least one file task was accepted for this event: not whether a file was
                                    written, and nothing about Sound. */
    int64_t dispatch_timestamp;  /**< When Vigil created the event, in Unix milliseconds; equals timestamp for owners
                                    that observe their condition themselves. */
    const uint64_t *file_ids;    /**< The files for this event, as the file_id of their file callbacks, borrowed for
                                    the call and never null: per file channel, Photo before Video, the task accepted
                                    for it, else the one PENDING or WRITING as the channel refused it, a continuous
                                    clip included; a channel with neither adds nothing, and an id's channel is told
                                    by the extension of its file callback's path, or by rule info while the task is
                                    held. The tasks may still be canceled or fail. */
    size_t file_ids_len;         /**< The number of file_ids; 0 when no channel has one. */
} vg_rule_evt_t;

typedef enum vg_file_evt_op_type_e {
    VG_FILE_EVT_OP_ADDED = 1,
} vg_file_evt_op_type_e;

/**
 * One published file, borrowed with its strings for the duration of on_file_event_cb. A file is created for at most
 * one rule event and listed by every one dispatched while it was PENDING or WRITING; a rule event may produce zero,
 * one, or two files (Photo and Video).
 */
typedef struct vg_file_evt_t
{
    vg_file_evt_op_type_e op_type;
    const char *path; /**< Where the file was published. */
    size_t size_bytes;
    const char *attributes; /**< The task's status at publication: COMPLETED or INTERRUPTED. */
    int64_t timestamp;      /**< Publication time in Unix milliseconds, not the rule event's. */
    uint64_t event_uid;     /**< The rule event's uid; 0 for a continuous recording clip. */
    uint64_t file_id;       /**< This file's identity, never 0, as the rule events list it and rule info reports it
                               (iFileID). */
} vg_file_evt_t;

/**
 * The configuration root, the log, and the event callbacks.
 */
typedef struct vg_config_basic_t
{
    vg_log_level_e log_level; /**< Reserved: every message reaches log_cb, since filtering is compile-time
                                 (VG_LOG_LEVEL). */
    /** Optional. Called on any thread, possibly under Vigil's locks: return promptly and make no C API call. msg is
     * null-terminated and borrowed for the call; the status is ignored. */
    vg_sta_e (*log_cb)(vg_log_level_e level, const char *msg, size_t bytes_len);
    /** Optional. Once per dispatched rule event, after its file tasks were accepted or refused, on the dispatching
     * thread (the rule thread, or an HTTP activation's) with no Vigil lock held; return promptly. Not an ordered
     * stream: a file callback for the same event may come first. */
    void (*on_rule_event_cb)(const vg_rule_evt_t *evt);
    /** Optional. Once per file a recording Slot publishes (never for a manual capture), on the publishing worker
     * outside Vigil's locks; calls from the Photo and Video workers may overlap. Return promptly, and do not eject,
     * format, or configure the publishing Slot from it. */
    void (*on_file_event_cb)(const vg_file_evt_t *evt);
    const char *config_directory; /**< Required: where the configuration files are read at init and stored by the
                                     configuration POSTs. */
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
 * One GPIO pin the host offers; a GPIO rule selects it by num or name.
 */
typedef struct vg_config_gpio_pin_t
{
    int num;
    char name[VG_GPIO_PIN_NAME_MAX_LEN]; /**< Null-terminated. */
    vg_gpio_pin_mod_e state;             /**< The current mode. */
    vg_gpio_pin_mod_e capabilities;      /**< The supported modes, as a bitmask. */
} vg_config_gpio_pin_t;

/**
 * The host's pin list, filled by available_cb.
 */
typedef struct vg_config_gpio_pins_t
{
    void (*free_cb)(void *pins); /**< Optional: called once with the pins array when Vigil is done with it; null for a
                                    list the host keeps. */
    vg_config_gpio_pin_t *pins;
    size_t len;
} vg_config_gpio_pins_t;

/**
 * The GPIO callbacks, all optional; a GPIO rule needs available_cb, acquire_cb, release_cb, and the sync callback of
 * its signal mode. They run on the rule thread, on API request threads, and in init and deinit, possibly under the rule
 * lock and concurrently: make no C API call from them.
 */
typedef struct vg_config_gpios_t
{
    /** Fills every field of *pins and returns 0, or returns an error and leaves it alone. Vigil calls the list's
     * free_cb once, on the same thread, before returning. */
    vg_sta_e (*available_cb)(vg_config_gpio_pins_t *pins);
    /** Prepares a pin for a GPIO rule; a nonzero status refuses the rule. */
    vg_sta_e (*acquire_cb)(int pin, vg_gpio_pin_mod_e state, size_t debounce_ms);
    void (*release_cb)(int pin);                 /**< Once per acquisition: when the rule is replaced or the service
                                                    stops. */
    vg_gpio_pin_lvl_e (*sync_level_cb)(int pin); /**< The current level (level rules, rule info). */
    vg_gpio_pin_evt_e (*sync_event_cb)(int pin); /**< The edge observed since the last call (edge rules). */
} vg_config_gpios_t;

/**
 * One TTY: a TTY rule selects it by name and connects to its Unix socket, on which the host's peer listens.
 */
typedef struct vg_config_tty_t
{
    const char *name;        /**< Required. */
    const char *socket_path; /**< Required. */
    size_t buffer_size;      /**< The rule's read buffer, in bytes: nonzero, and a rule whose command does not fit is
                                refused. */
} vg_config_tty_t;

/**
 * The host's TTYs, borrowed with their strings until vg_service_deinit returns.
 */
typedef struct vg_config_ttys_t
{
    vg_config_tty_t *ttys;
    size_t len;
} vg_config_ttys_t;

/**
 * Storage callbacks and paths. The strings are borrowed until vg_service_deinit returns; those passed to the
 * callbacks, for the call.
 */
typedef struct vg_config_storage_t
{
    /** Optional. After a Slot is mounted, on the storage thread or the Slot's own, with no Vigil lock held. Return
     * promptly, and do not eject, format, or configure that Slot from it. */
    void (*on_mount_cb)(const char *dev, const char *mount_path);
    /** Optional. Before a Slot other than the internal one is unmounted, with Vigil's Slot locks held, on the storage
     * thread, the thread of an EJECT (the async task worker's for an async one), or that of vg_service_init or
     * vg_service_deinit: for every such Slot removed, ejected, or torn down, mounted or not, so it does not pair with
     * on_mount_cb. Return promptly and make no C API call. */
    void (*on_unmount_cb)(const char *dev, const char *mount_path);
    /** Optional; without it nothing is hidden. Whether the block device of that bare name (e.g. mmcblk0p1) is kept
     * out of storage management, which mounts any other partition it can under /mnt, selected or not, and opens its
     * root to 1000:1000, mode 0777. The internal device's Slot is built whatever the answer, but a hidden name's
     * device events are ignored. */
    bool (*is_hidden_device_cb)(const char *dev);
    /** The internal storage device as /dev/<kernel name>, e.g. /dev/mmcblk0p1, which the device events name, or null
     * for none. Another spelling (a symlink, /dev/block/...) matches no event, and unless the node is a whole disk or
     * hidden, the scan also builds an ordinary Slot over the same device, mounted again under /mnt. */
    const char *internal_device_path;
    /** The internal device's mount point, derived from its name when null. Mount paths, this one included, are
     * Vigil's own: before mounting a device at one, Vigil detaches what is mounted there and removes what it holds. */
    const char *internal_mount_point;
    /** The record data root under each mount point, e.g. DCIM, with no leading '/'; DCIM when null or invalid. */
    const char *data_directory_path;
    /** The nginx relay directory, Vigil's own: removed with its contents at init and created again on a relay
     * request; null disables the relay. */
    const char *relay_directory_path;
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

/**
 * A request body for a C API POST. Vigil reads the payload during the call only and never frees it; free_cb is for
 * the host's own use through VG_HTTP_API_SAFE_RELEASE_REQUEST.
 */
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

/**
 * A response body a C API function fills on success; free_cb releases it. Pass it zero-initialized or released with
 * VG_HTTP_API_SAFE_RELEASE_RESPONSE (EINVAL otherwise); on failure it is left untouched.
 */
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
            (response)->free_cb = NULL; /* released: the structure can be passed to a C API function again */          \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

#ifdef __cplusplus
}
#endif

#endif
