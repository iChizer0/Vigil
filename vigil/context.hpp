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
#include <type_traits>
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
            shared_inference_result.reset();
            // Update last evaluation result to not matched
            clearRuleEvent();
            return false;
        }

        shared_time.tick();

        if (inference_subscribers.load(std::memory_order_relaxed) > 0)
        {
            const std::unique_lock lock(inference_mutex);
            if (inference_queue.empty())
                return false;
            auto &[ts, result] = inference_queue.front();
            shared_time.updateUnixTime(ts); // Update timestamp to that of the inference result
            if (auto *detection_vec = std::get_if<DetectionResultVec>(&result))
                shared_inference_result.detections = std::move(*detection_vec);
            else if (auto *classification_vec = std::get_if<ClassificationResultVec>(&result))
                shared_inference_result.classifications = std::move(*classification_vec);
            inference_queue.pop();
        }
        else
            shared_inference_result.reset();

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
        slot_media_buffer.enqueue(std::forward<T>(frame));
    }

    // Clear media frames (thread-safe)
    void clearMediaBuffer() noexcept
    {
        slot_media_buffer.clear();
    }

    FileWriterSettingsWithRevision getWriterSettings() const noexcept
    {
        const std::shared_lock lock(writer_settings_mutex);
        return { writer_settings_revision.load(std::memory_order_acquire), writer_settings };
    }

    // Try to update rule event
    bool tryUpdateRuleEvent(vg_rule_type_e type, const std::string &id) const noexcept
    {
        try
        {
            const auto [steady_time, unix_time, _] = shared_time.getAll();
            if (c_api.basic.on_rule_event_cb)
            {
                const auto evt = vg_rule_evt_t { .type = type, .id = id.c_str(), .timestamp = unix_time };
                c_api.basic.on_rule_event_cb(&evt);
            }
            {
                const std::unique_lock lock(rule_event_mutex);
                if (!rule_event)
                {
                    rule_event = std::make_shared<WriteEvent>(steady_time, unix_time);
                    rule_event_last = rule_event;
                    rule_event_owner.emplace(type, id, unix_time);
                    return true;
                }
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
    std::mutex media_pause_mutex;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> media_paused = false;
    // Sync record writer settings
    mutable std::shared_mutex writer_settings_mutex;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> writer_settings_revision = 0;
    FileWriterSettings writer_settings = FileWriterSettings();

    // Shared video/audio frame buffers
    template<size_t Limit>
    struct SharedMediaBuffer
    {
        static_assert(Limit > 0, "Limit must be greater than 0");

        std::mutex mutex;
        std::deque<MediaFrame> queue;

        void clear() noexcept
        {
            const std::unique_lock lock(mutex);
            queue.clear();
        }

        template<typename T, std::enable_if_t<std::is_same_v<std::remove_cvref_t<T>, MediaFrame>, bool> = true>
        void enqueue(T &&frame) noexcept
        {
            const std::unique_lock lock(mutex);
            auto rit = queue.crbegin();
            for (; rit != queue.crend(); ++rit)
            {
                if (rit->time_stamp <= frame.time_stamp)
                    break;
            }
            queue.insert(rit.base(), std::forward<T>(frame));
            while (queue.size() >= Limit)
                queue.pop_front();
        }
    };
    SharedMediaBuffer<kMediaMuxerBufferSize> slot_media_buffer;

    // Shared time (thread-safe)
    struct SharedTime
    {
    private:
        mutable std::shared_mutex mutex;
        std::chrono::steady_clock::time_point _steady_time = {};
        int64_t _unix_time = 0;
        LocalTime _local_time = LocalTime::now();

    protected:
        friend Context;

        void tick() noexcept
        {
            const std::unique_lock lock(mutex);
            _steady_time = std::chrono::steady_clock::now();
            const auto now = std::chrono::system_clock::now();
            _unix_time = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            _local_time = LocalTime(now);
        }

        void updateUnixTime(int64_t ts) noexcept
        {
            const std::unique_lock lock(mutex);
            _unix_time = ts;
        }

    public:
        std::chrono::steady_clock::time_point steadyTime() const noexcept
        {
            const std::shared_lock lock(mutex);
            return _steady_time;
        }

        int64_t unixTime() const noexcept
        {
            const std::shared_lock lock(mutex);
            return _unix_time;
        }

        LocalTime localTime() const noexcept
        {
            const std::shared_lock lock(mutex);
            return _local_time;
        }

        std::tuple<std::chrono::steady_clock::time_point, int64_t, LocalTime> getAll() const noexcept
        {
            const std::shared_lock lock(mutex);
            return { _steady_time, _unix_time, _local_time };
        }
    } shared_time;

    // Shared inference results (not thread-safe)
    struct SharedInferenceResult
    {
        DetectionResultVec detections = {};
        ClassificationResultVec classifications = {};

        void reset()
        {
            if (!detections.empty())
                detections.clear();
            if (!classifications.empty())
                classifications.clear();
        }
    } shared_inference_result;

    // C API
    const vg_config_t c_api;

    // Constructor
    Context(const vg_config_t &cfg) : c_api(cfg) { }

    // Destructor
    ~Context() = default;
};

} // namespace vigil

#endif
