#include <algorithm>
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

#include <nlohmann/json.hpp>

#include "inference.pb.h"
#include "vigil.hpp"
#include "vigil_capi.h"
#include "vigil_ctypes.h"

namespace vigil_c_api {

static inline std::shared_mutex vigil_instance_mutex; // Used for HTTP API
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

    const std::unique_lock lock(vigil_instance_mutex);
    const std::lock_guard lock2(vigil_instance_scoped_suspend_mutex);
    vigil_instance_scoped_suspend_count = 0; // Reset scoped suspend count on init
    try
    {
        if (vigil_instance)
            return EALREADY;
        vigil_instance = std::make_unique<vigil::Vigil>(*config);
        if (!vigil_instance)
            return ENOMEM;
    }
    catch (const std::exception &e)
    {
        vg_log_exception(*config, e);
        vigil_instance.reset();
        return EFAULT;
    }

    return 0;
}

vg_sta_e vg_service_deinit()
{
    const std::unique_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return 0;
    vigil_instance.reset();
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
    const std::shared_lock lock(vigil_instance_mutex);
    if (!vigil_instance)
        return ESRCH;

    try
    {
        const std::unique_lock lock2(vigil_instance_scoped_suspend_mutex);
        {
            auto &ctx = vigil_instance->context();
            ctx.media_paused.store(true, std::memory_order_release);
            while (wait && ctx.media_writers_active.load(std::memory_order_acquire) > 0)
                std::this_thread::yield(); // Wait for writers to finish
        }
        ++vigil_instance_scoped_suspend_count;
        return 0;
    }
    catch (const std::exception &e)
    {
        return EFAULT;
    }
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
            if (reset_media_queue)
                vigil_instance->context().clearMediaBuffer();
            vigil_instance->context().media_paused.store(false, std::memory_order_release);
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
        result.first = proto_result.timestamp_ms();

        if (proto_result.has_detection())
        {
            vigil::DetectionResultVec detections;
            detections.reserve(proto_result.detection().entries_size());

            auto scale = [](float val, size_t max_dim) -> vigil::DetectionResult::value_type {
                if (!std::isfinite(val))
                    return 0;
                auto scaled
                    = static_cast<vigil::DetectionResult::value_type>(std::round(val * static_cast<float>(max_dim)));
                return std::clamp(scaled, std::numeric_limits<vigil::DetectionResult::value_type>::min(),
                    std::numeric_limits<vigil::DetectionResult::value_type>::max());
            };

            for (const auto &entry: proto_result.detection().entries())
            {
                vigil::DetectionResult det;
                det.class_id = entry.class_id();
                det.confidence = entry.score();

                det.object.top_left.x = scale(entry.box().left(), vigil::kMaskWidth);
                det.object.top_left.y = scale(entry.box().top(), vigil::kMaskHeight);
                det.object.bottom_right.x = scale(entry.box().right(), vigil::kMaskWidth);
                det.object.bottom_right.y = scale(entry.box().bottom(), vigil::kMaskHeight);

                detections.push_back(std::move(det));
            }
            result.second = std::move(detections);
        }
        else if (proto_result.has_classification())
        {
            vigil::ClassificationResultVec classifications;
            classifications.reserve(proto_result.classification().entries_size());
            for (const auto &entry: proto_result.classification().entries())
            {
                vigil::ClassificationResult cls;
                cls.class_id = entry.class_id();
                cls.confidence = entry.score();
                classifications.push_back(std::move(cls));
            }
            result.second = std::move(classifications);
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
        auto res_j = vigil_instance->ruleEndpoint().getRuleConfig();
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
        auto res_j = vigil_instance->ruleEndpoint().updateRuleConfig(req_j, true);
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
