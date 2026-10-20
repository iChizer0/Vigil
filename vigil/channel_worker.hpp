#pragma once
#ifndef VIGIL_CHANNEL_WORKER_HPP
#define VIGIL_CHANNEL_WORKER_HPP

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

#include "constants.hpp"
#include "context.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil::workers {

using namespace vigil::constants;
using namespace vigil::types;

// The per-task loop of one automatic file channel (docs/PLAN.md section 7.2), over an adapter that stands for
// the Slot it writes to:
//
//   bool stopRequested() const;                           the Slot is stopping: end the loop and any hold
//   bool isReadyToWrite() const;                          the Slot can take a task
//   uint64_t registration() const;                        the Slot's admission registration: the worker claims
//                                                         only tasks bound to it (section 8.1)
//   std::unique_ptr<W> createWriter(const std::shared_ptr<FileTask> &);   the writer of a claimed task; throws
//                                                                           when it cannot be built
//   void publish(CacheFileInfo &&);                       the filesystem publication and the FILE callback
//   void markFailed();                                    the consecutive failure limit was reached
//   std::string_view name() const;                        for logging
//
// The writer W runs its task to the terminal status with operator(), reports endTime(), and hands over the
// recorded file with takePublication() (empty when nothing is published), generating its thumbnail while it
// still holds its media operation. A task was admitted at dispatch with its budget reserved (section 8.1); it
// runs as claim, write (the writer finishes its sink on every path), takePublication, release the writer and
// with it the media operation, publish outside it, release the budget reservation now that the index sees the
// file, then the hold: the channel holds a Completed or Failed
// task until the interval (plus the cooldown after a failure) has elapsed from the writer's start, and only
// then completes it, while Canceled and Interrupted (a closed gate) complete at once. A constructor failure is
// a failed task like any other. The cooldown and the consecutive failure limit, which marks the adapter failed
// and ends the loop, are constructor parameters with the production defaults (section 13). A Slot runs one
// worker per file channel, Photo and Video: each claims from its own channel and counts its own failures, and
// the two share the Slot's probes, its budget registry, and its publication lock.
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

    // The thread body: one step per probe period until the adapter requests a stop or the failure limit is
    // reached. A task accepted before the worker started is its own Slot's, bound at admission, and is served
    // like any other.
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

    // One probe and, when it claims one, one task: Idle when nothing was claimed, Ran when a task was completed,
    // Stop when the loop must end (a stop request, or the failure limit). The recheck before a claim covers the
    // Slot (ready, and the task's own by registration) and the media pause, never the schedule (docs/PLAN.md
    // section 8.1); the writer rechecks the rule and revision gates before it creates anything.
    Step step() noexcept
    {
        if (!_adapter.isReadyToWrite())
            return Step::Idle;
        // The writer's admission is the linearization point of a pause (section 8.3); this cheap pre-check keeps
        // the task unclaimed.
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

    // Runs one claimed task through to its completion, holding the budget reservation it was admitted with until
    // its file is published. False when the loop must end.
    bool runTask(const std::shared_ptr<FileTask> &task) noexcept
    {
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Starting a ", channelName(), " writer task for ", _adapter.name());
        auto admission = std::move(task->reservation); // released below, or with this frame on a failure
        auto status = WriteEvent::Status::Failed;
        auto end = std::chrono::steady_clock::now() + task->settings->interval; // a constructor failure holds from here
        try
        {
            auto writer = _adapter.createWriter(task);
            end = writer->endTime(); // the interval counts from the writer's start
            status = (*writer)()->status.load(std::memory_order_acquire);
            auto publication = writer->takePublication(); // the thumbnail is generated under the operation
            writer.reset();                               // the media operation leaves here
            if (publication)
                _adapter.publish(std::move(*publication)); // the rename and the FILE callback run outside it
            admission.release();                           // the index sees the file now; nothing is in flight
        }
        catch (const std::exception &e)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, channelName(), " writer task failed for ", _adapter.name(), ": ",
                e.what());
        }
        return finishTask(task, status, end);
    }

    // Ends one task: the channel holds it through the interval (plus the cooldown after a failure), as before,
    // and only then is it completed. False when the loop must end.
    bool finishTask(const std::shared_ptr<FileTask> &task, WriteEvent::Status status,
        std::chrono::steady_clock::time_point end) noexcept
    {
        // Completed and Failed hold the channel until the interval (plus the failure cooldown) has elapsed.
        // Canceled and Interrupted (a closed gate) do not: the loop re-checks the gates on its next probe.
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
        _ctx.file_channels.of(task->type).complete(task, status);
        if (_adapter.stopRequested())
            return false;
        if (_consecutive_failures >= _consecutive_failure_max)
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "The ", channelName(), " worker of ", _adapter.name(),
                " reached the consecutive failure limit, stopping");
            _adapter.markFailed();
            return false;
        }
        return true;
    }

    Context &_ctx;
    Adapter &_adapter;
    const CaptureType _type; // the channel this worker serves
    const std::chrono::steady_clock::duration _failure_cooldown;
    const size_t _consecutive_failure_max;
    size_t _consecutive_failures = 0;
};

} // namespace vigil::workers

#endif
