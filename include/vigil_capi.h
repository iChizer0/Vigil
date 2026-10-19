#ifndef VIGIL_CAPI_H
#define VIGIL_CAPI_H

#ifdef VG_CAPI_VERSION
#error "Duplicate definition of VG_CAPI_VERSION"
#else
#define VG_CAPI_VERSION "2025.11.25"
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
 * @param config Mutable configuration structure describing host callbacks.
 * @return vg_sta_e 0 on success, otherwise an error code.
 * @note May launch child threads for media processing and HTTP serving, and the Unix socket path used for TTY should be
 * created by the caller.
 */
vg_sta_e vg_service_init(vg_config_t *config);

/**
 * @brief Shuts down the Vigil runtime and releases any allocated resources (thread-safe).
 *
 * @return vg_sta_e 0 when teardown succeeds; error status otherwise.
 * @note Blocks until all child threads have exited and resources are cleaned up.
 */
vg_sta_e vg_service_deinit();

// - MARK: Media API

/**
 * @brief Queues a video frame for processing or archival by Vigil (thread-safe).
 *
 * @param frame Pointer to the frame metadata and payload description.
 * @return vg_sta_e 0 when accepted, or an error status on failure.
 * @note May block if internal buffers are full until space becomes available when recording.
 */
vg_sta_e vg_media_enqueue_video_frame(const vg_video_frame_t *frame);

/**
 * @brief Queues an audio frame corresponding to the given source (thread-safe).
 *
 * @param frame Pointer to the audio frame metadata and payload description.
 * @return vg_sta_e 0 when accepted, or an error status on failure.
 * @note May block if internal buffers are full until space becomes available when recording.
 */
vg_sta_e vg_media_enqueue_audio_frame(const vg_audio_frame_t *frame);

/**
 * @brief Suspends media writing operations, optionally waiting for in-flight writes to complete (thread-safe).
 *
 * @param wait When true, blocks until every admitted media operation (video create/write/finish, image capture,
 * thumbnail generation) has completed; nothing new is admitted until the matching resume.
 * @return vg_sta_e 0 on success, otherwise an error status (except EAGAIN when wait is false).
 * @note While suspended, incoming media frames may be dropped if internal buffers fill up. Generally used when the
 * frame settings are being changed to avoid partial writes.
 */
vg_sta_e vg_media_scoped_suspend_write(bool wait);

/**
 * @brief Resumes media writing operations, optionally resetting the internal media queue (thread-safe).
 *
 * @param reset_media_queue When true, clears any queued media frames that were not yet written.
 * @return vg_sta_e 0 on success, otherwise an error status.
 * @note Use with reset_media_queue=true to discard any stale media frames that may have accumulated while writing was
 * suspended.
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
