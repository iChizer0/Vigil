#pragma once
#ifndef VIGIL_CONTEXT_HPP
#define VIGIL_CONTEXT_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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
    // C API
    const vg_config_t c_api;

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

    // Tick function to update context state. The clocks advance whether or not rules are enabled, and an
    // input's timestamp is its owner's event time only: it never replaces the current time (docs/PLAN.md
    // section 5.1).
    void tick() noexcept
    {
        shared_time.tick();
        if (!record_rule_enabled.load(std::memory_order_relaxed))
        {
            resetInference();
            shared_inference_result.reset();
            shared_rule_event.cancelPending(); // a claimed event is completed by its writer
            sound_executor.cancelPending();    // a playing callback finishes on its own
            return;
        }

        {
            const std::unique_lock lock(inference_mutex);
            auto it = inference_queue.begin();
            if (it == inference_queue.end())
            {
                shared_inference_result.reset();
                return;
            }
            shared_inference_result = std::move(*it);
            inference_queue.pop_front();
        }
    }

    // Inference result enqueueing (thread-safe)
    template<typename T>
    void enqueueInferenceResult(T &&result) noexcept
    {
        static_assert(std::is_same_v<std::remove_cvref_t<T>, InferenceResult>,
            "enqueueInferenceResult only accepts InferenceResult type.");
        if (!record_rule_enabled.load(std::memory_order_relaxed))
        {
            const std::unique_lock lock(inference_mutex);
            inference_queue.clear();
            return; // Skip enqueueing new inference results
        }

        if (inference_subscribers.load(std::memory_order_relaxed) == 0
            && !std::holds_alternative<std::monostate>(result.data))
            return;

        {
            const std::unique_lock lock(inference_mutex);
            while (inference_queue.size() >= kInferenceQueueSize)
                inference_queue.pop_front();
            inference_queue.push_back(std::forward<T>(result));
        }
    }

    void resetInference(std::string_view source_id = {})
    {
        if (source_id.empty())
        {
            const std::unique_lock lock(inference_mutex);
            inference_queue.clear();
        }
        else
        {
            const std::unique_lock lock(inference_mutex);
            for (auto it = inference_queue.begin(); it != inference_queue.end();)
            {
                if (it->source_id == source_id)
                    it = inference_queue.erase(it);
                else
                    ++it;
            }
        }
    }

    // Media frame enqueueing (thread-safe)
    template<typename T>
    void enqueueMediaFrame(const T &frame) noexcept
    {
        slot_media_buffer.enqueue(frame);
        capture_media_buffer.enqueue(frame);
    }

    // Clear media frames (thread-safe)
    void clearMediaBuffer() noexcept
    {
        slot_media_buffer.clear();
        capture_media_buffer.clear();
    }

    bool tryCreateWriterEvent(std::shared_ptr<EventOwner> owner) const noexcept
    {
        if (!owner)
            return false;
        const auto steady_time = shared_time.steadyTime();
        const auto settings = action_settings.get().settings;
        bool writer_created = false;
        try
        {
            {
                const std::unique_lock lock(shared_rule_event.mutex);
                if (!shared_rule_event.current)
                {
                    // The file task carries its owner's event time (the input's timestamp for FRAME and direct
                    // EVENT owners), not the current time.
                    auto event = std::make_shared<WriteEvent>(steady_time, owner->timestamp, owner->uid);
                    shared_rule_event.current = std::move(event);
                    shared_rule_event.current_owner = owner;
                    writer_created = true;
                }
            }
            // Sound is attempted on its own (docs/PLAN.md section 7.1): a rejected or failed trigger changes
            // nothing else, and writer_created reports file acceptance only. No configuration enables it before
            // Phase 4D.
            if (settings && settings->sound_enabled)
                sound_executor.tryTrigger();
            if (c_api.basic.on_rule_event_cb)
            {
                const auto evt = vg_rule_evt_t { .type = owner->type,
                    .id = owner->id.c_str(),
                    .timestamp = owner->timestamp,
                    .uid = owner->uid,
                    .source_id = owner->source_id.c_str(),
                    .source_delivery = EventOwner::SourceDeliveryString(owner->source_delivery),
                    .writer_created = writer_created,
                    .dispatch_timestamp = owner->dispatch_timestamp };
                c_api.basic.on_rule_event_cb(&evt);
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(log, VG_LOG_LEVEL_ERROR, "Failed to publish rule evaluation event: ", e.what());
            return false;
        }
        catch (...)
        {
            VG_LOG(log, VG_LOG_LEVEL_ERROR, "Unknown exception occurred while publishing rule evaluation event");
            return false;
        }
        return writer_created;
    }

    // Shared data
    // Used to control rule evaluation and inference subscriptions
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<bool> record_rule_enabled = false;
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<int> inference_subscribers = 0;
    // Sync inference results queue, updated in enqueueInferenceResult()
    std::mutex inference_mutex;
    std::deque<InferenceResult> inference_queue;
    // Media-operation barrier (docs/PLAN.md section 8.3). Every writer holds one admitted operation from its
    // construction to its destruction, which spans every media callback it makes (create, write, finish, image
    // capture) and the thumbnail generated when its file is published. Admission and pause share one mutex, so
    // nothing is admitted after a pause, and a waiting pause returns only once every operation admitted before
    // it has left.
    struct MediaBarrier
    {
    public:
        // One admitted operation, which leaves the barrier when destroyed. Empty when admission was refused.
        class Operation final
        {
        public:
            explicit Operation(MediaBarrier &barrier) noexcept : _barrier(barrier.enter() ? &barrier : nullptr) { }

            ~Operation() noexcept
            {
                if (_barrier)
                    _barrier->leave();
            }

            Operation(const Operation &) = delete;
            Operation &operator=(const Operation &) = delete;

            explicit operator bool() const noexcept
            {
                return _barrier != nullptr;
            }

        private:
            MediaBarrier *_barrier;
        };

        // Closes admission; with `wait`, also blocks until every admitted operation has left.
        void pause(bool wait) noexcept
        {
            std::unique_lock lock(_mutex);
            _paused.store(true, std::memory_order_release);
            if (wait)
                _idle.wait(lock, [this] { return _active.load(std::memory_order_acquire) == 0; });
        }

        void resume() noexcept
        {
            const std::lock_guard lock(_mutex);
            _paused.store(false, std::memory_order_release);
        }

        bool isPaused() const noexcept
        {
            return _paused.load(std::memory_order_acquire);
        }

        size_t activeOperations() const noexcept
        {
            return _active.load(std::memory_order_acquire);
        }

    private:
        bool enter() noexcept
        {
            const std::lock_guard lock(_mutex);
            if (_paused.load(std::memory_order_acquire))
                return false;
            _active.fetch_add(1, std::memory_order_release);
            return true;
        }

        void leave() noexcept
        {
            {
                const std::lock_guard lock(_mutex);
                _active.fetch_sub(1, std::memory_order_release);
            }
            _idle.notify_all();
        }

        std::mutex _mutex;
        std::condition_variable _idle;
        alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _paused = false;
        alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> _active = 0;
    } media_barrier;
    // Callback for slot status check (for HTTP rule)
    std::shared_mutex slot_check_mutex;
    std::pair<const void * /* owner */, std::function<bool(const void *)>> slot_check = { nullptr, nullptr };

    // Shared rule event
    struct SharedRuleEvent
    {
    protected:
        friend Context;

        mutable std::shared_mutex mutex;
        std::shared_ptr<WriteEvent> last;
        std::shared_ptr<EventOwner> last_owner;
        std::shared_ptr<WriteEvent> current;
        std::shared_ptr<EventOwner> current_owner;

        // Retires the current event with its terminal status. Call with the lock held.
        void retireUnsafe(WriteEvent::Status status) noexcept
        {
            current->status.store(status, std::memory_order_release);
            last = std::move(current);
            current.reset();
            last_owner = std::move(current_owner);
            current_owner.reset();
        }

    public:
        // Claims the current event for a writer: Pending becomes Writing. Null when nothing is pending.
        std::shared_ptr<WriteEvent> claim() noexcept
        {
            const std::unique_lock lock(mutex);
            if (!current || current->status.load(std::memory_order_acquire) != WriteEvent::Status::Pending)
                return nullptr;
            current->status.store(WriteEvent::Status::Writing, std::memory_order_release);
            return current;
        }

        // Cancels the current event unless a writer has claimed it. A claimed event belongs to its writer,
        // which interrupts itself when rules are disabled and completes the event (docs/PLAN.md section 7.2).
        void cancelPending() noexcept
        {
            const std::unique_lock lock(mutex);
            if (current && current->status.load(std::memory_order_acquire) == WriteEvent::Status::Pending)
                retireUnsafe(WriteEvent::Status::Canceled);
        }

        // Completes `expected` with its terminal status if it is still the current event. A stale event never
        // touches a newer one.
        void complete(const std::shared_ptr<WriteEvent> &expected, WriteEvent::Status status) noexcept
        {
            const std::unique_lock lock(mutex);
            if (expected && current == expected)
                retireUnsafe(status);
        }

        std::shared_ptr<WriteEvent> getLast() const noexcept
        {
            const std::shared_lock lock(mutex);
            return last;
        }

        std::shared_ptr<WriteEvent> getCurrent() const noexcept
        {
            const std::shared_lock lock(mutex);
            return current;
        }

        std::tuple<std::shared_ptr<WriteEvent>, std::shared_ptr<EventOwner>, std::shared_ptr<WriteEvent>,
            std::shared_ptr<EventOwner>>
        get() const noexcept
        {
            const std::shared_lock lock(mutex);
            return { last, last_owner, current, current_owner };
        }

    } mutable shared_rule_event;

    // The published action settings snapshot and its revisions (docs/PLAN.md section 7.1). The global revision
    // detects configuration-commit conflicts: `update` publishes only for a caller that saw the current one, so
    // a commit and a media refresh cannot interleave. Each file channel has its own revision, bumped only when
    // that channel's settings change, which is what its writer watches: a change to the other channel or to
    // Sound does not stop it.
    struct SharedActionSettings
    {
    public:
        struct Loaded
        {
            size_t revision;
            size_t photo_revision;
            size_t video_revision;
            std::shared_ptr<const ActionSettings> settings; // null only if the first allocation failed
        };

        // Publishes `next` if `rev` is still the current revision; an equal snapshot publishes nothing. Returns
        // false, with nothing applied, on a conflict.
        bool update(size_t rev, std::shared_ptr<const ActionSettings> next) noexcept
        {
            if (!next) [[unlikely]]
                return false;
            const std::unique_lock lock(mutex);
            const auto cur = revision.load(std::memory_order_acquire);
            if (rev != cur)
                return false;
            if (settings && *settings == *next)
                return true;
            if (!settings || !ActionSettings::SameSettings(settings->photo, next->photo))
                photo_revision.fetch_add(1, std::memory_order_release);
            if (!settings || !ActionSettings::SameSettings(settings->video, next->video))
                video_revision.fetch_add(1, std::memory_order_release);
            settings = std::move(next);
            revision.store(cur + 1, std::memory_order_release);
            return true;
        }

        // A media refresh: the current configuration with its watermarks estimated again, as one snapshot.
        // Publishes the defaults (no file channel) when nothing was published yet.
        void sync(const vg_config_media_t &media_capi) noexcept
        {
            for (;;)
            {
                const auto loaded = get();
                auto next = loaded.settings ? loaded.settings->refreshed(&media_capi)
                                            : ActionSettings::create(false, false, nullptr, nullptr, &media_capi);
                if (!next) [[unlikely]]
                    return; // allocation failure: nothing to publish
                if (update(loaded.revision, std::move(next)))
                    return;
            }
        }

        Loaded get() const noexcept
        {
            const std::shared_lock lock(mutex);
            return { revision.load(std::memory_order_acquire), photo_revision.load(std::memory_order_acquire),
                video_revision.load(std::memory_order_acquire), settings };
        }

        size_t getRevision() const noexcept
        {
            return revision.load(std::memory_order_acquire);
        }

        // The revision of the file channel that writes `type`: Image is the Photo channel, Video the Video one.
        size_t channelRevision(CaptureType type) const noexcept
        {
            return (type == CaptureType::Image ? photo_revision : video_revision).load(std::memory_order_acquire);
        }

    private:
        mutable std::shared_mutex mutex;
        alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> revision = 0;
        alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> photo_revision = 0;
        alignas(kHardwareDestructiveInterferenceSize) std::atomic<size_t> video_revision = 0;
        std::shared_ptr<const ActionSettings> settings;
    } action_settings;

    // Shared video/audio frame buffers
    template<size_t PeriodLimit>
    struct SharedMediaBuffer
    {
        std::mutex mutex;
        std::deque<MediaFrame> queue;
        bool drop_until_keyframe = false;

        void clear() noexcept
        {
            const std::unique_lock lock(mutex);
            queue.clear();
        }

        void clearToLastKeyFrame() noexcept
        {
            const std::unique_lock lock(mutex);
            auto s = queue.rbegin();
            auto e = queue.rend();
            for (; s != e; ++s)
            {
                if (s->isKeyFrame())
                {
                    ++s;
                    break;
                }
            }
            queue.erase(queue.begin(), s.base());
        }

        template<typename T, std::enable_if_t<std::is_same_v<std::remove_cvref_t<T>, MediaFrame>, bool> = true>
        void enqueue(T &&frame) noexcept
        {
            const std::unique_lock lock(mutex);
            if (drop_until_keyframe) [[unlikely]]
            {
                if (frame.isKeyFrame())
                    drop_until_keyframe = false;
                else
                    return;
            }

            if (queue.empty() || queue.back().time_stamp <= frame.time_stamp) [[likely]]
            {
                queue.emplace_back(std::forward<T>(frame));
                tryDropFramesInternal();
                return;
            }

            auto rit = queue.crbegin();
            for (; rit != queue.crend(); ++rit)
            {
                if (rit->time_stamp <= frame.time_stamp) [[likely]]
                    break;
            }
            queue.insert(rit.base(), std::forward<T>(frame));
            tryDropFramesInternal();
        }

    private:
        void tryDropFramesInternal() noexcept
        {
            auto s = queue.begin();
            auto e = queue.begin();
            for (auto t = std::chrono::steady_clock::now() - std::chrono::seconds(PeriodLimit); e != queue.end(); ++e)
            {
                if (e->time_stamp >= t)
                    break;
            }
            if (s == e)
                return;
            for (; e != queue.end(); ++e)
            {
                if (e->isKeyFrame())
                    break;
            }
            if (e != queue.end())
                queue.erase(s, e);
            else
            {
                drop_until_keyframe = true;
                queue.clear();
            }
        }
    };
    SharedMediaBuffer<kSlotMediaMuxerBufferPeriod> slot_media_buffer;
    SharedMediaBuffer<kCaptureMediaMuxerBufferPeriod> capture_media_buffer;

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
    std::optional<InferenceResult> shared_inference_result;

    // The Sound action executor (docs/PLAN.md section 7.3): one worker, one state, no queue. A trigger is
    // accepted from Idle only, and only while the host provides `sound_play_cb`, media is not paused, and the
    // executor is not stopping; a Pending or Playing retrigger is rejected, not queued. The worker claims a
    // Pending trigger, rechecks that Sound and rules are still enabled and media is not paused (a commit that
    // closed a gate since the trigger cancels it), holds a media operation (section 8.3) while it calls the
    // callback outside every lock, and returns to Idle whatever the callback returned: a failure is logged, never
    // replayed. Disabling Sound or rules and stopping cancel a Pending trigger; a Playing callback finishes under
    // its bounded-completion contract and `stopAndJoin` waits for it. Without a callback there is no worker and
    // Sound is unavailable.
    struct SoundActionExecutor
    {
    public:
        enum class State { Idle, Pending, Playing, Stopping };

        // `run_worker == false` is a test seam: no thread is started and the test plays pending triggers
        // through SoundActionExecutorTestAccess. Production code always runs the worker.
        explicit SoundActionExecutor(Context &ctx, bool run_worker = true)
            : _ctx(ctx), _available(ctx.c_api.media.sound_play_cb != nullptr)
        {
            if (_available && run_worker)
                _worker = std::thread(&SoundActionExecutor::workerLoop, this);
        }

        // Explicit stopAndJoin precedes destruction (Vigil::~Vigil); this is the fallback.
        ~SoundActionExecutor() noexcept
        {
            stopAndJoin();
        }

        SoundActionExecutor(const SoundActionExecutor &) = delete;
        SoundActionExecutor &operator=(const SoundActionExecutor &) = delete;

        // Whether the host provides the callback. A configured Sound action without it is never accepted.
        bool available() const noexcept
        {
            return _available;
        }

        // Accepts a trigger from Idle. False when Sound is unavailable or stopping, media is paused, or a trigger
        // is pending or playing; the caller checks the configuration and the rule gates.
        bool tryTrigger() noexcept
        {
            {
                const std::lock_guard lock(_mutex);
                if (!_available || _stop || _state != State::Idle || _ctx.media_barrier.isPaused())
                    return false;
                _state = State::Pending;
            }
            _wake.notify_all();
            return true;
        }

        // Cancels a Pending trigger; a Playing callback is left to finish.
        void cancelPending() noexcept
        {
            const std::lock_guard lock(_mutex);
            if (_state == State::Pending)
                _state = State::Idle;
        }

        State state() const noexcept
        {
            const std::lock_guard lock(_mutex);
            return _state;
        }

        // Refuses further triggers, cancels a Pending one, lets a Playing callback return, and joins the worker.
        // Idempotent; the executor stays stopped.
        void stopAndJoin() noexcept
        {
            {
                const std::lock_guard lock(_mutex);
                _stop = true;
                if (_state != State::Playing) // a Playing callback moves to Stopping when it returns
                    _state = State::Stopping;
            }
            _wake.notify_all();
            if (_worker.joinable())
                _worker.join();
        }

    private:
        friend struct SoundActionExecutorTestAccess; // defined by the module tests only

        void workerLoop() noexcept
        {
            for (;;)
            {
                {
                    std::unique_lock lock(_mutex);
                    _wake.wait(lock, [this] { return _stop || _state == State::Pending; });
                    if (_stop)
                        return;
                }
                playPending();
            }
        }

        // Claims the Pending trigger and plays it; false when nothing is pending. Admission to the media barrier
        // happens here, so a trigger accepted before a pause is canceled instead of played.
        bool playPending() noexcept
        {
            {
                const std::lock_guard lock(_mutex);
                if (_state != State::Pending)
                    return false;
                _state = State::Playing;
            }
            bool played = false;
            vg_sta_e ret = 0;
            {
                // The gates are rechecked after the claim, as the file writer does before creating anything: a
                // commit that disabled Sound or rules since the trigger cancels it instead of playing.
                const MediaBarrier::Operation operation(_ctx.media_barrier);
                const auto settings = _ctx.action_settings.get().settings;
                if (operation && settings && settings->sound_enabled
                    && _ctx.record_rule_enabled.load(std::memory_order_relaxed))
                {
                    played = true;
                    ret = _ctx.c_api.media.sound_play_cb(_ctx.c_api.media.user_data);
                }
            }
            if (!played)
                VG_LOG(_ctx.log, VG_LOG_LEVEL_INFO,
                    "Sound trigger canceled: media is paused, or Sound or rules were disabled");
            else if (ret != 0)
                VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Sound playback failed, code: ", static_cast<int>(ret));
            {
                const std::lock_guard lock(_mutex);
                _state = _stop ? State::Stopping : State::Idle;
            }
            _wake.notify_all();
            return true;
        }

        Context &_ctx;
        const bool _available;
        mutable std::mutex _mutex;
        std::condition_variable _wake;
        State _state = State::Idle;
        bool _stop = false;
        std::thread _worker;
    } mutable sound_executor; // last member: it starts after everything it uses exists, and stops first

    // Constructor. `run_sound_worker` is the test seam of SoundActionExecutor.
    Context(const vg_config_t &cfg, bool run_sound_worker = true) : c_api(cfg), sound_executor(*this, run_sound_worker)
    {
        action_settings.sync(c_api.media);
    }

    // Destructor
    ~Context() = default;
};

} // namespace vigil

#endif
