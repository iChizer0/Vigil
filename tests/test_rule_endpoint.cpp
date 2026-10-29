// RuleEndpoint: startup from configuration files, the rule loop, the configuration endpoints, HTTP activation, and
// rule info. The rule thread is not started; each test drives one iteration at a time.

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
#include <thread>

#include "vigil/configs.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/endpoints.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"
#include "fake_media.hpp"
#include "fake_slot.hpp"

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
using namespace vigil::writers;
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

// One fake pin for a GPIO rule, acquired at most once at a time, as a real pin is.
struct FakeGPIO
{
    static inline vg_config_gpio_pin_t pins[1] = { { .num = 7,
        .name = "GPIO_07",
        .state = VG_GPIO_PIN_MODE_INP_FLOATING,
        .capabilities = static_cast<vg_gpio_pin_mod_e>(VG_GPIO_PIN_MODE_INP_FLOATING | VG_GPIO_PIN_MODE_INP_PULLUP) } };
    static inline int acquired = 0;
    static inline int released = 0;
    static inline bool held = false;
    static inline vg_gpio_pin_evt_e event = VG_GPIO_PIN_EVT_NONE;

    static void reset()
    {
        acquired = released = 0;
        held = false;
        event = VG_GPIO_PIN_EVT_NONE;
    }

    static vg_sta_e Available(vg_config_gpio_pins_t *out)
    {
        out->free_cb = nullptr;
        out->pins = pins;
        out->len = 1;
        return 0;
    }

    static vg_sta_e Acquire(int, vg_gpio_pin_mod_e, size_t)
    {
        if (held)
            return EBUSY;
        held = true;
        ++acquired;
        return 0;
    }

    static void Release(int)
    {
        held = false;
        ++released;
    }

    static vg_gpio_pin_lvl_e SyncLevel(int)
    {
        return VG_GPIO_PIN_LVL_LOW;
    }

    static vg_gpio_pin_evt_e SyncEvent(int)
    {
        const auto e = event;
        event = VG_GPIO_PIN_EVT_NONE;
        return e;
    }
};

struct Fixture
{
    std::string root; // temporary directory, removed afterwards
    std::string dir;  // configuration directory; outlives ctx, which borrows its buffer
    FakeMedia media;  // the video sink the continuous-recording cases run a writer against
    std::shared_ptr<Context> ctx;
    std::unique_ptr<FakeSlotAdmission> slot; // registered and ready, as an enabled Slot is
    std::unique_ptr<RuleEndpoint> endpoint;

    // With `unwritable_store` the configuration "directory" is a regular file, so nothing can be stored. With
    // `with_sound` the host provides a playback callback, so Sound is available; its worker is not started,
    // so a trigger stays Pending for rule info to report. With `with_gpio` the host provides the fake pin.
    explicit Fixture(bool unwritable_store = false, bool with_sound = false, bool with_gpio = false)
        : root(MakeTempDir()), dir(root)
    {
        if (unwritable_store)
        {
            dir = root + "/blocker";
            WriteFile(dir, "x");
        }
        auto cfg = MakeFakeConfig(dir.c_str());
        if (with_sound)
            cfg.media.sound_play_cb = [](void *) -> vg_sta_e { return 0; };
        if (with_gpio)
        {
            cfg.gpios.available_cb = &FakeGPIO::Available;
            cfg.gpios.acquire_cb = &FakeGPIO::Acquire;
            cfg.gpios.release_cb = &FakeGPIO::Release;
            cfg.gpios.sync_level_cb = &FakeGPIO::SyncLevel;
            cfg.gpios.sync_event_cb = &FakeGPIO::SyncEvent;
            FakeGPIO::reset();
        }
        cfg.media.user_data = &media;
        cfg.media.video_file_create_cb = &FakeMedia::Create;
        cfg.media.video_file_finish_cb = &FakeMedia::Finish;
        cfg.media.video_file_write_video_frame_cb = &FakeMedia::WriteVideo;
        cfg.media.video_file_write_audio_frame_cb = &FakeMedia::WriteAudio;
        ctx = std::make_shared<Context>(cfg, /* run_sound_worker = */ !with_sound);
        media.ctx = ctx.get();
        slot = std::make_unique<FakeSlotAdmission>(*ctx);
        RuleEventRecorder::clear();
    }

    ~Fixture()
    {
        endpoint.reset();
        slot.reset();
        ctx.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
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

    void pushKeyFrame()
    {
        uint8_t data[4] = { 1, 2, 3, 4 };
        const vg_video_frame_t frame { .pts = ++pts, .data = data, .bytes_len = sizeof data, .key_frame = true };
        const std::lock_guard lock(media.mutex); // a writer on another thread reads the queue
        media.queue.emplace_back(frame);
    }

    // Claims the task the Video channel holds, as its worker would.
    std::shared_ptr<FileTask> claimVideo()
    {
        const auto task = ctx->file_channels.video.claim(slot->registration());
        VG_REQUIRE(task != nullptr);
        return task;
    }

private:
    int64_t pts = 0;
};

// A video writer over the fixture's fake media for a claimed task, run on its own thread: what the Video worker
// does between the claim and the publication. Destruction joins it.
struct RunningWriter
{
    VideoWriter<FakeAdapter> writer;
    std::thread thread;

    RunningWriter(Fixture &f, const std::shared_ptr<FileTask> &task)
        : writer(std::ref(f.media), std::ref(*f.ctx), task->settings, task->event, task->settings_revision, task->kind),
          thread([this] { writer(); })
    {
    }

    ~RunningWriter()
    {
        join();
    }

    void join()
    {
        if (thread.joinable())
            thread.join();
    }
};

// Spins until `pred` holds: a bounded wait on the writer thread, not a timing assumption.
template<typename Pred>
bool WaitFor(Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// The frames the fake sink has written, read under its lock: the writer runs on another thread.
int FramesWritten(FakeMedia &media)
{
    const std::lock_guard lock(media.records);
    return media.video_frames;
}

// The complete version-2 document (the shape GET returns) with at most one enabled file action: "MP4" enables
// video, "JPG"/"RAW" enable photo, nullptr enables nothing. Every other field holds its default.
nlohmann::json RecordJson(bool enabled, const char *format, long interval_ms)
{
    const bool video = format && std::string(format) == "MP4";
    const bool photo = format && !video;
    nlohmann::json j;
    j["iSchemaVersion"] = 2;
    j["bRuleEnabled"] = enabled;
    j["bContinuousRecordEnabled"] = false;
    j["dActions"]["dPhoto"] = { { "bEnabled", photo }, { "sFormat", photo ? format : "JPG" },
        { "iIntervalMs", photo ? interval_ms : 5000L } };
    j["dActions"]["dVideo"] = { { "bEnabled", video }, { "iIntervalMs", video ? interval_ms : 5000L } };
    j["dActions"]["dSound"] = { { "bEnabled", false } };
    return j;
}

// `j` with the three actions switched as given.
nlohmann::json WithActions(nlohmann::json j, bool photo, bool video, bool sound)
{
    j["dActions"]["dPhoto"]["bEnabled"] = photo;
    j["dActions"]["dVideo"]["bEnabled"] = video;
    j["dActions"]["dSound"]["bEnabled"] = sound;
    return j;
}

// `j` with continuous recording configured.
nlohmann::json WithContinuous(nlohmann::json j)
{
    j["bContinuousRecordEnabled"] = true;
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

// What the worker does at the end of its task: completes the task its channel holds.
void CompleteCurrent(Context &ctx, WriteEvent::Status status)
{
    if (const auto [task, held] = ctx.file_channels.latest(); held)
        ctx.file_channels.of(task->type).complete(task, status);
}

// The interval of the enabled file channel in the published snapshot, or -1 without one.
long ChannelIntervalMs(const Context &ctx)
{
    const auto settings = ctx.action_settings.get().settings;
    const auto channel = settings ? settings->fileChannel() : nullptr;
    return channel ? static_cast<long>(channel->interval.count()) : -1L;
}

std::string ChannelFormat(const Context &ctx)
{
    const auto settings = ctx.action_settings.get().settings;
    const auto channel = settings ? settings->fileChannel() : nullptr;
    return channel ? channel->format : std::string();
}

} // namespace

VG_TEST_CASE(startup_without_files_uses_defaults_and_writes_nothing)
{
    Fixture f;
    f.start();
    VG_CHECK(!f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, nullptr, 0));
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
    const std::string record = RecordJson(true, "JPG", 777).dump(); // compact, unlike the stored form
    const std::string rule = R"({"sCurrentSelected":"TIMER","dTimer":{"iIntervalSeconds":0}})";
    const std::string schedule
        = R"({"bEnabled":true,"lActiveWeekdays":[{"sStart":"Sun 00:00:00","sEnd":"Sat 24:00:00"}]})";
    WriteFile(f.file("record_config.json"), record);
    WriteFile(f.file("record_rule_config.json"), rule);
    WriteFile(f.file("schedule_rule_config.json"), schedule);

    f.start();
    VG_CHECK(f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(true, "JPG", 777));
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), nlohmann::json::parse(rule));
    VG_CHECK_EQ(f.endpoint->getScheduleRuleConfig(), nlohmann::json::parse(schedule));
    VG_CHECK_EQ(ChannelIntervalMs(*f.ctx), 777L);
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

VG_TEST_CASE(startup_leaves_unusable_files_untouched_and_keeps_rules_disabled)
{
    // A record configuration file that cannot be applied keeps the first-start defaults.
    for (const std::string &text: { std::string("{ this is not json"), RecordJson(true, "MP4", 0).dump(),
             std::string(R"({"bRuleEnabled":true,"dWriterConfig":{"sFormat":"MP4","iIntervalMs":777}})") })
    {
        Fixture f;
        WriteFile(f.file("record_config.json"), text);
        f.start();
        VG_CHECK(!f.ctx->record_rule_enabled.load());
        VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, nullptr, 0));
        VG_CHECK_EQ(ChannelIntervalMs(*f.ctx), -1L); // no file channel
        VG_CHECK_EQ(ReadFile(f.file("record_config.json")), text);
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    }

    // A valid record configuration next to an unusable rule or schedule file: the file is left untouched and rules
    // stay disabled until valid POSTs, ending with the record configuration, recover.
    struct Case
    {
        const char *name;
        std::string text;
    };
    for (const auto &c:
        { Case { "record_rule_config.json", R"({"sCurrentSelected":"TIMER","dTimer":{"iIntervalSeconds":-5}})" },
            Case { "record_rule_config.json", "[ nope" }, Case { "schedule_rule_config.json", R"({"bEnabled":true})" },
            Case { "schedule_rule_config.json", "{ nope" } })
    {
        Fixture f;
        WriteFile(f.file("record_config.json"), RecordJson(true, "MP4", 5000).dump());
        WriteFile(f.file(c.name), c.text);
        f.start();
        VG_CHECK(!f.ctx->record_rule_enabled.load());
        VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, "MP4", 5000)); // reported as disabled
        VG_CHECK_EQ(ReadFile(f.file(c.name)), c.text);
        VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), SelectedRuleJson("ALWAYS_ON"));
        VG_CHECK_EQ(f.endpoint->getScheduleRuleConfig().at("bEnabled").get<bool>(), false);
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
        VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EPERM);

        // The manager repairs the unusable file first: the schedule by posting what GET shows, which is stored
        // although it equals the schedule in memory, the rule by posting one; then the record configuration.
        if (std::string(c.name) == "schedule_rule_config.json")
        {
            const auto schedule = f.endpoint->getScheduleRuleConfig();
            VG_CHECK_EQ(f.postScheduleRuleConfig(schedule), 0);
            VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file(c.name))), schedule);
        }
        VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
        VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_rule_config.json"))), TimerRuleJson(0));
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 }); // still disabled
        VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 1 });

        // A restart over the repaired files does not fail closed: rules are enabled, and the first evaluation
        // dispatches an accepted event.
        f.endpoint.reset();
        f.ctx->file_channels.cancelPending(); // the event's task, as the runtime's teardown retires it
        RuleEventRecorder::clear();
        f.start();
        VG_CHECK(f.ctx->record_rule_enabled.load());
        VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(true, "MP4", 5000));
        VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), TimerRuleJson(0));
        f.evaluate();
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK(events[0].writer_created);
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
    VG_CHECK(!events[0].writer_created);                // the timer's writer event is still pending
    VG_CHECK_EQ(events[0].timestamp, int64_t { 4242 }); // the input's time is the event time
    VG_CHECK(before <= after);
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
    CompleteCurrent(*f.ctx, WriteEvent::Status::Completed);
    nlohmann::json disabled;
    disabled["bEnabled"] = false;
    disabled["lActiveWeekdays"] = nlohmann::json::array();
    VG_CHECK_EQ(f.postScheduleRuleConfig(disabled), 0);
    f.evaluate();
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].writer_created);
}

VG_TEST_CASE(the_schedule_gate_is_published_with_the_configuration_and_on_every_tick_before_the_enable_check)
{
    // The gate lives in the context, so admission reads the same state as the rule loop and the HTTP precheck;
    // publishing it on every tick keeps it current while rules are disabled. Each change it publishes, a closure or an
    // opening, breaks continuous recording's continuity; a tick that finds it unchanged does not.
    Fixture f;
    f.start();
    VG_CHECK(!f.ctx->record_rule_enabled.load());
    VG_CHECK(f.ctx->schedule_open.load()); // no schedule configured: open
    const auto breaks = f.ctx->continuity_breaks.load();
    VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(3 * kHour, 4 * kHour)), 0);
    VG_CHECK(!f.ctx->schedule_open.load()); // published with the configuration
    VG_CHECK_EQ(f.ctx->continuity_breaks.load(), breaks + 1);
    f.evaluate();
    VG_CHECK_EQ(f.ctx->continuity_breaks.load(), breaks + 1);
    f.ctx->schedule_open.store(true);
    f.evaluate(); // and on every tick, rules disabled or not
    VG_CHECK(!f.ctx->schedule_open.load());
    VG_CHECK_EQ(f.ctx->continuity_breaks.load(), breaks + 2);
    VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(-kHour, kHour)), 0);
    VG_CHECK(f.ctx->schedule_open.load());
    VG_CHECK_EQ(f.ctx->continuity_breaks.load(), breaks + 3);
    f.ctx->schedule_open.store(false);
    f.evaluate();
    VG_CHECK(f.ctx->schedule_open.load());
    VG_CHECK_EQ(f.ctx->continuity_breaks.load(), breaks + 4);
    f.evaluate();
    VG_CHECK_EQ(f.ctx->continuity_breaks.load(), breaks + 4);
}

VG_TEST_CASE(a_dispatch_without_a_usable_slot_reports_the_owner_and_the_next_owner_proceeds)
{
    // Nothing is pending, so the next tick's owner is dispatched in turn instead of waiting behind a pending event.
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0); // fires on every evaluated tick
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    f.slot->ready = false;
    for (int i = 0; i < 3; ++i)
    {
        f.evaluate();
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
        VG_CHECK(!events[0].writer_created);
    }
    auto info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), true);
    VG_CHECK(info.at("dLastRuleEvent").is_null()); // nothing was ever accepted
    VG_CHECK(info.at("dRecordSlot").is_null());    // the registered admission is not a storage Slot here

    f.slot->ready = true;
    f.evaluate();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].writer_created);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), false);
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("sStatus").get<std::string>(), "PENDING");
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

VG_TEST_CASE(rule_info_reports_the_event_time_and_the_dispatch_time)
{
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("HTTP")), 0); // never fires from the loop by itself
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);

    const auto before = NowMs();
    f.ctx->enqueueInferenceResult(DirectEvent(4242, "cam0"));
    f.evaluate();
    const auto after = NowMs();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_UNKNOWN);
    VG_CHECK(events[0].writer_created);
    VG_CHECK_EQ(events[0].timestamp, int64_t { 4242 });

    auto owner = f.endpoint->getRuleInfo().at("dLastRuleEventOwner");
    VG_CHECK_EQ(owner.at("sRuleType").get<std::string>(), "UNKNOWN");
    VG_CHECK_EQ(owner.at("sSourceID").get<std::string>(), "cam0");
    VG_CHECK_EQ(owner.at("iTimestamp").get<int64_t>(), int64_t { 4242 });
    const auto dispatched = owner.at("iDispatchTimestamp").get<int64_t>();
    VG_CHECK(dispatched >= before && dispatched <= after);
    VG_CHECK_EQ(owner.at("iUID").get<uint64_t>(), events[0].uid);

    // An owner that observes its own condition reports the same value twice.
    CompleteCurrent(*f.ctx, WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.activateHTTP(), 0);
    owner = f.endpoint->getRuleInfo().at("dLastRuleEventOwner");
    VG_CHECK_EQ(owner.at("sRuleType").get<std::string>(), "HTTP");
    VG_CHECK_EQ(owner.at("iTimestamp").get<int64_t>(), owner.at("iDispatchTimestamp").get<int64_t>());
}

VG_TEST_CASE(rule_info_reports_the_channel_busy_through_the_hold)
{
    // bReadyForNewEvent stays false and nothing is accepted until the worker completes the task after its hold, even
    // once the write has finished and the task's status is terminal.
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    f.evaluate();
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].writer_created);

    const auto task = f.ctx->file_channels.video.claim(f.slot->registration()); // the worker starts the task
    VG_REQUIRE(task != nullptr);
    auto info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), false);
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("sStatus").get<std::string>(), "WRITING");
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("iOwnerUID").get<uint64_t>(), events[0].uid);

    task->event->status.store(WriteEvent::Status::Completed); // the write finished; the hold follows
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), false);
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("sStatus").get<std::string>(), "COMPLETED");
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("iUID").get<uint64_t>(), events[0].uid);
    f.evaluate(); // the timer fires, but the held channel accepts nothing
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(!events[0].writer_created);

    f.ctx->file_channels.video.complete(task, WriteEvent::Status::Completed); // the hold elapsed
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), true);
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("sStatus").get<std::string>(), "COMPLETED");
    f.evaluate();
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].writer_created);
    VG_CHECK_EQ(f.endpoint->getRuleInfo().at("dLastRuleEvent").at("sStatus").get<std::string>(), "PENDING");
}

VG_TEST_CASE(rule_info_reports_the_last_dispatched_owner_and_the_state_of_each_action)
{
    // A file channel is IDLE, or PENDING, WRITING, and HOLDING through the task it holds; Sound is UNAVAILABLE
    // without the host's callback, else its executor state.
    Fixture f(/* unwritable_store = */ false, /* with_sound = */ true);
    f.start();
    auto info = f.endpoint->getRuleInfo();
    VG_CHECK(info.at("dLastDispatchedOwner").is_null());
    for (const char *name: { "dPhoto", "dVideo" })
    {
        const auto &channel = info.at("dActionStates").at(name);
        VG_CHECK_EQ(channel.at("bEnabled").get<bool>(), false);
        VG_CHECK_EQ(channel.at("bReady").get<bool>(), false);
        VG_CHECK_EQ(channel.at("sState").get<std::string>(), "IDLE");
        VG_CHECK(channel.at("dEvent").is_null());
    }
    VG_CHECK_EQ(info.at("dActionStates").at("dSound").at("bEnabled").get<bool>(), false);
    VG_CHECK_EQ(info.at("dActionStates").at("dSound").at("bReady").get<bool>(), false);
    VG_CHECK_EQ(info.at("dActionStates").at("dSound").at("sState").get<std::string>(), "IDLE"); // available

    // Every action under a Timer rule: enabled and ready; one dispatch leaves both channels PENDING, with the
    // owner's UID on each held event, and Sound Pending.
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
    VG_CHECK_EQ(f.postRecordConfig(WithActions(RecordJson(true, "MP4", 5000), true, true, true)), 0);
    info = f.endpoint->getRuleInfo();
    for (const char *name: { "dPhoto", "dVideo", "dSound" })
    {
        VG_CHECK_EQ(info.at("dActionStates").at(name).at("bEnabled").get<bool>(), true);
        VG_CHECK_EQ(info.at("dActionStates").at(name).at("bReady").get<bool>(), true);
        VG_CHECK_EQ(info.at("dActionStates").at(name).at("sState").get<std::string>(), "IDLE");
    }
    f.evaluate();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].writer_created);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dLastDispatchedOwner").at("iUID").get<uint64_t>(), events[0].uid);
    VG_CHECK_EQ(info.at("dLastDispatchedOwner").at("sRuleType").get<std::string>(), "TIMER");
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("iUID").get<uint64_t>(), events[0].uid);
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), false);
    for (const char *name: { "dPhoto", "dVideo" })
    {
        const auto &channel = info.at("dActionStates").at(name);
        VG_CHECK_EQ(channel.at("bReady").get<bool>(), false);
        VG_CHECK_EQ(channel.at("sState").get<std::string>(), "PENDING");
        VG_CHECK_EQ(channel.at("dEvent").at("sStatus").get<std::string>(), "PENDING");
        VG_CHECK_EQ(channel.at("dEvent").at("iOwnerUID").get<uint64_t>(), events[0].uid);
    }
    VG_CHECK_EQ(info.at("dActionStates").at("dSound").at("bReady").get<bool>(), false);
    VG_CHECK_EQ(info.at("dActionStates").at("dSound").at("sState").get<std::string>(), "PENDING");

    // The Video worker claims its task and the Photo task completes: WRITING beside IDLE, Sound Idle again.
    auto &photo = f.ctx->file_channels.photo;
    auto &video = f.ctx->file_channels.video;
    const auto video_task = video.claim(f.slot->registration());
    VG_REQUIRE(video_task != nullptr);
    photo.complete(photo.current(), WriteEvent::Status::Completed);
    f.ctx->sound_executor.cancelPending();
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), false); // Video holds its task
    VG_CHECK_EQ(info.at("dActionStates").at("dPhoto").at("sState").get<std::string>(), "IDLE");
    VG_CHECK_EQ(info.at("dActionStates").at("dPhoto").at("bReady").get<bool>(), true);
    VG_CHECK(info.at("dActionStates").at("dPhoto").at("dEvent").is_null());
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "WRITING");
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("dEvent").at("sStatus").get<std::string>(), "WRITING");
    VG_CHECK_EQ(info.at("dActionStates").at("dSound").at("sState").get<std::string>(), "IDLE");
    VG_CHECK_EQ(info.at("dActionStates").at("dSound").at("bReady").get<bool>(), true);

    // The write finished: the channel holds the task through its interval (COMPLETED) or its failure cooldown
    // (FAILED), HOLDING either way, and is IDLE once the hold elapsed.
    video_task->event->status.store(WriteEvent::Status::Completed);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "HOLDING");
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("dEvent").at("sStatus").get<std::string>(), "COMPLETED");
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bReady").get<bool>(), false);
    video_task->event->status.store(WriteEvent::Status::Failed);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "HOLDING");
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("dEvent").at("sStatus").get<std::string>(), "FAILED");
    video.complete(video_task, WriteEvent::Status::Failed);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), true);
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "IDLE");
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bReady").get<bool>(), true);
    VG_CHECK(info.at("dActionStates").at("dVideo").at("dEvent").is_null());

    // The last dispatched owner moves with every dispatch, accepted or not: with the Slot not ready, the next
    // Timer tick is reported and rejected by both channels, Sound is triggered on its own, and the last rule
    // event owner stays the owner of the last accepted task.
    f.slot->ready = false;
    f.evaluate();
    const auto rejected = RuleEventRecorder::take();
    VG_REQUIRE(rejected.size() == size_t { 1 });
    VG_CHECK(!rejected[0].writer_created);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dLastDispatchedOwner").at("iUID").get<uint64_t>(), rejected[0].uid);
    VG_CHECK_EQ(info.at("dLastRuleEventOwner").at("iUID").get<uint64_t>(), events[0].uid);
    VG_CHECK_EQ(info.at("dLastRuleEvent").at("sStatus").get<std::string>(), "FAILED");
    VG_CHECK_EQ(info.at("dActionStates").at("dPhoto").at("sState").get<std::string>(), "IDLE");
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "IDLE");
    VG_CHECK_EQ(info.at("dActionStates").at("dSound").at("sState").get<std::string>(), "PENDING");

    // Without the host's callback Sound is UNAVAILABLE however it is configured.
    Fixture g;
    g.start();
    VG_CHECK_EQ(g.postRecordRuleConfig(TimerRuleJson(0)), 0);
    VG_CHECK_EQ(g.postRecordConfig(WithActions(RecordJson(true, nullptr, 0), false, false, true)), 0);
    const auto sound = g.endpoint->getRuleInfo().at("dActionStates").at("dSound");
    VG_CHECK_EQ(sound.at("bEnabled").get<bool>(), true);
    VG_CHECK_EQ(sound.at("bReady").get<bool>(), false);
    VG_CHECK_EQ(sound.at("sState").get<std::string>(), "UNAVAILABLE");
}

VG_TEST_CASE(rule_info_reports_a_continuous_clip_as_the_video_state_and_not_as_a_rule_event)
{
    // bReadyForNewEvent, dLastRuleEvent, and dLastRuleEventOwner see triggered tasks only.
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
    VG_CHECK_EQ(f.postRecordConfig(WithContinuous(RecordJson(true, "MP4", 5000))), 0);
    auto info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bEnabled").get<bool>(), true); // as configured
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bContinuous").get<bool>(), true);
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bReady").get<bool>(), false); // suppressed
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "IDLE");
    VG_CHECK(!info.at("dActionStates").at("dPhoto").contains("bContinuous"));

    // The tick admits the clip (PENDING, UID 0) and dispatches a Timer owner that no channel accepts, since
    // triggered Video is suppressed: reported, while the triggered view shows no rule event.
    f.evaluate();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(!events[0].writer_created);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), true);
    VG_CHECK(info.at("dLastRuleEvent").is_null());
    VG_CHECK(info.at("dLastRuleEventOwner").is_null());
    VG_CHECK_EQ(info.at("dLastDispatchedOwner").at("iUID").get<uint64_t>(), events[0].uid);
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "PENDING");
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("dEvent").at("iOwnerUID").get<uint64_t>(), uint64_t { 0 });
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bReady").get<bool>(), false);

    // Claimed: WRITING; completed: IDLE, and still no rule event.
    auto &video = f.ctx->file_channels.video;
    const auto clip = f.claimVideo();
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "WRITING");
    video.complete(clip, WriteEvent::Status::Completed);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "IDLE");
    VG_CHECK(info.at("dLastRuleEvent").is_null()); // a completed clip is no rule event either

    // Without the Video action configured, continuous recording still runs the channel: not enabled, continuous.
    VG_CHECK_EQ(f.postRecordConfig(WithContinuous(RecordJson(true, nullptr, 0))), 0);
    info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bEnabled").get<bool>(), false);
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bContinuous").get<bool>(), true);
    f.evaluate();
    VG_CHECK_EQ(f.endpoint->getRuleInfo().at("dActionStates").at("dVideo").at("sState").get<std::string>(), "PENDING");
}

VG_TEST_CASE(continuous_recording_is_admitted_by_the_rule_loop_whatever_the_rule_flag_and_obeys_the_schedule)
{
    // A stored configuration records from startup, unless the selected rule is ALWAYS_ON, which cannot be combined
    // with it: a POST is refused, and a stored pair fails closed with continuous recording off as well.
    Fixture f;
    f.start();
    const auto doc = WithContinuous(RecordJson(false, nullptr, 0));    // rules disabled, nothing but the switch
    VG_CHECK_EQ(f.postRecordConfig(doc), kEndpointErrorBase + EINVAL); // ALWAYS_ON, the default, is selected
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("HTTP")), 0);
    VG_CHECK_EQ(f.postRecordConfig(doc), 0);
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), doc);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_config.json"))), doc);
    VG_CHECK(!f.ctx->record_rule_enabled.load());
    auto &video = f.ctx->file_channels.video;

    f.evaluate();
    const auto clip = video.current();
    VG_REQUIRE(clip != nullptr);
    VG_CHECK(clip->kind == FileTask::Kind::Continuous);
    VG_CHECK_EQ(clip->event->owner_uid, uint64_t { 0 });
    VG_CHECK_EQ(clip->slot_registration, f.slot->registration());
    VG_CHECK(RuleEventRecorder::take().empty()); // no rule event, and no rule evaluated
    f.evaluate();
    VG_CHECK(video.current() == clip); // held: nothing new
    video.complete(f.claimVideo(), WriteEvent::Status::Completed);
    f.evaluate();
    VG_CHECK(video.current() != nullptr && video.current() != clip); // the next clip follows
    video.cancelPending();

    // The gates: no usable Slot, media paused, the schedule closed.
    f.slot->ready = false;
    f.evaluate();
    VG_CHECK(video.current() == nullptr);
    f.slot->ready = true;
    f.ctx->media_barrier.pause(false);
    f.evaluate();
    VG_CHECK(video.current() == nullptr);
    f.ctx->media_barrier.resume();
    VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(3 * kHour, 4 * kHour)), 0); // closed now
    f.evaluate();
    VG_CHECK(video.current() == nullptr);
    VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(-kHour, kHour)), 0); // open
    f.evaluate();
    const auto pending = video.current();
    VG_REQUIRE(pending != nullptr);

    // Admitted while open, then the schedule closes before a worker starts it: the closure cancels nothing,
    // and the writer records nothing (Canceled, no sink), since the schedule is a continuous clip's gate.
    VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(3 * kHour, 4 * kHour)), 0);
    VG_CHECK(video.current() == pending);
    f.pushKeyFrame();
    {
        RunningWriter writer(f, f.claimVideo());
        writer.join();
    }
    VG_CHECK(pending->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK_EQ(f.media.creates, 0);

    // A stored continuous configuration records from startup, rules disabled or not.
    Fixture g;
    WriteFile(g.file("record_config.json"), doc.dump());
    WriteFile(g.file("record_rule_config.json"), SelectedRuleJson("HTTP").dump());
    g.start();
    VG_CHECK_EQ(g.endpoint->getRecordConfig(), doc);
    g.evaluate();
    VG_REQUIRE(g.ctx->file_channels.video.current() != nullptr);
    VG_CHECK(g.ctx->file_channels.video.current()->kind == FileTask::Kind::Continuous);

    // Stored under the default ALWAYS_ON, the pair fails closed: nothing records, both switches read false with
    // the file untouched, and a supporting rule, then the configuration, recovers.
    Fixture h;
    WriteFile(h.file("record_config.json"), doc.dump());
    h.start();
    VG_CHECK(!h.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(h.endpoint->getRecordConfig(), RecordJson(false, nullptr, 0));
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(h.file("record_config.json"))), doc);
    h.evaluate();
    VG_CHECK(h.ctx->file_channels.video.current() == nullptr);
    VG_CHECK_EQ(h.postRecordRuleConfig(SelectedRuleJson("HTTP")), 0);
    h.evaluate();
    VG_CHECK(h.ctx->file_channels.video.current() == nullptr); // until the configuration is posted again
    VG_CHECK_EQ(h.postRecordConfig(doc), 0);
    h.evaluate();
    VG_REQUIRE(h.ctx->file_channels.video.current() != nullptr);
    VG_CHECK(h.ctx->file_channels.video.current()->kind == FileTask::Kind::Continuous);
}

VG_TEST_CASE(schedule_closure_finishes_a_running_continuous_clip_and_reopening_admits_a_new_one)
{
    // The clip ends at its next check, Interrupted with UID 0 and its sink finished.
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("HTTP")), 0); // any rule but ALWAYS_ON
    VG_CHECK_EQ(f.postRecordConfig(WithContinuous(RecordJson(false, nullptr, 0))), 0);
    auto &video = f.ctx->file_channels.video;
    f.evaluate();
    const auto clip = video.current();
    VG_REQUIRE(clip != nullptr);
    f.pushKeyFrame();
    {
        RunningWriter writer(f, f.claimVideo());
        VG_REQUIRE(WaitFor([&] { return FramesWritten(f.media) == 1; })); // the clip has started
        VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(3 * kHour, 4 * kHour)), 0);
        writer.join();
    }
    VG_CHECK(clip->event->status.load() == WriteEvent::Status::Interrupted);
    VG_CHECK_EQ(clip->event->owner_uid, uint64_t { 0 });
    VG_CHECK_EQ(f.media.creates, 1);
    VG_CHECK_EQ(f.media.finishes, 1);
    f.evaluate();
    VG_CHECK(video.current() == clip); // the worker completes it; the closed schedule admits nothing either way
    video.complete(clip, WriteEvent::Status::Interrupted);
    f.evaluate();
    VG_CHECK(video.current() == nullptr);
    VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(-kHour, kHour)), 0);
    f.evaluate();
    VG_REQUIRE(video.current() != nullptr);
    VG_CHECK(video.current() != clip && video.current()->kind == FileTask::Kind::Continuous);
}

VG_TEST_CASE(a_mode_switch_finishes_the_running_clip_with_its_uid_before_the_other_mode_starts)
{
    // Switching to continuous cancels the pending triggered task; either switch interrupts a running task through the
    // Video revision, and the new mode's first task is admitted only once the channel is free.
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
    const auto triggered = RecordJson(true, "MP4", 5000);
    const auto continuous = WithContinuous(triggered);
    auto &video = f.ctx->file_channels.video;

    // A pending triggered task, then the switch: canceled, and the next tick admits a continuous clip while
    // the Timer owner is reported without acceptance.
    VG_CHECK_EQ(f.postRecordConfig(triggered), 0);
    f.evaluate();
    const auto pending = video.current();
    VG_REQUIRE(pending != nullptr && pending->kind == FileTask::Kind::Triggered);
    VG_CHECK_EQ(f.postRecordConfig(continuous), 0);
    VG_CHECK(video.current() == nullptr && pending->event->status.load() == WriteEvent::Status::Canceled);
    f.evaluate();
    const auto clip = video.current();
    VG_REQUIRE(clip != nullptr && clip->kind == FileTask::Kind::Continuous);
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 2 });
    VG_CHECK(events[0].writer_created && !events[1].writer_created);

    // The clip runs; the switch back interrupts it at its next check, UID 0 kept, and the channel holds it
    // until its worker completes it: the Timer event is admitted only afterwards, with the owner's UID.
    f.pushKeyFrame();
    {
        RunningWriter writer(f, f.claimVideo());
        VG_REQUIRE(WaitFor([&] { return FramesWritten(f.media) == 1; }));
        VG_CHECK_EQ(f.postRecordConfig(triggered), 0);
        writer.join();
    }
    VG_CHECK(clip->event->status.load() == WriteEvent::Status::Interrupted);
    VG_CHECK_EQ(clip->event->owner_uid, uint64_t { 0 });
    VG_CHECK_EQ(f.media.finishes, 1);
    f.evaluate();
    VG_CHECK(video.current() == clip);
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(!events[0].writer_created);
    video.complete(clip, WriteEvent::Status::Interrupted);
    f.evaluate();
    const auto task = video.current();
    VG_REQUIRE(task != nullptr && task->kind == FileTask::Kind::Triggered);
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].writer_created);
    VG_CHECK_EQ(task->event->owner_uid, events[0].uid);

    // And the reverse: the running triggered clip is interrupted by the switch to continuous and keeps its
    // owner's UID; the first continuous clip waits for the channel.
    f.pushKeyFrame();
    {
        RunningWriter writer(f, f.claimVideo());
        VG_REQUIRE(WaitFor([&] { return FramesWritten(f.media) == 2; }));
        VG_CHECK_EQ(f.postRecordConfig(continuous), 0);
        writer.join();
    }
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Interrupted);
    VG_CHECK_EQ(task->event->owner_uid, events[0].uid);
    VG_CHECK_EQ(f.media.finishes, 2);
    f.evaluate();
    VG_CHECK(video.current() == task);
    video.complete(task, WriteEvent::Status::Interrupted);
    f.evaluate();
    VG_REQUIRE(video.current() != nullptr);
    VG_CHECK(video.current()->kind == FileTask::Kind::Continuous);
}

VG_TEST_CASE(http_activation_needs_no_storage_while_continuous_recording_suppresses_the_video_action)
{
    // With continuous recording configured, a Video-only configuration has no file action for a rule event; a Photo
    // action makes storage a precondition again and is admitted beside the clip.
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("HTTP")), 0);
    VG_CHECK_EQ(f.postRecordConfig(WithContinuous(RecordJson(true, "MP4", 5000))), 0);
    f.slot->ready = false;
    VG_CHECK_EQ(f.activateHTTP(), 0);
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(!events[0].writer_created);
    VG_CHECK(!f.ctx->file_channels.holdsTask());

    VG_CHECK_EQ(f.postRecordConfig(WithContinuous(WithActions(RecordJson(true, "MP4", 5000), true, true, false))), 0);
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EBUSY);
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    f.slot->ready = true;
    f.evaluate(); // the tick admits the clip; the HTTP rule dispatches nothing by itself
    auto &channels = f.ctx->file_channels;
    VG_REQUIRE(channels.video.current() != nullptr);
    VG_CHECK_EQ(f.activateHTTP(), 0);
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(events[0].writer_created); // Photo accepted beside the continuous clip
    VG_REQUIRE(channels.photo.current() != nullptr);
    VG_CHECK_EQ(channels.photo.current()->event->owner_uid, events[0].uid);
    VG_CHECK_EQ(channels.video.current()->event->owner_uid, uint64_t { 0 });
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

VG_TEST_CASE(a_failed_rule_switch_releases_the_new_rule_before_the_committed_one_is_rebuilt)
{
    // A record rule POST whose store fails after the new rule was built rolls back to the committed rule, which needs
    // the same GPIO pin.
    Fixture f(false, false, /* with_gpio = */ true);
    f.start();
    nlohmann::json rising = SelectedRuleJson("GPIO");
    rising["dGPIO"]
        = { { "sName", "GPIO_07" }, { "sState", "PULL_UP" }, { "sSignal", "RISING" }, { "iDebounceDurationMs", 100 } };
    VG_CHECK_EQ(f.postRecordRuleConfig(rising), 0);
    VG_CHECK_EQ(FakeGPIO::acquired, 1);
    VG_CHECK_EQ(FakeGPIO::released, 0);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_rule_config.json"))), rising);

    // The store's temporary file cannot be written, so the next save fails once the new rule exists (the store
    // removes the blocker when the save fails, so only this one does).
    std::filesystem::create_directory(f.file(".record_rule_config.json"));
    auto falling = rising;
    falling["dGPIO"]["sSignal"] = "FALLING";
    VG_CHECK(f.postRecordRuleConfig(falling) != 0);
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), rising);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_rule_config.json"))), rising);
    VG_CHECK_EQ(FakeGPIO::acquired, 3); // the committed rule released, the failed one acquired and released,
    VG_CHECK_EQ(FakeGPIO::released, 2); // the committed one acquired again
    VG_CHECK(FakeGPIO::held);

    // The committed rule is live on its pin.
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    FakeGPIO::event = VG_GPIO_PIN_EVT_RISING;
    f.evaluate();
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_GPIO);
    VG_CHECK_EQ(events[0].id, "RISING");
    VG_CHECK_EQ(events[0].source_id, "GPIO_07");
}

VG_TEST_CASE(record_config_post_persists_only_valid_payloads_in_canonical_form)
{
    Fixture f;
    f.start();
    const auto valid = RecordJson(true, "MP4", 1234);
    VG_CHECK_EQ(f.postRecordConfig(valid), 0);
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), valid);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_config.json"))), valid);
    VG_CHECK(!std::filesystem::exists(f.file(".record_config.json")));
    VG_CHECK(f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(ChannelIntervalMs(*f.ctx), 1234L);
    const auto revision = f.ctx->action_settings.getRevision();

    // Rejected payloads change neither the runtime nor the file: an invalid interval, and two actions that
    // ALWAYS_ON, the default rule, cannot pace.
    const auto res = f.endpoint->updateRecordConfig(RecordJson(true, "MP4", 0));
    VG_CHECK_EQ(res.at("code").get<int>(), kEndpointErrorBase + EINVAL);
    VG_CHECK(res.contains("message"));
    auto both = RecordJson(true, "MP4", 5000);
    both["dActions"]["dPhoto"]["bEnabled"] = true;
    VG_CHECK_EQ(f.postRecordConfig(both), kEndpointErrorBase + EINVAL);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, nullptr, 0)), kEndpointErrorBase + EINVAL); // rules on, no action
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), valid);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_config.json"))), valid);
    VG_CHECK(f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(f.ctx->action_settings.getRevision(), revision);
    VG_CHECK(!std::filesystem::exists(f.file(".record_config.json")));

    // Unknown fields are dropped: the file holds the canonical document.
    auto extra = RecordJson(false, "JPG", 2000);
    extra["sNote"] = "dropped";
    VG_CHECK_EQ(f.postRecordConfig(extra), 0);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_config.json"))), RecordJson(false, "JPG", 2000));
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, "JPG", 2000));
    VG_CHECK(!f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(ChannelFormat(*f.ctx), "JPG");
    VG_CHECK_EQ(ChannelIntervalMs(*f.ctx), 2000L);

    // Rules off with no file action is a valid state: the snapshot has no file channel.
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(false, nullptr, 0)), 0);
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, nullptr, 0));
    VG_CHECK_EQ(ChannelIntervalMs(*f.ctx), -1L);
}

VG_TEST_CASE(record_config_post_fails_closed_when_the_store_is_unwritable)
{
    Fixture f(/* unwritable_store = */ true);
    f.start();
    VG_CHECK(!f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, nullptr, 0));

    const auto res = f.endpoint->updateRecordConfig(RecordJson(true, "MP4", 1234));
    VG_CHECK(res.at("code").get<int>() != 0);
    VG_CHECK(res.contains("message"));
    // Nothing was applied because nothing could be persisted.
    VG_CHECK(!f.ctx->record_rule_enabled.load());
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, nullptr, 0));
    VG_CHECK_EQ(ChannelIntervalMs(*f.ctx), -1L); // the defaults: no file channel
    f.evaluate();
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
}

VG_TEST_CASE(schedule_post_fails_closed_when_the_store_is_unwritable)
{
    // A POST that repeats the schedule in memory answers the store's error too, and a changed one that cannot be
    // stored applies nothing.
    Fixture f(/* unwritable_store = */ true);
    f.start();
    const auto unchanged = f.endpoint->getScheduleRuleConfig();
    VG_CHECK(f.postScheduleRuleConfig(unchanged) != 0);
    auto changed = unchanged;
    changed["bEnabled"] = true;
    VG_CHECK(f.postScheduleRuleConfig(changed) != 0);
    VG_CHECK_EQ(f.endpoint->getScheduleRuleConfig(),
        unchanged); // nothing was applied because nothing could be persisted
}

VG_TEST_CASE(the_always_on_invariant_is_enforced_by_both_posts_in_either_order)
{
    // To run several actions, select another rule first; to return to ALWAYS_ON, simplify the actions first. A
    // rejected POST changes neither the runtime nor a file.
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), SelectedRuleJson("ALWAYS_ON")); // the default selection
    const auto photo_video = WithActions(RecordJson(true, "MP4", 5000), true, true, false);
    const auto everything = WithActions(RecordJson(true, "MP4", 5000), true, true, true);
    const auto sound_only = WithActions(RecordJson(false, nullptr, 0), false, false, true);
    const auto rule_only = RecordJson(true, nullptr, 0);
    for (const auto &doc: { photo_video, everything, sound_only, rule_only })
    {
        const auto res = f.endpoint->updateRecordConfig(doc);
        VG_CHECK_EQ(res.at("code").get<int>(), kEndpointErrorBase + EINVAL);
        VG_CHECK(res.contains("message"));
    }
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(false, nullptr, 0));
    VG_CHECK(!std::filesystem::exists(f.file("record_config.json")));
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "JPG", 1000)), 0); // one channel: what ALWAYS_ON paces

    // Another rule first, then every action: both channels accept, and the RULE report is file acceptance.
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
    VG_CHECK_EQ(f.postRecordConfig(everything), 0);
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), everything);
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_config.json"))), everything);
    {
        const auto settings = f.ctx->action_settings.get().settings;
        VG_REQUIRE(settings != nullptr);
        VG_CHECK(settings->photo && settings->video && settings->sound_enabled);
    }
    f.evaluate();
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(events[0].writer_created);
    VG_CHECK(f.ctx->file_channels.photo.holdsTask() && f.ctx->file_channels.video.holdsTask());

    // ALWAYS_ON cannot be selected over it: the selection, its file, and the rule instance are unchanged.
    const auto always = SelectedRuleJson("ALWAYS_ON");
    const auto res = f.endpoint->updateRecordRuleConfig(always);
    VG_CHECK_EQ(res.at("code").get<int>(), kEndpointErrorBase + EINVAL);
    VG_CHECK(res.contains("message"));
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), TimerRuleJson(0));
    VG_CHECK_EQ(nlohmann::json::parse(ReadFile(f.file("record_rule_config.json"))), TimerRuleJson(0));
    CompleteCurrent(*f.ctx, WriteEvent::Status::Completed);
    CompleteCurrent(*f.ctx, WriteEvent::Status::Completed);
    f.evaluate();
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);

    // Simplify the actions first, then ALWAYS_ON; from there the limit applies to the record configuration.
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    VG_CHECK_EQ(f.postRecordRuleConfig(always), 0);
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), always);
    VG_CHECK_EQ(f.postRecordConfig(photo_video), kEndpointErrorBase + EINVAL);
    VG_CHECK_EQ(f.postRecordConfig(WithActions(RecordJson(true, "MP4", 5000), false, true, true)),
        kEndpointErrorBase + EINVAL); // Sound
    VG_CHECK_EQ(f.postRecordConfig(rule_only), kEndpointErrorBase + EINVAL);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(false, "MP4", 5000)), 0); // rules off, one channel armed
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(false, nullptr, 0)), 0);  // the first-start state
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "RAW", 2000)), 0);
    VG_CHECK_EQ(f.endpoint->getRecordConfig(), RecordJson(true, "RAW", 2000));

    // No rule selected: no limit; selecting ALWAYS_ON afterwards is checked against what was committed.
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("")), 0);
    VG_CHECK_EQ(f.postRecordConfig(sound_only), 0);
    VG_CHECK_EQ(f.postRecordRuleConfig(always), kEndpointErrorBase + EINVAL);
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), SelectedRuleJson(""));
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(false, nullptr, 0)), 0);
    VG_CHECK_EQ(f.postRecordRuleConfig(always), 0);
}

VG_TEST_CASE(startup_fails_closed_when_the_stored_pair_violates_the_always_on_invariant)
{
    // A violation keeps rules disabled like an unusable file, with both files left in place and both documents
    // reported; a supporting rule, then the record configuration, recovers.
    const auto everything = WithActions(RecordJson(true, "MP4", 5000), true, true, true);
    {
        Fixture f; // no rule file: the default ALWAYS_ON is selected
        const std::string record = everything.dump();
        WriteFile(f.file("record_config.json"), record);
        f.start();
        VG_CHECK(!f.ctx->record_rule_enabled.load());
        VG_CHECK_EQ(f.endpoint->getRecordConfig(), WithActions(RecordJson(false, "MP4", 5000), true, true, true));
        VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), SelectedRuleJson("ALWAYS_ON"));
        VG_CHECK_EQ(ReadFile(f.file("record_config.json")), record);
        VG_CHECK(!std::filesystem::exists(f.file("record_rule_config.json")));
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
        VG_CHECK_EQ(f.postRecordConfig(everything), kEndpointErrorBase + EINVAL); // ALWAYS_ON is still selected
        VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(0)), 0);
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 }); // rules are still disabled
        VG_CHECK_EQ(f.postRecordConfig(everything), 0);
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 1 });
    }
    {
        Fixture f; // an explicit ALWAYS_ON selection over the same actions
        const std::string record = everything.dump();
        const std::string rule = SelectedRuleJson("ALWAYS_ON").dump();
        WriteFile(f.file("record_config.json"), record);
        WriteFile(f.file("record_rule_config.json"), rule);
        f.start();
        VG_CHECK(!f.ctx->record_rule_enabled.load());
        VG_CHECK_EQ(ReadFile(f.file("record_config.json")), record);
        VG_CHECK_EQ(ReadFile(f.file("record_rule_config.json")), rule);
        VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), SelectedRuleJson("ALWAYS_ON"));
    }
    {
        Fixture f; // a supporting rule stored with the same actions: the pair is valid and runs
        WriteFile(f.file("record_config.json"), everything.dump());
        WriteFile(f.file("record_rule_config.json"), TimerRuleJson(0).dump());
        f.start();
        VG_CHECK(f.ctx->record_rule_enabled.load());
        VG_CHECK_EQ(f.endpoint->getRecordConfig(), everything);
        f.evaluate();
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK(events[0].writer_created);
        VG_CHECK(f.ctx->file_channels.photo.holdsTask() && f.ctx->file_channels.video.holdsTask());
    }
}

VG_TEST_CASE(always_on_runs_each_single_file_action_with_its_format_and_paces_on_its_interval)
{
    // Under ALWAYS_ON, the default rule: the format and the interval reach the task unchanged, nothing is converted
    // to continuous recording, the rule waits through the hold without consuming its interval, and rule disable and
    // schedule closure stop it.
    struct Single
    {
        const char *format;
        CaptureType type;
        long interval_ms;
    };
    for (const Single &s: { Single { "MP4", CaptureType::Video, 20 }, Single { "JPG", CaptureType::Image, 30 },
             Single { "RAW", CaptureType::Image, 40 } })
    {
        const auto other_type = s.type == CaptureType::Video ? CaptureType::Image : CaptureType::Video;
        const char *own_key = s.type == CaptureType::Video ? "dVideo" : "dPhoto";
        const char *other_key = s.type == CaptureType::Video ? "dPhoto" : "dVideo";
        const auto interval = std::chrono::milliseconds(s.interval_ms);
        Fixture f;
        f.start();
        VG_CHECK_EQ(f.endpoint->getRecordRuleConfig(), SelectedRuleJson("ALWAYS_ON")); // the default selection
        const auto doc = RecordJson(true, s.format, s.interval_ms);
        VG_CHECK_EQ(f.postRecordConfig(doc), 0);
        VG_CHECK_EQ(f.endpoint->getRecordConfig(), doc); // returned as posted: no continuous conversion
        const auto settings = f.ctx->action_settings.get().settings;
        VG_REQUIRE(settings != nullptr);
        VG_CHECK(settings->continuous == nullptr);
        const auto channel = settings->fileChannel();
        VG_REQUIRE(channel != nullptr);
        VG_CHECK(channel->type == s.type);
        VG_CHECK_EQ(channel->format, s.format);
        VG_CHECK(channel->interval == interval);
        VG_CHECK(settings->channel(other_type) == nullptr);

        // The first interval is elapsed at selection: the first tick dispatches into the configured channel.
        f.evaluate();
        auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_ALWAYS_ON);
        VG_CHECK(events[0].writer_created);
        const auto first_start = f.ctx->shared_time.steadyTime(); // the rule's reference: the tick it fired on
        auto &own = f.ctx->file_channels.of(s.type);
        const auto task = own.current();
        VG_REQUIRE(task != nullptr);
        VG_CHECK(task->kind == FileTask::Kind::Triggered);
        VG_CHECK(task->type == s.type);
        VG_REQUIRE(task->owner != nullptr);
        VG_CHECK_EQ(task->owner->uid, events[0].uid);
        VG_CHECK_EQ(task->event->owner_uid, events[0].uid);
        VG_CHECK_EQ(task->settings->format, s.format);
        VG_CHECK(task->settings->interval == interval);
        if (s.type == CaptureType::Video)
            VG_CHECK(task->settings->video_clip_length == kVideoClipLengthDefault);
        VG_CHECK(f.ctx->file_channels.of(other_type).current() == nullptr);
        auto info = f.endpoint->getRuleInfo();
        VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), false);
        VG_CHECK_EQ(info.at("dActionStates").at(own_key).at("sState").get<std::string>(), "PENDING");
        VG_CHECK_EQ(info.at("dActionStates").at(other_key).at("sState").get<std::string>(), "IDLE");
        VG_CHECK_EQ(info.at("dActionStates").at(other_key).at("bEnabled").get<bool>(), false);
        VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bContinuous").get<bool>(), false);

        // Held through the write and the hold, the channel blocks the rule, which keeps its reference time.
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
        VG_CHECK(own.claim(f.slot->registration()) == task); // the worker runs the task ...
        own.complete(task, WriteEvent::Status::Completed);   // ... and completes it after its hold

        // Re-armed no earlier than the interval after the previous start: every tick before that dispatches
        // nothing, and the first tick at or after it dispatches again. The rule reads the context's steady time,
        // which moves on the tick, so the comparison is exact.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool dispatched = false;
        while (!dispatched && std::chrono::steady_clock::now() < deadline)
        {
            f.evaluate();
            dispatched = RuleEventRecorder::size() == size_t { 1 };
            const auto since_start = f.ctx->shared_time.steadyTime() - first_start;
            if (dispatched)
                VG_CHECK(since_start >= interval);
            else
                VG_CHECK(since_start < interval);
        }
        VG_REQUIRE(dispatched);
        events = RuleEventRecorder::take();
        VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_ALWAYS_ON);
        VG_CHECK(events[0].writer_created);
        VG_CHECK(events[0].uid > task->owner->uid);
        const auto next = own.current();
        VG_REQUIRE(next != nullptr);
        VG_CHECK(next != task);
        VG_CHECK_EQ(next->settings->format, s.format);

        // Rule disable dispatches nothing and retires the pending task; a closed schedule dispatches nothing
        // either, and reopening it lets the rule fire again.
        VG_CHECK_EQ(f.postRecordConfig(RecordJson(false, s.format, s.interval_ms)), 0);
        f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
        VG_CHECK(own.current() == nullptr);
        VG_CHECK(next->event->status.load() == WriteEvent::Status::Canceled);
        VG_CHECK_EQ(f.postRecordConfig(doc), 0);
        VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(3 * kHour, 4 * kHour)), 0); // closed
        for (int i = 0; i < 3; ++i)
            f.evaluate();
        VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
        VG_CHECK(own.current() == nullptr);
        VG_CHECK_EQ(f.postScheduleRuleConfig(ScheduleWindowJson(-kHour, kHour)), 0); // open
        VG_CHECK(WaitFor([&] {
            f.evaluate();
            return RuleEventRecorder::size() == size_t { 1 };
        }));
        events = RuleEventRecorder::take();
        VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_ALWAYS_ON);
        VG_CHECK(events[0].writer_created);
        const auto reopened = own.current();
        VG_REQUIRE(reopened != nullptr);
        VG_CHECK_EQ(reopened->settings->format, s.format);
        VG_CHECK(reopened->settings->interval == interval);
    }
}

VG_TEST_CASE(activation_without_a_file_action_needs_no_storage_and_reports_the_owner)
{
    // Rule-only and Sound-only configurations. Sound has no callback in this fixture, so its trigger is unavailable.
    Fixture f;
    f.start();
    f.slot->close(); // no Slot at all
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("HTTP")), 0);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, nullptr, 0)), 0); // rule-only
    VG_CHECK_EQ(f.activateHTTP(), 0);
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_HTTP);
    VG_CHECK(!events[0].writer_created);
    VG_CHECK_EQ(f.activateHTTP(), 0); // nothing is pending: the next activation proceeds
    VG_CHECK_EQ(RuleEventRecorder::take().size(), size_t { 1 });
    const auto info = f.endpoint->getRuleInfo();
    VG_CHECK_EQ(info.at("bReadyForNewEvent").get<bool>(), true);
    VG_CHECK(info.at("dLastRuleEvent").is_null());

    VG_CHECK_EQ(f.postRecordConfig(WithActions(RecordJson(true, nullptr, 0), false, false, true)), 0); // Sound-only
    VG_CHECK_EQ(f.activateHTTP(), 0);
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(!events[0].writer_created);
    VG_CHECK(f.ctx->sound_executor.state() == Context::SoundActionExecutor::State::Idle); // unavailable

    // A file action makes storage a precondition again.
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EBUSY);
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    f.slot->open();
    VG_CHECK_EQ(f.activateHTTP(), 0);
    VG_CHECK_EQ(RuleEventRecorder::take().size(), size_t { 1 });
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
    CompleteCurrent(*f.ctx, WriteEvent::Status::Completed);

    // A storage slot that is not ready refuses the request before any RULE report; so does no slot at all.
    f.slot->ready = false;
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EBUSY);
    f.slot->ready = true;
    f.slot->close();
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EBUSY);
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    f.slot->open();
    VG_CHECK_EQ(f.activateHTTP(), 0);
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 1 });

    // The HTTP rule never fires from the rule loop itself.
    CompleteCurrent(*f.ctx, WriteEvent::Status::Completed);
    RuleEventRecorder::clear();
    f.evaluate();
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
}

VG_TEST_CASE(config_store_reports_missing_and_invalid_files_and_saves_atomically)
{
    Fixture f;
    ConfigStore store(*f.ctx, "store_test.json");
    const auto file = f.file("store_test.json");

    nlohmann::json loaded;
    VG_CHECK_EQ(store.loadFromFile(loaded), ENOENT);
    WriteFile(file, "{ not json");
    VG_CHECK_EQ(store.loadFromFile(loaded), EINVAL);

    // The document is written to a temporary file and renamed into place; nothing else is left behind.
    const nlohmann::json doc = { { "a", 1 } };
    VG_CHECK_EQ(store.saveToFile(doc), 0);
    VG_CHECK_EQ(ReadFile(file), doc.dump(4));
    VG_CHECK(!std::filesystem::exists(f.file(".store_test.json")));
    VG_CHECK_EQ(store.loadFromFile(loaded), 0);
    VG_CHECK_EQ(loaded, doc);
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

VG_TEST_CASE(http_activation_runs_the_rule_callback_outside_the_rule_lock)
{
    // A RULE callback may call back into the configuration endpoints, here replacing the rule.
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    VG_CHECK_EQ(f.postRecordRuleConfig(SelectedRuleJson("HTTP")), 0);
    int nested = -1;
    RuleEventRecorder::hook = [&](const vg_rule_evt_t &) { nested = f.postRecordRuleConfig(TimerRuleJson(0)); };
    VG_CHECK_EQ(f.activateHTTP(), 0);
    RuleEventRecorder::hook = nullptr;
    VG_CHECK_EQ(nested, 0);
    VG_CHECK_EQ(f.endpoint->getRecordRuleConfig().at("sCurrentSelected").get<std::string>(), "TIMER");
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_HTTP);
    VG_CHECK(events[0].writer_created);
    VG_CHECK_EQ(f.activateHTTP(), kEndpointErrorBase + EPERM); // the selected rule is not HTTP
}

VG_TEST_CASE(timer_rule_paces_on_the_context_clock_and_does_not_catch_up_after_a_closure)
{
    // A held channel does not pace the rule: the owner is reported with nothing accepted. Real time passes here, as
    // the clock has no fake; every wait is bounded.
    const auto interval = std::chrono::seconds(1);
    Fixture f;
    f.start();
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    VG_CHECK_EQ(f.postRecordRuleConfig(TimerRuleJson(1)), 0);
    auto &video = f.ctx->file_channels.of(CaptureType::Video);

    f.evaluate(); // the first interval is elapsed at selection
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(events[0].writer_created);
    const auto first = f.ctx->shared_time.steadyTime();
    const auto task = video.current();
    VG_REQUIRE(task != nullptr);

    // Re-armed no earlier than the interval after the tick it fired on, the channel still holding the task.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool dispatched = false;
    while (!dispatched && std::chrono::steady_clock::now() < deadline)
    {
        f.evaluate();
        dispatched = RuleEventRecorder::size() == size_t { 1 };
        const auto since_first = f.ctx->shared_time.steadyTime() - first;
        if (dispatched)
            VG_CHECK(since_first >= interval);
        else
            VG_CHECK(since_first < interval);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    VG_REQUIRE(dispatched);
    events = RuleEventRecorder::take();
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(!events[0].writer_created); // refused by the held channel, reported all the same
    VG_CHECK(video.current() == task);
    const auto second = f.ctx->shared_time.steadyTime();

    // Disabled for longer than two intervals: nothing is evaluated meanwhile (the pending task is canceled by
    // the disable), one owner comes on the first tick after reopening, and none on the ticks right after it.
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(false, "MP4", 5000)), 0);
    while (std::chrono::steady_clock::now() - second < 2 * interval + std::chrono::milliseconds(100))
    {
        f.evaluate();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    VG_CHECK(video.current() == nullptr);
    VG_CHECK_EQ(f.postRecordConfig(RecordJson(true, "MP4", 5000)), 0);
    f.evaluate();
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(events[0].writer_created);
    const auto reopened = f.ctx->shared_time.steadyTime();
    for (int i = 0; i < 5 && f.ctx->shared_time.steadyTime() - reopened < interval; ++i)
        f.evaluate(); // the ticks right after it, within the interval
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
}

VG_TEST_MAIN()
