#pragma once
#ifndef VIGIL_TESTS_FAKE_CONFIG_HPP
#define VIGIL_TESTS_FAKE_CONFIG_HPP

// Deterministic C configuration for tests: no peripherals, no storage device, no media sinks.
// Rule events are recorded in process-wide storage because vg_config_basic_t callbacks carry no
// user_data pointer. Logging goes to stderr so that `ctest --output-on-failure` shows it.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "vigil_ctypes.h"

namespace vigil::tests {

struct RecordedRuleEvent
{
    vg_rule_type_e type;
    std::string id;
    int64_t timestamp;
    int64_t dispatch_timestamp;
    uint64_t uid;
    std::string source_id;
    std::string source_delivery;
    bool writer_created;
    bool id_is_null;
    bool source_id_is_null;
    bool source_delivery_is_null;
};

class RuleEventRecorder final
{
public:
    static void callback(const vg_rule_evt_t *evt)
    {
        if (!evt)
            return;
        const std::lock_guard lock(mutex());
        events().push_back(RecordedRuleEvent {
            .type = evt->type,
            .id = evt->id ? evt->id : "",
            .timestamp = evt->timestamp,
            .dispatch_timestamp = evt->dispatch_timestamp,
            .uid = evt->uid,
            .source_id = evt->source_id ? evt->source_id : "",
            .source_delivery = evt->source_delivery ? evt->source_delivery : "",
            .writer_created = evt->writer_created,
            .id_is_null = evt->id == nullptr,
            .source_id_is_null = evt->source_id == nullptr,
            .source_delivery_is_null = evt->source_delivery == nullptr,
        });
    }

    static std::vector<RecordedRuleEvent> take()
    {
        const std::lock_guard lock(mutex());
        std::vector<RecordedRuleEvent> out;
        out.swap(events());
        return out;
    }

    static void clear()
    {
        const std::lock_guard lock(mutex());
        events().clear();
    }

    static size_t size()
    {
        const std::lock_guard lock(mutex());
        return events().size();
    }

private:
    static std::mutex &mutex()
    {
        static std::mutex m;
        return m;
    }

    static std::vector<RecordedRuleEvent> &events()
    {
        static std::vector<RecordedRuleEvent> v;
        return v;
    }
};

inline vg_sta_e TestLogCallback(vg_log_level_e level, const char *msg, size_t bytes_len)
{
    const char *prefix = "[?] ";
    switch (level)
    {
        case VG_LOG_LEVEL_DEBUG:
            prefix = "[D] ";
            break;
        case VG_LOG_LEVEL_INFO:
            prefix = "[I] ";
            break;
        case VG_LOG_LEVEL_WARN:
            prefix = "[W] ";
            break;
        case VG_LOG_LEVEL_ERROR:
            prefix = "[E] ";
            break;
        case VG_LOG_LEVEL_FATAL:
            prefix = "[F] ";
            break;
        default:
            break;
    }
    std::fprintf(stderr, "%s%.*s\n", prefix, static_cast<int>(bytes_len), msg ? msg : "");
    return 0;
}

// A configuration whose only live callbacks are logging and rule event recording. Every other
// callback is null, so nothing in a test can reach a device, a socket, or a filesystem by accident.
inline vg_config_t MakeFakeConfig(const char *config_directory = nullptr)
{
    vg_config_t config {};
    config.basic.log_level = VG_LOG_LEVEL_DEBUG;
    config.basic.log_cb = &TestLogCallback;
    config.basic.on_rule_event_cb = &RuleEventRecorder::callback;
    config.basic.config_directory = config_directory;
    return config;
}

} // namespace vigil::tests

#endif
