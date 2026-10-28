// Manual captures through the real CaptureEndpoint, writer, and adapter into a temporary directory, with fake media
// callbacks; no Slot, no device. Premise: the temporary filesystem has more than 872 MiB free, a video capture's
// critical mark, since every capture reserves against its own mark at its start. The capture thread is not started:
// each test drives one probe at a time, on a thread of its own where a video capture has to be fed and stopped
// meanwhile.

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/statvfs.h>

#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/endpoints.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/types.hpp"
#include "vigil/utils.hpp"

#include "check.hpp"
#include "fake_config.hpp"
#include "fake_media.hpp"
#include "fake_slot.hpp"

namespace vigil::endpoints {

struct CaptureEndpointTestAccess
{
    // One probe of the capture thread: runs the active capture to its end when there is one and media is not
    // paused, returns at once otherwise.
    static void probeOnce(CaptureEndpoint &endpoint)
    {
        endpoint.captureTaskLoop();
    }
};

} // namespace vigil::endpoints

using namespace vigil;
using namespace vigil::types;
using namespace vigil::constants;
using namespace vigil::endpoints;
using namespace vigil::utils;
using namespace vigil::tests;

namespace {

std::string MakeTempDir()
{
    std::string path = (std::filesystem::temp_directory_path() / "vigil-test-XXXXXX").string();
    if (::mkdtemp(path.data()) == nullptr)
        throw std::runtime_error("mkdtemp failed");
    return path;
}

std::string ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

constexpr int Code(int err)
{
    return kEndpointErrorBase + err;
}

constexpr auto kBound = std::chrono::seconds(5); // what a wait on the capture thread stays well under

// The log callback with a hook on the message: a test reenters the endpoint from inside the capture loop's
// catch, which logs before it completes the event a second time.
struct LogHook
{
    static inline std::function<void(std::string_view)> on_message;

    static vg_sta_e callback(vg_log_level_e level, const char *msg, size_t len)
    {
        TestLogCallback(level, msg, len);
        if (on_message && msg)
            on_message(std::string_view(msg, len));
        return 0;
    }
};

struct Fixture
{
    std::string root; // the temporary directory, removed afterwards; also the "internal mount point" captures go under
    FakeMedia media;
    std::shared_ptr<Context> ctx;
    std::unique_ptr<CaptureEndpoint> endpoint;
    int64_t pts = 0;

    explicit Fixture(bool with_callbacks = true) : root(MakeTempDir())
    {
        auto cfg = MakeFakeConfig();
        cfg.basic.log_cb = &LogHook::callback;
        cfg.media.user_data = &media;
        if (with_callbacks)
        {
            cfg.media.video_file_create_cb = &FakeMedia::Create;
            cfg.media.video_file_finish_cb = &FakeMedia::Finish;
            cfg.media.video_file_write_video_frame_cb = &FakeMedia::WriteVideo;
            cfg.media.video_file_write_audio_frame_cb = &FakeMedia::WriteAudio;
            cfg.media.image_capture_cb = &FakeMedia::Capture;
        }
        cfg.storage.internal_mount_point = root.c_str(); // a capture must go under a mount point
        ctx = std::make_shared<Context>(cfg);
        media.ctx = ctx.get();
        endpoint = std::make_unique<CaptureEndpoint>(ctx, false);
    }

    ~Fixture()
    {
        LogHook::on_message = nullptr;
        endpoint.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    nlohmann::json start(const std::string &output, const char *format, std::optional<int> length = std::nullopt)
    {
        nlohmann::json j { { "sOutput", output }, { "sFormat", format } };
        if (length)
            j["iVideoLengthSeconds"] = *length;
        return endpoint->doCaptureStart(j);
    }

    nlohmann::json stop()
    {
        return endpoint->doCaptureStop(nlohmann::json::object());
    }

    nlohmann::json status()
    {
        return endpoint->getCaptureStatus();
    }

    std::string lastStatus()
    {
        return status().at("dLastCapture").at("sStatus").get<std::string>();
    }

    void probe()
    {
        CaptureEndpointTestAccess::probeOnce(*endpoint);
    }

    // A fake media counter, read under the records lock: the capture may be running on another thread.
    int count(int FakeMedia::*field)
    {
        const std::lock_guard lock(media.records);
        return media.*field;
    }

    // Waits for `done` within the bound; false when it never held.
    static bool WaitFor(const std::function<bool()> &done)
    {
        for (const auto deadline = std::chrono::steady_clock::now() + kBound;;)
        {
            if (done())
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // The host's side of a capture: a file at the path the callback was given.
    static void WriteHostFile(const std::string &path, const char *content = "data")
    {
        std::ofstream out(path, std::ios::trunc);
        out << content;
        if (!out)
            throw std::runtime_error("failed to write " + path);
    }

    // Enqueues a video frame as the C API does.
    void push(bool key_frame)
    {
        uint8_t data[4] = { 1, 2, 3, 4 };
        const vg_video_frame_t frame { .pts = ++pts, .data = data, .bytes_len = sizeof data, .key_frame = key_frame };
        ctx->enqueueMediaFrame(MediaFrame(frame));
    }

    size_t bufferedCaptureFrames()
    {
        const std::lock_guard lock(ctx->capture_media_buffer.mutex);
        return ctx->capture_media_buffer.queue.size();
    }
};

} // namespace

VG_TEST_CASE(a_manual_capture_runs_through_its_cache_name_and_reports_its_lifecycle)
{
    // The host writes the image under the dot-prefixed cache name, and the endpoint renames it once the callback
    // returned. The output directory does not exist yet: the capture creates it.
    Fixture f;
    const auto out = f.root + "/out";
    VG_CHECK(!std::filesystem::exists(out));
    VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
    VG_CHECK(f.status().at("dLastCapture").is_null());

    const auto started = f.start(out, "JPG");
    VG_REQUIRE(started.at("code").get<int>() == 0);
    const auto name = started.at("dCapture").at("sFileName").get<std::string>();
    VG_CHECK(name.starts_with("Capture ") && name.ends_with(".jpg"));
    VG_CHECK_EQ(started.at("dCapture").at("sOutputDirectory").get<std::string>(), out);
    VG_CHECK_EQ(started.at("dCapture").at("sStatus").get<std::string>(), std::string("PENDING"));
    VG_CHECK(!started.at("dCapture").contains("iVideoLengthSeconds"));
    const auto id = started.at("dCapture").at("sID").get<std::string>();
    {
        const auto s = f.status();
        VG_CHECK(!s.at("bReadyToStartNew").get<bool>());
        VG_CHECK(!s.at("bStopRequested").get<bool>());
        VG_CHECK_EQ(s.at("dLastCapture").at("sID").get<std::string>(), id);
        VG_CHECK_EQ(s.at("dLastCapture").at("sStatus").get<std::string>(), std::string("PENDING"));
    }

    std::string asked_path;
    f.media.on_capture = [&] {
        asked_path = f.media.last_path;
        Fixture::WriteHostFile(asked_path);
    };
    f.probe();
    VG_CHECK_EQ(f.media.captures, 1);
    VG_CHECK_EQ(asked_path, out + "/." + name);
    VG_CHECK_EQ(f.media.last_format, std::string("JPG"));
    VG_CHECK_EQ(f.media.active_while_capturing, size_t { 1 }); // the media operation, held through the callback
    VG_CHECK(std::filesystem::is_directory(out));
    VG_CHECK(std::filesystem::is_regular_file(out + "/" + name));
    VG_CHECK(!std::filesystem::exists(asked_path));
    VG_CHECK_EQ(f.ctx->media_barrier.activeOperations(), size_t { 0 });
    VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(*FilesystemIdOf(out)), FileSize { 0 });
    {
        const auto s = f.status();
        VG_CHECK(s.at("bReadyToStartNew").get<bool>());
        VG_CHECK_EQ(s.at("dLastCapture").at("sID").get<std::string>(), id);
        VG_CHECK_EQ(s.at("dLastCapture").at("sStatus").get<std::string>(), std::string("COMPLETED"));
    }

    // An output path with the format's extension names the file itself, and a stale cache file of that name is
    // removed before the host is asked to write.
    Fixture::WriteHostFile(out + "/.pic.jpg", "stale");
    const auto named = f.start(out + "/pic.jpg", "JPG");
    VG_REQUIRE(named.at("code").get<int>() == 0);
    VG_CHECK_EQ(named.at("dCapture").at("sFileName").get<std::string>(), std::string("pic.jpg"));
    VG_CHECK_EQ(named.at("dCapture").at("sOutputDirectory").get<std::string>(), out);
    bool stale_removed = false;
    f.media.on_capture = [&] {
        stale_removed = !std::filesystem::exists(f.media.last_path);
        Fixture::WriteHostFile(f.media.last_path, "fresh");
    };
    f.probe();
    VG_CHECK_EQ(f.media.captures, 2);
    VG_CHECK(stale_removed);
    VG_CHECK_EQ(ReadFile(out + "/pic.jpg"), std::string("fresh"));
    VG_CHECK(!std::filesystem::exists(out + "/.pic.jpg"));
    VG_CHECK_EQ(f.lastStatus(), std::string("COMPLETED"));
}

VG_TEST_CASE(start_and_stop_answer_busy_missing_repeated_and_invalid_requests)
{
    Fixture f;
    const auto out = f.root + "/out";
    VG_CHECK_EQ(f.stop().at("code").get<int>(), Code(ENOENT));                         // nothing to stop
    VG_CHECK_EQ(f.start("relative/out", "JPG").at("code").get<int>(), Code(EINVAL));   // not absolute
    VG_CHECK_EQ(f.start("/elsewhere/out", "JPG").at("code").get<int>(), Code(EINVAL)); // not under a mount point
    VG_CHECK_EQ(f.start(out, "PNG").at("code").get<int>(), Code(EINVAL));
    VG_CHECK_EQ(f.start(out, "MP4", 0).at("code").get<int>(), Code(EINVAL));
    VG_CHECK(f.status().at("dLastCapture").is_null()); // a refused start leaves nothing behind
    VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());

    VG_REQUIRE(f.start(out, "JPG").at("code").get<int>() == 0);
    VG_CHECK_EQ(f.start(out, "JPG").at("code").get<int>(), Code(EBUSY)); // one capture at a time
    VG_CHECK_EQ(f.stop().at("code").get<int>(), 0);
    VG_CHECK(f.status().at("bStopRequested").get<bool>());
    VG_CHECK_EQ(f.stop().at("code").get<int>(), Code(EALREADY));

    // A stop reaches a video capture between its frames; an image capture that has not run yet is still taken,
    // and the request is cleared with the capture.
    f.media.on_capture = [&] { Fixture::WriteHostFile(f.media.last_path); };
    f.probe();
    VG_CHECK_EQ(f.media.captures, 1);
    VG_CHECK_EQ(f.lastStatus(), std::string("COMPLETED"));
    VG_CHECK(!f.status().at("bStopRequested").get<bool>());
    VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
    VG_CHECK_EQ(f.stop().at("code").get<int>(), Code(ENOENT));

    // A video capture stopped before its first key frame records nothing: Canceled at once, no file, and
    // nothing held.
    VG_REQUIRE(f.start(out + "/none.mp4", "MP4").at("code").get<int>() == 0);
    VG_CHECK_EQ(f.stop().at("code").get<int>(), 0);
    f.probe();
    VG_CHECK_EQ(f.lastStatus(), std::string("CANCELED"));
    VG_CHECK(!std::filesystem::exists(out + "/none.mp4") && !std::filesystem::exists(out + "/.none.mp4"));
    VG_CHECK_EQ(f.media.creates, 0);
    VG_CHECK_EQ(f.ctx->media_barrier.activeOperations(), size_t { 0 });
    VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(*FilesystemIdOf(f.root)), FileSize { 0 });
    VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
    VG_CHECK(!f.status().at("bStopRequested").get<bool>());
}

VG_TEST_CASE(a_stop_ends_a_video_capture_on_the_next_key_frame_and_a_pause_interrupts_it)
{
    Fixture f;
    const auto out = f.root + "/out";
    f.media.on_write = [&] { Fixture::WriteHostFile(f.media.last_path); }; // the host's clip, at the cache path

    // Without a length the capture runs until stopped: the stop finishes the clip, which ends on the next key
    // frame, written frames in between included, and leaves that key frame buffered for the next capture.
    const auto started = f.start(out, "MP4");
    VG_REQUIRE(started.at("code").get<int>() == 0);
    VG_CHECK(!started.at("dCapture").contains("iVideoLengthSeconds"));
    const auto name = started.at("dCapture").at("sFileName").get<std::string>();
    VG_CHECK(name.ends_with(".mp4"));
    f.push(true); // the starting key frame, waiting in the buffer
    std::jthread runner([&] { f.probe(); });
    VG_CHECK(Fixture::WaitFor([&] { return f.count(&FakeMedia::video_frames) >= 1; }));
    VG_CHECK_EQ(f.lastStatus(), std::string("WRITING"));
    VG_CHECK(!f.status().at("bReadyToStartNew").get<bool>());
    VG_CHECK_EQ(f.stop().at("code").get<int>(), 0);
    VG_CHECK(f.status().at("bStopRequested").get<bool>());
    f.push(false); // still written: the clip ends on a key frame
    VG_CHECK(Fixture::WaitFor([&] { return f.count(&FakeMedia::video_frames) >= 2; }));
    f.push(true); // the closing key frame
    runner.join();
    VG_CHECK_EQ(f.count(&FakeMedia::video_frames), 2);
    VG_CHECK_EQ(f.count(&FakeMedia::finishes), 1);
    VG_CHECK_EQ(f.media.last_format, std::string("MP4"));
    VG_CHECK(std::filesystem::is_regular_file(out + "/" + name));
    VG_CHECK(!std::filesystem::exists(out + "/." + name));
    VG_CHECK_EQ(f.bufferedCaptureFrames(), size_t { 1 });
    VG_CHECK_EQ(f.lastStatus(), std::string("COMPLETED"));
    VG_CHECK(!f.status().at("bStopRequested").get<bool>());
    VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
    VG_CHECK_EQ(f.ctx->media_barrier.activeOperations(), size_t { 0 });

    // A pause stops a running capture between frames and keeps what was written: Interrupted.
    f.ctx->clearMediaBuffer();
    VG_REQUIRE(f.start(out + "/paused.mp4", "MP4", 60).at("code").get<int>() == 0);
    VG_CHECK_EQ(f.status().at("dLastCapture").at("iVideoLengthSeconds").get<int>(), 60);
    f.push(true);
    std::jthread paused_runner([&] { f.probe(); });
    VG_CHECK(Fixture::WaitFor([&] { return f.count(&FakeMedia::video_frames) >= 3; }));
    f.ctx->media_barrier.pause(false);
    paused_runner.join();
    f.ctx->media_barrier.resume();
    VG_CHECK_EQ(f.count(&FakeMedia::finishes), 2);
    VG_CHECK(std::filesystem::is_regular_file(out + "/paused.mp4"));
    VG_CHECK(!std::filesystem::exists(out + "/.paused.mp4"));
    VG_CHECK_EQ(f.lastStatus(), std::string("INTERRUPTED"));
    VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
    VG_CHECK_EQ(f.ctx->media_barrier.activeOperations(), size_t { 0 });
}

VG_TEST_CASE(a_paused_barrier_keeps_a_capture_pending_and_manual_and_automatic_tasks_share_one_budget)
{
    Fixture f;
    const auto out = f.root + "/out";
    VG_REQUIRE(f.start(out, "JPG").at("code").get<int>() == 0);

    // Nothing runs while media is paused: the capture waits as Pending and runs once media resumes.
    f.ctx->media_barrier.pause(false);
    f.probe();
    VG_CHECK_EQ(f.media.captures, 0);
    VG_CHECK_EQ(f.lastStatus(), std::string("PENDING"));
    VG_CHECK(!f.status().at("bReadyToStartNew").get<bool>());
    f.ctx->media_barrier.resume();

    // The capture's bytes are in flight on the output filesystem while it runs: with headroom for exactly one Photo,
    // an automatic admission there is refused inside the capture callback and granted afterwards.
    const auto filesystem = *FilesystemIdOf(f.root);
    FakeSlotAdmission slot(*f.ctx, filesystem);
    const auto marks = WaterMarks::from("JPG", 1, nullptr); // the estimate of one Photo, the capture's as well
    slot.free_bytes = marks.critical_remain_bytes + marks.task_bytes;
    FileSize seen = -1;
    bool refused_while_capturing = false;
    f.media.on_capture = [&] {
        seen = f.ctx->in_flight_budgets.inFlight(filesystem);
        refused_while_capturing = f.ctx->slot_admission.tryAdmit(marks, marks.task_bytes).registration == 0;
        Fixture::WriteHostFile(f.media.last_path);
    };
    f.probe();
    VG_CHECK_EQ(f.media.captures, 1);
    VG_CHECK_EQ(f.lastStatus(), std::string("COMPLETED"));
    VG_CHECK_EQ(seen, marks.task_bytes);
    VG_CHECK(refused_while_capturing);
    VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(filesystem), FileSize { 0 });
    VG_CHECK(f.ctx->slot_admission.tryAdmit(marks, marks.task_bytes).registration != 0);

    // The other way round: an automatic reservation that leaves the output filesystem no headroom refuses the
    // capture at its start, which fails it before any callback and leaves no file.
    struct statvfs st;
    VG_REQUIRE(::statvfs(f.root.c_str(), &st) == 0);
    const auto free_bytes = static_cast<FileSize>(st.f_bavail) * static_cast<FileSize>(st.f_frsize);
    auto automatic = f.ctx->slot_admission.tryAdmit(marks, free_bytes); // everything free is spoken for
    VG_REQUIRE(automatic.registration != 0);
    VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(filesystem), free_bytes);
    f.media.on_capture = nullptr;
    VG_REQUIRE(f.start(out + "/refused.jpg", "JPG").at("code").get<int>() == 0);
    f.probe();
    VG_CHECK_EQ(f.media.captures, 1);
    VG_CHECK_EQ(f.lastStatus(), std::string("FAILED"));
    VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
    VG_CHECK(!std::filesystem::exists(out + "/refused.jpg") && !std::filesystem::exists(out + "/.refused.jpg"));
    automatic.reservation.release();
    VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(filesystem), FileSize { 0 });
}

VG_TEST_CASE(a_failed_capture_leaves_no_file_and_a_missing_callback_fails_it_at_once)
{
    {
        Fixture f;
        const auto out = f.root + "/out";
        const auto filesystem = *FilesystemIdOf(f.root);

        // The image callback fails after writing part of the file: Failed, and the partial file is removed rather
        // than published under the final name.
        f.media.capture_result = EIO;
        f.media.on_capture = [&] { Fixture::WriteHostFile(f.media.last_path, "partial"); };
        VG_REQUIRE(f.start(out + "/broken.jpg", "JPG").at("code").get<int>() == 0);
        f.probe();
        VG_CHECK_EQ(f.media.captures, 1);
        VG_CHECK_EQ(f.lastStatus(), std::string("FAILED"));
        VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
        VG_CHECK(!std::filesystem::exists(out + "/broken.jpg") && !std::filesystem::exists(out + "/.broken.jpg"));
        VG_CHECK_EQ(f.ctx->media_barrier.activeOperations(), size_t { 0 });
        VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(filesystem), FileSize { 0 });

        // A video write failure finishes the sink exactly once and removes the partial clip the same way.
        f.media.capture_result = 0;
        f.media.write_result = EIO;
        f.media.on_write = [&] { Fixture::WriteHostFile(f.media.last_path, "partial"); };
        VG_REQUIRE(f.start(out + "/broken.mp4", "MP4").at("code").get<int>() == 0);
        f.push(true);
        f.probe(); // the first write fails, so the probe returns on its own
        VG_CHECK_EQ(f.media.video_frames, 1);
        VG_CHECK_EQ(f.media.finishes, 1);
        VG_CHECK_EQ(f.lastStatus(), std::string("FAILED"));
        VG_CHECK(!std::filesystem::exists(out + "/broken.mp4") && !std::filesystem::exists(out + "/.broken.mp4"));
        VG_CHECK_EQ(f.ctx->media_barrier.activeOperations(), size_t { 0 });
        VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(filesystem), FileSize { 0 });
    }
    {
        // Without the host's callbacks the writer cannot be built, and the event is completed twice: by the adapter
        // while the writer's constructor unwinds, then by the loop's catch. A capture started between the two (from
        // the catch's log callback) stays the active one.
        Fixture f(false);
        const auto out = f.root + "/out";
        VG_REQUIRE(f.start(out, "JPG").at("code").get<int>() == 0);
        int started_between = -1;
        LogHook::on_message = [&](std::string_view message) {
            if (message.find("Exception in capture task loop") != std::string_view::npos)
                started_between = f.start(out + "/next.jpg", "JPG").at("code").get<int>();
        };
        f.probe();
        LogHook::on_message = nullptr;
        VG_CHECK_EQ(started_between, 0);
        VG_CHECK_EQ(f.ctx->media_barrier.activeOperations(), size_t { 0 });
        VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(*FilesystemIdOf(f.root)), FileSize { 0 });
        VG_CHECK(std::filesystem::is_empty(out)); // created before the writer was built, nothing written
        const auto s = f.status();
        VG_CHECK(!s.at("bReadyToStartNew").get<bool>());
        VG_CHECK_EQ(s.at("dLastCapture").at("sFileName").get<std::string>(), std::string("next.jpg"));
        VG_CHECK_EQ(s.at("dLastCapture").at("sStatus").get<std::string>(), std::string("PENDING"));
        f.probe(); // the capture started in between runs next and fails the same way, which retires it
        VG_CHECK_EQ(f.lastStatus(), std::string("FAILED"));
        VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
    }
}

VG_TEST_CASE(a_capture_whose_file_cannot_be_put_under_its_name_fails)
{
    // A directory holds the capture's name, so the rename fails, or the callback returned 0 without writing: FAILED
    // either way, nothing is left under the cache name, and what was in the way is kept.
    Fixture f;
    const auto out = f.root + "/out";
    std::filesystem::create_directories(out + "/taken.jpg");
    VG_REQUIRE(f.start(out + "/taken.jpg", "JPG").at("code").get<int>() == 0);
    f.media.on_capture = [&] { Fixture::WriteHostFile(f.media.last_path); };
    f.probe();
    VG_CHECK_EQ(f.media.captures, 1);
    VG_CHECK_EQ(f.lastStatus(), std::string("FAILED"));
    VG_CHECK(f.status().at("bReadyToStartNew").get<bool>());
    VG_CHECK(std::filesystem::is_directory(out + "/taken.jpg") && std::filesystem::is_empty(out + "/taken.jpg"));
    VG_CHECK(!std::filesystem::exists(out + "/.taken.jpg"));
    VG_CHECK_EQ(f.ctx->media_barrier.activeOperations(), size_t { 0 });
    VG_CHECK_EQ(f.ctx->in_flight_budgets.inFlight(*FilesystemIdOf(out)), FileSize { 0 });

    f.media.on_capture = nullptr;
    VG_REQUIRE(f.start(out + "/nothing.jpg", "JPG").at("code").get<int>() == 0);
    f.probe();
    VG_CHECK_EQ(f.media.captures, 2);
    VG_CHECK_EQ(f.lastStatus(), std::string("FAILED"));
    VG_CHECK(!std::filesystem::exists(out + "/nothing.jpg"));
}

VG_TEST_MAIN()
