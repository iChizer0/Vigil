#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "inference.pb.h"
#include "vigil.hpp"
#include "vigil_capi.h"
#include "vigil_ctypes.h"

namespace vigil_c_api {

// Lifecycle (docs/PLAN.md section 8.3). `vigil_instance_lifecycle_mutex` serializes init and deinit for their
// whole duration. `vigil_instance_mutex` only guards the instance pointer: every other call holds it shared
// while it uses the runtime, and init/deinit take it exclusively just to publish or detach the pointer. The
// runtime is built and destroyed with no instance lock held, so a callback that reenters the C API meanwhile
// gets ESRCH instead of a deadlock, and no worker is joined under the entrance lock.
static inline std::mutex vigil_instance_lifecycle_mutex;
static inline std::shared_mutex vigil_instance_mutex;
static inline std::mutex vigil_instance_scoped_suspend_mutex;
static inline size_t vigil_instance_scoped_suspend_count = 0;
static inline std::unique_ptr<vigil::Vigil> vigil_instance = nullptr;

} // namespace vigil_c_api

using namespace vigil_c_api;

extern "C" {

static void vg_log_exception(const vg_config_t &config, const std::exception &e)
{
    if (config.basic.log_cb)
    {
        const auto msg = e.what();
        const auto len = std::strlen(msg);
        config.basic.log_cb(VG_LOG_LEVEL_ERROR, msg, len);
    }
}

// - MARK: Core API

vg_sta_e vg_service_init(vg_config_t *config)
{
    if (!config)
        return EINVAL;

    const std::lock_guard lifecycle(vigil_instance_lifecycle_mutex);
    {
        const std::shared_lock lock(vigil_instance_mutex);
        if (vigil_instance)
            return EALREADY;
    }

    std::unique_ptr<vigil::Vigil> instance;
    try
    {
        instance = std::make_unique<vigil::Vigil>(*config); // a throwing constructor unwinds what it started
    }
    catch (const std::exception &e)
    {
        vg_log_exception(*config, e);
        return EFAULT;
    }

    {
        const std::lock_guard lock2(vigil_instance_scoped_suspend_mutex);
        vigil_instance_scoped_suspend_count = 0; // Reset scoped suspend count on init
    }
    const std::unique_lock lock(vigil_instance_mutex);
    vigil_instance = std::move(instance);
    return 0;
}

vg_sta_e vg_service_deinit()
{
    const std::lock_guard lifecycle(vigil_instance_lifecycle_mutex);
    std::unique_ptr<vigil::Vigil> instance;
    {
        const std::unique_lock lock(vigil_instance_mutex);
        instance = std::move(vigil_instance); // every later call returns ESRCH
    }
    instance.reset(); // stops and joins the workers with no instance lock held
    return 0;
}

vg_sta_e vg_media_enqueue_video_frame(const vg_video_frame_t *frame)
{
    if (!frame)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        vigil_instance->context().enqueueMediaFrame(vigil::MediaFrame(*frame));
    }
    catch (const std::exception &e)
    {
        return EFAULT;
    }

    return 0;
}

vg_sta_e vg_media_enqueue_audio_frame(const vg_audio_frame_t *frame)
{
    if (!frame)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        vigil_instance->context().enqueueMediaFrame(vigil::MediaFrame(*frame));
    }
    catch (const std::exception &e)
    {
        return EFAULT;
    }

    return 0;
}

vg_sta_e vg_media_scoped_suspend_write(bool wait)
{
    std::shared_ptr<vigil::Context> ctx;
    {
        const std::shared_lock lock(vigil_instance_mutex);
        if (!vigil_instance)
            return ESRCH;
        ctx = vigil_instance->contextPtr();
    }

    // Drained with no instance lock held: a media callback may reenter the C API, and deinit may detach the
    // runtime meanwhile (its workers leave the barrier as they are joined). `ctx` keeps the barrier alive.
    const std::unique_lock lock2(vigil_instance_scoped_suspend_mutex);
    ctx->media_barrier.pause(wait);
    ++vigil_instance_scoped_suspend_count;
    return 0;
}

vg_sta_e vg_media_scoped_resume_write(bool reset_media_queue)
{
    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        const std::unique_lock lock2(vigil_instance_scoped_suspend_mutex);
        if (vigil_instance_scoped_suspend_count == 1)
        {
            vigil_instance->context().action_settings.sync(vigil_instance->context().c_api.media);
            if (reset_media_queue)
                vigil_instance->context().clearMediaBuffer();
            vigil_instance->context().media_barrier.resume();
            vigil_instance_scoped_suspend_count = 0;
            return 0;
        }
        else if (vigil_instance_scoped_suspend_count > 1)
        {
            --vigil_instance_scoped_suspend_count;
            return 0;
        }
        return ECANCELED;
    }
    catch (const std::exception &e)
    {
        return EFAULT;
    }
}

vg_sta_e vg_inference_enqueue_protobuf(const void *data, size_t data_len)
{
    if (!data || data_len == 0)
        return EINVAL;

    if (data_len > static_cast<size_t>(std::numeric_limits<int>::max()))
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        ::InferenceResult proto_result;
        if (!proto_result.ParseFromArray(data, static_cast<int>(data_len)))
            return EINVAL;

        vigil::InferenceResult result;
        result.timestamp_ms = proto_result.timestamp_ms();
        if (!proto_result.source_id().empty())
            result.source_id = proto_result.source_id();
        if (proto_result.delivery() == INFERENCE_DELIVERY_EVENT)
        {
            vigil_instance->context().enqueueInferenceResult(std::move(result));
            return 0;
        }

        auto scale = [](float val, size_t max_dim) -> vigil::DetectionResult::value_type {
            using value_type = vigil::DetectionResult::value_type;
            if (!std::isfinite(val))
                return 0;

            const auto scaled = std::round(val * static_cast<float>(max_dim));
            if (scaled <= static_cast<float>(std::numeric_limits<value_type>::min()))
                return std::numeric_limits<value_type>::min();
            if (scaled >= static_cast<float>(std::numeric_limits<value_type>::max()))
                return std::numeric_limits<value_type>::max();
            return static_cast<value_type>(scaled);
        };

        auto append_detection = [&scale](vigil::DetectionResultVec &detections, const auto &entry) {
            vigil::DetectionResult det;
            det.class_name = entry.class_name();
            det.confidence = entry.score();

            det.object.top_left.x = scale(entry.box().left(), vigil::kMaskWidth);
            det.object.top_left.y = scale(entry.box().top(), vigil::kMaskHeight);
            det.object.bottom_right.x = scale(entry.box().right(), vigil::kMaskWidth);
            det.object.bottom_right.y = scale(entry.box().bottom(), vigil::kMaskHeight);

            detections.push_back(std::move(det));
        };

        if (proto_result.has_detection())
        {
            vigil::DetectionResultVec detections;
            detections.reserve(proto_result.detection().entries_size());
            for (const auto &entry: proto_result.detection().entries())
                append_detection(detections, entry);
            result.data = std::move(detections);
        }
        else if (proto_result.has_tracking())
        {
            // The rule engine accepts detection/classification variants only.
            // A tracked object's current class, confidence, and box are enough
            // to evaluate detection rules; its track_id is not represented.
            vigil::DetectionResultVec detections;
            detections.reserve(proto_result.tracking().entries_size());
            for (const auto &entry: proto_result.tracking().entries())
                append_detection(detections, entry);
            result.data = std::move(detections);
        }
        else if (proto_result.has_keypoints())
        {
            // A keypoint instance can supply an object-level class, confidence,
            // and box. Project those instances to detections for the rule engine;
            // point-only instances have no representable rule-engine result.
            vigil::DetectionResultVec detections;
            detections.reserve(proto_result.keypoints().instances_size());
            for (const auto &instance: proto_result.keypoints().instances())
            {
                if (!instance.has_object() || !instance.object().has_box())
                    continue;
                append_detection(detections, instance.object());
            }
            result.data = std::move(detections);
        }
        else if (proto_result.has_classification())
        {
            vigil::ClassificationResultVec classifications;
            classifications.reserve(proto_result.classification().entries_size());
            for (const auto &entry: proto_result.classification().entries())
            {
                vigil::ClassificationResult cls;
                cls.class_name = entry.class_name();
                cls.confidence = entry.score();
                classifications.push_back(std::move(cls));
            }
            result.data = std::move(classifications);
        }
        else
        {
            return ENOTSUP;
        }

        vigil_instance->context().enqueueInferenceResult(std::move(result));
    }
    catch (const std::exception &e)
    {
        return EFAULT;
    }

    return 0;
}

vg_sta_e vg_inference_queue_reset(const char *source_id)
{
    if (!source_id)
        source_id = "";

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        vigil_instance->context().resetInference(source_id);
    }
    catch (const std::exception &e)
    {
        return EFAULT;
    }
    return 0;
}

// - MARK: HTTP API

static vg_sta_e vg_http_api_json_2_response(const nlohmann::json &j, vg_http_api_response_t &response)
{
    const auto response_str = j.dump();
    size_t len = response_str.size();
    char *body = static_cast<char *>(std::malloc(len + 1));
    if (!body)
        return ENOMEM;
    std::memcpy(body, response_str.data(), len);
    body[len] = '\0';
    response.free_cb = std::free;
    response.body = body;
    response.body_len = len;
    return 0;
}

static nlohmann::json vg_http_api_request_2_json(const vg_http_api_request_t &request)
{
    if (!request.payload || request.payload_len == 0)
        return {};
    if (request.payload_len > vigil::kHTTPAPIRequestMaxBytes) // bounded before parsing (docs/PLAN.md section 5.2)
        throw vigil::Exception("Request body of " + std::to_string(request.payload_len) + " bytes exceeds the limit of "
                               + std::to_string(vigil::kHTTPAPIRequestMaxBytes) + " bytes");
    return nlohmann::json::parse(static_cast<const char *>(request.payload),
        static_cast<const char *>(request.payload) + request.payload_len);
}

vg_sta_e vg_http_api_rule_config_get(vg_http_api_response_t *response)
{
    if (!response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto res_j = vigil_instance->ruleEndpoint().getRecordConfig();
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_rule_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response)
{
    if (!request || !response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto req_j = vg_http_api_request_2_json(*request);
        auto res_j = vigil_instance->ruleEndpoint().updateRecordConfig(req_j, true);
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_rule_schedule_config_get(vg_http_api_response_t *response)
{
    if (!response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto res_j = vigil_instance->ruleEndpoint().getScheduleRuleConfig();
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_rule_schedule_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response)
{
    if (!request || !response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto req_j = vg_http_api_request_2_json(*request);
        auto res_j = vigil_instance->ruleEndpoint().updateScheduleRuleConfig(req_j, true);
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_rule_info_get(vg_http_api_response_t *response)
{
    if (!response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto res_j = vigil_instance->ruleEndpoint().getRuleInfo();
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_rule_record_config_get(vg_http_api_response_t *response)
{
    if (!response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto res_j = vigil_instance->ruleEndpoint().getRecordRuleConfig();
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_rule_record_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response)
{
    if (!request || !response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto req_j = vg_http_api_request_2_json(*request);
        auto res_j = vigil_instance->ruleEndpoint().updateRecordRuleConfig(req_j, true);
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_rule_http_rule_activate_post(const vg_http_api_request_t *request,
    vg_http_api_response_t *response)
{
    if (!request || !response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto req_j = vg_http_api_request_2_json(*request);
        auto res_j = vigil_instance->ruleEndpoint().doHTTPRuleActivate(req_j);
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_storage_config_get(vg_http_api_response_t *response)
{
    if (!response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto res_j = vigil_instance->storageEndpoint().getStorageConfig();
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_storage_config_post(const vg_http_api_request_t *request, vg_http_api_response_t *response)
{
    if (!request || !response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto req_j = vg_http_api_request_2_json(*request);
        auto res_j = vigil_instance->storageEndpoint().updateStorageConfig(req_j, true);
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_storage_status_get(vg_http_api_response_t *response)
{
    if (!response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto res_j = vigil_instance->storageEndpoint().getStorageStatus();
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_storage_control_post(const vg_http_api_request_t *request, vg_http_api_response_t *response)
{
    if (!request || !response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto req_j = vg_http_api_request_2_json(*request);
        auto res_j = vigil_instance->storageEndpoint().doStorageControl(req_j);
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_capture_status_get(vg_http_api_response_t *response)
{
    if (!response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto res_j = vigil_instance->captureEndpoint().getCaptureStatus();
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_capture_start_post(const vg_http_api_request_t *request, vg_http_api_response_t *response)
{
    if (!request || !response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto req_j = vg_http_api_request_2_json(*request);
        auto res_j = vigil_instance->captureEndpoint().doCaptureStart(req_j);
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

vg_sta_e vg_http_api_capture_stop_post(const vg_http_api_request_t *request, vg_http_api_response_t *response)
{
    if (!request || !response || response->free_cb || response->body)
        return EINVAL;

    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        auto req_j = vg_http_api_request_2_json(*request);
        auto res_j = vigil_instance->captureEndpoint().doCaptureStop(req_j);
        return vg_http_api_json_2_response(res_j, *response);
    }
    catch (const std::exception &e)
    {
        vg_log_exception(vigil_instance->context().c_api, e);
        return EFAULT;
    }
}

} // extern "C"
