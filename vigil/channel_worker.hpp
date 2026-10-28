#pragma once
#ifndef VIGIL_CHANNEL_WORKER_HPP
#define VIGIL_CHANNEL_WORKER_HPP

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "constants.hpp"
#include "context.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil::workers {

using namespace vigil::constants;
using namespace vigil::types;

// The per-task loop of one automatic file channel, over an adapter that stands for the Slot it writes to:
//
//   bool stopRequested() const;
//   bool isReadyToWrite() const;
//   uint64_t registration() const;                                       only tasks bound to it are claimed
//   std::unique_ptr<W> createWriter(const std::shared_ptr<FileTask> &);  throws when it cannot be built
//   bool publish(CacheFileInfo &&);                                      false when the file could not be published
//   void prepareContinuousClip(std::optional<steady_clock::time_point> boundary);
//   void markFailed();                                                   before the failed task is completed
//   std::string_view name() const;
//
// The writer W runs its task with operator() and provides endTime(), boundaryKeyFrame(), and takePublication().
template<typename Adapter>
class FileChannelWorker final
{
public:
    enum class Step { Idle, Ran, Stop };

    FileChannelWorker(Context &ctx, Adapter &adapter, CaptureType type,
        std::chrono::steady_clock::duration failure_cooldown = kWriterFailureCooldown,
        size_t consecutive_failure_max = kWriterConsecutiveFailureMax) noexcept
        : _ctx(ctx), _adapter(adapter), _type(type), _failure_cooldown(failure_cooldown),
          _consecutive_failure_max(consecutive_failure_max)
    {
    }

    void run() noexcept
    {
        while (!_adapter.stopRequested())
        {
            const auto start = std::chrono::steady_clock::now();
            switch (step())
            {
                case Step::Stop:
                    return;
                case Step::Ran:
                    continue; // the hold paced this round
                case Step::Idle:
                    break;
            }
            if (const auto elapsed = std::chrono::steady_clock::now() - start; elapsed < kWriterProbeInterval)
                std::this_thread::sleep_for(kWriterProbeInterval - elapsed);
        }
    }

    // Before a claim, the Slot and the pause are rechecked, not the schedule; the writer rechecks its gates before it
    // creates anything.
    Step step() noexcept
    {
        if (!_adapter.isReadyToWrite())
            return Step::Idle;
        // Leaves the task pending while paused; the writer's admission is what orders a task against a pause
        if (_ctx.media_barrier.isPaused())
            return Step::Idle;
        const auto task = _ctx.file_channels.of(_type).claim(_adapter.registration());
        if (!task)
            return Step::Idle;
        return runTask(task) ? Step::Ran : Step::Stop;
    }

private:
    static inline constexpr const std::chrono::milliseconds kHoldPollInterval = std::chrono::milliseconds(10);

    const char *channelName() const noexcept
    {
        return _type == CaptureType::Image ? "Photo" : "Video";
    }

    // False when the loop must end.
    bool runTask(const std::shared_ptr<FileTask> &task) noexcept
    {
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Starting a ", channelName(), " writer task for ", _adapter.name());
        auto admission = std::move(task->reservation); // released after publication, or at once on a throw
        const bool continuous = task->kind == FileTask::Kind::Continuous;
        const auto boundary = std::exchange(_boundary, std::nullopt);
        auto status = WriteEvent::Status::Failed;
        auto end = std::chrono::steady_clock::now() + task->settings->interval; // a constructor failure holds from here
        try
        {
            if (continuous) // a triggered clip keeps its pre-roll
                _adapter.prepareContinuousClip(boundary);
            auto writer = _adapter.createWriter(task);
            end = writer->endTime(); // the interval counts from the writer's start
            status = (*writer)()->status.load(std::memory_order_acquire);
            const auto boundary_key_frame = writer->boundaryKeyFrame();
            auto publication = writer->takePublication(); // the thumbnail is generated under the operation
            writer.reset();                               // the media operation leaves here
            if (publication && !_adapter.publish(std::move(*publication)))
            {
                status = WriteEvent::Status::Failed; // a lost file is a failed task, reported so through the hold
                task->event->status.store(status, std::memory_order_release);
            }
            if (continuous && status == WriteEvent::Status::Completed)
                _boundary = boundary_key_frame;
            admission.release(); // the index sees the file now
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, channelName(), " writer task failed for ", _adapter.name(), ": ",
                e.what());
            admission.release();
        }
        return finishTask(task, status, end);
    }

    // False when the loop must end.
    bool finishTask(const std::shared_ptr<FileTask> &task, WriteEvent::Status status,
        std::chrono::steady_clock::time_point end) noexcept
    {
        // Completed and Failed hold the channel until the interval, plus the cooldown after a failure, has elapsed;
        // Canceled and Interrupted do not
        if (status == WriteEvent::Status::Completed || status == WriteEvent::Status::Failed)
        {
            if (status == WriteEvent::Status::Failed)
            {
                end += _failure_cooldown;
                ++_consecutive_failures;
            }
            else
                _consecutive_failures = 0;
            while (std::chrono::steady_clock::now() < end && !_adapter.stopRequested())
                std::this_thread::sleep_for(kHoldPollInterval);
        }
        // Marked before the task completes, so nothing is admitted once the channel is idle; a stop skips the mark
        const bool stopping = _adapter.stopRequested();
        const bool limit_reached = !stopping && _consecutive_failures >= _consecutive_failure_max;
        if (limit_reached)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "The ", channelName(), " worker of ", _adapter.name(),
                " reached the consecutive failure limit, stopping");
            _adapter.markFailed();
        }
        _ctx.file_channels.of(task->type).complete(task, status);
        return !stopping && !limit_reached;
    }

    Context &_ctx;
    Adapter &_adapter;
    const CaptureType _type;
    const std::chrono::steady_clock::duration _failure_cooldown;
    const size_t _consecutive_failure_max;
    size_t _consecutive_failures = 0;
    // The key frame the last task ended on, when it was a continuous clip completed on one and published, left for the
    // next clip
    std::optional<std::chrono::steady_clock::time_point> _boundary;
};

} // namespace vigil::workers

#endif
