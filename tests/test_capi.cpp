// Characterization tests for the C API lifecycle (docs/PLAN.md section 8.3): the instance guard, nested
// scoped suspension, reentry from a callback while the runtime is being built or torn down, and a nested C API
// call from a callback on the thread of a C API call. The runtime
// is real (rule, storage, and capture threads, plus the selected Slot's manager and channel workers) over the
// fake configuration: the internal "device" is a path that does not exist, every scanned block device is
// hidden, and no media sink is reachable, so nothing here touches a device or needs privileges.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "vigil/constants.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil_capi.h"
#include "vigil_ctypes.h"

#include "check.hpp"
#include "fake_config.hpp"

using namespace vigil::tests;

namespace {

std::string MakeTempDir()
{
    std::string path = (std::filesystem::temp_directory_path() / "vigil-test-XXXXXX").string();
    if (::mkdtemp(path.data()) == nullptr)
        throw std::runtime_error("mkdtemp failed");
    return path;
}

// A log callback that, while armed, reenters the C API from the thread that is building or tearing down the
// runtime (both log on that thread). Log lines from the runtime's own threads are only printed.
struct Reentry
{
    static inline std::atomic<bool> armed = false;
    static inline std::atomic<std::thread::id> thread;
    static inline std::atomic<int> count = 0;
    static inline std::atomic<int> status = -1;

    static vg_sta_e log(vg_log_level_e level, const char *msg, size_t len)
    {
        TestLogCallback(level, msg, len);
        if (armed.load() && std::this_thread::get_id() == thread.load())
        {
            vg_http_api_response_t response {};
            status.store(vg_http_api_rule_info_get(&response));
            if (response.free_cb && response.body)
                response.free_cb(response.body);
            ++count;
        }
        return 0;
    }

    static void arm()
    {
        thread.store(std::this_thread::get_id());
        count.store(0);
        status.store(-1);
        armed.store(true);
    }

    static void disarm()
    {
        armed.store(false);
    }
};

struct Fixture
{
    std::string root;        // temporary directory, removed afterwards; also the configuration directory
    std::string device;      // the internal "device": a path that does not exist, so nothing is ever mounted
    std::string mount_point; // where it would be mounted
    vg_config_t config;

    Fixture() : root(MakeTempDir()), device(root + "/no-such-device"), mount_point(root + "/mnt")
    {
        config = MakeFakeConfig(root.c_str());
        config.basic.log_cb = &Reentry::log;
        config.storage.internal_device_path = device.c_str();
        config.storage.internal_mount_point = mount_point.c_str();
        config.storage.is_hidden_device_cb = [](const char *) { return true; }; // never touch a real block device
        Reentry::disarm();
    }

    ~Fixture()
    {
        Reentry::disarm();
        vg_service_deinit();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

nlohmann::json Get(vg_sta_e (*api)(vg_http_api_response_t *))
{
    vg_http_api_response_t response {};
    VG_REQUIRE(api(&response) == 0);
    const auto *body = static_cast<const char *>(response.body);
    auto j = nlohmann::json::parse(body, body + response.body_len);
    response.free_cb(response.body);
    return j;
}

bool MediaPaused()
{
    return Get(&vg_http_api_rule_info_get).at("bMediaPaused").get<bool>();
}

// The storage manager thread adds the internal Slot on its first scan and enables it once it is selected, both
// on its own schedule: a condition to wait for, not a delay.
nlohmann::json WaitForInternalSlot(const Fixture &f, bool enabled)
{
    while (true)
    {
        const auto status = Get(&vg_http_api_storage_status_get); // outlives the loop over its slots
        for (const auto &slot: status.at("lSlots"))
        {
            if (slot.at("sDevPath").get<std::string>() == f.device && slot.at("bEnabled").get<bool>() == enabled)
                return slot;
        }
        std::this_thread::yield();
    }
}

// Posts `payload` to a C API entry point and returns the code of its JSON result.
int Post(vg_sta_e (*api)(const vg_http_api_request_t *, vg_http_api_response_t *), const nlohmann::json &payload)
{
    std::string body = payload.dump();
    vg_http_api_request_t request { .free_cb = nullptr, .payload = body.data(), .payload_len = body.size() };
    vg_http_api_response_t response {};
    VG_REQUIRE(api(&request, &response) == 0);
    const auto *text = static_cast<const char *>(response.body);
    const auto result = nlohmann::json::parse(text, text + response.body_len);
    response.free_cb(response.body);
    return result.at("code").get<int>();
}

// Selects the internal "device" as the enabled Slot through the C API: its manager thread and its Photo and
// Video channel workers start and idle, since the device does not exist (docs/PLAN.md section 7.2), and its
// destructor logs and joins them during teardown.
nlohmann::json SelectInternalSlot(const Fixture &f)
{
    nlohmann::json config;
    config["dSelectSlotToEnable"]["sByDevPath"] = f.device;
    config["dSelectSlotToEnable"]["sByUUID"] = nullptr;
    VG_REQUIRE(Post(&vg_http_api_storage_config_post, config) == 0);
    return WaitForInternalSlot(f, true);
}

// The host's playback callback: counts its calls, which come from the runtime's Sound worker.
struct SoundCalls
{
    static inline std::atomic<int> count = 0;

    static vg_sta_e play(void *)
    {
        ++count;
        return 0;
    }
};

// Spins until `pred` holds: a bounded wait on a runtime thread, not a timing assumption.
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

} // namespace

VG_TEST_CASE(the_instance_guard_reports_missing_and_duplicate_runtimes)
{
    Fixture f;
    vg_http_api_response_t response {};
    VG_CHECK_EQ(vg_service_deinit(), 0); // nothing to tear down
    VG_CHECK_EQ(vg_http_api_rule_info_get(&response), ESRCH);
    VG_CHECK_EQ(vg_media_scoped_suspend_write(false), ESRCH);
    VG_CHECK_EQ(vg_media_scoped_resume_write(false), ESRCH);
    VG_CHECK_EQ(vg_service_init(nullptr), EINVAL);

    VG_REQUIRE(vg_service_init(&f.config) == 0);
    VG_CHECK_EQ(vg_service_init(&f.config), EALREADY);
    VG_CHECK(!MediaPaused());

    VG_CHECK_EQ(vg_service_deinit(), 0);
    VG_CHECK_EQ(vg_http_api_rule_info_get(&response), ESRCH);
    VG_CHECK_EQ(vg_service_deinit(), 0);
    VG_REQUIRE(vg_service_init(&f.config) == 0); // the runtime can be built again
    VG_CHECK(!MediaPaused());
}

VG_TEST_CASE(scoped_suspension_nests_and_resumes_only_when_balanced)
{
    Fixture f;
    VG_REQUIRE(vg_service_init(&f.config) == 0);
    VG_CHECK_EQ(vg_media_scoped_resume_write(false), ECANCELED); // nothing to resume
    VG_CHECK_EQ(vg_media_scoped_suspend_write(true), 0);         // nothing in flight: returns at once
    VG_CHECK(MediaPaused());
    VG_CHECK_EQ(vg_media_scoped_suspend_write(false), 0);
    VG_CHECK_EQ(vg_media_scoped_resume_write(false), 0);
    VG_CHECK(MediaPaused()); // the outer suspension is still open
    VG_CHECK_EQ(vg_media_scoped_resume_write(true), 0);
    VG_CHECK(!MediaPaused());
    VG_CHECK_EQ(vg_media_scoped_resume_write(false), ECANCELED);
}

VG_TEST_CASE(a_callback_can_reenter_the_c_api_while_the_runtime_is_built_or_torn_down)
{
    Fixture f;
    // The runtime logs while it is built; a log callback that calls back into the C API from that thread
    // finds no instance yet, instead of blocking on the lock init used to hold (a deadlock before 2026-10-19).
    Reentry::arm();
    VG_REQUIRE(vg_service_init(&f.config) == 0);
    Reentry::disarm();
    VG_CHECK(Reentry::count.load() > 0);
    VG_CHECK_EQ(Reentry::status.load(), ESRCH);

    // The same while it is torn down, with the selected Slot's threads among those joined: the instance is
    // detached before any worker is joined.
    SelectInternalSlot(f);
    Reentry::arm();
    VG_CHECK_EQ(vg_service_deinit(), 0);
    Reentry::disarm();
    VG_CHECK(Reentry::count.load() > 0);
    VG_CHECK_EQ(Reentry::status.load(), ESRCH);
}

VG_TEST_CASE(request_bodies_are_bounded_before_parsing)
{
    Fixture f;
    VG_REQUIRE(vg_service_init(&f.config) == 0);
    std::string body = "{}";
    body.resize(vigil::constants::kHTTPAPIRequestMaxBytes, ' '); // exactly the limit: parsed and answered
    vg_http_api_request_t request { .free_cb = nullptr, .payload = body.data(), .payload_len = body.size() };
    vg_http_api_response_t response {};
    VG_CHECK_EQ(vg_http_api_rule_http_rule_activate_post(&request, &response), 0);
    VG_REQUIRE(response.body != nullptr);
    const auto *text = static_cast<const char *>(response.body);
    const auto j = nlohmann::json::parse(text, text + response.body_len);
    VG_CHECK_EQ(j.at("code").get<int>(), vigil::constants::kEndpointErrorBase + EPERM); // rules are disabled
    response.free_cb(response.body);
    response = {};

    body.resize(vigil::constants::kHTTPAPIRequestMaxBytes + 1, ' '); // one byte over: refused before parsing
    request.payload = body.data();
    request.payload_len = body.size();
    VG_CHECK_EQ(vg_http_api_rule_http_rule_activate_post(&request, &response), EFAULT);
    VG_CHECK(response.body == nullptr);
}

VG_TEST_CASE(activation_without_a_file_action_needs_no_storage_and_plays_the_sound)
{
    // Rule-only and Sound-only configurations through the C API (docs/PLAN.md sections 4.1 and 5.2), with no
    // Slot selected: the activation is dispatched without storage, the RULE callback reports it without file
    // acceptance, and the Sound worker plays the trigger; a file action makes storage a precondition again.
    Fixture f;
    f.config.media.sound_play_cb = &SoundCalls::play;
    SoundCalls::count = 0;
    RuleEventRecorder::clear();
    VG_REQUIRE(vg_service_init(&f.config) == 0);
    nlohmann::json http;
    http["sCurrentSelected"] = "HTTP";
    VG_CHECK_EQ(Post(&vg_http_api_rule_record_config_post, http), 0);
    auto record = nlohmann::json::parse(R"({
        "iSchemaVersion": 2, "bRuleEnabled": true, "bContinuousRecordEnabled": false,
        "dActions": { "dPhoto": { "bEnabled": false, "sFormat": "JPG", "iIntervalMs": 5000 },
                      "dVideo": { "bEnabled": false, "iIntervalMs": 5000 }, "dSound": { "bEnabled": true } }
    })");
    VG_CHECK_EQ(Post(&vg_http_api_rule_config_post, record), 0);
    VG_CHECK_EQ(Post(&vg_http_api_rule_http_rule_activate_post, nlohmann::json::object()), 0);
    auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_HTTP);
    VG_CHECK(!events[0].writer_created);
    VG_CHECK(WaitFor([] { return SoundCalls::count.load() == 1; }));

    record["dActions"]["dSound"]["bEnabled"] = false; // rule-only
    VG_CHECK_EQ(Post(&vg_http_api_rule_config_post, record), 0);
    VG_CHECK_EQ(Post(&vg_http_api_rule_http_rule_activate_post, nlohmann::json::object()), 0);
    events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK(!events[0].writer_created);

    record["dActions"]["dVideo"]["bEnabled"] = true;
    VG_CHECK_EQ(Post(&vg_http_api_rule_config_post, record), 0);
    VG_CHECK_EQ(Post(&vg_http_api_rule_http_rule_activate_post, nlohmann::json::object()),
        vigil::constants::kEndpointErrorBase + EBUSY); // no storage is ready to write
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 0 });
    VG_CHECK_EQ(SoundCalls::count.load(), 1);
    VG_CHECK_EQ(vg_service_deinit(), 0);
}

VG_TEST_CASE(continuous_recording_is_accepted_through_the_c_api_and_suppresses_the_video_action_for_activation)
{
    // docs/PLAN.md sections 4.1 and 8.2 (Phase 5A-3) through the C API, with no Slot ready: the switch is
    // accepted and returned canonically, rule info reports the Video channel as continuous and not ready for a
    // rule event, the rule loop admits nothing without storage, an HTTP activation of a Video-only
    // configuration needs no storage since triggered Video is suppressed, and ALWAYS_ON cannot be selected
    // over it.
    Fixture f;
    RuleEventRecorder::clear();
    VG_REQUIRE(vg_service_init(&f.config) == 0);
    nlohmann::json http;
    http["sCurrentSelected"] = "HTTP";
    VG_CHECK_EQ(Post(&vg_http_api_rule_record_config_post, http), 0);
    const auto record = nlohmann::json::parse(R"({
        "iSchemaVersion": 2, "bRuleEnabled": true, "bContinuousRecordEnabled": true,
        "dActions": { "dPhoto": { "bEnabled": false, "sFormat": "JPG", "iIntervalMs": 5000 },
                      "dVideo": { "bEnabled": true, "iIntervalMs": 5000 }, "dSound": { "bEnabled": false } }
    })");
    VG_CHECK_EQ(Post(&vg_http_api_rule_config_post, record), 0);
    VG_CHECK_EQ(Get(&vg_http_api_rule_config_get), record);
    const auto info = Get(&vg_http_api_rule_info_get);
    VG_CHECK_EQ(info.at("bRecordEnabled").get<bool>(), true);
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bEnabled").get<bool>(), true);
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bContinuous").get<bool>(), true);
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("bReady").get<bool>(), false);
    VG_CHECK_EQ(info.at("dActionStates").at("dVideo").at("sState").get<std::string>(), "IDLE"); // no Slot
    VG_CHECK_EQ(Post(&vg_http_api_rule_http_rule_activate_post, nlohmann::json::object()), 0);
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_HTTP);
    VG_CHECK(!events[0].writer_created);
    nlohmann::json always_on;
    always_on["sCurrentSelected"] = "ALWAYS_ON";
    VG_CHECK_EQ(Post(&vg_http_api_rule_record_config_post, always_on), vigil::constants::kEndpointErrorBase + EINVAL);
    VG_CHECK_EQ(vg_service_deinit(), 0);
}

VG_TEST_CASE(the_selected_slot_starts_its_manager_and_both_channel_workers_and_deinit_joins_them)
{
    // The runtime's full thread set (docs/PLAN.md sections 7.2 and 8.3): selecting the internal Slot starts its
    // manager thread and its Photo and Video workers, deinit joins them, and the runtime builds again on the
    // stored selection.
    Fixture f;
    VG_REQUIRE(vg_service_init(&f.config) == 0);
    auto slot = SelectInternalSlot(f);
    VG_CHECK(slot.at("bSelected").get<bool>());
    VG_CHECK(!slot.at("bWriting").get<bool>());
    VG_CHECK_EQ(vg_service_deinit(), 0);

    VG_REQUIRE(vg_service_init(&f.config) == 0); // the selection was stored: the Slot is enabled again
    slot = WaitForInternalSlot(f, true);
    VG_CHECK(slot.at("bSelected").get<bool>());
    VG_CHECK_EQ(vg_service_deinit(), 0);
}

VG_TEST_CASE(init_validates_the_configuration_and_the_api_refuses_empty_frames_and_unreleased_responses)
{
    // The C API boundary (docs/PLAN.md section 10): a configuration without its directory is refused before
    // anything is built, a frame without a payload is refused instead of being kept by reference, a response
    // structure must be zero-initialized or released (the release macro clears it completely since
    // 2026-10-20, so it can be reused), and a GPIO query that fails without writing its list lists nothing.
    Fixture f;
    auto no_directory = f.config;
    no_directory.basic.config_directory = nullptr;
    VG_CHECK_EQ(vg_service_init(&no_directory), EINVAL);
    VG_CHECK_EQ(vg_service_init(nullptr), EINVAL);
    f.config.gpios.available_cb = [](vg_config_gpio_pins_t *) -> vg_sta_e { return -1; }; // the list untouched
    VG_REQUIRE(vg_service_init(&f.config) == 0);

    uint8_t payload[4] = { 1, 2, 3, 4 };
    vg_video_frame_t video { .pts = 1, .data = nullptr, .bytes_len = sizeof payload, .key_frame = 1 };
    VG_CHECK_EQ(vg_media_enqueue_video_frame(&video), EINVAL);
    video.data = payload;
    video.bytes_len = 0;
    VG_CHECK_EQ(vg_media_enqueue_video_frame(&video), EINVAL);
    video.bytes_len = sizeof payload;
    VG_CHECK_EQ(vg_media_enqueue_video_frame(&video), 0);
    VG_CHECK_EQ(vg_media_enqueue_video_frame(nullptr), EINVAL);
    vg_audio_frame_t audio { .pts = 1, .data = payload, .bytes_len = 0 };
    VG_CHECK_EQ(vg_media_enqueue_audio_frame(&audio), EINVAL);
    audio.bytes_len = sizeof payload;
    VG_CHECK_EQ(vg_media_enqueue_audio_frame(&audio), 0);

    vg_http_api_response_t response {};
    VG_CHECK_EQ(vg_http_api_rule_info_get(&response), 0);
    VG_REQUIRE(response.free_cb != nullptr && response.body != nullptr);
    VG_CHECK_EQ(vg_http_api_rule_info_get(&response), EINVAL); // still holding a body
    const auto *text = static_cast<const char *>(response.body);
    VG_CHECK(nlohmann::json::parse(text, text + response.body_len).at("dAvailableGPIOs").empty());
    VG_HTTP_API_SAFE_RELEASE_RESPONSE(&response);
    VG_CHECK(response.free_cb == nullptr && response.body == nullptr && response.body_len == size_t { 0 });
    VG_CHECK_EQ(vg_http_api_rule_info_get(&response), 0); // reusable once released
    VG_HTTP_API_SAFE_RELEASE_RESPONSE(&response);
    VG_CHECK_EQ(vg_service_deinit(), 0);
}

VG_TEST_CASE(a_callback_on_the_thread_of_a_c_api_call_can_call_the_c_api_but_not_the_lifecycle)
{
    // The RULE callback of an HTTP activation runs on the activation's thread, inside the C API call. A nested
    // C API call from it borrows the activation's hold on the instance instead of locking it again (undefined
    // before 2026-10-20), so it can read rule info and post a configuration; deinit, which would wait for the
    // exclusive lock against the hold the thread itself keeps, and init, which would lock shared again for its
    // existence check, return EDEADLK instead.
    Fixture f;
    RuleEventRecorder::clear();
    struct HookGuard
    {
        ~HookGuard()
        {
            RuleEventRecorder::hook = nullptr;
        }
    } hook_guard; // the hook captures this frame by reference
    VG_REQUIRE(vg_service_init(&f.config) == 0);
    nlohmann::json http;
    http["sCurrentSelected"] = "HTTP";
    VG_CHECK_EQ(Post(&vg_http_api_rule_record_config_post, http), 0);
    auto record = nlohmann::json::parse(R"({
        "iSchemaVersion": 2, "bRuleEnabled": true, "bContinuousRecordEnabled": false,
        "dActions": { "dPhoto": { "bEnabled": false, "sFormat": "JPG", "iIntervalMs": 5000 },
                      "dVideo": { "bEnabled": false, "iIntervalMs": 5000 }, "dSound": { "bEnabled": false } }
    })");
    VG_CHECK_EQ(Post(&vg_http_api_rule_config_post, record), 0);

    int nested = 0;
    vg_sta_e info_status = -1, config_status = -1, deinit_status = -1, init_status = -1;
    int config_code = -1;
    nlohmann::json info;
    RuleEventRecorder::hook = [&](const vg_rule_evt_t &) {
        ++nested;
        vg_http_api_response_t response {};
        info_status = vg_http_api_rule_info_get(&response);
        if (info_status == 0)
        {
            const auto *text = static_cast<const char *>(response.body);
            info = nlohmann::json::parse(text, text + response.body_len);
        }
        VG_HTTP_API_SAFE_RELEASE_RESPONSE(&response);
        auto changed = record;
        changed["dActions"]["dPhoto"]["iIntervalMs"] = 777;
        std::string body = changed.dump(); // the C API directly: a failing VG_REQUIRE would unwind the callback
        const vg_http_api_request_t request { .free_cb = nullptr, .payload = body.data(), .payload_len = body.size() };
        config_status = vg_http_api_rule_config_post(&request, &response);
        if (config_status == 0)
        {
            const auto *text = static_cast<const char *>(response.body);
            config_code = nlohmann::json::parse(text, text + response.body_len).at("code").get<int>();
        }
        VG_HTTP_API_SAFE_RELEASE_RESPONSE(&response);
        deinit_status = vg_service_deinit();
        init_status = vg_service_init(&f.config);
    };
    VG_CHECK_EQ(Post(&vg_http_api_rule_http_rule_activate_post, nlohmann::json::object()), 0);
    RuleEventRecorder::hook = nullptr;
    VG_CHECK_EQ(nested, 1);
    VG_CHECK_EQ(info_status, 0);
    VG_CHECK_EQ(info.at("dLastDispatchedOwner").at("sRuleType").get<std::string>(), "HTTP"); // the event reported
    VG_CHECK_EQ(config_status, 0);
    VG_CHECK_EQ(config_code, 0);
    VG_CHECK_EQ(deinit_status, EDEADLK);
    VG_CHECK_EQ(init_status, EDEADLK);
    VG_CHECK_EQ(Get(&vg_http_api_rule_config_get).at("dActions").at("dPhoto").at("iIntervalMs").get<int>(), 777);
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 1 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_HTTP);
    VG_CHECK_EQ(vg_service_deinit(), 0); // outside any call, the runtime still exists
}

VG_TEST_MAIN()
