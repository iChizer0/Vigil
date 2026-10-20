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
#include <unordered_map>
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
            file_channels.cancelPending();  // a claimed task is completed by its worker
            sound_executor.cancelPending(); // a playing callback finishes on its own
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

    // Dispatches one owner after attribution (docs/PLAN.md sections 5.1, 6.2, 7.1, and 8.1): records it as the
    // last dispatched owner, attempts a task in every file channel the snapshot enables and a Sound trigger
    // when it enables Sound, then reports the owner through the RULE callback, whose `writer_created` is file
    // acceptance only: at least one channel accepted a task. A channel accepts nothing while it holds a task,
    // when its settings changed since the snapshot was loaded, when a gate closed since the caller evaluated
    // it, or without a usable Slot with headroom; a rejected file action leaves nothing pending, so the next
    // owner proceeds. Sound is admitted by its executor against the same gates, and a rejected or failed
    // trigger changes nothing else. Returns whether the owner's file work was admitted: true when a channel
    // accepted a task or the snapshot enables no file action (a rule-only or Sound-only configuration), which
    // is what the HTTP activation answers 0 for (section 5.2); false when every enabled channel refused. Runs
    // on the rule thread and on the HTTP request thread.
    bool dispatchRuleEvent(std::shared_ptr<EventOwner> owner) const noexcept
    {
        if (!owner)
            return false;
        const auto loaded = action_settings.get();
        bool accepted = false;
        bool file_action_enabled = false;
        try
        {
            last_dispatched_owner.set(owner);
            if (loaded.settings)
            {
                file_action_enabled = loaded.settings->fileChannel() != nullptr;
                if (file_channels.photo.tryAccept(owner, loaded))
                    accepted = true;
                if (file_channels.video.tryAccept(owner, loaded))
                    accepted = true;
                if (loaded.settings->sound_enabled)
                    sound_executor.tryTrigger();
            }
            if (c_api.basic.on_rule_event_cb)
            {
                const auto evt = vg_rule_evt_t { .type = owner->type,
                    .id = owner->id.c_str(),
                    .timestamp = owner->timestamp,
                    .uid = owner->uid,
                    .source_id = owner->source_id.c_str(),
                    .source_delivery = EventOwner::SourceDeliveryString(owner->source_delivery),
                    .writer_created = accepted,
                    .dispatch_timestamp = owner->dispatch_timestamp };
                c_api.basic.on_rule_event_cb(&evt);
            }
        }
        catch (const std::exception &e)
        {
            VG_LOG(log, VG_LOG_LEVEL_ERROR, "Failed to dispatch rule event: ", e.what());
            return false;
        }
        catch (...)
        {
            VG_LOG(log, VG_LOG_LEVEL_ERROR, "Unknown exception occurred while dispatching rule event");
            return false;
        }
        return accepted || !file_action_enabled;
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
    // The schedule gate (docs/PLAN.md section 8.1): published by the rule loop on every tick before the enable
    // check, and by the endpoint when the schedule configuration changes, so dispatch from the rule thread and
    // the HTTP thread reads one state inside the admission section. Open until the first publication, as a
    // runtime without a schedule is.
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<bool> schedule_open = true;

    // The published action settings snapshot and its revisions (docs/PLAN.md section 7.1). The global revision
    // detects configuration-commit conflicts: `update` publishes only for a caller that saw the current one, so
    // a commit and a media refresh cannot interleave. Each file channel has its own revision, bumped only when
    // that channel's recording settings change, which is what its writer watches: a change to the other
    // channel, to Sound, or to the watermarks (admission state, section 9) does not stop it.
    struct SharedActionSettings
    {
    public:
        struct Loaded
        {
            size_t revision;
            size_t photo_revision;
            size_t video_revision;
            std::shared_ptr<const ActionSettings> settings; // null only if the first allocation failed

            // The loaded revision of the file channel that writes `type`.
            size_t channelRevision(CaptureType type) const noexcept
            {
                return type == CaptureType::Image ? photo_revision : video_revision;
            }
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
            if (!settings || !ActionSettings::SameRecording(settings->photo, next->photo))
                photo_revision.fetch_add(1, std::memory_order_release);
            if (!settings || !ActionSettings::SameRecording(settings->video, next->video))
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

    // One automatic file channel, Photo or Video (docs/PLAN.md section 7.2): at most one accepted task, pending
    // or running, and the last retired one. The channel holds its task from acceptance until the worker
    // completes it, which follows the interval hold and the failure cooldown (section 6.2), and accepts nothing
    // meanwhile. Acceptance is admission (section 8.1): the gates, the channel, and the registered Slot are
    // checked in one critical section, and a task carries the Slot it is bound to and its budget reservation.
    // Every operation is identity-aware: completing or canceling task A never touches a newer task B, and a
    // worker claims only a task bound to its Slot. `requestStop` on a running task comes with its first caller,
    // the continuous-mode handover of Phase 5A.
    struct FileChannel
    {
    public:
        struct Tasks
        {
            std::shared_ptr<FileTask> current; // held: pending, running, or finished and within its hold
            std::shared_ptr<FileTask> last;    // the last retired one
        };

        FileChannel(const Context &ctx, CaptureType type) noexcept : _ctx(ctx), _type(type) { }

        CaptureType type() const noexcept
        {
            return _type;
        }

        // Accepts a task for `owner` on this channel, from the snapshot the dispatcher loaded, in one critical
        // section (docs/PLAN.md section 8.1): the snapshot enables the channel, the channel is idle (its last
        // task completed, after its hold), its settings revision is still the loaded one, the gates are open
        // (rules enabled, the schedule open, media not paused), and the registered Slot is usable and has
        // headroom, whose reservation the task carries with the Slot's registration. Null otherwise: nothing is
        // left pending, and the next owner is tried afresh. A task bound to old settings, an old Slot, or a
        // closed gate is never created.
        std::shared_ptr<FileTask> tryAccept(const std::shared_ptr<EventOwner> &owner,
            const SharedActionSettings::Loaded &loaded)
        {
            const auto settings = loaded.settings ? loaded.settings->channel(_type) : nullptr;
            if (!settings)
                return nullptr;
            const auto steady_time = _ctx.shared_time.steadyTime();
            const std::unique_lock lock(_mutex);
            if (_current || _ctx.action_settings.channelRevision(_type) != loaded.channelRevision(_type))
                return nullptr;
            if (!_ctx.record_rule_enabled.load(std::memory_order_relaxed)
                || !_ctx.schedule_open.load(std::memory_order_acquire) || _ctx.media_barrier.isPaused())
                return nullptr;
            auto admitted = _ctx.slot_admission.tryAdmit(loaded.settings->watermarks, settings->watermarks.task_bytes);
            if (!admitted.registration)
                return nullptr;
            // The task carries its owner's event time (the input's timestamp for FRAME and direct EVENT owners),
            // not the current time.
            _current = std::make_shared<FileTask>(_type, owner, settings, loaded.channelRevision(_type),
                _ctx.file_task_sequence.fetch_add(1, std::memory_order_relaxed) + 1, admitted.registration,
                std::move(admitted.reservation),
                std::make_shared<WriteEvent>(steady_time, owner->timestamp, owner->uid));
            return _current;
        }

        // Claims the pending task for the worker of the Slot registered as `registration`: Pending becomes
        // Writing. Null when nothing is pending, or when the pending task is bound to another registration,
        // which is not this worker's to run (docs/PLAN.md section 7.2; closing that Slot's admission cancels it).
        std::shared_ptr<FileTask> claim(uint64_t registration) noexcept
        {
            const std::unique_lock lock(_mutex);
            if (!_current || _current->event->status.load(std::memory_order_acquire) != WriteEvent::Status::Pending
                || _current->slot_registration != registration)
                return nullptr;
            _current->event->status.store(WriteEvent::Status::Writing, std::memory_order_release);
            return _current;
        }

        // Cancels the pending task unless a worker has claimed it. A claimed task belongs to its worker, which
        // interrupts itself when a gate closes and completes the task.
        void cancelPending() noexcept
        {
            const std::unique_lock lock(_mutex);
            if (_current && _current->event->status.load(std::memory_order_acquire) == WriteEvent::Status::Pending)
                retireUnsafe(WriteEvent::Status::Canceled);
        }

        // Cancels the pending task if it is bound to the Slot registered as `registration`, whose admission is
        // closing (docs/PLAN.md section 9). A claimed task is finished by its worker.
        void cancelPendingBoundTo(uint64_t registration) noexcept
        {
            const std::unique_lock lock(_mutex);
            if (_current && _current->event->status.load(std::memory_order_acquire) == WriteEvent::Status::Pending
                && _current->slot_registration == registration)
                retireUnsafe(WriteEvent::Status::Canceled);
        }

        // Completes `expected` with its terminal status if it is still the held task. A stale task never
        // touches a newer one.
        void complete(const std::shared_ptr<FileTask> &expected, WriteEvent::Status status) noexcept
        {
            const std::unique_lock lock(_mutex);
            if (expected && _current == expected)
                retireUnsafe(status);
        }

        // The gate of a running task of this channel (docs/PLAN.md section 8.1): rules are enabled and the
        // settings revision the task was bound to is still the current one. The writer checks the media pause
        // and its Slot's own continuation condition besides.
        bool isOpenFor(size_t settings_revision) const noexcept
        {
            return _ctx.record_rule_enabled.load(std::memory_order_relaxed)
                   && _ctx.action_settings.channelRevision(_type) == settings_revision;
        }

        // Whether the channel holds a task: from acceptance until completion, through the hold.
        bool holdsTask() const noexcept
        {
            const std::shared_lock lock(_mutex);
            return _current != nullptr;
        }

        std::shared_ptr<FileTask> current() const noexcept
        {
            const std::shared_lock lock(_mutex);
            return _current;
        }

        std::shared_ptr<FileTask> last() const noexcept
        {
            const std::shared_lock lock(_mutex);
            return _last;
        }

        Tasks get() const noexcept
        {
            const std::shared_lock lock(_mutex);
            return { _current, _last };
        }

    private:
        // Retires the held task with its terminal status; the reservation of a task retired unrun goes with it
        // (a claimed task's reservation is its worker's). Call with the lock held.
        void retireUnsafe(WriteEvent::Status status) noexcept
        {
            _current->reservation.release();
            _current->event->status.store(status, std::memory_order_release);
            _last = std::move(_current);
            _current.reset();
        }

        const Context &_ctx;
        const CaptureType _type;
        mutable std::shared_mutex _mutex;
        std::shared_ptr<FileTask> _current;
        std::shared_ptr<FileTask> _last;
    };

    // The two automatic file channels and what is reported across them (docs/PLAN.md section 6.2).
    struct FileChannels
    {
    public:
        struct Latest
        {
            std::shared_ptr<FileTask> task; // null before the first acceptance
            bool held;                      // whether any channel holds a task
        };

        explicit FileChannels(const Context &ctx) noexcept
            : photo(ctx, CaptureType::Image), video(ctx, CaptureType::Video)
        {
        }

        // The channel that writes `type`: Image is the Photo channel, Video the Video one.
        FileChannel &of(CaptureType type) noexcept
        {
            return type == CaptureType::Image ? photo : video;
        }

        const FileChannel &of(CaptureType type) const noexcept
        {
            return type == CaptureType::Image ? photo : video;
        }

        // Whether any channel holds a task: ALWAYS_ON waits for it (docs/PLAN.md section 4.4).
        bool holdsTask() const noexcept
        {
            return photo.holdsTask() || video.holdsTask();
        }

        // Cancels the pending task of each channel (rule disable, shutdown).
        void cancelPending() noexcept
        {
            photo.cancelPending();
            video.cancelPending();
        }

        // Cancels the pending task of each channel that is bound to the Slot registered as `registration`.
        void cancelPendingBoundTo(uint64_t registration) noexcept
        {
            photo.cancelPendingBoundTo(registration);
            video.cancelPendingBoundTo(registration);
        }

        // What rule info reports: the most recently accepted task across both channels, that is the held task
        // accepted last if any channel holds one, else the retired task accepted last.
        Latest latest() const noexcept
        {
            const auto later = [](const std::shared_ptr<FileTask> &a,
                                   const std::shared_ptr<FileTask> &b) -> std::shared_ptr<FileTask> {
                return !a ? b : !b ? a : a->sequence > b->sequence ? a : b;
            };
            const auto p = photo.get(), v = video.get();
            if (const auto held = later(p.current, v.current))
                return { held, true };
            return { later(p.last, v.last), false };
        }

        FileChannel photo;
        FileChannel video;
    } mutable file_channels;

    // The acceptance sequence across both channels, which orders their tasks for rule info.
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<uint64_t> file_task_sequence = 0;

    // The owner of the last dispatch, whatever the channels did with it (docs/PLAN.md section 6.2), which rule
    // info reports as `dLastDispatchedOwner`.
    struct LastDispatchedOwner
    {
    public:
        void set(std::shared_ptr<EventOwner> owner) noexcept
        {
            const std::unique_lock lock(_mutex);
            _owner = std::move(owner);
        }

        std::shared_ptr<EventOwner> get() const noexcept
        {
            const std::shared_lock lock(_mutex);
            return _owner;
        }

    private:
        mutable std::shared_mutex _mutex;
        std::shared_ptr<EventOwner> _owner;
    } mutable last_dispatched_owner;

    // In-flight budgets (docs/PLAN.md section 9): `types::InFlightBudgets`, shared by automatic Photo and Video
    // and manual capture on the same filesystem.
    using InFlightBudgets = types::InFlightBudgets;
    InFlightBudgets in_flight_budgets;

    // The admission interface of the enabled Slot (docs/PLAN.md sections 7.1, 8.1, and 9), which the Slot
    // registers while it is enabled and a file channel reads inside its admission section.
    struct SlotAdmission
    {
        virtual ~SlotAdmission() = default;

        // Whether the Slot can take a task now: enabled and in its ready state (admission, not continuation).
        virtual bool isReadyToWrite() const noexcept = 0;

        // The quota and free-space probes of one task against the aggregate admission mark, with the task's own
        // bytes reserved in the same critical section (section 9). Empty when refused.
        virtual InFlightBudgets::Reservation tryAdmit(const WaterMarks &admission, FileSize task_bytes) noexcept = 0;
    };

    // The registered Slot admission, which replaces the slot check callback: at most one Slot, the enabled one,
    // with a registration that the tasks it admits are bound to, so a worker runs only its own Slot's tasks and
    // closing admission retires what the Slot accepted but no worker claimed. Lock order: a channel lock, then
    // this registry, then the Slot's own locks; `close` takes the channel locks after releasing the registry.
    struct SlotAdmissionRegistry
    {
    public:
        struct Admitted
        {
            uint64_t registration = 0; // 0: refused
            InFlightBudgets::Reservation reservation;
        };

        explicit SlotAdmissionRegistry(Context &ctx) noexcept : _ctx(ctx) { }

        // Opens admission for `slot`: the tasks accepted from here on bind to the returned registration, which
        // is never 0 and never reused.
        uint64_t open(SlotAdmission &slot) noexcept
        {
            const std::unique_lock lock(_mutex);
            _slot = &slot;
            _registration = ++_sequence;
            return _registration;
        }

        // Closes admission for `registration` if it is the open one, then cancels the pending tasks bound to
        // it: nothing is accepted for the Slot afterwards, and a claimed task is finished by its worker.
        void close(uint64_t registration) noexcept
        {
            {
                const std::unique_lock lock(_mutex);
                if (_registration != registration)
                    return;
                _slot = nullptr;
                _registration = 0;
            }
            _ctx.file_channels.cancelPendingBoundTo(registration);
        }

        // Whether a usable Slot is registered: the HTTP precheck of a file action (docs/PLAN.md section 5.2).
        bool isReadyToWrite() const noexcept
        {
            const std::shared_lock lock(_mutex);
            return _slot && _slot->isReadyToWrite();
        }

        // Admits one task on the registered Slot: its registration with the reservation, or registration 0
        // when no usable Slot is registered or it has no headroom.
        Admitted tryAdmit(const WaterMarks &admission, FileSize task_bytes) const noexcept
        {
            const std::shared_lock lock(_mutex);
            if (!_slot || !_slot->isReadyToWrite())
                return {};
            auto reservation = _slot->tryAdmit(admission, task_bytes);
            if (!reservation)
                return {};
            return { _registration, std::move(reservation) };
        }

        // Runs `fn` on the registered Slot, null when none, under the registry lock (rule info reads its status).
        template<typename F>
        auto with(F &&fn) const
        {
            const std::shared_lock lock(_mutex);
            return fn(static_cast<const SlotAdmission *>(_slot));
        }

    private:
        Context &_ctx;
        mutable std::shared_mutex _mutex;
        SlotAdmission *_slot = nullptr;
        uint64_t _registration = 0;
        uint64_t _sequence = 0;
    } slot_admission;

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

        static constexpr std::string_view StateString(State state) noexcept
        {
            switch (state)
            {
                case State::Idle:
                    return "IDLE";
                case State::Pending:
                    return "PENDING";
                case State::Playing:
                    return "PLAYING";
                case State::Stopping:
                    return "STOPPING";
            }
            return "UNKNOWN";
        }

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

        // Accepts a trigger from Idle, in one critical section with the gates file admission reads (docs/PLAN.md
        // section 8.1): rules enabled, the schedule open, media not paused. False when Sound is unavailable or
        // stopping, a gate is closed, or a trigger is pending or playing; the caller checks that the
        // configuration enables Sound.
        bool tryTrigger() noexcept
        {
            {
                const std::lock_guard lock(_mutex);
                if (!_available || _stop || _state != State::Idle
                    || !_ctx.record_rule_enabled.load(std::memory_order_relaxed)
                    || !_ctx.schedule_open.load(std::memory_order_acquire) || _ctx.media_barrier.isPaused())
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
    Context(const vg_config_t &cfg, bool run_sound_worker = true)
        : c_api(cfg), file_channels(*this), slot_admission(*this), sound_executor(*this, run_sound_worker)
    {
        action_settings.sync(c_api.media);
    }

    // Destructor
    ~Context() = default;
};

} // namespace vigil

#endif
