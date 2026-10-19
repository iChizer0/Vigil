// Characterization tests for RuleEndpoint: startup from configuration files, the rule-loop iteration
// (gates, direct EVENT bypass, dispatch), the three configuration endpoints, HTTP activation, and the
// rule-info projection (docs/PLAN.md sections 4.3, 5.1, 5.2, 6.2; matrix R1, R3, C2, W1). The rule
// thread is not started; each test drives one iteration at a time through RuleEndpointTestAccess.

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

#include "vigil/configs.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/endpoints.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"

namespace vigil::endpoints {

struct RuleEndpointTestAccess
{
    static void evaluateOnce(RuleEndpoint &endpoint)
    {
        endpoint.ruleTaskLoop();
    }
};

} // namespace vigil::endpoints

using namespace vigil;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::endpoints;
using namespace vigil::tests;

namespace {

std::string MakeTempDir()
{
    std::string path = (std::filesystem::temp_directory_path() / "vigil-test-XXXXXX").string();
    if (::mkdtemp(path.data()) == nullptr)
        throw std::runtime_error("mkdtemp failed");
    return path;
}

void WriteFile(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream out(path, std::ios::trunc);
    out << text;
    if (!out)
        throw std::runtime_error("failed to write " + path.string());
}

std::string ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

struct Fixture
{
    std::string dir; // outlives ctx: the C configuration borrows its buffer
    std::shared_ptr<Context> ctx;
    std::unique_ptr<RuleEndpoint> endpoint;

    Fixture() : dir(MakeTempDir()), ctx(std::make_shared<Context>(MakeFakeConfig(dir.c_str())))
    {
        RuleEventRecorder::clear();
    }

    ~Fixture()
    {
        endpoint.reset();
        ctx.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void start()
    {
        endpoint = std::make_unique<RuleEndpoint>(ctx, /* run_rule_thread = */ false);
    }

    void evaluate()
    {
        RuleEndpointTestAccess::evaluateOnce(*endpoint);
    }

    std::filesystem::path file(const char *name) const
    {
        return std::filesystem::path(dir) / name;
    }

    int postRecordConfig(const nlohmann::json &j)
    {
        return endpoint->updateRecordConfig(j).at("code").get<int>();
    }

    int postRecordRuleConfig(const nlohmann::json &j)
    {
        return endpoint->updateRecordRuleConfig(j).at("code").get<int>();
    }

    int postScheduleRuleConfig(const nlohmann::json &j)
    {
        return endpoint->updateScheduleRuleConfig(j).at("code").get<int>();
    }

    int activateHTTP()
    {
        return endpoint->doHTTPRuleActivate(nlohmann::json::object()).at("code").get<int>();
    }
};

nlohmann::json RecordJson(bool enabled, const char *format, long interval_ms)
{
    nlohmann::json j;
    j["bRuleEnabled"] = enabled;
    j["dWriterConfig"]["sFormat"] = format;
    j["dWriterConfig"]["iIntervalMs"] = interval_ms;
    return j;
}

nlohmann::json TimerRuleJson(int interval_seconds)
{
    nlohmann::json j;
    j["sCurrentSelected"] = "TIMER";
    j["dTimer"]["iIntervalSeconds"] = interval_seconds;
    return j;
}

nlohmann::json SelectedRuleJson(const char *type)
{
    nlohmann::json j;
    j["sCurrentSelected"] = type;
    return j;
}

// An enabled schedule with one window placed relative to the current local time.
nlohmann::json ScheduleWindowJson(std::chrono::seconds start_from_now, std::chrono::seconds end_from_now)
{
    const auto now = LocalTime::now().tm();
    const long base = now.tm_wday * 86400L + now.tm_hour * 3600L + now.tm_min * 60L + now.tm_sec;
    const auto format = [base](long offset) {
        long t = ((base + offset) % 604800L + 604800L) % 604800L;
        const int day = static_cast<int>(t / 86400L);
        t %= 86400L;
        const int hour = static_cast<int>(t / 3600L);
        t %= 3600L;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d", kTMDayNamesMap[static_cast<size_t>(day)], hour,
            static_cast<int>(t / 60L), static_cast<int>(t % 60L));
        return std::string(buf);
    };
    nlohmann::json window;
    window["sStart"] = format(start_from_now.count());
    window["sEnd"] = format(end_from_now.count());
    nlohmann::json j;
    j["bEnabled"] = true;
    j["lActiveWeekdays"] = nlohmann::json::array({ window });
    return j;
}

InferenceResult DirectEvent(int64_t ts, const std::string &source)
{
    InferenceResult r;
    r.timestamp_ms = ts;
    r.source_id = source;
    return r;
}

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

constexpr auto kHour = std::chrono::hours(1);

} // namespace

VG_TEST_CASE(startup_without_files_uses_defaults_and_writes_nothing)
{
    Fixture f;
    f.start();
    VG_CHECK(!f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, "MP4", 5000));
    VG_CHECK_EQ(f.endpoint->getScheduleRuleConfig(),
        nlohmann::json::parse(
            R"({"bEnabled":false,"lActiveWeekdays":[{"sStart":"Mon 00:00:00","sEnd":"Sun 24:00:00"}]})"));
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), SelectedRuleJson("ALWAYS_ON"));

    const auto info = f.endpoint->getRuleInfo();
    VG_CHECK(!info.contains("code"));
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), true);
    VG_CHECK(info.at("dLastRuleEvent").is_null());
    VG_CHECK(info.at("dLastRuleEventOwner").is_null());
    VG_CHECK_EQ(info.at("bRecordEnabled").get<bool>(), false);
    VG_CHECK_EQ(info.at("bMediaPaused").get<bool>(), false);
    VG_CHECK(info.at("dRecordSlot").is_null());
    VG_CHECK(info.at("dAvailableGPIOs").is_object() && info.at("dAvailableGPIOs").empty());
    VG_CHECK(info.at("dAvailableTTYs").is_object() && info.at("dAvailableTTYs").empty());
    VG_CHECK_EQ(info.at("bVideoClipLengthSeconds").get<long>(), kVideoClipLengthDefault.count());

    for (const char *name: { "record_config.json", "schedule_rule_config.json", "record_rule_config.json" })
        VG_CHECK(!std::filesystem::exists(f.file(name)));
}

VG_TEST_CASE(startup_reloads_persisted_configuration_without_rewriting_files)
{
    Fixture f;
    const std::string record = R"({"bRuleEnabled":true,"dWriterConfig":{"sFormat":"JPG","iIntervalMs":777}})";
    const std::string rule = R"({"sCurrentSelected":"TIMER","dTimer":{"iIntervalSeconds":0}})";
    const std::string schedule
        = R"({"bEnabled":true,"lActiveWeekdays":[{"sStart":"Sun 00:00:00","sEnd":"Sat 24:00:00"}]})";
    WriteFile(f.file("record_config.json"), record);
    WriteFile(f.file("record_rule_config.json"), rule);
    WriteFile(f.file("schedule_rule_config.json"), schedule);

    f.start();
    VG_CHECK(f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), nlohmann::json::parse(record));
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), nlohmann::json::parse(rule));
    VG_CHECK_EQ(f.endpoint->getScheduleRuleConfig(), nlohmann::json::parse(schedule));
    VG_CHECK_EQ(f.ctx->shared_writer_settings.get().second->interval.count(), 777L);
    // Files are read at startup, never rewritten.
    VG_CHECK_EQ(ReadFile(f.file("record_config.json")), record);
    VG_CHECK_EQ(ReadFile(f.file("record_rule_config.json")), rule);
    VG_CHECK_EQ(ReadFile(f.file("schedule_rule_config.json")), schedule);

    f.evaluate();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(events[0].writer_created);
}

VG_TEST_CASE(startup_keeps_defaults_and_leaves_invalid_files_untouched)
{
    {
        Fixture f;
        const std::string corrupt = "{ this is not json";
        WriteFile(f.file("record_config.json"), corrupt);
        f.start();
        VG_CHECK(!f.ctx->record_rule_enabled.load());
        VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, "MP4", 5000));
        VG_CHECK_EQ(ReadFile(f.file("record_config.json")), corrupt);
    }
    {
        Fixture f;
        const std::string invalid = R"({"bRuleEnabled":true,"dWriterConfig":{"sFormat":"MP4","iIntervalMs":0}})";
        WriteFile(f.file("record_config.json"), invalid); // well-formed JSON, rejected content
        f.start();
        VG_CHECK(!f.ctx->record_rule_enabled.load());
        VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, "MP4", 5000));
        VG_CHECK_EQ(ReadFile(f.file("record_config.json")), invalid);
    }
    {
        // Today an invalid selected-rule file leaves the default ALWAYS_ON rule live when the record
        // configuration enables rules. Phase 1C changes this to fail closed (docs/PLAN.md section 4.3) and
        // flips the two checks after evaluate().
        Fixture f;
        WriteFile(f.file("record_config.json"), RecordJson(true, "MP4", 5000).dump());
        const std::string invalid_rule = R"({"sCurrentSelected":"TIMER","dTimer":{"iIntervalSeconds":-5}})";
        WriteFile(f.file("record_rule_config.json"), invalid_rule);
        f.start();
        VG_CHECK_EQ(ReadFile(f.file("record_rule_config.json")), invalid_rule);
        VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), SelectedRuleJson("ALWAYS_ON"));
        f.evaluate();
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_ALWAYS_ON);
    }
    {
        // Likewise an invalid schedule file keeps the default (disabled, therefore open) gate today.
        Fixture f;
        WriteFile(f.file("record_config.json"), RecordJson(true, "MP4", 5000).dump());
        const std::string invalid_schedule = R"({"bEnabled":true})";
        WriteFile(f.file("schedule_rule_config.json"), invalid_schedule);
        f.start();
        VG_CHECK_EQ(ReadFile(f.file("schedule_rule_config.json")), invalid_schedule);
        VG_CHECK_EQ(f.endpoint->getScheduleRuleConfig().at("bEnabled").get<bool>(), false);
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 1 });
    }
}

VG_TEST_CASE(evaluation_is_idle_while_rules_are_disabled)
{
    Fixture f;
    f.start();
    f.ctx->enqueueInferenceResult(DirectEvent(1, "cam0")); // dropped: rules are disabled
    for (int i = 0; i < 3; ++i)
        f.evaluate();
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    VG_CHECK(f.endpoint->getRuleInfo().at("dLastRuleEvent").is_null());
}

VG_TEST_CASE(direct_event_bypasses_the_selected_rule_on_its_tick)
{
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0); // fires on every evaluated tick
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    VG_CHECK(f.ctx->record_rule_enabled.load());

    f.evaluate(); // tick 1: the timer fires and owns the writer event
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(events[0].writer_created);

    const auto before = NowMs();
    f.ctx->enqueueInferenceResult(DirectEvent(4242, "cam0"));
    f.evaluate(); // tick 2: the queued EVENT owns the tick; the timer is not evaluated
    const auto after = NowMs();
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_UNKNOWN);
    VG_CHECK_EQ(events[0].id, "");
    VG_CHECK_EQ(events[0].source_id, "cam0");
    VG_CHECK_EQ(events[0].source_delivery, "EVENT");
    VG_CHECK(!events[0].writer_created); // the timer's writer event is still pending
    // Today the direct EVENT owner is stamped with the dispatch time, not with the input time (4242);
    // the input time only reaches the context clock. See docs/PLAN.md section 6.1.
    VG_CHECK(events[0].timestamp >= before && events[0].timestamp <= after);
    VG_CHECK_EQ(f.ctx->inference_queue.size(), size_t { 0 });

    f.evaluate(); // tick 3: no input, the timer is evaluated again
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(!events[0].writer_created);
}

VG_TEST_CASE(schedule_closure_blocks_dispatch_and_reopening_restores_it)
{
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);

    // A one-hour window that starts three hours from now: the gate is closed.
    VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(3 * kHour, 4 * kHour)), 0);
    f.ctx->enqueueInferenceResult(DirectEvent(1, "cam0"));
    f.evaluate();
    f.evaluate();
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    VG_CHECK_EQ(f.ctx->inference_queue.size(), size_t { 0 });  // the closed tick consumed the EVENT; nothing is held
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EPERM); // HTTP activation is refused while closed

    // A window around now reopens the gate.
    VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(-kHour, kHour)), 0);
    f.evaluate();
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(events[0].writer_created);

    // Disabling the schedule opens the gate as well.
    f.ctx->shared_rule_event.reset(WriteEvent::Status::Completed);
    nlohmann::json disabled;
    disabled["bEnabled"] = false;
    disabled["lActiveWeekdays"] = nlohmann::json::array();
    VG_CHECK_EQ(f.postScheduleRuleConfig(disabled), 0);
    f.evaluate();
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].writer_created);
}

VG_TEST_CASE(rule_disable_cancels_the_current_event_and_rule_info_reports_it)
{
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    f.evaluate();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });

    auto info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), false);
    VG_CHECK_EQ(info.at("bRecordEnabled").get<bool>(), true);
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("sStatus").get<std::string>(), "PENDING");
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("iOwnerUID").get<uint64_t>(), events[0].uid);
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("sRuleType").get<std::string>(), "TIMER");
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("sRuleID").get<std::string>(), "");
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("sSourceID").get<std::string>(), "");
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("sSourceDelivery").get<std::string>(), "EVENT");
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("iUID").get<uint64_t>(), events[0].uid);
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("iTimestamp").get<int64_t>(), events[0].timestamp);

    VG_CHECK_EQ(f.postRecordConfig(RecordJson(false, "MP4", 5000)), 0);
    f.evaluate(); // the disabled tick cancels the pending event and dispatches nothing
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), true);
    VG_CHECK_EQ(info.at("bRecordEnabled").get<bool>(), false);
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("sStatus").get<std::string>(), "CANCELED");
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("iOwnerUID").get<uint64_t>(), events[0].uid);
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("sRuleType").get<std::string>(), "TIMER");
}

VG_TEST_CASE(record_rule_config_switches_rules_persists_and_rolls_back_failures)
{
    Fixture f;
    f.start();
    const auto timer = TimerRuleJson(0);
    VG_CHECK_EQ(f.postRecordRuleConfig(timer), 0);
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), timer);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_rule_config.json"))), timer);

    // Unknown type: rejected, previous rule and file kept.
    auto res = f.endpoint->updateRecordRuleConfig(SelectedRuleJson("NOPE"));
    VG_CHECK_EQ(res.at("code").get<int>(), kEndpointErrorBase + EINVAL);
    VG_CHECK(res.contains("message"));
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), timer);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_rule_config.json"))), timer);

    // Rules whose construction fails in this configuration (no GPIO callbacks, no TTYs, missing block).
    nlohmann::json gpio = SelectedRuleJson("GPIO");
    gpio["dGPIO"]
        = { { "sName", "x" }, { "sState", "INPUT_PULLUP" }, { "sSignal", "RISING" }, { "iDebounceDurationMs", 0 } };
    VG_CHECK_EQ(f.postRecordRuleConfig(gpio), kEndpointErrorBase + EINVAL);
    nlohmann::json tty = SelectedRuleJson("TTY");
    tty["dTTY"] = { { "sName", "uart" }, { "sCommand", "go" } };
    VG_CHECK_EQ(f.postRecordRuleConfig(tty), kEndpointErrorBase + EINVAL);
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("TIMER")), kEndpointErrorBase + EINVAL);
    nlohmann::json bad_timer = TimerRuleJson(-1);
    VG_CHECK_EQ(f.postRecordRuleConfig(bad_timer), kEndpointErrorBase + EINVAL);
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), timer);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_rule_config.json"))), timer);

    // An empty selection clears the rule: nothing is evaluated.
    const auto none = SelectedRuleJson("");
    VG_CHECK_EQ(f.postRecordRuleConfig(none), 0);
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), none);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_rule_config.json"))), none);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    f.evaluate();
    f.evaluate();
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });

    // Back to the default rule.
    const auto always = SelectedRuleJson("ALWAYS_ON");
    VG_CHECK_EQ(f.postRecordRuleConfig(always), 0);
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), always);
    f.evaluate();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_ALWAYS_ON);
}

VG_TEST_CASE(record_config_post_persists_only_valid_payloads)
{
    Fixture f;
    f.start();
    const auto valid = RecordJson(true, "MP4", 1234);
    VG_CHECK_EQ(f.postRecordConfig(valid), 0);
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), valid);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_config.json"))), valid);
    VG_CHECK(f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(f.ctx->shared_writer_settings.get().second->interval.count(), 1234L);

    // A rejected payload changes neither the runtime nor the file.
    const auto res = f.endpoint->updateRecordConfig(RecordJson(true, "MP4", 0));
    VG_CHECK_EQ(res.at("code").get<int>(), kEndpointErrorBase + EINVAL);
    VG_CHECK(res.contains("message"));
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), valid);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_config.json"))), valid);
    VG_CHECK_EQ(f.ctx->shared_writer_settings.get().second->interval.count(), 1234L);

    // The file holds the posted document verbatim (unknown fields included); GET returns the known fields.
    auto extra = RecordJson(false, "JPG", 2000);
    extra["sNote"] = "kept";
    VG_CHECK_EQ(f.postRecordConfig(extra), 0);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_config.json"))), extra);
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, "JPG", 2000));
    VG_CHECK(!f.ctx->record_rule_enabled.load());
}

VG_TEST_CASE(http_activation_reports_busy_and_gate_errors)
{
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EPERM); // rules disabled
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EPERM); // the selected rule is not HTTP
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("HTTP")), 0);

    VG_CHECK_EQ(f.activateHTTP(), 0);
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_HTTP);
    VG_CHECK(events[0].writer_created);
    VG_CHECK_EQ(events[0].source_delivery, "EVENT");

    // While the writer event is pending the activation is reported but answered EBUSY.
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EBUSY);
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(!events[0].writer_created);
    f.ctx->shared_rule_event.reset(WriteEvent::Status::Completed);

    // A storage slot that is not ready refuses the request before any RULE report.
    {
        const std::unique_lock lock(f.ctx->slot_check_mutex);
        f.ctx->slot_check = { nullptr, [](const void *) { return false; } };
    }
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EBUSY);
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    {
        const std::unique_lock lock(f.ctx->slot_check_mutex);
        f.ctx->slot_check = { nullptr, nullptr };
    }
    VG_CHECK_EQ(f.activateHTTP(), 0);
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 1 });

    // The HTTP rule never fires from the rule loop itself.
    f.ctx->shared_rule_event.reset(WriteEvent::Status::Completed);
    RuleEventRecorder::clear();
    f.evaluate();
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
}

// Last on purpose: UIDs are process-global and only ever increase.
VG_TEST_CASE(rule_info_carries_exact_uids_above_2_to_53)
{
    const auto big = (uint64_t { 1 } << 53) + 1;
    {
        const EventOwner bump(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event, static_cast<int64_t>(big));
        VG_CHECK_EQ(bump.uid, big);
    }
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    f.evaluate();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].uid > big);

    const auto info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("iUID").get<uint64_t>(), events[0].uid);
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("iOwnerUID").get<uint64_t>(), events[0].uid);
    VG_CHECK(info.dump().find(std::to_string(events[0].uid)) != std::string::npos);
}

VG_TEST_MAIN()
