#pragma once
#ifndef VIGIL_ASYNC_TASK_HPP
#define VIGIL_ASYNC_TASK_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "configs.hpp"
#include "constants.hpp"
#include "context.hpp"
#include "json.hpp" // IWYU pragma: keep
#include "log.hpp"
#include "status.hpp"
#include "storage.hpp"

namespace vigil::async_task {

using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::status;
using namespace vigil::storage;

enum class AsyncTaskStatus {
    Queued,
    Running,
    Succeeded,
    Failed,
    CancelRequested,
    Canceled,
};

class AsyncTask
{
private:
    static inline std::atomic<size_t> _counter = 0;

    static inline size_t nextCounter() noexcept
    {
        return _counter.fetch_add(1, std::memory_order_relaxed);
    }

public:
    AsyncTask() noexcept
        : _id(nextCounter()), _created_timestamp(std::chrono::system_clock::now()),
          _updated_timestamp(_created_timestamp), _status(AsyncTaskStatus::Queued), _progress_percent(0)
    {
    }

    virtual ~AsyncTask() = default;

    inline AsyncTaskStatus status() const noexcept
    {
        return _status.load(std::memory_order_acquire);
    }

    bool isDone(AsyncTaskStatus status) const noexcept
    {
        return status == AsyncTaskStatus::Succeeded || status == AsyncTaskStatus::Failed
               || status == AsyncTaskStatus::Canceled;
    }

    bool isCancelable(AsyncTaskStatus status) const noexcept
    {
        switch (status)
        {
            case AsyncTaskStatus::Queued:
                return true;
            case AsyncTaskStatus::Running:
                return isRunningCancelable();
            default:
                return false;
        }
    }

    void inspect(nlohmann::json &j) const
    {
        serializeTask(j);

        j["sUID"] = uid();
        j["iCreatedTimestamp"]
            = std::chrono::duration_cast<std::chrono::milliseconds>(_created_timestamp.time_since_epoch()).count();
        j["iUpdatedTimestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            _updated_timestamp.load(std::memory_order_acquire).time_since_epoch())
                                     .count();

        const auto status = _status.load(std::memory_order_acquire);
        j["sStatus"] = [status]() {
            switch (status)
            {
                case AsyncTaskStatus::Queued:
                    return "QUEUED";
                case AsyncTaskStatus::Running:
                    return "RUNNING";
                case AsyncTaskStatus::Succeeded:
                    return "SUCCEEDED";
                case AsyncTaskStatus::Failed:
                    return "FAILED";
                case AsyncTaskStatus::CancelRequested:
                    return "CANCEL_REQUESTED";
                case AsyncTaskStatus::Canceled:
                    return "CANCELED";
                default:
                    return "UNKNOWN";
            }
        }();

        j["bCancelable"] = isCancelable(status);
        j["bDone"] = isDone(status);

        {
            const auto progress = _progress_percent.load(std::memory_order_relaxed);
            j["iProgressPercent"] = std::min(std::max(progress, 0), 100);
        }
        {
            auto res_j = nlohmann::json::object();
            serializeResult(res_j);
            j["dResult"] = res_j;
        }
    }

    bool cancel() noexcept
    {
        auto status = _status.load(std::memory_order_acquire);
        if (!isCancelable(status))
            return false;
        while (!_status.compare_exchange_strong(status, AsyncTaskStatus::CancelRequested, std::memory_order_acq_rel))
        {
            if (!isCancelable(status))
                return false;
        }
        touchUpdated();
        return true;
    }

    void operator()(const Context &ctx, void *external_ctx) noexcept
    {
        auto status = _status.load(std::memory_order_acquire);
        while (status == AsyncTaskStatus::Queued)
        {
            if (_status.compare_exchange_strong(status, AsyncTaskStatus::Running, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                status = AsyncTaskStatus::Running;
                break;
            }
        }
        if (status == AsyncTaskStatus::CancelRequested)
        {
            _status.store(AsyncTaskStatus::Canceled, std::memory_order_release);
            touchUpdated();
            return;
        }
        if (status != AsyncTaskStatus::Running)
            return;
        touchUpdated();

        AsyncTaskStatus final_status = AsyncTaskStatus::Failed;
        try
        {
            final_status = executeTask(ctx, external_ctx);
        }
        catch (const std::exception &e)
        {
            final_status = AsyncTaskStatus::Failed;
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Exception during async task execution: ", e.what());
        }
        catch (...)
        {
            final_status = AsyncTaskStatus::Failed;
            VG_LOG(ctx.log, VG_LOG_LEVEL_ERROR, "Unknown exception during async task execution");
        }

        _status.store(final_status, std::memory_order_release);
        touchUpdated();
    }

    std::string uid() const noexcept
    {
        return std::to_string(_id) + "-" + std::to_string(tag());
    }

    virtual uint64_t tag() const noexcept = 0;

    virtual bool isRunningCancelable() const noexcept = 0;

protected:
    inline bool cancelRequested() const noexcept
    {
        return _status.load(std::memory_order_acquire) == AsyncTaskStatus::CancelRequested;
    }

    inline void touchUpdated() const noexcept
    {
        _updated_timestamp.store(std::chrono::system_clock::now(), std::memory_order_release);
    }

    virtual void serializeTask(nlohmann::json &j) const = 0;

    virtual void serializeResult(nlohmann::json &j) const = 0;

    virtual AsyncTaskStatus executeTask(const Context &ctx, void *external_ctx) = 0;

private:
    const size_t _id;
    const std::chrono::system_clock::time_point _created_timestamp;
    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<
        std::chrono::system_clock::time_point> _updated_timestamp;
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<AsyncTaskStatus> _status;

protected:
    alignas(kHardwareDestructiveInterferenceSize) std::atomic<int> _progress_percent;
    alignas(kHardwareDestructiveInterferenceSize) mutable std::shared_mutex _result_mutex;
};

class StorageAsyncTask: public AsyncTask
{
public:
    static std::string StableName(const StorageControlConfig &config)
    {
        return std::string(StorageControlConfig::ActionString(config.action)) + "(" + config.slot_dev_path + ")";
    }

    static uint64_t StableTag(const StorageControlConfig &config) noexcept
    {
        std::hash<std::string> hasher;
        return hasher(config.slot_dev_path)
               ^ (hasher(std::string(StorageControlConfig::ActionString(config.action))) << 1);
    }

    explicit StorageAsyncTask(StorageControlConfig &&config)
        : AsyncTask(), _dev_path(config.slot_dev_path), _action(StorageControlConfig::ActionString(config.action)),
          _action_type(config.action), _tag(StableTag(config))
    {
        if (_dev_path.empty())
            throw std::invalid_argument("AsyncStorageAction dev_path cannot be empty");
        if (_action_type == StorageControlConfig::ActionType::Unknown)
            throw std::invalid_argument("AsyncStorageAction action has invalid action type");

        if (_action_type == StorageControlConfig::ActionType::RemoveFilesOrDirectories)
            _action_request.files_or_directories_to_remove = std::move(config.files_or_directories_to_remove);
    }

    ~StorageAsyncTask() override = default;

    uint64_t tag() const noexcept override
    {
        return _tag;
    }

    bool isRunningCancelable() const noexcept override
    {
        return StorageControlConfig::ActionIsRunningCancelable(_action_type);
    }

protected:
    struct Request
    {
        std::optional<std::vector<std::filesystem::path>> files_or_directories_to_remove;
    };

    struct Result
    {
        vg_sta_e code = 0;
        std::string message;
        std::shared_ptr<SlotFileOpsProgress> file_ops_progress = nullptr;
    };
    void serializeTask(nlohmann::json &j) const override
    {
        j["sDevPath"] = _dev_path;
        j["sAction"] = _action;
    }

    void serializeResult(nlohmann::json &j) const override
    {
        const std::shared_lock<std::shared_mutex> lock(_result_mutex);

        if (_action_result.code != 0)
            j["iCode"] = _action_result.code;
        if (!_action_result.message.empty())
            j["sMessage"] = _action_result.message;
        if (_action_result.file_ops_progress)
        {
            j["sFileOpsType"] = SlotFileOpsProgress::TypeToString(_action_result.file_ops_progress->type);
            j["iRequestedCount"] = _action_result.file_ops_progress->requested_count;
            j["iProcessedCount"] = _action_result.file_ops_progress->processed_count;
            j["iSuccessCount"] = _action_result.file_ops_progress->success_count;
            j["iFailedCount"] = _action_result.file_ops_progress->failed_count;
            // Intentionally report aggregate counts only; retaining per-path results can become expensive for large
            // storage operations.
        }
    }

    AsyncTaskStatus executeTask(const Context &, void *external_ctx) override
    {
        vg_sta_e ret = 0;
        std::string message;

        if (!external_ctx)
        {
            ret = EINVAL;
            message = "External context is required for executing async storage action";
        }
        else
        {
            Storage *storage = static_cast<Storage *>(external_ctx);
            switch (_action_type)
            {
                case StorageControlConfig::ActionType::FormatSlot:
                    ret = handleActionFormat(*storage);
                    break;
                case StorageControlConfig::ActionType::EjectSlot:
                    ret = handleActionEject(*storage);
                    break;
                case StorageControlConfig::ActionType::RemoveFilesOrDirectories:
                    ret = handleActionRemoveFilesOrDirectories(*storage);
                    break;
                case StorageControlConfig::ActionType::FreeUpSlot:
                    ret = handleActionFreeUp(*storage);
                    break;
                default:
                    ret = EINVAL;
                    message
                        = "Unsupported async storage action type: " + std::to_string(static_cast<int>(_action_type));
                    break;
            }
        }

        {
            const std::unique_lock lock(_result_mutex);
            _action_result.code = ret;
            if (!message.empty())
                _action_result.message = std::move(message);
        }

        if (cancelRequested())
            return AsyncTaskStatus::Canceled;
        return ret == 0 ? AsyncTaskStatus::Succeeded : AsyncTaskStatus::Failed;
    }

private:
    vg_sta_e handleActionFormat(Storage &storage)
    {
        const auto ret = storage.formatSlot(_dev_path);
        if (ret == 0)
            _progress_percent.store(100, std::memory_order_release);
        return ret;
    }

    vg_sta_e handleActionEject(Storage &storage)
    {
        const auto ret = storage.ejectSlot(_dev_path);
        if (ret == 0)
            _progress_percent.store(100, std::memory_order_release);
        return ret;
    }

    vg_sta_e handleActionRemoveFilesOrDirectories(Storage &storage)
    {
        if (!_action_request.files_or_directories_to_remove || _action_request.files_or_directories_to_remove->empty())
            return EINVAL;

        auto progress = std::make_shared<SlotFileOpsProgress>(SlotFileOpsProgress::Type::Remove);
        {
            const std::unique_lock lock(_result_mutex);
            _action_result.file_ops_progress = progress;
        }

        const auto cancel_check = [this]() { return cancelRequested(); };
        const auto on_progress = [this, progress](const SlotFileOpsProgress &new_progress) {
            if (new_progress.requested_count > 0)
                _progress_percent.store(
                    static_cast<int>(std::roundf(static_cast<float>(new_progress.processed_count)
                                                 / static_cast<float>(new_progress.requested_count) * 100.f)),
                    std::memory_order_relaxed);
            {
                const std::unique_lock lock(_result_mutex);
                *progress = new_progress;
            }
            touchUpdated();
        };

        return storage.removeFilesOrDirectories(_dev_path, *_action_request.files_or_directories_to_remove,
            progress.get(), cancel_check, on_progress);
    }

    vg_sta_e handleActionFreeUp(Storage &storage)
    {
        auto progress = std::make_shared<SlotFileOpsProgress>(SlotFileOpsProgress::Type::FreeUp);

        {
            const std::unique_lock lock(_result_mutex);
            _action_result.file_ops_progress = progress;
        }

        const auto cancel_check = [this]() { return cancelRequested(); };
        const auto on_progress = [this, progress](const SlotFileOpsProgress &new_progress) {
            if (new_progress.requested_count > 0)
                _progress_percent.store(
                    static_cast<int>(std::roundf(static_cast<float>(new_progress.processed_count)
                                                 / static_cast<float>(new_progress.requested_count) * 100.f)),
                    std::memory_order_relaxed);
            {
                const std::unique_lock lock(_result_mutex);
                *progress = new_progress;
            }
            touchUpdated();
        };

        const auto ret = storage.freeUpSlot(_dev_path, progress.get(), cancel_check, on_progress);
        if (ret == 0)
            _progress_percent.store(100, std::memory_order_release);
        return ret;
    }

private:
    const std::string _dev_path;
    const std::string _action;
    const StorageControlConfig::ActionType _action_type;
    const uint64_t _tag;

    Request _action_request;
    Result _action_result;
};

class AsyncTaskWorker
{
public:
    using TaskFilter = std::function<bool(const AsyncTask &)>;

    // Maximum number of done tasks retained per tag for subsequent status/cancel queries.
    static inline constexpr size_t kRecentHistoryLimit = 3;

    // Maximum number of tasks queued but not yet running.
    static inline constexpr size_t kMaxQueuedTasks = 6;

    explicit AsyncTaskWorker(Context &ctx, void *external_ctx) : _ctx(ctx), _external_ctx(external_ctx)
    {
        _stop_token.store(false, std::memory_order_release);
        _worker_thread = std::make_unique<std::thread>([this]() { workerLoop(); });
        if (!_worker_thread)
            throw std::runtime_error("Failed to start AsyncTaskWorker thread");
    }

    ~AsyncTaskWorker() noexcept
    {
        _stop_token.store(true, std::memory_order_release);
        {
            const std::unique_lock lock(_task_mutex);
            // Best-effort cancellation of anything still in flight.
            if (_running_task)
                _running_task->cancel();
            for (auto &task: _queued_tasks)
                task->cancel();
        }
        _task_cv.notify_all();
        try
        {
            if (_worker_thread && _worker_thread->joinable())
                _worker_thread->join();
        }
        catch (...)
        {
        }
    }

    AsyncTaskWorker(const AsyncTaskWorker &) = delete;
    AsyncTaskWorker &operator=(const AsyncTaskWorker &) = delete;
    AsyncTaskWorker(AsyncTaskWorker &&) = delete;
    AsyncTaskWorker &operator=(AsyncTaskWorker &&) = delete;

    bool full() const noexcept
    {
        const std::shared_lock lock(_task_mutex);
        return _queued_tasks.size() >= kMaxQueuedTasks;
    }

    std::optional<std::string> submit(std::shared_ptr<AsyncTask> task)
    {
        if (!task)
            return std::nullopt;
        if (task->status() != AsyncTaskStatus::Queued)
            return std::nullopt;

        auto uid = task->uid();
        {
            const std::unique_lock lock(_task_mutex);
            if (_stop_token.load(std::memory_order_acquire))
                return std::nullopt;
            if (_queued_tasks.size() >= kMaxQueuedTasks)
                return std::nullopt;
            _queued_tasks.push_back(std::move(task));
        }
        _task_cv.notify_one();
        return uid;
    }

    std::vector<std::pair<size_t, std::shared_ptr<AsyncTask>>> status(const TaskFilter &filter) const
    {
        std::vector<std::pair<size_t, std::shared_ptr<AsyncTask>>> tasks;

        {
            const std::shared_lock lock(_task_mutex);

            // Queued: position 1..N, front is next to run.
            {
                size_t pos = 1;
                for (const auto &task: _queued_tasks)
                {
                    if (task && (!filter || filter(*task)))
                    {
                        tasks.emplace_back(pos, task);
                    }
                    ++pos;
                }
            }

            // Running: position 0.
            if (_running_task && (!filter || filter(*_running_task)))
                tasks.emplace_back(0, _running_task);

            // History: done tasks; no queue position.
            for (const auto &[_, bucket]: _history)
            {
                for (const auto &task: bucket)
                {
                    if (task && (!filter || filter(*task)))
                    {
                        tasks.emplace_back(std::numeric_limits<size_t>::max(), task);
                    }
                }
            }
        }

        return tasks;
    }

    std::vector<std::shared_ptr<AsyncTask>> cancel(const TaskFilter &filter)
    {
        std::vector<std::shared_ptr<AsyncTask>> cancelled_tasks;
        {
            const std::unique_lock lock(_task_mutex);
            if (_running_task && (!filter || filter(*_running_task)))
            {
                if (_running_task->cancel())
                    cancelled_tasks.push_back(_running_task);
            }
            for (auto &task: _queued_tasks)
            {
                if (task && (!filter || filter(*task)))
                    if (task->cancel())
                        cancelled_tasks.push_back(task);
            }
        }
        if (!cancelled_tasks.empty())
            _task_cv.notify_all();
        return cancelled_tasks;
    }

private:
    void workerLoop() noexcept
    {
        while (true)
        {
            std::shared_ptr<AsyncTask> task;
            {
                std::unique_lock<std::shared_mutex> lock(_task_mutex);
                _task_cv.wait(lock,
                    [this]() { return _stop_token.load(std::memory_order_acquire) || !_queued_tasks.empty(); });
                if (_stop_token.load(std::memory_order_acquire) && _queued_tasks.empty())
                    break;

                task = std::move(_queued_tasks.front());
                _queued_tasks.pop_front();
                _running_task = task;
            }

            if (task)
            {
                try
                {
                    (*task)(_ctx, _external_ctx);
                }
                catch (const std::exception &e)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "AsyncTaskWorker caught exception: ", e.what());
                }
                catch (...)
                {
                    VG_LOG(_ctx.log, VG_LOG_LEVEL_ERROR, "AsyncTaskWorker caught unknown exception");
                }
            }

            {
                const std::unique_lock<std::shared_mutex> lock(_task_mutex);
                _running_task.reset();
                if (task)
                {
                    auto &bucket = _history[task->tag()];
                    bucket.push_back(std::move(task));
                    while (bucket.size() > kRecentHistoryLimit)
                        bucket.pop_front();
                }
            }
        }
    }

private:
    Context &_ctx;
    void *const _external_ctx;

    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _stop_token { false };
    std::unique_ptr<std::thread> _worker_thread;

    mutable std::shared_mutex _task_mutex;
    std::condition_variable_any _task_cv;

    std::deque<std::shared_ptr<AsyncTask>> _queued_tasks;
    std::shared_ptr<AsyncTask> _running_task;
    std::unordered_map<uint64_t, std::deque<std::shared_ptr<AsyncTask>>> _history;
};

} // namespace vigil::async_task

#endif
