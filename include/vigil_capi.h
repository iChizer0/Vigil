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

/*
 * Every function is thread-safe and returns 0 or an errno value. All but the two lifecycle functions return ESRCH
 * without a runtime, also while one is being built or torn down, and may be called from a callback, on the thread of
 * the call that invoked it too, unless the callback's own description says otherwise. There is no size or version
 * negotiation: the library and the host are built against the same headers.
 */

// - MARK: Core API

/**
 * Builds the runtime from `config` and starts its threads; one runtime at a time.
 *
 * `config` is copied; its callbacks, user_data, strings, and ttys array are borrowed until vg_service_deinit returns.
 * config_directory is required. Every callback is optional unless its field says otherwise: a rule or action that
 * needs a missing one is refused, or fails when it runs. Storage management runs mount, fsck, and mkfs as child
 * processes: leave SIGCHLD at its default disposition and do not reap them, or they are reported as failed.
 *
 * @return 0; EINVAL for a null config or config_directory; EDEADLK from a callback on the thread of another C API
 * call; EALREADY while a runtime exists; EFAULT when the runtime could not be built (the cause goes to log_cb).
 */
vg_sta_e vg_service_init(const vg_config_t *config);

/**
 * Stops the runtime: joins every thread, then releases it.
 *
 * Neither this function nor vg_service_init may be called from a callback: on a Vigil thread, deinit would join the
 * thread it runs on, which terminates the process. A callback on a storage Slot's own thread (on_file_event_cb,
 * on_mount_cb, and the media callbacks of the Photo and Video workers) must not eject, format, or configure that Slot
 * either: an eject would join the thread the callback runs on, and the other two wait on a mutex a concurrent disable
 * holds while it joins that thread.
 *
 * @return 0; EDEADLK from a callback on the thread of another C API call.
 */
vg_sta_e vg_service_deinit(void);

// - MARK: Media API

/**
 * Copies one encoded video frame into the media buffer, which keeps the last 10 seconds, trimmed at a key frame. It
 * never waits for space, only for a buffer lock a writer holds briefly.
 *
 * Frames must keep arriving while recording can run: a clip ends after one buffer period without them, and a
 * triggered clip starts from the frames buffered before it, however old. While the pipeline is stopped, keep writing
 * suspended (vg_media_scoped_suspend_write) and end the suspension with vg_media_scoped_resume_write(true).
 *
 * @return 0; EINVAL for a null frame or one without a payload; EFAULT when it could not be buffered.
 */
vg_sta_e vg_media_enqueue_video_frame(const vg_video_frame_t *frame);

/**
 * The same for an audio frame, buffered with the video frames and trimmed with them.
 */
vg_sta_e vg_media_enqueue_audio_frame(const vg_audio_frame_t *frame);

/**
 * Suspends media writing until the matching vg_media_scoped_resume_write; suspensions nest. Nothing is admitted
 * meanwhile, and with `wait` the call returns only once every admitted media operation has ended (a clip from its
 * admission through its finish and thumbnail, an image capture, a sound playback). Neither this function nor the
 * resume is for a media callback.
 *
 * For a stopped or reconfigured pipeline. A suspension cancels a Sound trigger not yet playing and breaks the
 * continuity of continuous recording: the next clip starts at a fresh key frame, whatever the resume's
 * reset_media_queue. A clip checks the suspension as it starts and between frames, so it still writes the frames that
 * arrive as a suspension begins or ends; without `wait`, an admitted clip records through a suspension that has ended
 * by its next check.
 *
 * @return 0.
 */
vg_sta_e vg_media_scoped_suspend_write(bool wait);

/**
 * Ends the innermost suspension. The one that ends the outermost admits media again and, with `reset_media_queue`,
 * first drops the buffered frames: the buffer is trimmed only as frames arrive, so after a stall of the pipeline a
 * triggered clip's pre-roll and a capture would otherwise start with frames from before it.
 *
 * @return 0; ECANCELED when nothing is suspended (nothing is dropped then); EFAULT on failure.
 */
vg_sta_e vg_media_scoped_resume_write(bool reset_media_queue);

// - MARK: Inference API

/**
 * Enqueues one serialized InferenceResult (inference.proto) for the inference rules. 0 means queued, not consumed: a
 * FRAME is dropped while no inference rule is selected, any result while rules are disabled or the schedule is
 * closed, and the oldest of 60 queued results when another arrives; the rule tick consumes one every 10 ms.
 *
 * timestamp_ms is the event time in Unix milliseconds and is reported as such, 0 included: Vigil never substitutes
 * the current time. A FRAME needs a detection, tracking, keypoints, or classification result (keypoints are reduced to
 * their instances' object boxes, and an instance without one is dropped); an EVENT's result, if any, is ignored, and
 * it dispatches at once, bypassing the selected rule.
 *
 * @return 0; EINVAL for a null, empty, or oversized buffer, one that does not parse, or a negative timestamp_ms;
 * ENOTSUP for a FRAME without a supported result; EFAULT on failure.
 */
vg_sta_e vg_inference_enqueue_protobuf(const void *data, size_t data_len);

/**
 * Drops the results queued and not yet consumed: those of `source_id`, or all of them for null or empty. Nothing else
 * changes, a rule's debounce state included.
 *
 * @return 0; EFAULT on failure.
 */
vg_sta_e vg_inference_queue_reset(const char *source_id);

// - MARK: HTTP API

/*
 * The endpoints of docs/minimal-http-api.md. A POST reads `request` during the call and never frees it. On success a
 * function fills `response`, passed zero-initialized or released, with the JSON body (null-terminated, body_len
 * excluding the terminator) and its free_cb; the endpoint's own result is the body's `code`, which a POST always
 * carries and a GET only on failure. On failure `response` is left untouched.
 *
 * Returns 0; EINVAL for a null request or response, or a response not released; ENOMEM when the body could not be
 * allocated; EFAULT for a request body over 1 MiB or one that does not parse, or when the endpoint failed (the cause
 * goes to log_cb).
 */

// Rule endpoints
vg_sta_e vg_http_api_rule_config_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_rule_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

vg_sta_e vg_http_api_rule_info_get(vg_http_api_response_t *response);

vg_sta_e vg_http_api_rule_schedule_config_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_rule_schedule_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

vg_sta_e vg_http_api_rule_record_config_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_rule_record_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

vg_sta_e vg_http_api_rule_http_rule_activate_post(const vg_http_api_request_t *request,
    vg_http_api_response_t *response);

// Storage endpoints
vg_sta_e vg_http_api_storage_config_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_storage_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

vg_sta_e vg_http_api_storage_status_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_storage_control_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

// Capture endpoints
vg_sta_e vg_http_api_capture_status_get(vg_http_api_response_t *response);
vg_sta_e vg_http_api_capture_start_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);
vg_sta_e vg_http_api_capture_stop_post(const vg_http_api_request_t *request, vg_http_api_response_t *response);

#ifdef __cplusplus
}
#endif

#endif
