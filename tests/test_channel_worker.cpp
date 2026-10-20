// Characterization tests for the file channel worker over a fake Slot adapter and fake media (docs/PLAN.md
// section 7.2; matrix W1): claim, write, publish outside the media operation and before the hold, complete; the
// failure cooldown and the consecutive failure limit with injected constants; Canceled and Interrupted do not
// hold; a stale task never touches a newer one through the loop; and the thread body with its stop.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "vigil/channel_worker.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/types.hpp"
#include "vigil/writers.hpp"

#include "check.hpp"
#include "fake_config.hpp"
#include "fake_media.hpp"

using namespace vigil;
using namespace vigil::types;
using namespace vigil::constants;
using namespace vigil::workers;
using namespace vigil::writers;
using namespace vigil::tests;

namespace {

using Clock = std::chrono::steady_clock;

constexpr Context::InFlightBudgets::FilesystemId kFakeFilesystem = 42;

// What the worker handed over for publication, and what it observed at that moment.
struct Publication
{
    std::filesystem::path path;
    WriteEvent::Status status;
    bool held;                // a channel still held the task: the publication precedes the hold
    size_t active_operations; // media operations in flight: none, since the writer was released first
    FileSize in_flight;       // the budget still reserved: the task's own bytes, released after publication
};

// The worker's view of a Slot, over FakeMedia writers and the Context's budgets. The flags are atomics because
// the thread case flips them while the worker runs.
struct FakeSlot
{
    FakeMedia &media;
    Context &ctx;
    std::atomic<bool> ready = true;
    std::atomic<FileSize> free_bytes = std::numeric_limits<FileSize>::max() / 2;
    std::atomic<bool> stop = false;
    std::atomic<int> failure_marks = 0;
    std::atomic<size_t> published = 0;
    std::vector<Publication> publications; // read only while the worker is not running

    FakeSlot(FakeMedia &m, Context &c) : media(m), ctx(c) { }

    bool stopRequested() const noexcept
    {
        return stop.load();
    }

    bool isReadyToWrite() const noexcept
    {
        return ready.load();
    }

    Context::InFlightBudgets::Reservation tryAdmit(const WaterMarks &watermarks) noexcept
    {
        return ctx.in_flight_budgets.tryReserve(kFakeFilesystem, watermarks.task_bytes, free_bytes.load(),
            watermarks.critical_remain_bytes);
    }

    std::unique_ptr<FakeAdapter> createWriter(const std::shared_ptr<FileTask> &task)
    {
        if (task->type == CaptureType::Image)
            return std::make_unique<ImageWriter<FakeAdapter>>(std::ref(media), std::ref(ctx), task->settings,
                task->event, task->settings_revision);
        return std::make_unique<VideoWriter<FakeAdapter>>(std::ref(media), std::ref(ctx), task->settings, task->event,
            task->settings_revision);
    }

    void publish(CacheFileInfo &&info) noexcept
    {
        publications.push_back(Publication { info.path, info.event->status.load(), ctx.file_channels.holdsTask(),
            ctx.media_barrier.activeOperations(), ctx.in_flight_budgets.inFlight(kFakeFilesystem) });
        ++published;
    }

    void markFailed() noexcept
    {
        ++failure_marks;
    }

    std::string_view name() const noexcept
    {
        return "fake slot";
    }
};

using Worker = FileChannelWorker<FakeSlot>;
using Step = Worker::Step;

constexpr auto kLong = std::chrono::milliseconds(5000); // an interval no case waits out
constexpr auto kBound = std::chrono::seconds(1);        // what a step without a hold stays well under

struct Fixture
{
    FakeMedia media;
    Context ctx;
    FakeSlot slot;
    int64_t pts = 0;

    // Rules are enabled, as they are whenever a task exists: the fake writers watch their channel's gate.
    Fixture() : ctx(Config(media)), slot(media, ctx)
    {
        media.ctx = &ctx;
        ctx.record_rule_enabled.store(true);
        RuleEventRecorder::clear();
    }

    static vg_config_t Config(FakeMedia &media)
    {
        auto cfg = MakeFakeConfig();
        cfg.media.user_data = &media;
        cfg.media.video_file_create_cb = &FakeMedia::Create;
        cfg.media.video_file_finish_cb = &FakeMedia::Finish;
        cfg.media.video_file_write_video_frame_cb = &FakeMedia::WriteVideo;
        cfg.media.video_file_write_audio_frame_cb = &FakeMedia::WriteAudio;
        cfg.media.image_capture_cb = &FakeMedia::Capture;
        return cfg;
    }

    // Publishes a snapshot with one enabled channel of `type` and `interval`, built directly.
    void enable(CaptureType type, std::chrono::milliseconds interval)
    {
        const bool video = type == CaptureType::Video;
        const auto settings
            = FileWriterSettings::create(type, video ? kRecordWriterDefaultFormat : kRecordPhotoDefaultFormat, interval,
                kRecordWriterPrefixDefault, kVideoClipLengthDefault, &ctx.c_api.media);
        auto &shared = ctx.action_settings;
        VG_REQUIRE(shared.update(shared.getRevision(), ActionSettings::create(false, false, video ? nullptr : settings,
                                                           video ? settings : nullptr, &ctx.c_api.media)));
    }

    Context::FileChannel &channel(CaptureType type)
    {
        return ctx.file_channels.of(type);
    }

    // Dispatches a Timer owner and returns the task the enabled channel accepted.
    std::shared_ptr<FileTask> dispatch()
    {
        VG_REQUIRE(ctx.dispatchRuleEvent(
            std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event)));
        const auto [task, held] = ctx.file_channels.latest();
        VG_REQUIRE(task != nullptr && held);
        return task;
    }

    void pushFrame(bool key_frame)
    {
        uint8_t data[4] = { 1, 2, 3, 4 };
        const vg_video_frame_t frame { .pts = ++pts, .data = data, .bytes_len = sizeof data, .key_frame = key_frame };
        media.queue.emplace_back(frame);
    }

    void pushKeyFrame()
    {
        pushFrame(true);
    }

    FileSize inFlight() const noexcept
    {
        return ctx.in_flight_budgets.inFlight(kFakeFilesystem);
    }

    // Spins until `pred` holds: a bounded wait on the worker thread, not a timing assumption.
    template<typename Pred>
    static bool waitFor(Pred pred)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!pred())
        {
            if (Clock::now() > deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    }
};

} // namespace

VG_TEST_CASE(a_task_is_claimed_written_published_outside_the_operation_and_completed_after_the_hold)
{
    Fixture f;
    f.enable(CaptureType::Image, std::chrono::milliseconds(40));
    auto &photo = f.channel(CaptureType::Image);
    const auto task = f.dispatch();
    Worker worker(f.ctx, f.slot);

    const auto before = Clock::now();
    VG_CHECK(worker.step() == Step::Ran);
    const auto elapsed = Clock::now() - before;
    VG_CHECK_EQ(f.media.captures, 1);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK(photo.current() == nullptr);
    VG_CHECK(photo.last() == task);
    VG_CHECK(elapsed >= std::chrono::milliseconds(40)); // the hold: the channel was held through the interval
    VG_REQUIRE(f.slot.publications.size() == size_t { 1 });
    const auto &publication = f.slot.publications[0];
    VG_CHECK_EQ(publication.path.string(), "/fake/record.file");
    VG_CHECK(publication.status == WriteEvent::Status::Completed);
    VG_CHECK(publication.held);                               // published before the hold ended
    VG_CHECK_EQ(publication.active_operations, size_t { 0 }); // and after the writer released its operation
    VG_CHECK_EQ(f.media.active_at_destruction, size_t { 1 }); // which it held until then
    VG_CHECK_EQ(publication.in_flight, task->settings->watermarks.task_bytes); // its budget, released after
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
    VG_CHECK_EQ(f.slot.failure_marks.load(), 0);
    VG_CHECK(worker.step() == Step::Idle); // nothing pending
}

VG_TEST_CASE(canceled_and_interrupted_tasks_complete_at_once_without_a_hold)
{
    {
        Fixture f; // the gate is closed when the task starts: nothing is recorded, nothing is published
        f.enable(CaptureType::Image, kLong);
        f.media.stop_after_frames = 0;
        const auto task = f.dispatch();
        Worker worker(f.ctx, f.slot);
        const auto before = Clock::now();
        VG_CHECK(worker.step() == Step::Ran);
        VG_CHECK(Clock::now() - before < kBound);
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Canceled);
        VG_CHECK(f.channel(CaptureType::Image).current() == nullptr);
        VG_CHECK_EQ(f.media.captures, 0);
        VG_CHECK(f.slot.publications.empty());
    }
    {
        Fixture f; // interrupted after one frame: the sink is finished and the file is published, with no hold
        f.enable(CaptureType::Video, kLong);
        f.pushKeyFrame();
        f.media.stop_after_frames = 1;
        const auto task = f.dispatch();
        Worker worker(f.ctx, f.slot);
        const auto before = Clock::now();
        VG_CHECK(worker.step() == Step::Ran);
        VG_CHECK(Clock::now() - before < kBound);
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Interrupted);
        VG_CHECK(f.channel(CaptureType::Video).current() == nullptr);
        VG_CHECK_EQ(f.media.video_frames, 1);
        VG_CHECK_EQ(f.media.finishes, 1);
        VG_CHECK_EQ(f.media.thumbnails, 1);
        VG_REQUIRE(f.slot.publications.size() == size_t { 1 });
        VG_CHECK(f.slot.publications[0].status == WriteEvent::Status::Interrupted);
        VG_CHECK(f.slot.publications[0].held);
        VG_CHECK_EQ(f.slot.publications[0].active_operations, size_t { 0 });
    }
}

VG_TEST_CASE(a_failed_task_holds_through_the_cooldown_and_the_limit_stops_the_worker)
{
    Fixture f;
    f.enable(CaptureType::Image, std::chrono::milliseconds(20));
    auto &photo = f.channel(CaptureType::Image);
    Worker worker(f.ctx, f.slot, std::chrono::milliseconds(30), 2); // injected: a 30 ms cooldown, two failures

    f.media.capture_result = EIO;
    auto task = f.dispatch();
    const auto before = Clock::now();
    VG_CHECK(worker.step() == Step::Ran); // one failure: held through the interval and the cooldown
    VG_CHECK(Clock::now() - before >= std::chrono::milliseconds(50));
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
    VG_CHECK(photo.current() == nullptr && photo.last() == task);
    VG_CHECK(f.slot.publications.empty()); // a failed task's file is not published
    VG_CHECK_EQ(f.slot.failure_marks.load(), 0);

    f.media.capture_result = 0; // a success resets the count
    task = f.dispatch();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.slot.publications.size(), size_t { 1 });

    f.media.capture_result = EIO;
    f.dispatch();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK_EQ(f.slot.failure_marks.load(), 0);
    task = f.dispatch();
    VG_CHECK(worker.step() == Step::Stop); // the second consecutive failure reaches the limit
    VG_CHECK_EQ(f.slot.failure_marks.load(), 1);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed); // completed before the stop
    VG_CHECK(photo.current() == nullptr && photo.last() == task);
    VG_CHECK_EQ(f.media.captures, 4);
}

VG_TEST_CASE(a_stop_request_ends_the_hold_after_completing_the_task_and_the_thread_body_retires_stale_work)
{
    Fixture f;
    f.enable(CaptureType::Image, kLong);
    auto &photo = f.channel(CaptureType::Image);
    {
        Worker worker(f.ctx, f.slot);
        const auto task = f.dispatch();
        f.slot.stop = true; // requested before the task starts: it still runs and completes, with no hold
        const auto before = Clock::now();
        VG_CHECK(worker.step() == Step::Stop);
        VG_CHECK(Clock::now() - before < kBound);
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Completed);
        VG_CHECK(photo.current() == nullptr && photo.last() == task);
        VG_CHECK_EQ(f.slot.publications.size(), size_t { 1 });
        f.slot.stop = false;
    }
    // The thread body: a task from before the worker started is stale; later ones are served until the stop.
    const auto stale = f.dispatch();
    f.slot.ready = false;
    Worker worker(f.ctx, f.slot);
    std::thread thread([&] { worker.run(); });
    VG_REQUIRE(Fixture::waitFor([&] { return stale->event->status.load() == WriteEvent::Status::Canceled; }));
    VG_CHECK(photo.current() == nullptr);
    const auto served = f.dispatch();
    f.slot.ready = true;
    VG_REQUIRE(Fixture::waitFor([&] { return f.slot.published.load() == 2; }));
    VG_CHECK(photo.current() == served); // published, and now held through the interval
    f.slot.stop = true;                  // which the stop ends
    thread.join();
    VG_CHECK(served->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK(photo.current() == nullptr && photo.last() == served);
    VG_CHECK_EQ(f.slot.publications.size(), size_t { 2 });
    VG_CHECK_EQ(f.media.captures, 2);
}

VG_TEST_CASE(a_stale_task_never_touches_a_newer_one_through_the_loop)
{
    // While task A is being written it is completed from elsewhere (as a stop would) and B is accepted: the
    // worker's completion of A leaves B untouched (docs/PLAN.md section 7.2).
    Fixture f;
    f.enable(CaptureType::Image, std::chrono::milliseconds(20));
    auto &photo = f.channel(CaptureType::Image);
    const auto a = f.dispatch();
    std::shared_ptr<FileTask> b;
    f.media.on_capture = [&] {
        photo.complete(a, WriteEvent::Status::Interrupted);
        b = f.dispatch();
    };
    Worker worker(f.ctx, f.slot);
    VG_CHECK(worker.step() == Step::Ran);
    VG_REQUIRE(b != nullptr);
    VG_CHECK(photo.current() == b);
    VG_CHECK(b->event->status.load() == WriteEvent::Status::Pending);
    VG_CHECK(photo.last() == a);
    VG_CHECK(a->event->status.load() == WriteEvent::Status::Completed); // what the writer recorded
    VG_CHECK_EQ(f.slot.publications.size(), size_t { 1 });              // and A's file was published
    f.media.on_capture = nullptr;
    VG_CHECK(worker.step() == Step::Ran); // B runs as usual
    VG_CHECK(b->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK(photo.last() == b);
}

VG_TEST_CASE(the_probe_claims_nothing_without_an_enabled_channel_a_ready_slot_headroom_or_media)
{
    Fixture f;
    Worker worker(f.ctx, f.slot);
    VG_CHECK(worker.step() == Step::Idle); // no enabled channel: nothing to serve
    f.enable(CaptureType::Image, kLong);
    const auto task = f.dispatch();
    auto &photo = f.channel(CaptureType::Image);
    f.slot.ready = false;
    VG_CHECK(worker.step() == Step::Idle);
    f.slot.ready = true;
    f.slot.free_bytes = 0; // no headroom: nothing is reserved either
    VG_CHECK(worker.step() == Step::Idle);
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    f.slot.free_bytes = std::numeric_limits<FileSize>::max() / 2;
    f.ctx.media_barrier.pause(false);
    VG_CHECK(worker.step() == Step::Idle);
    VG_CHECK(photo.current() == task); // left pending for the next probe
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Pending);
    VG_CHECK_EQ(f.media.captures, 0);
    f.ctx.media_barrier.resume();
    f.slot.stop = true; // the stop ends the hold at once, so this case does not wait out the interval
    VG_CHECK(worker.step() == Step::Stop);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.media.captures, 1);
}

VG_TEST_CASE(the_budget_is_reserved_at_admission_and_released_on_every_terminal_path)
{
    // The task's own bytes are in flight from admission until its file is published (docs/PLAN.md section 9),
    // whatever the outcome: success, failure, cancellation, and interruption all release them.
    {
        Fixture f; // success: still reserved at publication, released before the hold
        f.enable(CaptureType::Image, std::chrono::milliseconds(20));
        FileSize seen = -1;
        f.media.on_capture = [&] { seen = f.inFlight(); };
        const auto task = f.dispatch();
        Worker worker(f.ctx, f.slot);
        VG_CHECK(worker.step() == Step::Ran);
        VG_CHECK_EQ(seen, task->settings->watermarks.task_bytes);
        VG_REQUIRE(f.slot.publications.size() == size_t { 1 });
        VG_CHECK_EQ(f.slot.publications[0].in_flight, task->settings->watermarks.task_bytes);
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    }
    {
        Fixture f; // failure: nothing is published, the budget is still released before the cooldown
        f.enable(CaptureType::Image, std::chrono::milliseconds(20));
        f.media.capture_result = EIO;
        FileSize seen = -1;
        f.media.on_capture = [&] { seen = f.inFlight(); };
        const auto task = f.dispatch();
        Worker worker(f.ctx, f.slot, std::chrono::milliseconds(10), 10);
        VG_CHECK(worker.step() == Step::Ran);
        VG_CHECK_EQ(seen, task->settings->watermarks.task_bytes);
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
        VG_CHECK(f.slot.publications.empty());
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    }
    {
        Fixture f; // cancellation: the gate was closed before anything was recorded
        f.enable(CaptureType::Image, kLong);
        f.media.stop_after_frames = 0;
        const auto task = f.dispatch();
        Worker worker(f.ctx, f.slot);
        VG_CHECK(worker.step() == Step::Ran);
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Canceled);
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    }
    {
        Fixture f; // interruption of a clip: reserved while frames are written, released after publication
        f.enable(CaptureType::Video, kLong);
        f.pushKeyFrame();
        f.media.stop_after_frames = 1;
        FileSize seen = -1;
        f.media.on_write = [&] { seen = f.inFlight(); };
        const auto task = f.dispatch();
        Worker worker(f.ctx, f.slot);
        VG_CHECK(worker.step() == Step::Ran);
        VG_CHECK_EQ(seen, task->settings->watermarks.task_bytes);
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Interrupted);
        VG_REQUIRE(f.slot.publications.size() == size_t { 1 });
        VG_CHECK_EQ(f.slot.publications[0].in_flight, task->settings->watermarks.task_bytes);
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    }
    {
        Fixture f; // a refused admission reserves nothing and leaves the task pending
        f.enable(CaptureType::Image, kLong);
        const auto task = f.dispatch();
        f.slot.free_bytes = task->settings->watermarks.critical_remain_bytes; // not above the mark
        Worker worker(f.ctx, f.slot);
        VG_CHECK(worker.step() == Step::Idle);
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Pending);
        VG_CHECK_EQ(f.media.captures, 0);
    }
}

VG_TEST_CASE(a_watermark_change_does_not_stop_a_running_clip_but_a_recording_change_does)
{
    // A running task is bound to its channel's recording settings, not to the watermarks (docs/PLAN.md section
    // 9): republishing the same recording with a raised admission mark moves no channel revision, so the clip
    // completes; republishing another interval does, so the clip is interrupted at its next check.
    Fixture f;
    f.enable(CaptureType::Video, std::chrono::milliseconds(20));
    auto &shared = f.ctx.action_settings;
    const auto republish = [&](std::chrono::milliseconds interval, size_t batch) {
        const auto settings = FileWriterSettings::create(CaptureType::Video, kRecordWriterDefaultFormat, interval,
            kRecordWriterPrefixDefault, kVideoClipLengthDefault,
            WaterMarks::from(kRecordWriterDefaultFormat, batch, nullptr));
        VG_REQUIRE(shared.update(shared.getRevision(),
            ActionSettings::create(false, false, nullptr, settings, &f.ctx.c_api.media)));
    };
    Worker worker(f.ctx, f.slot);

    // Three frames and a closing key frame: the clip completes on the fourth. After the second frame the same
    // recording is republished with the watermarks of a longer clip.
    f.pushKeyFrame();
    f.pushFrame(false);
    f.pushFrame(false);
    f.pushKeyFrame();
    f.media.finish_after_frames = 3;
    f.media.on_write = [&] {
        if (f.media.video_frames == 2)
            republish(std::chrono::milliseconds(20), 2 * kVideoClipLengthDefault.count());
    };
    auto task = f.dispatch();
    const auto admission_before = shared.get().settings->watermarks;
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.media.video_frames, 3);
    VG_CHECK_EQ(shared.channelRevision(CaptureType::Video), task->settings_revision); // the gate stayed open
    VG_CHECK(!(shared.get().settings->watermarks == admission_before));               // admission moved
    VG_REQUIRE(f.slot.publications.size() == size_t { 1 });
    VG_CHECK(f.slot.publications[0].status == WriteEvent::Status::Completed);

    // The closing key frame starts the next clip; after its second frame another interval is republished.
    f.pushFrame(false);
    f.pushFrame(false);
    f.pushKeyFrame();
    f.media.finish_after_frames = 6;
    f.media.on_write = [&] {
        if (f.media.video_frames == 5)
            republish(std::chrono::milliseconds(30), kVideoClipLengthDefault.count());
    };
    task = f.dispatch();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Interrupted);
    VG_CHECK_EQ(f.media.video_frames, 5);
    VG_CHECK(shared.channelRevision(CaptureType::Video) != task->settings_revision);
    VG_REQUIRE(f.slot.publications.size() == size_t { 2 });
    VG_CHECK(f.slot.publications[1].status == WriteEvent::Status::Interrupted);
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
}

VG_TEST_MAIN()
