// Characterization tests for the C API lifecycle (docs/PLAN.md section 8.3): the instance guard, nested
// scoped suspension, and reentry from a callback while the runtime is being built or torn down. The runtime
// is real (rule, storage, and capture threads, plus the selected Slot's manager and channel workers) over the
// fake configuration: the internal "device" is a path that does not exist, every scanned block device is
// hidden, and no media sink is reachable, so nothing here touches a device or needs privileges.

#include <atomic>
#include <cerrno>
#include <cstddef>
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

// Selects the internal "device" as the enabled Slot through the C API: its manager thread and its Photo and
// Video channel workers start and idle, since the device does not exist (docs/PLAN.md section 7.2), and its
// destructor logs and joins them during teardown.
nlohmann::json SelectInternalSlot(const Fixture &f)
{
    nlohmann::json config;
    config["dSelectSlotToEnable"]["sByDevPath"] = f.device;
    config["dSelectSlotToEnable"]["sByUUID"] = nullptr;
    std::string body = config.dump();
    vg_http_api_request_t request { .free_cb = nullptr, .payload = body.data(), .payload_len = body.size() };
    vg_http_api_response_t response {};
    VG_REQUIRE(vg_http_api_storage_config_post(&request, &response) == 0);
    const auto *text = static_cast<const char *>(response.body);
    const auto result = nlohmann::json::parse(text, text + response.body_len);
    response.free_cb(response.body);
    VG_REQUIRE(result.at("code").get<int>() == 0);
    return WaitForInternalSlot(f, true);
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

VG_TEST_MAIN()
