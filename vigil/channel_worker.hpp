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

// The per-task loop of an automatic file channel (docs/PLAN.md section 7.2), over an adapter that stands for the
// Slot it writes to:
//
//   bool stopRequested() const;                           the Slot is stopping: end the loop and any hold
//   bool isReadyToWrite() const;                          the Slot can take a task
//   Context::InFlightBudgets::Reservation tryAdmit(const WaterMarks &);   the quota and free-space probes with
//                                                                           the task's budget reserved (section 9)
//   std::unique_ptr<W> createWriter(const std::shared_ptr<FileTask> &);   the writer of a claimed task; throws
//                                                                           when it cannot be built
//   void publish(CacheFileInfo &&);                       the filesystem publication and the FILE callback
//   void markFailed();                                    the consecutive failure limit was reached
//   std::string_view name() const;                        for logging
//
// The writer W runs its task to the terminal status with operator(), reports endTime(), and hands over the
// recorded file with takePublication() (empty when nothing is published), generating its thumbnail while it
// still holds its media operation. One task runs as admit, claim, write (the writer finishes its sink on every
// path), takePublication, release the writer and with it the media operation, publish outside it, release the
// budget reservation now that the index sees the file, then the hold: the channel holds a Completed or Failed
// task until the interval (plus the cooldown after a failure) has elapsed from the writer's start, and only
// then completes it, while Canceled and Interrupted (a closed gate) complete at once. A constructor failure is
// a failed task like any other. The cooldown and the consecutive failure limit, which marks the adapter failed
// and ends the loop, are constructor parameters with the production defaults (section 13). One worker per Slot
// serves the enabled channel until Phase 4C-4 gives each channel its own.
template<typename Adapter>
class FileChannelWorker final
{
public:
    enum class Step { Idle, Ran, Stop };

    FileChannelWorker(Context &ctx, Adapter &adapter,
        std::chrono::steady_clock::duration failure_cooldown = kWriterFailureCooldown,
        size_t consecutive_failure_max = kWriterConsecutiveFailureMax) noexcept
        : _ctx(ctx), _adapter(adapter), _failure_cooldown(failure_cooldown),
          _consecutive_failure_max(consecutive_failure_max)
    {
    }

    // The thread body: a task from before this worker started is stale; then one step per probe period until
    // the adapter requests a stop or the failure limit is reached.
    void run() noexcept
    {
        _ctx.file_channels.cancelPending();
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

    // One probe and, when it admits one, one task: Idle when nothing was claimed, Ran when a task was completed,
    // Stop when the loop must end (a stop request, or the failure limit).
    Step step() noexcept
    {
        const auto loaded = _ctx.action_settings.get();
        if (!loaded.settings) [[unlikely]]
        {
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "ActionSettings is null in the channel worker of ", _adapter.name(),
                ", cannot start a writer task");
            return Step::Idle;
        }
        // The single enabled file channel (docs/PLAN.md section 7.1). Nothing to write without one; a pending
        // task is retired by the rule loop, since rules cannot stay enabled without a file action (section 4.1).
        const auto channel = loaded.settings->fileChannel();
        if (!channel || !_adapter.isReadyToWrite())
            return Step::Idle;
        auto admission = _adapter.tryAdmit(loaded.settings->watermarks);
        if (!admission)
            return Step::Idle;
        // The writer's admission is the linearization point of a pause (section 8.3); this cheap pre-check keeps
        // the task unclaimed.
        if (_ctx.media_barrier.isPaused())
            return Step::Idle;
        const auto task = _ctx.file_channels.of(channel->type).claim();
        if (!task)
            return Step::Idle;
        return runTask(task, std::move(admission)) ? Step::Ran : Step::Stop;
    }

private:
    static inline constexpr const std::chrono::milliseconds kHoldPollInterval = std::chrono::milliseconds(10);

    // Runs one claimed task through to its completion, holding its budget reservation until its file is
    // published. False when the loop must end.
    bool runTask(const std::shared_ptr<FileTask> &task, Context::InFlightBudgets::Reservation admission) noexcept
    {
        VG_LOG(_ctx.log, VG_LOG_LEVEL_DEBUG, "Starting a writer task for ", _adapter.name());
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
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "Writer task failed for ", _adapter.name(), ": ", e.what());
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
            VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "The channel worker of ", _adapter.name(),
                " reached the consecutive failure limit, stopping");
            _adapter.markFailed();
            return false;
        }
        return true;
    }

    Context &_ctx;
    Adapter &_adapter;
    const std::chrono::steady_clock::duration _failure_cooldown;
    const size_t _consecutive_failure_max;
    size_t _consecutive_failures = 0;
};

} // namespace vigil::workers

#endif
