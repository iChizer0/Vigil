#ifndef VIGIL_CTYPES_H
#define VIGIL_CTYPES_H

#ifdef VG_CTYPES_VERSION
#error "Duplicate definition of VG_CTYPES_VERSION"
#else
#define VG_CTYPES_VERSION "2026.10.20"
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

/**
 * @brief One encoded video frame. Enqueued by the host, it must carry a payload (non-null data, positive
 * bytes_len), which Vigil copies during the call. Handed to a write callback, data is Vigil's copy: read-only
 * and valid for the duration of the call.
 */
typedef struct vg_video_frame_t
{
    int64_t pts;
    void *data;
    size_t bytes_len;
    int key_frame; /**< 1 when the frame is a key frame (IDR), 0 otherwise; no other value counts as one. */
} vg_video_frame_t;

/**
 * @brief One encoded audio frame, with the payload contract of vg_video_frame_t.
 */
typedef struct vg_audio_frame_t
{
    int64_t pts;
    void *data;
    size_t bytes_len;
} vg_audio_frame_t;

/**
 * @brief Callback surface for media storage, capture, and playback operations.
 *
 * Allows the host application to provide file lifecycle hooks, image capture handling, and sound playback for
 * the recordings and actions initiated by Vigil.
 *
 * Status: 0 is success, and any other value, positive or negative, is a failure that Vigil logs with the value
 * and never retries; a failed file task counts toward the per-channel consecutive failure limit that stops the
 * recording Slot. Strings and buffers passed to a callback are borrowed for the duration of the call. Threads:
 * the file, capture, and thumbnail callbacks run on Vigil's Photo and Video workers and on the capture thread,
 * and sound_play_cb on the sound worker, all outside Vigil's locks, so they may call the C API except the scoped
 * suspension functions (see vg_media_scoped_suspend_write); max_bytes_estimate_cb is called on the thread of
 * vg_service_init, of a record configuration or manual capture POST, and of vg_media_scoped_resume_write, there
 * under the suspension lock, so keep it a pure function that does not call the C API.
 *
 * @note Layout: sound_play_cb was appended in 2026.10.19. Growing this structure shifts the fields that follow it
 * in vg_config_t, and the initialization API has no size or version negotiation, so the library and every caller
 * must be built against the same headers; an older binary cannot be mixed with the new layout.
 */
typedef struct vg_config_media_t
{
    void *user_data; /**< User data pointer passed to all media callbacks. */
    int64_t (*max_bytes_estimate_cb)(const char *format, size_t batch,
        void *user_data); /**< Optional. The bytes that `batch` seconds of a `format` video, or one `format` image
                             (batch 1), may take; without it, or when it returns 0 or less, Vigil's built-in table
                             is used. */
    vg_sta_e (*video_file_create_cb)(int *fd, const char *filepath, const char *format,
        void *user_data); /**< Required for video recording. Creates the media sink at filepath (its directory
                             exists) and returns 0 with a usable descriptor in *fd. Any other return, or 0 with a
                             negative *fd, fails the task; *fd is then ignored, so the host must not leave a
                             descriptor open. The descriptor is passed to the write callbacks and then to
                             video_file_finish_cb exactly once; Vigil never closes it. */
    vg_sta_e (
        *video_file_finish_cb)(int fd, void *user_data); /**< Required for video recording. Completes any pending work
                             and closes the media sink; called once per created file, also after a failed write. */
    vg_sta_e (*video_file_write_video_frame_cb)(int fd, const vg_video_frame_t *frame,
        void *user_data); /**< Required for video recording. Writes a video frame into the active recording. */
    vg_sta_e (*video_file_write_audio_frame_cb)(int fd, const vg_audio_frame_t *frame,
        void *user_data); /**< Required for video recording, even when no audio is ever enqueued. Writes an audio
                             frame into the active recording. */
    vg_sta_e (*image_capture_cb)(const char *filepath, const char *format,
        void *user_data); /**< Required for the Photo action and manual image capture. Captures a still image of
                             the given format and writes it to filepath (its directory exists). */
    vg_sta_e (*thumbnail_generate_cb)(const void *src_data, size_t src_data_len, const char *src_format,
        const char *thumb_filepath, const char *thumb_format, int max_wh, void *user_data); /**< Optional. Generates
                         a thumbnail image from the source image data and writes it to thumb_filepath; without it,
                         or when it fails, the video is published without a thumbnail. */
    vg_sta_e (*sound_play_cb)(void *user_data); /**< Optional (null means the Sound action is unavailable). Plays the
                         configured sound once and returns when playback and its cleanup are finished; 0 on success,
                         otherwise an error that is logged and not retried. Called from Vigil's sound worker, outside
                         internal locks, at most once at a time. It must complete in bounded time, also while the
                         host is shutting down: Vigil cannot interrupt it and waits for it before joining. */
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
 * @brief One dispatched rule event, as handed to on_rule_event_cb. The structure and its strings are borrowed
 * for the duration of the callback: copy what you keep.
 */
typedef struct vg_rule_evt_t
{
    vg_rule_type_e type;         /**< Type of the rule that owns the event; UNKNOWN for a direct EVENT input. */
    const char *id;              /**< The condition: the inference member ID, the GPIO signal, or the TTY command; empty
                                    otherwise. Internal managed string, null terminated, do not free. */
    int64_t timestamp;           /**< Event time in Unix milliseconds: the input's timestamp for FRAME and direct EVENT
                                    owners, otherwise when the condition was observed. */
    uint64_t uid;                /**< Unique identifier for the rule event, can be used for event correlation. */
    const char *source_id;       /**< The inference source for FRAME and direct EVENT owners, the pin name for GPIO, the
                                    TTY name for TTY; empty otherwise (never null). */
    const char *source_delivery; /**< FRAME for inference frame owners, EVENT for every other owner (never empty). */
    bool writer_created;         /**< At least one file task was accepted for this event; not whether a file was
                                    written, and nothing about Sound. */
    int64_t dispatch_timestamp;  /**< When Vigil created the event, in Unix milliseconds; equals timestamp for owners
                                    that observe their condition themselves. Appended in 2026.10.19. */
} vg_rule_evt_t;

typedef enum vg_file_evt_op_type_e {
    VG_FILE_EVT_OP_ADDED = 1,
} vg_file_evt_op_type_e;

/**
 * @brief One published file, as handed to on_file_event_cb. The structure and its strings are borrowed for the
 * duration of the callback: copy what you keep. A file serves at most one rule event (event_uid), a rule event
 * may produce zero, one, or two files (Photo and Video), and a continuous recording clip serves none.
 */
typedef struct vg_file_evt_t
{
    vg_file_evt_op_type_e op_type; /**< Type of file event operation. */
    const char *path;              /**< Internal managed string, null terminated, do not free. */
    size_t size_bytes;             /**< Size of the file in bytes. */
    const char *attributes;        /**< Internal managed string, null terminated, do not free. */
    int64_t timestamp;             /**< Publication time in Unix milliseconds; the rule event's own time is in the rule
                                        callback. */
    uint64_t event_uid; /**< UID of the rule event the file serves, for correlation; 0 for a continuous recording
                             clip, which serves no rule event. */
} vg_file_evt_t;

/**
 * @brief Basic runtime configuration for the Vigil library.
 *
 * Supplies the configuration root, the logging callback, and the event callbacks.
 */
typedef struct vg_config_basic_t
{
    vg_log_level_e log_level; /**< Reserved: every message reaches log_cb, since filtering is compile-time
                                 (VG_LOG_LEVEL). */
    vg_sta_e (*log_cb)(vg_log_level_e level, const char *msg, size_t bytes_len); /**< Optional. Called on any
                         thread, possibly under Vigil's locks: return promptly and do not call the C API from it.
                         msg is borrowed for the call and null-terminated; the status is ignored. */
    void (*on_rule_event_cb)(const vg_rule_evt_t *evt); /**< Optional. Called once per dispatched rule event, on the
                         thread that dispatched it (the rule thread, or the HTTP request thread for an HTTP
                         activation), after the event's file tasks were accepted or refused, with no Vigil lock
                         held, so it may call the C API, also inside an HTTP activation, which invokes it on the
                         activation's thread. Reporting is best-effort, not an ordered stream: a file callback for
                         the same event may arrive before this one, and an event is followed by zero, one, or two
                         file callbacks (Photo and Video), none announced in advance. Return promptly, and do not
                         call vg_service_init or vg_service_deinit from it. */
    void (*on_file_event_cb)(const vg_file_evt_t *evt); /**< Optional. Called once per file a recording Slot
                         publishes (manual capture files are not reported), from the worker that published it,
                         after it released its media operation and the publication lock; a slot's Photo and Video
                         workers publish independently, so two calls may overlap. Return promptly, and do not call
                         vg_service_init or vg_service_deinit from it. */
    const char *config_directory;                       /**< Required (EINVAL when null): the configuration root,
                                                           read during vg_service_init. */
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
    void (*free_cb)(void *pins); /**< Optional: called once with pins when Vigil is done with the list; null for a
                                    list the host keeps. */
    vg_config_gpio_pin_t *pins;
    size_t len;
} vg_config_gpio_pins_t;

/**
 * @brief GPIO callbacks. All are optional: without available_cb no pin is listed in rule info and no GPIO rule
 * can be selected; a GPIO rule also needs acquire_cb, release_cb, and the sync callback of its signal mode.
 * They run on the rule thread, on the thread of a record rule configuration POST or a rule info GET, and on the
 * vg_service_init and vg_service_deinit threads for a stored rule; sync_level_cb may be called from the rule
 * thread and a rule info GET at the same time. Do not call the C API from them: a record rule POST and the rule
 * thread hold the rule lock while they run.
 */
typedef struct vg_config_gpios_t
{
    vg_sta_e (*available_cb)(vg_config_gpio_pins_t *pins); /**< Fills every field of *pins and returns 0, or returns
                         an error and leaves it alone (a nonzero status lists no pin and refuses a GPIO rule). Vigil
                         reads the list and calls its free_cb once, on the same thread, before returning, also when
                         parsing the rule fails. */
    vg_sta_e (*acquire_cb)(int pin, vg_gpio_pin_mod_e state, size_t debounce_ms); /**< Prepares a pin for a GPIO rule; a
                               nonzero status refuses the rule and is logged with the value. */
    void (*release_cb)(int pin);                 /**< Releases an acquired pin: once per acquisition, when the rule is
                                                    replaced or the service stops. */
    vg_gpio_pin_lvl_e (*sync_level_cb)(int pin); /**< Reads the current level of a pin (level rules, rule info). */
    vg_gpio_pin_evt_e (*sync_event_cb)(int pin); /**< Returns the edge observed since the last call (edge rules). */
} vg_config_gpios_t;

/**
 * @brief Configuration for a single TTY endpoint.
 *
 * Maps a logical name to the Unix domain socket used for communication.
 */
typedef struct vg_config_tty_t
{
    const char *name;        /**< Required: the name a TTY rule selects, shown in rule info. */
    const char *socket_path; /**< Required: the Unix socket a TTY rule connects to (an entry without one cannot
                                be selected). */
    size_t buffer_size;      /**< Size of the internal read buffer for the TTY. */
} vg_config_tty_t;

/**
 * @brief The host's TTYs. The array and its strings are borrowed until vg_service_deinit returns: Vigil reads
 * them whenever a TTY rule is parsed and on every rule info GET.
 */
typedef struct vg_config_ttys_t
{
    vg_config_tty_t *ttys;
    size_t len;
} vg_config_ttys_t;

/**
 * @brief Storage callbacks and paths. The strings are borrowed until vg_service_deinit returns: the device and
 * mount paths are read by the storage thread's scan and on every manual capture start, the relay path whenever
 * a Slot is created, and the data directory at init. The strings passed to the callbacks are borrowed for the
 * duration of the call.
 */
typedef struct vg_config_storage_t
{
    void (*on_mount_cb)(const char *dev, const char *mount_path);   /**< Optional. Called after a Slot is mounted,
                         on the storage or the Slot's thread, with no Vigil lock held. */
    void (*on_unmount_cb)(const char *dev, const char *mount_path); /**< Optional. Called from the destructor of a
                         Slot other than the internal one, before it is unmounted, on the thread that releases it
                         (storage, an HTTP request, or deinit), with Vigil's Slot locks held: return promptly and do
                         not call the C API from it. */
    bool (*is_hidden_device_cb)(const char *dev); /**< Optional (nothing is hidden without it). Whether the block
                                                    device of that bare name (e.g. mmcblk0p1) is kept out of storage
                                                    management; its answer is ignored for internal_device_path. */
    const char *internal_device_path; /**< Device path to the internal storage device, e.g. /dev/mmcblk0p1; null for
                                         no internal device. */
    const char *internal_mount_point; /**< Filesystem path to the internal storage mount point; derived from the
                                         device name when null. */
    const char *data_directory_path;  /**< Data storage root relative to a mount point, e.g. DCIM, no leading '/';
                                         DCIM when null or invalid. */
    const char *relay_directory_path; /**< Filesystem path to the nginx relay directory for Vigil relay data; null
                                         disables the relay. */
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
 * @brief A request body for a C API POST. Vigil reads the payload during the call only and never frees it;
 * free_cb is for the host's own use through VG_HTTP_API_SAFE_RELEASE_REQUEST.
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
 * @brief A response body filled by a C API function on success: free_cb releases body. Pass it zero-initialized,
 * or released with VG_HTTP_API_SAFE_RELEASE_RESPONSE: a structure that still holds a body or a free_cb is refused
 * with EINVAL. On failure the structure is left untouched.
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
