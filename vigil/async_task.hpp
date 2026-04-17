#pragma once
#ifndef VIGIL_ASYNC_TASK_HPP
#define VIGIL_ASYNC_TASK_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "vigil_ctypes.h"

namespace vigil::configs {
struct StorageControlConfig;
}

namespace vigil::storage {
class Storage;
}

namespace vigil::types {

enum class AsyncTaskStatus {
    Queued,
    Running,
    Succeeded,
    Failed,
    CancelRequested,
    Canceled,
};

constexpr bool IsFinalAsyncTaskStatus(const AsyncTaskStatus status) noexcept
{
    return status == AsyncTaskStatus::Succeeded || status == AsyncTaskStatus::Failed
           || status == AsyncTaskStatus::Canceled;
}

constexpr std::string_view AsyncTaskStatusString(const AsyncTaskStatus status) noexcept
{
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
}

struct AsyncTask final
{
    std::string uid;
    int64_t created_timestamp = 0;
    AsyncTaskStatus status = AsyncTaskStatus::Queued;
    int progress_percent = -1;
    int64_t updated_timestamp = 0;
    std::vector<std::filesystem::path> remove_paths;
    std::unique_ptr<nlohmann::json> result;
};

class AsyncTaskWorker
{
public:
    explicit AsyncTaskWorker(vigil::storage::Storage &storage);
    ~AsyncTaskWorker() noexcept;

    AsyncTaskWorker(const AsyncTaskWorker &) = delete;
    AsyncTaskWorker &operator=(const AsyncTaskWorker &) = delete;
    AsyncTaskWorker(AsyncTaskWorker &&) = delete;
    AsyncTaskWorker &operator=(AsyncTaskWorker &&) = delete;

    void handleSubmit(const vigil::configs::StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret);
    void handleStatus(const vigil::configs::StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret);
    void handleCancel(const vigil::configs::StorageControlConfig &control_config, nlohmann::json &res_j, vg_sta_e &ret);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace vigil::types

#endif
