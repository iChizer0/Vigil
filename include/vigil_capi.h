#ifndef VIGIL_CAPI_H
#define VIGIL_CAPI_H

#ifdef VG_CAPI_VERSION
#error "Duplicate definition of VG_CAPI_VERSION"
#else
#define VG_CAPI_VERSION "2026.10.29"
#endif

#include "vigil_ctypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// - MARK: Core API

/**
 * @brief Initializes the Vigil runtime with the provided configuration surface.
 *
 * Initializes internal subsystems and registers the supplied callbacks so
 * media, GPIO, and HTTP integrations become active for subsequent calls (singleton, thread-safe).
 *
 * @param config The configuration, copied: the callbacks, user_data, strings, and the ttys array it points to are
 * borrowed until vg_service_deinit returns. config_directory is required and every callback optional unless its
 * field says otherwise; a rule or an action that needs a missing callback is refused or fails when it runs.
 * @return vg_sta_e 0 on success; EINVAL for a null config or config_directory, EALREADY while a runtime exists,
 * EDEADLK from a callback on the thread of another C API call, EFAULT when the runtime could not be built (the cause
 * goes to log_cb).
 * @note May launch child threads for media processing and HTTP serving, and the Unix socket path used for TTY should be
 * created by the caller. Storage management runs mount, fsck, and mkfs as child processes: the host leaves SIGCHLD at
 * its default disposition and does not reap Vigil's children, or those commands are reported as failed.
 */
vg_sta_e vg_service_init(vg_config_t *config);

/**
 * @brief Shuts down the Vigil runtime and releases any allocated resources (thread-safe).
 *
 * @return vg_sta_e 0 when teardown succeeds; EDEADLK from a callback on the thread of another C API call.
 * @note Blocks until all child threads have exited and resources are cleaned up. Neither this function nor
 * vg_service_init may be called from a callback: on a Vigil thread, deinit would join the thread it runs on, which
 * terminates the process. Any other C API function may, also on the thread of the call that invoked it, unless the
 * callback's own description says otherwise; while the runtime is built or torn down such a call returns ESRCH. A
 * callback on a storage Slot's own thread (on_file_event_cb, on_mount_cb, and the Photo and Video workers' media
 * callbacks) must not eject, format, or configure that Slot: an eject would join the thread the callback runs on, and
 * a format or a configuration would wait on the Slot's mutex while a concurrent disable holds it to join that thread.
 */
vg_sta_e vg_service_deinit();

// - MARK: Media API

/**
 * @brief Queues a video frame for processing or archival by Vigil (thread-safe).
 *
 * @param frame Pointer to the frame metadata and payload description; the payload is copied during the call.
 * @return vg_sta_e 0 when accepted, EINVAL for a null frame or one without a payload, or an error status on failure.
 * @note Never waits for space, only for a buffer lock a writer holds briefly: the buffers keep the last 10 seconds,
 * trimmed at a key frame. Frames must keep arriving while recording can run: a clip ends at the input-loss wait
 * without them, and a triggered clip starts from the frames buffered before it, however old. While its pipeline is
 * stopped, the host keeps writing suspended (vg_media_scoped_suspend_write) and ends the suspension with
 * vg_media_scoped_resume_write(true).
 */
vg_sta_e vg_media_enqueue_video_frame(const vg_video_frame_t *frame);

/**
 * @brief Queues an audio frame corresponding to the given source (thread-safe).
 *
 * @param frame Pointer to the audio frame metadata and payload description; the payload is copied during the call.
 * @return vg_sta_e 0 when accepted, EINVAL for a null frame or one without a payload, or an error status on failure.
 * @note Buffered as video frames are, trimmed at a video key frame.
 */
vg_sta_e vg_media_enqueue_audio_frame(const vg_audio_frame_t *frame);

/**
 * @brief Suspends media writing operations, optionally waiting for in-flight writes to complete (thread-safe).
 *
 * @param wait When true, blocks until every admitted media operation (video create/write/finish, image capture,
 * thumbnail generation, sound playback) has completed; nothing new is admitted until the matching resume. Not to be
 * called from a media callback: a waiting suspension drains the callbacks under the lock this function takes.
 * @return vg_sta_e 0 on success, ESRCH without a runtime.
 * @note While suspended, incoming media frames may be dropped if internal buffers fill up. Generally used when the
 * frame settings are being changed to avoid partial writes. A suspension also cancels a Sound trigger not yet playing,
 * and ends the continuity of continuous recording: the clip after it starts at a fresh key frame, whatever the
 * resume's reset_media_queue. A video clip checks the suspension as it starts and between frames, so it can still
 * write the frames that arrive as one begins or ends; without wait, an admitted clip records through a suspension
 * that has ended by its next check.
 */
vg_sta_e vg_media_scoped_suspend_write(bool wait);

/**
 * @brief Resumes media writing operations, optionally resetting the internal media queue (thread-safe).
 *
 * @param reset_media_queue When true, clears any queued media frames that were not yet written.
 * @return vg_sta_e 0 on success, ECANCELED when no suspension is in progress (nothing is cleared then), otherwise an
 * error status.
 * @note Use with reset_media_queue=true to discard any stale media frames that may have accumulated while writing was
 * suspended, also to end a suspension that spans a stall of the host's pipeline: the buffer is trimmed only as frames
 * arrive, so without the reset a triggered clip's pre-roll and a capture start with the frames from before the stall.
 * Only the resume that ends the outermost suspension clears them.
 */
vg_sta_e vg_media_scoped_resume_write(bool reset_media_queue);

// - MARK: Inference API

/**
 * @brief Submits an opaque protobuf payload describing inference results (thread-safe).
 *
 * Allows custom inference schemas to be forwarded to Vigil for storage or
 * downstream handling without first unpacking them in the C API boundary.
 *
 * @param data Pointer to the protobuf-encoded message buffer.
 * @param data_len Length of the protobuf buffer in bytes.
 * @return vg_sta_e 0 when accepted, otherwise error status.
 * @note The message's timestamp_ms is the event time in Unix milliseconds, reported as such for FRAME and direct
 * EVENT owners (0 too: Vigil never substitutes the current time); it must not be negative.
 */
vg_sta_e vg_inference_enqueue_protobuf(const void *data, size_t data_len);

/**
 * @brief Invalidates queued state for one inference source specified by source_id (thread-safe).
 *
 * @param source_id Identifier of the inference source whose queued state should be reset, empty means all sources.
 * @return vg_sta_e 0 on success, otherwise an error status.
 */
vg_sta_e vg_inference_queue_reset(const char *source_id);

// - MARK: HTTP API

// Rule Endpoint APIs
vg_sta_e vg_http_api_rule_config_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_rule_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

vg_sta_e vg_http_api_rule_info_get(vg_http_api_response_t *response);

vg_sta_e vg_http_api_rule_schedule_config_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_rule_schedule_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

vg_sta_e vg_http_api_rule_record_config_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_rule_record_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

vg_sta_e vg_http_api_rule_http_rule_activate_post(const vg_http_api_request_t *request,
    vg_http_api_response_t *response);

// Storage Endpoint APIs
vg_sta_e vg_http_api_storage_config_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_storage_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

vg_sta_e vg_http_api_storage_status_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_storage_control_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

// Capture Endpoint APIs
vg_sta_e vg_http_api_capture_status_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_capture_start_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);
vg_sta_e vg_http_api_capture_stop_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

#ifdef __cplusplus
}
#endif

#endif
