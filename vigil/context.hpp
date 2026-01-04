#pragma once
#ifndef VIGIL_CONTEXT_HPP
#define VIGIL_CONTEXT_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <utility>
#include <variant>

#include "constants.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil {

using namespace vigil::types;
using namespace vigil::constants;

struct Context
{
    // Logging utility
    template<typename... Args>
    void log(vg_log_level_e level, Args &&...args) const noexcept
    {
#if VG_LOG_INTERNAL_ENABLED
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
        (std::cout << ... << std::forward<Args>(args)) << std::endl;
#else
        if (c_api.basic.log_cb)
        {
            std::ostringstream oss;
            (oss << ... << std::forward<Args>(args));
            const auto msg = oss.str();
            c_api.basic.log_cb(level, msg.c_str(), msg.size());
        }
#endif
    }

    // Tick function to update context state
    bool tick() noexcept
    {
        if (!rule_enabled.load(std::memory_order_relaxed))
        {
            if (!detections.empty())
                detections.clear();
            if (!classifications.empty())
                classifications.clear();

            // Update last evaluation result to not matched
            clearRuleEvent();

            return false;
        }

        {
            steady_time = std::chrono::steady_clock::now();
            const auto now = std::chrono::system_clock::now();
            unix_time = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            local_time = LocalTime(now);
        }

        if (inference_subscribers.load(std::memory_order_relaxed) > 0)
        {
            const std::unique_lock lock(inference_mutex);

            if (inference_queue.empty())
                return false;

            auto &[ts, result] = inference_queue.front();
            unix_time = ts; // Update timestamp to that of the inference result
            if (auto *detection_vec = std::get_if<DetectionResultVec>(&result))
                detections = std::move(*detection_vec);
            else if (auto *classification_vec = std::get_if<ClassificationResultVec>(&result))
                classifications = std::move(*classification_vec);
            inference_queue.pop();
        }
        else
        {
            if (!detections.empty())
                detections.clear();
            if (!classifications.empty())
                classifications.clear();
        }

        return true;
    }

    // Inference result enqueueing (thread-safe)
    template<typename T>
    void enqueueInferenceResult(T &&result) noexcept
    {
        static_assert(std::is_same_v<std::remove_cvref_t<T>, InferenceResult>,
            "enqueueInferenceResult only accepts InferenceResult type.");
        if (!rule_enabled.load(std::memory_order_relaxed) || inference_subscribers.load(std::memory_order_relaxed) == 0)
        {
            const std::unique_lock lock(inference_mutex);

            while (!inference_queue.empty())
                inference_queue.pop();

            return; // Skip enqueueing new inference results
        }

        const std::unique_lock lock(inference_mutex);

        while (inference_queue.size() >= kInferenceQueueSize)
            inference_queue.pop();
        inference_queue.push(std::forward<T>(result));
    }

    // Media frame enqueueing (thread-safe)
    template<typename T>
    void enqueueMediaFrame(T &&frame) noexcept
    {
        static_assert(std::is_same_v<std::remove_cvref_t<T>, MediaFrame>,
            "enqueueMediaFrame only accepts MediaFrame type.");
        const std::unique_lock lock(media_mutex);
        auto rit = media_buffer.crbegin();
        for (; rit != media_buffer.crend(); ++rit)
        {
            if (rit->time_stamp <= frame.time_stamp)
                break;
        }
        media_buffer.insert(rit.base(), std::forward<T>(frame));
        while (media_buffer.size() >= kMediaMuxerBufferSize)
            media_buffer.pop_front();
    }

    // Clear media frames (thread-safe)
    void clearMediaBuffer() noexcept
    {
        const std::unique_lock lock(media_mutex);
        media_buffer.clear();
    }

    FileWriterSettingsWithRevision getWriterSettings() const noexcept
    {
        const std::shared_lock lock(writer_settings_mutex);
        return { writer_settings_revision.load(std::memory_order_acquire), writer_settings };
    }

    // Try to update rule event (should be called in the same thread as tick())
    bool tryUpdateRuleEvent(vg_rule_type_e type, const std::string &id) const noexcept
    {
        try
        {
            const std::unique_lock lock(rule_event_mutex);
            if (!rule_event)
            {
                rule_event = std::make_shared<WriteEvent>();
                rule_event_last = rule_event;
                rule_event_owner.emplace(type, id, unix_time);
                return true;
            }
        }
        catch (std::exception &e)
        {
            VG_LOG(log, VG_LOG_LEVEL_ERROR, "Failed to create EvaluationResult in ruleTaskLoop: ", e.what());
        }
        return false;
    }

    // Clear rule event (thread-safe)
    void clearRuleEvent() const noexcept
    {
        const std::unique_lock lock(rule_event_mutex);
        rule_event.reset();
    }

    std::shared_ptr<WriteEvent> getRuleEvent() const noexcept
    {
        const std::shared_lock lock(rule_event_mutex);
        return rule_event;
    }

    // Shared data
    // Used to control rule evaluation and inference subscriptions
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<bool> rule_enabled = false;
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<int> inference_subscribers = 0;
    // Sync inference results queue, updated in enqueueInferenceResult()
    std::mutex inference_mutex;
    std::queue<InferenceResult> inference_queue;
    // Sync rule event, updated in RuleEndpoint::operator()()
    mutable std::shared_mutex rule_event_mutex;
    mutable std::shared_ptr<WriteEvent> rule_event;
    mutable std::shared_ptr<WriteEvent> rule_event_last;
    mutable std::optional<EventOwner> rule_event_owner;
    // Sync video/audio frame buffers, updated in enqueueMediaFrame()
    std::mutex media_mutex;
    std::deque<MediaFrame> media_buffer;
    std::mutex media_pause_mutex;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> media_paused = false;
    // Sync record writer settings
    mutable std::shared_mutex writer_settings_mutex;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> writer_settings_revision = 0;
    FileWriterSettings writer_settings = FileWriterSettings();

    // Local data, updated in tick()
    mutable std::chrono::steady_clock::time_point steady_time = {};
    mutable int64_t unix_time = 0;
    LocalTime local_time = LocalTime::now();
    DetectionResultVec detections = {};
    ClassificationResultVec classifications = {};

    // C APIs
    const vg_config_t c_api;

    // Constructor
    Context(const vg_config_t &cfg) : c_api(cfg) { }

    // Destructor
    ~Context() = default;
};

} // namespace vigil

#endif
