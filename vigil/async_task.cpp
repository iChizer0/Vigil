#include "async_task.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "configs.hpp"
#include "constants.hpp"
#include "storage.hpp"

namespace vigil::types {

using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::storage;

struct AsyncTaskWorker::Impl final
{
    static inline constexpr size_t kRecentHistoryLimit = 3;
    static inline constexpr size_t kPerSlotUndoneLimit = 3;

    struct TaskIdentity final
    {
        StorageControlConfig::ActionType action = StorageControlConfig::ActionType::Unknown;
        std::string_view slot_dev_path;
    };

    explicit Impl(Storage &storage)
        : _storage(storage), _stop_token(false), _worker_thread(), _task_mutex(), _task_cv(), _queued_tasks(),
          _running_task(), _task_histories(), _last_task_created_timestamp(0)
    {
        _worker_thread = std::make_unique<std::thread>(&Impl::workerThreadTask, this);
    }

    ~Impl() noexcept
    {
        try
        {
            {
                const std::lock_guard lock(_task_mutex);
                prepareWorkerShutdownLocked();
                _stop_token.store(true, std::memory_order_release);
            }
            _task_cv.notify_all();

            if (_worker_thread)
            {
                _worker_thread->join();
                _worker_thread.reset();
            }
        }
        catch (...)
        {
        }
    }

    void handleSubmit(const StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret)
    {
        const auto uid = makeTaskUid(control_config.action, control_config.slot_dev_path);

        {
            const std::unique_lock lock(_task_mutex);
            trimTaskHistoriesLocked();

            if (countUndoneTasksForSlotLocked(control_config.slot_dev_path) >= kPerSlotUndoneLimit)
            {
                ret = EBUSY;
                res_j["message"] = "Storage async submission limit reached for " + control_config.slot_dev_path;
                return;
            }

            auto &task_history = _task_histories[uid];
            auto task = std::make_shared<AsyncTask>();
            task->uid = uid;
            task->created_timestamp = nextTaskCreatedTimestampLocked();
            task->status = AsyncTaskStatus::Queued;
            task->updated_timestamp = task->created_timestamp;
            task->result = std::make_unique<nlohmann::json>(makeTaskResult());
            if (control_config.files_or_directories_to_remove)
            {
                task->remove_paths = *control_config.files_or_directories_to_remove;
                task->result = std::make_unique<nlohmann::json>(makeRemoveTaskInitialResult(task->remove_paths.size()));
            }

            task_history.emplace_back(task);
            trimTaskHistoryLocked(task_history);
            _queued_tasks.emplace_back(task);
            res_j["dTasks"] = serializeTaskHistoryLocked(task_history);
        }

        _task_cv.notify_one();
    }

    void handleStatus(const StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret)
    {
        const std::unique_lock lock(_task_mutex);
        trimTaskHistoriesLocked();

        if (const auto *task_history = findTaskHistoryLocked(control_config.action, control_config.slot_dev_path);
            task_history != nullptr && !task_history->empty())
        {
            res_j["dTasks"] = serializeTaskHistoryLocked(*task_history);
            return;
        }

        ret = ENOENT;
        res_j["message"] = "No " + std::string(StorageControlConfig::ActionString(control_config.action))
                           + " task history found for " + control_config.slot_dev_path;
    }

    void handleCancel(const StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret)
    {
        const auto action = control_config.action;
        const std::unique_lock lock(_task_mutex);
        trimTaskHistoriesLocked();

        auto *task_history = findTaskHistoryLocked(action, control_config.slot_dev_path);
        if (task_history == nullptr || task_history->empty())
        {
            ret = ENOENT;
            res_j["message"] = "No " + std::string(StorageControlConfig::ActionString(action))
                               + " task history found for " + control_config.slot_dev_path;
            return;
        }

        bool canceled_any = false;
        for (const auto &task: *task_history)
        {
            if (!task || IsFinalAsyncTaskStatus(task->status))
                continue;

            if (task->status == AsyncTaskStatus::Queued)
            {
                if (auto it = std::find(_queued_tasks.begin(), _queued_tasks.end(), task); it != _queued_tasks.end())
                    _queued_tasks.erase(it);
                task->status = AsyncTaskStatus::Canceled;
                task->result = mergedTaskResultLocked(task);
                setTaskResultStatus(*task->result, ECANCELED, "Canceled before start");
                task->updated_timestamp = unixTimeMs();
                canceled_any = true;
                continue;
            }

            if (task->status == AsyncTaskStatus::Running && isRunningCancelableTaskLocked(task))
            {
                task->status = AsyncTaskStatus::CancelRequested;
                task->updated_timestamp = unixTimeMs();
                canceled_any = true;
                continue;
            }

            if (task->status == AsyncTaskStatus::CancelRequested && isRunningCancelableTaskLocked(task))
                canceled_any = true;
        }

        res_j["dTasks"] = serializeTaskHistoryLocked(*task_history);
        if (canceled_any)
            return;

        ret = EBUSY;
        res_j["message"] = "No cancelable " + std::string(StorageControlConfig::ActionString(action))
                           + " tasks remain for " + control_config.slot_dev_path;
    }

private:
    static int64_t unixTimeMs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static std::optional<TaskIdentity> tryParseTaskUid(const std::string_view uid) noexcept
    {
        const auto sep = uid.find('\n');
        if (sep == std::string_view::npos)
            return std::nullopt;

        const auto action_str = uid.substr(0, sep);
        const auto slot_dev_path = uid.substr(sep + 1);
        if (slot_dev_path.empty())
            return std::nullopt;

        if (const auto action = StorageControlConfig::ActionTypeFromString(action_str); action.has_value())
            return TaskIdentity { .action = *action, .slot_dev_path = slot_dev_path };

        return std::nullopt;
    }

    static std::string makeTaskUid(const StorageControlConfig::ActionType action, std::string_view slot_dev_path)
    {
        return std::string(StorageControlConfig::ActionString(action)) + '\n' + std::string(slot_dev_path);
    }

    std::deque<std::shared_ptr<AsyncTask>> *findTaskHistoryLocked(const std::string_view uid)
    {
        if (auto it = _task_histories.find(std::string(uid)); it != _task_histories.end())
            return &it->second;
        return nullptr;
    }

    const std::deque<std::shared_ptr<AsyncTask>> *findTaskHistoryLocked(const std::string_view uid) const
    {
        if (auto it = _task_histories.find(std::string(uid)); it != _task_histories.end())
            return &it->second;
        return nullptr;
    }

    std::deque<std::shared_ptr<AsyncTask>> *findTaskHistoryLocked(const StorageControlConfig::ActionType action,
        std::string_view slot_dev_path)
    {
        return findTaskHistoryLocked(makeTaskUid(action, slot_dev_path));
    }

    const std::deque<std::shared_ptr<AsyncTask>> *findTaskHistoryLocked(const StorageControlConfig::ActionType action,
        std::string_view slot_dev_path) const
    {
        return findTaskHistoryLocked(makeTaskUid(action, slot_dev_path));
    }

    int64_t nextTaskCreatedTimestampLocked() noexcept
    {
        auto created_timestamp = unixTimeMs();
        if (created_timestamp <= _last_task_created_timestamp)
            created_timestamp = _last_task_created_timestamp + 1;
        _last_task_created_timestamp = created_timestamp;
        return created_timestamp;
    }

    static int computePercent(const size_t processed, const size_t total) noexcept
    {
        if (total == 0)
            return -1;
        return static_cast<int>(std::min<size_t>(100, (processed * 100) / total));
    }

    void trimTaskHistoryLocked(std::deque<std::shared_ptr<AsyncTask>> &task_history) const
    {
        while (task_history.size() > kRecentHistoryLimit && !task_history.empty() && task_history.front()
               && IsFinalAsyncTaskStatus(task_history.front()->status))
            task_history.pop_front();
    }

    void trimTaskHistoriesLocked()
    {
        for (auto it = _task_histories.begin(); it != _task_histories.end();)
        {
            trimTaskHistoryLocked(it->second);
            if (it->second.empty())
                it = _task_histories.erase(it);
            else
                ++it;
        }
    }

    size_t countUndoneTasksForSlotLocked(std::string_view slot_dev_path) const
    {
        size_t count = 0;
        for (const auto &[_, task_history]: _task_histories)
        {
            for (const auto &task: task_history)
            {
                const auto identity = task ? tryParseTaskUid(task->uid) : std::nullopt;
                if (identity.has_value() && identity->slot_dev_path == slot_dev_path
                    && !IsFinalAsyncTaskStatus(task->status))
                    ++count;
            }
        }
        return count;
    }

    static nlohmann::json buildRemoveTaskResult(const vigil::status::SlotRemoveFilesProgress &progress,
        const bool include_removed_paths)
    {
        auto result = nlohmann::json::object();
        result["iRequestedCount"] = progress.requested_count;
        result["iRemovedCount"] = progress.removed_count;
        result["iFailedCount"] = progress.failed_count;
        if (!include_removed_paths)
            return result;

        result["lRemovedFilesOrDirectories"] = nlohmann::json::array();
        for (const auto &path: progress.removed_paths)
            result["lRemovedFilesOrDirectories"].emplace_back(path.string());

        return result;
    }

    static nlohmann::json makeTaskResult()
    {
        return {
            { "iCode", nullptr },
            { "sMessage", nullptr },
        };
    }

    static void mergeTaskResultData(nlohmann::json &result, const nlohmann::json &data)
    {
        if (!data.is_object())
            return;
        for (const auto &[key, value]: data.items())
            result[key] = value;
    }

    static void setTaskResultStatus(nlohmann::json &result, const vg_sta_e result_code,
        const std::string_view result_message)
    {
        result["iCode"] = result_code;
        result["sMessage"] = std::string(result_message);
    }

    static nlohmann::json makeRemoveTaskInitialResult(const size_t requested_count)
    {
        auto result = makeTaskResult();
        result["iRequestedCount"] = requested_count;
        result["iRemovedCount"] = 0;
        result["iFailedCount"] = 0;
        return result;
    }

    std::unique_ptr<nlohmann::json> mergedTaskResultLocked(const std::shared_ptr<AsyncTask> &task,
        const nlohmann::json *data = nullptr) const
    {
        auto result = std::make_unique<nlohmann::json>(task && task->result ? *task->result : makeTaskResult());
        if (data)
            mergeTaskResultData(*result, *data);
        return result;
    }

    static nlohmann::json buildFreeUpTaskResult(const vigil::status::SlotFreeUpProgress &progress)
    {
        return {
            { "iTotalEntries", progress.total_entries },
            { "iProcessedCount", progress.processed_entries },
            { "iRemovedCount", progress.removed_entries },
            { "iFailedCount", progress.failed_entries },
        };
    }

    int queuePositionOfTaskLocked(const std::shared_ptr<AsyncTask> &task) const
    {
        if (_running_task && _running_task == task)
            return 0;
        for (size_t i = 0; i < _queued_tasks.size(); ++i)
        {
            if (_queued_tasks[i] == task)
                return static_cast<int>(i + 1);
        }
        return -1;
    }

    bool isRunningCancelableTaskLocked(const std::shared_ptr<AsyncTask> &task) const
    {
        if (!task)
            return false;
        if (const auto identity = tryParseTaskUid(task->uid); identity.has_value())
            return StorageControlConfig::ActionIsRunningCancelable(identity->action);
        return false;
    }

    bool isTaskCancelableLocked(const std::shared_ptr<AsyncTask> &task) const
    {
        if (!task)
            return false;
        switch (task->status)
        {
            case AsyncTaskStatus::Queued:
                return true;
            case AsyncTaskStatus::Running:
                return isRunningCancelableTaskLocked(task);
            default:
                return false;
        }
    }

    nlohmann::json serializeTaskLocked(const std::shared_ptr<AsyncTask> &task) const
    {
        auto task_j = nlohmann::json::object();
        if (!task)
            return task_j;

        const auto queue_position = queuePositionOfTaskLocked(task);
        const auto is_done = IsFinalAsyncTaskStatus(task->status);
        const auto identity = tryParseTaskUid(task->uid);

        task_j["iCreatedTimestamp"] = task->created_timestamp;
        task_j["sAction"] = identity.has_value()
                                ? nlohmann::json(std::string(StorageControlConfig::ActionString(identity->action)))
                                : nlohmann::json(nullptr);
        task_j["sSlotDevPath"]
            = identity.has_value() ? nlohmann::json(std::string(identity->slot_dev_path)) : nlohmann::json(nullptr);
        task_j["sStatus"] = std::string(AsyncTaskStatusString(task->status));
        task_j["bDone"] = is_done;
        task_j["bCancelable"] = isTaskCancelableLocked(task);
        task_j["iQueuePosition"]
            = (!is_done && queue_position >= 0) ? nlohmann::json(queue_position) : nlohmann::json(nullptr);
        task_j["iProgressPercent"]
            = task->progress_percent >= 0 ? nlohmann::json(task->progress_percent) : nlohmann::json(nullptr);
        task_j["iUpdatedTimestamp"] = task->updated_timestamp;
        task_j["dResult"] = task->result ? *task->result : nlohmann::json(nullptr);
        return task_j;
    }

    nlohmann::json serializeTaskHistoryLocked(const std::deque<std::shared_ptr<AsyncTask>> &task_history) const
    {
        auto task_history_j = nlohmann::json::object();
        for (const auto &task: task_history)
        {
            if (task)
                task_history_j[std::to_string(task->created_timestamp)] = serializeTaskLocked(task);
        }
        return task_history_j;
    }

    void updateTaskProgressFromRemove(const std::shared_ptr<AsyncTask> &task,
        const vigil::status::SlotRemoveFilesProgress &progress)
    {
        const std::lock_guard lock(_task_mutex);
        if (!task || IsFinalAsyncTaskStatus(task->status))
            return;
        task->progress_percent = computePercent(progress.processed_count, progress.requested_count);
        const auto result = buildRemoveTaskResult(progress, false);
        task->result = mergedTaskResultLocked(task, &result);
        task->updated_timestamp = unixTimeMs();
    }

    void updateTaskProgressFromFreeUp(const std::shared_ptr<AsyncTask> &task,
        const vigil::status::SlotFreeUpProgress &progress)
    {
        const std::lock_guard lock(_task_mutex);
        if (!task || IsFinalAsyncTaskStatus(task->status))
            return;
        task->progress_percent = computePercent(progress.processed_entries, progress.total_entries);
        const auto result = buildFreeUpTaskResult(progress);
        task->result = mergedTaskResultLocked(task, &result);
        task->updated_timestamp = unixTimeMs();
    }

    void completeTask(const std::shared_ptr<AsyncTask> &task, const AsyncTaskStatus status, const vg_sta_e result_code,
        std::string result_message, std::unique_ptr<nlohmann::json> result = nullptr,
        std::optional<int> progress_percent = std::nullopt)
    {
        const std::lock_guard lock(_task_mutex);
        if (!task)
            return;
        task->status = status;
        if (result)
            task->result = mergedTaskResultLocked(task, result.get());
        else
            task->result = mergedTaskResultLocked(task);
        setTaskResultStatus(*task->result, result_code, result_message);
        if (progress_percent.has_value())
            task->progress_percent = *progress_percent;
        else if (status == AsyncTaskStatus::Succeeded)
            task->progress_percent = 100;
        task->updated_timestamp = unixTimeMs();
        if (auto *task_history = findTaskHistoryLocked(task->uid); task_history != nullptr)
            trimTaskHistoryLocked(*task_history);
    }

    void prepareWorkerShutdownLocked() noexcept
    {
        for (const auto &task: _queued_tasks)
        {
            if (task && task->status == AsyncTaskStatus::Queued)
            {
                task->status = AsyncTaskStatus::Canceled;
                task->result = mergedTaskResultLocked(task);
                setTaskResultStatus(*task->result, ECANCELED, "Canceled during shutdown");
                task->updated_timestamp = unixTimeMs();
            }
        }
        _queued_tasks.clear();

        if (_running_task
            && (_running_task->status == AsyncTaskStatus::Running
                || _running_task->status == AsyncTaskStatus::CancelRequested)
            && isRunningCancelableTaskLocked(_running_task))
        {
            _running_task->status = AsyncTaskStatus::CancelRequested;
            _running_task->updated_timestamp = unixTimeMs();
        }

        trimTaskHistoriesLocked();
    }

    bool shouldCancelTask(const std::shared_ptr<AsyncTask> &task) const
    {
        const std::lock_guard lock(_task_mutex);
        return task && task->status == AsyncTaskStatus::CancelRequested;
    }

    void workerThreadTask() noexcept
    {
        while (true)
        {
            std::shared_ptr<AsyncTask> task;

            {
                std::unique_lock lock(_task_mutex);
                _task_cv.wait(lock,
                    [&]() { return _stop_token.load(std::memory_order_acquire) || !_queued_tasks.empty(); });

                if (_stop_token.load(std::memory_order_acquire))
                    return;
                if (_queued_tasks.empty())
                    continue;

                task = _queued_tasks.front();
                _queued_tasks.pop_front();

                if (!task || task->status != AsyncTaskStatus::Queued)
                    continue;

                _running_task = task;
                task->status = AsyncTaskStatus::Running;
                task->updated_timestamp = unixTimeMs();
            }

            executeTask(task);

            {
                const std::lock_guard lock(_task_mutex);
                if (_running_task == task)
                    _running_task.reset();
                trimTaskHistoriesLocked();
            }
        }
    }

    void executeTask(const std::shared_ptr<AsyncTask> &task) noexcept
    {
        if (!task)
            return;

        const auto identity = tryParseTaskUid(task->uid);
        if (!identity.has_value())
        {
            completeTask(task, AsyncTaskStatus::Failed, EINVAL, "Invalid async task uid");
            return;
        }

        const auto slot_dev_path = std::string(identity->slot_dev_path);

        try
        {
            switch (identity->action)
            {
                case StorageControlConfig::ActionType::FormatSlot: {
                    const auto ret = _storage.formatSlot(slot_dev_path);
                    if (ret == 0)
                        completeTask(task, AsyncTaskStatus::Succeeded, 0, "Completed");
                    else
                        completeTask(task, AsyncTaskStatus::Failed, ret, "Failed to format storage slot");
                }
                break;

                case StorageControlConfig::ActionType::FreeUpSlot: {
                    vigil::status::SlotFreeUpProgress progress;
                    const auto cancel_check = [this, task]() { return shouldCancelTask(task); };
                    const auto on_progress = [this, task](const vigil::status::SlotFreeUpProgress &new_progress) {
                        updateTaskProgressFromFreeUp(task, new_progress);
                    };
                    const auto ret = _storage.freeUpSlot(slot_dev_path, &progress, cancel_check, on_progress);
                    const auto result = buildFreeUpTaskResult(progress);
                    const auto percent = computePercent(progress.processed_entries, progress.total_entries);
                    if (ret == 0)
                        completeTask(task, AsyncTaskStatus::Succeeded, 0, "Completed",
                            std::make_unique<nlohmann::json>(result), 100);
                    else if (ret == ECANCELED)
                        completeTask(task, AsyncTaskStatus::Canceled, ret, "Canceled",
                            std::make_unique<nlohmann::json>(result), percent);
                    else
                        completeTask(task, AsyncTaskStatus::Failed, ret, "Failed to free up storage slot",
                            std::make_unique<nlohmann::json>(result), percent);
                }
                break;

                case StorageControlConfig::ActionType::EjectSlot: {
                    const auto ret = _storage.ejectSlot(slot_dev_path);
                    if (ret == 0)
                        completeTask(task, AsyncTaskStatus::Succeeded, 0, "Completed");
                    else
                        completeTask(task, AsyncTaskStatus::Failed, ret, "Failed to eject storage slot");
                }
                break;

                case StorageControlConfig::ActionType::RemoveFilesOrDirectories: {
                    vigil::status::SlotRemoveFilesProgress progress;
                    const auto cancel_check = [this, task]() { return shouldCancelTask(task); };
                    const auto on_progress = [this, task](const vigil::status::SlotRemoveFilesProgress &new_progress) {
                        updateTaskProgressFromRemove(task, new_progress);
                    };
                    const auto ret = _storage.removeFilesOrDirectories(slot_dev_path, task->remove_paths, &progress,
                        cancel_check, on_progress);
                    const auto result = buildRemoveTaskResult(progress, true);
                    const auto percent = computePercent(progress.processed_count, progress.requested_count);
                    if (ret == 0)
                        completeTask(task, AsyncTaskStatus::Succeeded, 0, "Completed",
                            std::make_unique<nlohmann::json>(result), 100);
                    else if (ret == ECANCELED)
                        completeTask(task, AsyncTaskStatus::Canceled, ret, "Canceled",
                            std::make_unique<nlohmann::json>(result), percent);
                    else
                        completeTask(task, AsyncTaskStatus::Failed, ret,
                            "Failed to remove files or directories on storage slot",
                            std::make_unique<nlohmann::json>(result), percent);
                }
                break;

                default:
                    completeTask(task, AsyncTaskStatus::Failed, EINVAL, "Unsupported async storage action");
                    break;
            }
        }
        catch (const std::exception &e)
        {
            completeTask(task, AsyncTaskStatus::Failed, EFAULT, e.what());
        }
    }

    Storage &_storage;

    alignas(kHardwareDestructiveInterferenceSize) std::atomic<bool> _stop_token;
    std::unique_ptr<std::thread> _worker_thread;

    mutable std::mutex _task_mutex;
    std::condition_variable _task_cv;
    std::deque<std::shared_ptr<AsyncTask>> _queued_tasks;
    std::shared_ptr<AsyncTask> _running_task;
    std::unordered_map<std::string, std::deque<std::shared_ptr<AsyncTask>>> _task_histories;
    int64_t _last_task_created_timestamp;
};

AsyncTaskWorker::AsyncTaskWorker(Storage &storage) : _impl(std::make_unique<Impl>(storage)) { }

AsyncTaskWorker::~AsyncTaskWorker() noexcept = default;

void AsyncTaskWorker::handleSubmit(const StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret)
{
    _impl->handleSubmit(control_config, res_j, ret);
}

void AsyncTaskWorker::handleStatus(const StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret)
{
    _impl->handleStatus(control_config, res_j, ret);
}

void AsyncTaskWorker::handleCancel(const StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret)
{
    _impl->handleCancel(control_config, res_j, ret);
}

} // namespace vigil::types
