// The file channel worker over a fake Slot adapter and fake media, with injected failure constants.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
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
#include "fake_slot.hpp"

using namespace vigil;
using namespace vigil::types;
using namespace vigil::constants;
using namespace vigil::workers;
using namespace vigil::writers;
using namespace vigil::tests;

namespace {

using Clock = std::chrono::steady_clock;

// What the worker handed over for publication, and what it observed at that moment.
struct Publication
{
    std::shared_ptr<WriteEvent> event; // of the task published
    std::filesystem::path path;
    WriteEvent::Status status;
    bool held;                // a channel still held the task: the publication precedes the hold
    size_t active_operations; // media operations in flight: none, since the writer was released first
    FileSize in_flight;       // the budget still reserved: the task's own bytes, released after publication
};

// The worker's view of a Slot over FakeMedia writers; the flags are atomics because the thread cases flip them while
// the workers run.
struct FakeSlot: FakeSlotAdmission
{
    FakeMedia &media;
    std::atomic<bool> stop = false;
    std::atomic<int> failure_marks = 0;
    std::atomic<size_t> published = 0;
    std::mutex publications_mutex;         // two workers may publish at once
    std::vector<Publication> publications; // read only while no worker is running

    FakeSlot(FakeMedia &m, Context &c) : FakeSlotAdmission(c), media(m) { }

    bool stopRequested() const noexcept
    {
        return stop.load();
    }

    std::optional<VideoWriter<FakeAdapter>::WaitBounds> wait_bounds; // injected into every video writer
    std::atomic<int> prepared = 0;                                   // continuous clips whose backlog was prepared
    std::function<void()> on_prepare;            // runs after the backlog was prepared: what arrives after the claim
    std::atomic<bool> throw_on_create = false;   // a writer that cannot be built
    std::atomic<bool> publish_fails = false;     // the file cannot be put in place
    std::atomic<bool> marked_while_held = false; // the channel still held the task when markFailed ran

    std::unique_ptr<FakeAdapter> createWriter(const std::shared_ptr<FileTask> &task)
    {
        if (throw_on_create)
            throw std::runtime_error("injected: the writer cannot be built");
        if (task->type == CaptureType::Image)
            return std::make_unique<ImageWriter<FakeAdapter>>(std::ref(media), std::ref(ctx), task->settings,
                task->event, task->settings_revision, task->kind);
        auto writer = std::make_unique<VideoWriter<FakeAdapter>>(std::ref(media), std::ref(ctx), task->settings,
            task->event, task->settings_revision, task->kind);
        if (wait_bounds)
            writer->setWaitBounds(*wait_bounds);
        return writer;
    }

    // What the Slot does to its media buffer before a continuous clip.
    void prepareContinuousClip(std::optional<Clock::time_point> boundary) noexcept
    {
        {
            const std::lock_guard lock(media.mutex);
            MediaFrame::KeepFrom(media.queue, boundary);
        }
        ++prepared;
        if (on_prepare)
            on_prepare();
    }

    bool publish(CacheFileInfo &&info) noexcept
    {
        const std::lock_guard lock(publications_mutex);
        publications.push_back(
            Publication { info.event, info.path, info.event->status.load(), ctx.file_channels.holdsTask(),
                ctx.media_barrier.activeOperations(), ctx.in_flight_budgets.inFlight(kFilesystem) });
        ++published;
        return !publish_fails;
    }

    // Counted only: the real Slot also refuses admission and cancels its pending tasks from the mark on, so a Video
    // task claimed after the Photo limit, as a case below does, is the fake's leniency.
    void markFailed() noexcept
    {
        marked_while_held = ctx.file_channels.holdsTask();
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

    std::shared_ptr<FileWriterSettings> settingsOf(CaptureType type, std::chrono::milliseconds interval)
    {
        const bool video = type == CaptureType::Video;
        return FileWriterSettings::create(type, video ? kRecordWriterDefaultFormat : kRecordPhotoDefaultFormat,
            interval, kRecordWriterPrefixDefault, kVideoClipLengthDefault, &ctx.c_api.media);
    }

    // Publishes a snapshot with the given channels, built directly.
    void publish(std::shared_ptr<FileWriterSettings> photo, std::shared_ptr<FileWriterSettings> video)
    {
        auto &shared = ctx.action_settings;
        VG_REQUIRE(shared.update(shared.getRevision(),
            ActionSettings::create(false, false, std::move(photo), std::move(video), &ctx.c_api.media)));
    }

    // Publishes a snapshot with one enabled channel of `type` and `interval`.
    void enable(CaptureType type, std::chrono::milliseconds interval)
    {
        const auto settings = settingsOf(type, interval);
        const bool video = type == CaptureType::Video;
        publish(video ? nullptr : settings, video ? settings : nullptr);
    }

    // Publishes a snapshot with both channels enabled, as a multi-action configuration does.
    void enableBoth(std::chrono::milliseconds photo_interval, std::chrono::milliseconds video_interval)
    {
        publish(settingsOf(CaptureType::Image, photo_interval), settingsOf(CaptureType::Video, video_interval));
    }

    // Publishes a snapshot with continuous recording configured and no other action.
    void enableContinuous()
    {
        auto &shared = ctx.action_settings;
        VG_REQUIRE(shared.update(shared.getRevision(),
            ActionSettings::create(true, false, nullptr, nullptr, &ctx.c_api.media)));
    }

    // Admits a continuous clip and returns it.
    std::shared_ptr<FileTask> dispatchContinuous()
    {
        VG_REQUIRE(ctx.dispatchContinuous());
        const auto task = ctx.file_channels.video.current();
        VG_REQUIRE(task != nullptr);
        return task;
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
        const std::lock_guard lock(media.mutex); // a video writer on another thread reads the queue
        media.queue.emplace_back(frame);
    }

    void pushKeyFrame()
    {
        pushFrame(true);
    }

    FileSize inFlight() const noexcept
    {
        return ctx.in_flight_budgets.inFlight(FakeSlotAdmission::kFilesystem);
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

    // A worker running its thread body on its own thread. Destruction requests the stop and joins, so a failed
    // REQUIRE unwinds instead of terminating.
    struct Running
    {
        FakeSlot &slot;
        std::thread thread;

        Running(FakeSlot &s, Worker &worker) : slot(s), thread([&worker] { worker.run(); }) { }

        ~Running()
        {
            slot.stop = true;
            if (thread.joinable())
                thread.join();
        }

        void join()
        {
            thread.join();
        }
    };
};

} // namespace

VG_TEST_CASE(a_task_is_claimed_written_published_outside_the_operation_and_completed_after_the_hold)
{
    Fixture f;
    f.enable(CaptureType::Image, std::chrono::milliseconds(40));
    auto &photo = f.channel(CaptureType::Image);
    const auto task = f.dispatch();
    Worker worker(f.ctx, f.slot, CaptureType::Image);

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
        Worker worker(f.ctx, f.slot, CaptureType::Image);
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
        Worker worker(f.ctx, f.slot, CaptureType::Video);
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
    // Injected: a 30 ms cooldown, two failures.
    Worker worker(f.ctx, f.slot, CaptureType::Image, std::chrono::milliseconds(30), 2);

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
    VG_CHECK(f.slot.marked_while_held); // marked before the task was completed: the Slot refuses admission first
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed); // completed before the stop
    VG_CHECK(photo.current() == nullptr && photo.last() == task);
    VG_CHECK_EQ(f.media.captures, 4);
}

VG_TEST_CASE(a_file_that_could_not_be_published_fails_its_task_from_the_publication_on)
{
    // The writer completed and the Slot could not put the file in place: the task is Failed from then on, through the
    // hold; it is held through the interval and the cooldown, and counts toward the limit like any failed task.
    Fixture f;
    f.enable(CaptureType::Image, std::chrono::milliseconds(200));
    auto &photo = f.channel(CaptureType::Image);
    Worker worker(f.ctx, f.slot, CaptureType::Image, std::chrono::milliseconds(30), 2); // injected: 30 ms, 2

    f.slot.publish_fails = true;
    auto task = f.dispatch();
    const auto before = Clock::now();
    auto step = Step::Idle;
    std::thread stepping([&] { step = worker.step(); });
    VG_CHECK(Fixture::waitFor([&] { return task->event->status.load() == WriteEvent::Status::Failed; }));
    VG_CHECK(photo.current() == task); // still held
    stepping.join();
    VG_CHECK(step == Step::Ran);
    VG_CHECK(Clock::now() - before >= std::chrono::milliseconds(230)); // the interval and the cooldown
    VG_CHECK(photo.current() == nullptr && photo.last() == task);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
    VG_REQUIRE(f.slot.publications.size() == size_t { 1 });
    VG_CHECK(f.slot.publications[0].status == WriteEvent::Status::Completed); // as the writer ended it
    VG_CHECK_EQ(f.slot.failure_marks.load(), 0);
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });

    task = f.dispatch();
    VG_CHECK(worker.step() == Step::Stop); // the second consecutive failure reaches the limit
    VG_CHECK_EQ(f.slot.failure_marks.load(), 1);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
    VG_CHECK_EQ(f.media.captures, 2);
}

VG_TEST_CASE(an_interrupted_clip_whose_file_could_not_be_published_fails_too)
{
    // Interrupted after its first frame, which would retire it at once, then lost at publication: held as failed.
    Fixture f;
    f.enable(CaptureType::Video, std::chrono::milliseconds(50));
    Worker worker(f.ctx, f.slot, CaptureType::Video, std::chrono::milliseconds(30), 10); // injected: 30 ms, 10
    f.pushKeyFrame();
    f.media.stop_after_frames = 1;
    f.slot.publish_fails = true;
    const auto task = f.dispatch();
    const auto before = Clock::now();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(Clock::now() - before >= std::chrono::milliseconds(80)); // the interval and the cooldown
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
    VG_REQUIRE(f.slot.publications.size() == size_t { 1 });
    VG_CHECK(f.slot.publications[0].status == WriteEvent::Status::Interrupted);
}

VG_TEST_CASE(a_continuous_clip_whose_file_could_not_be_published_leaves_no_boundary)
{
    // The lost clip completed on a key frame it left queued, but the next clip is an entry and drops it.
    Fixture f;
    f.slot.wait_bounds
        = VideoWriter<FakeAdapter>::WaitBounds { std::chrono::milliseconds(100), std::chrono::seconds(10) };
    Worker worker(f.ctx, f.slot, CaptureType::Video, std::chrono::milliseconds(30), 10); // injected: 30 ms, 10
    f.enableContinuous();
    f.slot.on_prepare = [&] {
        f.pushKeyFrame();
        f.pushFrame(false);
        f.pushKeyFrame();
    };
    f.media.finish_after_frames = 2;
    f.slot.publish_fails = true;
    const auto lost = f.dispatchContinuous();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(lost->event->status.load() == WriteEvent::Status::Failed);
    VG_CHECK_EQ(f.media.queue.size(), size_t { 1 }); // its closing key frame

    f.slot.publish_fails = false;
    size_t backlog = SIZE_MAX;
    f.slot.on_prepare = [&] {
        backlog = f.media.queue.size();
        f.pushKeyFrame();
    };
    f.media.finish_after_frames = 3;
    f.dispatchContinuous();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK_EQ(backlog, size_t { 0 });
}

VG_TEST_CASE(a_stop_request_ends_the_hold_after_completing_the_task_and_closing_admission_retires_pending_work)
{
    Fixture f;
    f.enable(CaptureType::Image, kLong);
    auto &photo = f.channel(CaptureType::Image);
    {
        Worker worker(f.ctx, f.slot, CaptureType::Image);
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
    // The thread body: a task accepted before the worker started is its own Slot's and waits for the Slot to be
    // ready; closing the Slot's admission retires it, and the reopened Slot's tasks are served until the stop.
    const auto early = f.dispatch();
    f.slot.ready = false;
    Worker worker(f.ctx, f.slot, CaptureType::Image);
    Fixture::Running running(f.slot, worker);
    VG_CHECK(photo.current() == early); // not retired by the worker's start
    f.slot.close();
    VG_CHECK(early->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(photo.current() == nullptr);
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    f.slot.open();
    f.slot.ready = true;
    const auto served = f.dispatch();
    VG_REQUIRE(Fixture::waitFor([&] { return f.slot.published.load() == 2; }));
    VG_CHECK(photo.current() == served); // published, and now held through the interval
    f.slot.stop = true;                  // which the stop ends
    running.join();
    VG_CHECK(served->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK(photo.current() == nullptr && photo.last() == served);
    VG_CHECK_EQ(f.slot.publications.size(), size_t { 2 });
    VG_CHECK_EQ(f.media.captures, 2);
}

VG_TEST_CASE(a_stale_task_never_touches_a_newer_one_through_the_loop)
{
    // While task A is being written it is completed from elsewhere (as a stop would) and B is accepted.
    Fixture f;
    f.enable(CaptureType::Image, std::chrono::milliseconds(20));
    auto &photo = f.channel(CaptureType::Image);
    const auto a = f.dispatch();
    std::shared_ptr<FileTask> b;
    f.media.on_capture = [&] {
        photo.complete(a, WriteEvent::Status::Interrupted);
        b = f.dispatch();
    };
    Worker worker(f.ctx, f.slot, CaptureType::Image);
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

VG_TEST_CASE(admission_refuses_without_headroom_and_the_probe_claims_nothing_without_a_ready_slot_or_media)
{
    Fixture f;
    Worker worker(f.ctx, f.slot, CaptureType::Image);
    VG_CHECK(worker.step() == Step::Idle); // nothing pending
    f.enable(CaptureType::Image, kLong);
    auto &photo = f.channel(CaptureType::Image);
    f.slot.free_bytes = 0; // no headroom: the dispatch is refused, and nothing is reserved or pending
    VG_CHECK(!f.ctx.dispatchRuleEvent(
        std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event)));
    VG_CHECK(photo.current() == nullptr);
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    VG_CHECK(worker.step() == Step::Idle);
    f.slot.free_bytes = std::numeric_limits<FileSize>::max() / 2;
    const auto task = f.dispatch(); // admitted: its bytes are in flight from here
    VG_CHECK_EQ(f.inFlight(), task->settings->watermarks.task_bytes);
    f.slot.ready = false;
    VG_CHECK(worker.step() == Step::Idle);
    f.slot.ready = true;
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
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
}

VG_TEST_CASE(the_budget_is_reserved_at_acceptance_and_released_on_every_terminal_path)
{
    // Success, failure, cancellation, interruption, and retirement before a claim all release the task's bytes.
    {
        Fixture f; // success: still reserved at publication, released before the hold
        f.enable(CaptureType::Image, std::chrono::milliseconds(20));
        FileSize seen = -1;
        f.media.on_capture = [&] { seen = f.inFlight(); };
        const auto task = f.dispatch();
        Worker worker(f.ctx, f.slot, CaptureType::Image);
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
        Worker worker(f.ctx, f.slot, CaptureType::Image, std::chrono::milliseconds(10), 10);
        VG_CHECK(worker.step() == Step::Ran);
        VG_CHECK_EQ(seen, task->settings->watermarks.task_bytes);
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
        VG_CHECK(f.slot.publications.empty());
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    }
    {
        Fixture f; // a writer that cannot be built: released at once, not with the worker's frame after the hold
        f.enable(CaptureType::Image, kLong);
        f.slot.throw_on_create = true;
        const auto task = f.dispatch();
        auto &photo = f.channel(CaptureType::Image);
        Worker worker(f.ctx, f.slot, CaptureType::Image);
        Fixture::Running running(f.slot, worker);
        VG_REQUIRE(Fixture::waitFor([&] { return f.inFlight() == 0; }));
        VG_CHECK(photo.current() == task); // still held: the interval and the cooldown have not elapsed
        f.slot.stop = true;                // which ends the hold
        running.join();
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
        VG_CHECK(photo.current() == nullptr && photo.last() == task);
        VG_CHECK(f.slot.publications.empty());
        VG_CHECK_EQ(f.media.captures, 0);
    }
    {
        Fixture f; // cancellation: the gate was closed before anything was recorded
        f.enable(CaptureType::Image, kLong);
        f.media.stop_after_frames = 0;
        const auto task = f.dispatch();
        Worker worker(f.ctx, f.slot, CaptureType::Image);
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
        Worker worker(f.ctx, f.slot, CaptureType::Video);
        VG_CHECK(worker.step() == Step::Ran);
        VG_CHECK_EQ(seen, task->settings->watermarks.task_bytes);
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Interrupted);
        VG_REQUIRE(f.slot.publications.size() == size_t { 1 });
        VG_CHECK_EQ(f.slot.publications[0].in_flight, task->settings->watermarks.task_bytes);
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    }
    {
        Fixture f; // retired before a claim: the reservation goes with the task
        f.enable(CaptureType::Image, kLong);
        const auto task = f.dispatch();
        VG_CHECK_EQ(f.inFlight(), task->settings->watermarks.task_bytes);
        f.channel(CaptureType::Image).cancelPending();
        VG_CHECK(task->event->status.load() == WriteEvent::Status::Canceled);
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    }
    {
        Fixture f; // a refused admission accepts nothing and reserves nothing
        f.enable(CaptureType::Image, kLong);
        f.slot.free_bytes = f.ctx.action_settings.get().settings->watermarks.critical_remain_bytes; // not above
        VG_CHECK(!f.ctx.dispatchRuleEvent(
            std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event)));
        VG_CHECK(f.channel(CaptureType::Image).current() == nullptr);
        VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
        VG_CHECK_EQ(f.media.captures, 0);
    }
}

VG_TEST_CASE(a_watermark_change_does_not_stop_a_running_clip_but_a_recording_change_does)
{
    // Republishing the same recording with a raised admission mark moves no channel revision, so the clip completes;
    // republishing another interval does, so the clip is interrupted at its next check.
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
    Worker worker(f.ctx, f.slot, CaptureType::Video);

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

VG_TEST_CASE(each_worker_serves_only_its_own_channel_with_its_own_budget_and_failure_count)
{
    // Each task reserves its own bytes against the aggregate mark. The Video worker steps inside the Photo capture
    // callback, so the two reservations overlap deterministically.
    Fixture f;
    f.enableBoth(std::chrono::milliseconds(20), std::chrono::milliseconds(20));
    auto &photo = f.channel(CaptureType::Image);
    auto &video = f.channel(CaptureType::Video);
    // Injected: a 10 ms cooldown, one failure, for each worker.
    Worker photo_worker(f.ctx, f.slot, CaptureType::Image, std::chrono::milliseconds(10), 1);
    Worker video_worker(f.ctx, f.slot, CaptureType::Video, std::chrono::milliseconds(10), 1);

    const auto snapshot = f.ctx.action_settings.get().settings;
    const auto photo_bytes = snapshot->photo->watermarks.task_bytes;
    const auto video_bytes = snapshot->video->watermarks.task_bytes;
    VG_CHECK_EQ(snapshot->watermarks.task_bytes, photo_bytes + video_bytes); // what both are admitted against

    // Only a Video task is pending: the Photo worker claims nothing and leaves it alone.
    f.dispatch();
    photo.cancelPending(); // its reservation goes with it
    const auto lone = video.current();
    VG_REQUIRE(lone != nullptr);
    VG_CHECK(photo_worker.step() == Step::Idle);
    VG_CHECK(video.current() == lone && lone->event->status.load() == WriteEvent::Status::Pending);
    VG_CHECK_EQ(f.inFlight(), video_bytes);
    f.pushKeyFrame(); // each clip here writes one key frame and ends on the next, which stays queued
    f.pushKeyFrame();
    f.media.finish_after_frames = 1;
    VG_CHECK(video_worker.step() == Step::Ran);
    VG_CHECK(lone->event->status.load() == WriteEvent::Status::Completed);

    // Both channels accept, each task with its own bytes reserved; the Video worker steps inside the Photo
    // capture, so both reservations are in flight at the Video publication, and the Photo's own alone at its own.
    f.dispatch();
    const auto photo_task = photo.current(), video_task = video.current();
    VG_REQUIRE(photo_task != nullptr && video_task != nullptr);
    VG_CHECK_EQ(f.inFlight(), photo_bytes + video_bytes);
    f.pushKeyFrame();
    f.pushKeyFrame();
    f.media.finish_after_frames = 2;
    f.media.on_capture = [&] {
        VG_CHECK_EQ(f.inFlight(), photo_bytes + video_bytes); // the Video task is still pending
        VG_CHECK(video_worker.step() == Step::Ran);
        VG_CHECK(video_task->event->status.load() == WriteEvent::Status::Completed);
        VG_CHECK_EQ(f.inFlight(), photo_bytes); // the Video's were released with its publication
    };
    VG_CHECK(photo_worker.step() == Step::Ran);
    f.media.on_capture = nullptr;
    VG_CHECK(photo_task->event->status.load() == WriteEvent::Status::Completed);
    VG_REQUIRE(f.slot.publications.size() == size_t { 3 });
    VG_CHECK(f.slot.publications[1].event == video_task->event);
    VG_CHECK_EQ(f.slot.publications[1].in_flight, photo_bytes + video_bytes);
    VG_CHECK(f.slot.publications[2].event == photo_task->event);
    VG_CHECK_EQ(f.slot.publications[2].in_flight, photo_bytes);
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    VG_CHECK_EQ(f.slot.failure_marks.load(), 0);
    // Two FILE publications for one RULE owner, each carrying its UID.
    VG_CHECK(photo_task->owner == video_task->owner);
    VG_CHECK_EQ(f.slot.publications[1].event->owner_uid, video_task->owner->uid);
    VG_CHECK_EQ(f.slot.publications[2].event->owner_uid, video_task->owner->uid);

    // A failing capture reaches the Photo worker's limit; the Video worker's count is its own, so it goes on.
    f.media.capture_result = EIO;
    f.dispatch();
    VG_CHECK(photo_worker.step() == Step::Stop);
    VG_CHECK_EQ(f.slot.failure_marks.load(), 1);
    VG_CHECK(photo.current() == nullptr && photo.last()->event->status.load() == WriteEvent::Status::Failed);
    const auto next_video = video.current();
    VG_REQUIRE(next_video != nullptr);
    f.pushKeyFrame();
    f.pushKeyFrame();
    f.media.finish_after_frames = 3;
    VG_CHECK(video_worker.step() == Step::Ran);
    VG_CHECK(next_video->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.slot.failure_marks.load(), 1);
    VG_CHECK_EQ(f.media.captures, 2);
    VG_CHECK_EQ(f.media.finishes, 3);
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
}

VG_TEST_CASE(closing_admission_retires_unclaimed_tasks_and_the_thread_bodies_serve_both_channels_at_once)
{
    // A worker's start retires nothing. Then the Photo worker completes a capture while the Video worker still waits
    // for the key frame that closes its clip, and the stop ends both holds.
    Fixture f;
    f.enableBoth(kLong, kLong);
    auto &photo = f.channel(CaptureType::Image);
    auto &video = f.channel(CaptureType::Video);
    f.media.finish_after_frames = 3; // the clip is finished after three frames and ends on the next key frame
    f.dispatch();
    const auto unclaimed_photo = photo.current(), unclaimed_video = video.current();
    VG_REQUIRE(unclaimed_photo != nullptr && unclaimed_video != nullptr);
    f.slot.ready = false; // the workers idle on their probes
    Worker photo_worker(f.ctx, f.slot, CaptureType::Image);
    Worker video_worker(f.ctx, f.slot, CaptureType::Video);
    Fixture::Running photo_thread(f.slot, photo_worker);
    Fixture::Running video_thread(f.slot, video_worker);
    VG_CHECK(photo.current() == unclaimed_photo && video.current() == unclaimed_video);
    f.slot.close();
    VG_CHECK(unclaimed_photo->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(unclaimed_video->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(!f.ctx.file_channels.holdsTask());
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    f.slot.open();
    f.slot.ready = true;

    // Both channels accept again. The clip has its starting key frame and two frames but no closing one, so the
    // Photo publishes while the Video is still writing.
    f.pushKeyFrame();
    f.pushFrame(false);
    f.pushFrame(false);
    f.dispatch();
    const auto photo_task = photo.current(), video_task = video.current();
    VG_REQUIRE(photo_task != nullptr && video_task != nullptr);
    VG_REQUIRE(Fixture::waitFor([&] {
        return f.slot.published.load() == 1 && video_task->event->status.load() == WriteEvent::Status::Writing;
    }));
    VG_CHECK(photo_task->event->status.load() == WriteEvent::Status::Completed); // and held through its interval
    f.pushKeyFrame();                                                            // closes the clip
    VG_REQUIRE(Fixture::waitFor([&] { return f.slot.published.load() == 2; }));
    f.slot.stop = true; // ends both holds
    photo_thread.join();
    video_thread.join();
    VG_CHECK(video_task->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK(photo.current() == nullptr && photo.last() == photo_task);
    VG_CHECK(video.current() == nullptr && video.last() == video_task);
    VG_REQUIRE(f.slot.publications.size() == size_t { 2 });
    VG_CHECK(f.slot.publications[0].event == photo_task->event);
    VG_CHECK(f.slot.publications[1].event == video_task->event);
    VG_CHECK_EQ(f.media.captures, 1);
    VG_CHECK_EQ(f.media.video_frames, 3);
    VG_CHECK_EQ(f.media.finishes, 1);
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
}

VG_TEST_CASE(a_continuous_clip_runs_without_an_owner_or_a_hold_and_an_entry_drops_the_backlog)
{
    // An adjacent clip keeps the boundary key frame the previous clip left; after a clip that ended without one, the
    // next clip is an entry again. A triggered clip keeps its pre-roll.
    Fixture f;
    f.slot.wait_bounds
        = VideoWriter<FakeAdapter>::WaitBounds { std::chrono::milliseconds(100), std::chrono::seconds(10) };
    auto &video = f.channel(CaptureType::Video);
    Worker worker(f.ctx, f.slot, CaptureType::Video);

    // A triggered clip first: its pre-roll is written (the backlog is not prepared) and its closing key frame
    // stays queued.
    f.enable(CaptureType::Video, std::chrono::milliseconds(20));
    f.pushKeyFrame();
    f.pushFrame(false);
    f.pushKeyFrame();
    f.media.finish_after_frames = 2;
    const auto triggered = f.dispatch();
    VG_CHECK_EQ(RuleEventRecorder::take().size(), size_t { 1 });
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(triggered->kind == FileTask::Kind::Triggered);
    VG_CHECK(triggered->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.media.video_frames, 2);
    VG_CHECK_EQ(f.slot.prepared.load(), 0);
    VG_CHECK_EQ(f.media.queue.size(), size_t { 1 });

    // Continuous recording configured and rules disabled: the entry drops the backlog (the triggered clip's
    // closing key frame and a frame after it) and records from the first key frame enqueued after the claim.
    // The clip is finished after two frames and ends on the next key frame, which stays queued.
    f.enableContinuous();
    f.ctx.record_rule_enabled.store(false);
    f.pushFrame(false);
    f.slot.on_prepare = [&] {
        f.pushKeyFrame();
        f.pushFrame(false);
        f.pushKeyFrame();
    };
    f.media.finish_after_frames = 4;
    const auto first = f.dispatchContinuous();
    VG_CHECK(first->kind == FileTask::Kind::Continuous);
    VG_CHECK(first->owner == nullptr);
    VG_CHECK_EQ(first->event->owner_uid, uint64_t { 0 });
    VG_CHECK_EQ(first->settings->interval.count(), 0L);
    VG_CHECK(RuleEventRecorder::take().empty()); // no rule event
    auto before = Clock::now();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(Clock::now() - before < kBound); // no hold
    VG_CHECK(first->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK(video.current() == nullptr); // completed at once
    VG_CHECK(video.last() == first);
    VG_CHECK_EQ(f.slot.prepared.load(), 1);
    VG_CHECK_EQ(f.media.video_frames, 4); // the dropped frames were not written
    VG_CHECK_EQ(f.media.creates, 2);
    VG_CHECK_EQ(f.media.queue.size(), size_t { 1 }); // the boundary key frame
    VG_REQUIRE(f.slot.publications.size() == size_t { 2 });
    VG_CHECK(f.slot.publications[1].status == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.slot.publications[1].event->owner_uid, uint64_t { 0 });
    VG_CHECK(f.slot.publications[1].held); // published before the completion, as any task
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });

    // The adjacent clip keeps the boundary key frame and the frames after it, and ends at the key frame wait
    // without a closing key frame.
    f.slot.on_prepare = nullptr;
    f.pushFrame(false);
    f.pushFrame(false);
    f.media.finish_after_frames = 7;
    const auto second = f.dispatchContinuous();
    before = Clock::now();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(Clock::now() - before >= std::chrono::milliseconds(100)); // the closing key frame wait
    VG_CHECK(second->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.slot.prepared.load(), 2);
    VG_CHECK_EQ(f.media.video_frames, 7); // the boundary key frame and both frames
    VG_CHECK(f.media.queue.empty());

    // After a clip that ended without a boundary key frame the next one is an entry again: what was queued
    // before the claim is dropped.
    f.pushKeyFrame();
    f.pushFrame(false);
    f.slot.on_prepare = [&] { f.pushKeyFrame(); };
    f.media.finish_after_frames = 8;
    const auto third = f.dispatchContinuous();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(third->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.slot.prepared.load(), 3);
    VG_CHECK_EQ(f.media.video_frames, 8);
    VG_CHECK_EQ(f.media.creates, 4);
    VG_CHECK(f.media.queue.empty());
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    VG_CHECK_EQ(f.slot.failure_marks.load(), 0);
    VG_CHECK(worker.step() == Step::Idle); // nothing admitted: the rule loop admits the next clip
}

VG_TEST_CASE(a_continuous_clip_ignores_the_rule_flag_and_is_finished_by_the_schedule_or_a_mode_switch)
{
    // A failed clip holds through the cooldown and counts toward the limit like any task.
    Fixture f;
    f.slot.wait_bounds
        = VideoWriter<FakeAdapter>::WaitBounds { std::chrono::milliseconds(100), std::chrono::seconds(10) };
    auto &video = f.channel(CaptureType::Video);
    f.enableContinuous();
    Worker worker(f.ctx, f.slot, CaptureType::Video, std::chrono::milliseconds(40), 2); // injected: 40 ms, 2
    // Every clip here is an entry (none completes on a key frame), so its frames arrive after the claim.
    f.slot.on_prepare = [&] {
        f.pushKeyFrame();
        f.pushFrame(false);
        f.pushFrame(false);
    };

    // Rules disabled after the first frame: the clip writes its three frames and completes.
    f.media.finish_after_frames = 3;
    f.media.on_write = [&] {
        if (f.media.video_frames == 1)
            f.ctx.record_rule_enabled.store(false);
    };
    auto task = f.dispatchContinuous();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(f.media.video_frames, 3);
    VG_CHECK(!f.ctx.record_rule_enabled.load());

    // The schedule closes after the first frame of the next clip: Interrupted at the next check, published
    // with UID 0, and no clip is admitted while closed.
    f.media.finish_after_frames = SIZE_MAX;
    f.media.on_write = [&] {
        if (f.media.video_frames == 4)
            f.ctx.schedule_open.store(false);
    };
    task = f.dispatchContinuous();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Interrupted);
    VG_CHECK_EQ(f.media.video_frames, 4);
    VG_REQUIRE(f.slot.publications.size() == size_t { 2 });
    VG_CHECK(f.slot.publications[1].status == WriteEvent::Status::Interrupted);
    VG_CHECK_EQ(f.slot.publications[1].event->owner_uid, uint64_t { 0 });
    VG_CHECK(!f.ctx.dispatchContinuous());
    f.ctx.schedule_open.store(true);

    // A mode switch while a clip runs (triggered Video published, continuous off): the Video revision moves,
    // the clip is interrupted at the next check, and no continuous clip is admitted afterwards.
    f.media.on_write = [&] {
        if (f.media.video_frames == 5)
            f.enable(CaptureType::Video, std::chrono::milliseconds(20));
    };
    task = f.dispatchContinuous();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Interrupted);
    VG_CHECK_EQ(f.media.video_frames, 5);
    VG_CHECK(f.ctx.action_settings.channelRevision(CaptureType::Video) != task->settings_revision);
    VG_CHECK(!f.ctx.dispatchContinuous());
    VG_CHECK(video.current() == nullptr);
    VG_REQUIRE(f.slot.publications.size() == size_t { 3 });
    VG_CHECK(f.slot.publications[2].status == WriteEvent::Status::Interrupted);

    // A failed clip (the sink cannot be created) holds through the cooldown; the second failure reaches the
    // limit and stops the worker.
    f.enableContinuous();
    f.media.on_write = nullptr;
    f.media.create_result = EIO;
    task = f.dispatchContinuous();
    const auto before = Clock::now();
    VG_CHECK(worker.step() == Step::Ran);
    VG_CHECK(Clock::now() - before >= std::chrono::milliseconds(40));
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
    VG_CHECK(video.current() == nullptr);
    task = f.dispatchContinuous();
    VG_CHECK(worker.step() == Step::Stop);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed);
    VG_CHECK_EQ(f.slot.failure_marks.load(), 1);
    VG_CHECK_EQ(f.slot.publications.size(), size_t { 3 });
    VG_CHECK_EQ(f.inFlight(), FileSize { 0 });
    VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
}

VG_TEST_CASE(a_closure_a_suspension_or_a_switch_between_two_clips_starts_the_second_one_fresh)
{
    // Each pair starts from an empty buffer: its first clip completes on the last key frame its own frames brought,
    // one frame arrives before the second clip's preparation, and the second clip's own frames arrive after it. The
    // second clip continues from the first one's key frame only when nothing broke continuity since the first clip
    // was claimed: not the schedule closing, media being suspended, or continuous recording switched off and on,
    // whether between the clips, between the second clip's admission and its claim, or unseen by the first clip.
    Fixture f;
    f.slot.wait_bounds
        = VideoWriter<FakeAdapter>::WaitBounds { std::chrono::milliseconds(100), std::chrono::seconds(10) };
    Worker worker(f.ctx, f.slot, CaptureType::Video);
    f.enableContinuous();
    f.ctx.record_rule_enabled.store(false);
    f.slot.on_prepare = [&] {
        f.pushKeyFrame();
        f.pushFrame(false);
        f.pushKeyFrame();
    };
    // Runs a clip finished after two frames, with `after_admission` between its admission and its claim and
    // `on_first_write` inside its first write; returns the pts of the frames it wrote.
    const auto clip = [&](const std::function<void()> &after_admission, const std::function<void()> &on_first_write) {
        f.media.video_pts.clear();
        f.media.finish_after_frames = f.media.framesWritten() + 2;
        bool written = false;
        f.media.on_write = [&] {
            if (!std::exchange(written, true) && on_first_write)
                on_first_write();
        };
        const auto task = f.dispatchContinuous();
        if (after_admission)
            after_admission();
        const auto step = worker.step();
        f.media.on_write = nullptr;
        VG_REQUIRE(step == Step::Ran);
        VG_REQUIRE(task->event->status.load() == WriteEvent::Status::Completed);
        return f.media.video_pts;
    };
    struct Hooks
    {
        std::function<void()> during_first = nullptr;    // inside the first clip's first write, which runs on
        std::function<void()> between = nullptr;         // after the first clip, before the second is admitted
        std::function<void()> after_admission = nullptr; // between the second clip's admission and its claim
    };
    // The pts of the key frame the first clip ended on, and of the frames the second clip wrote.
    const auto pair = [&](const Hooks &hooks) {
        {
            const std::lock_guard lock(f.media.mutex);
            f.media.queue.clear();
        }
        clip(nullptr, hooks.during_first);
        const int64_t boundary = f.pts;
        if (hooks.between)
            hooks.between();
        return std::pair(boundary, clip(hooks.after_admission, nullptr));
    };
    const auto push = [&] { f.pushFrame(false); };
    const auto closeAndReopen = [&] {
        f.ctx.publishScheduleGate(false);
        f.ctx.publishScheduleGate(true);
    };
    const auto suspendAndResume = [&] {
        f.ctx.suspendMedia(false);
        f.ctx.media_barrier.resume();
    };
    using Pts = std::vector<int64_t>;

    // Nothing between: the second clip continues from the boundary, with the frame that arrived meanwhile.
    auto [boundary, written] = pair({ .between = push });
    VG_CHECK(written == (Pts { boundary, boundary + 1 }));

    // The schedule closes and opens again between the clips, the frame arriving while it is closed.
    std::tie(boundary, written) = pair({ .between = [&] {
        f.ctx.publishScheduleGate(false);
        push();
        VG_CHECK(!f.ctx.dispatchContinuous());
        f.ctx.publishScheduleGate(true);
    } });
    VG_CHECK(written == (Pts { boundary + 2, boundary + 3 }));

    // Media is suspended between the clips and resumed without clearing the buffer.
    std::tie(boundary, written) = pair({ .between = [&] {
        f.ctx.suspendMedia(true);
        push();
        VG_CHECK(!f.ctx.dispatchContinuous());
        f.ctx.media_barrier.resume();
    } });
    VG_CHECK(written == (Pts { boundary + 2, boundary + 3 }));

    // Continuous recording is switched off and on between the clips: the Video revision moves.
    std::tie(boundary, written) = pair({ .between = [&] {
        f.enable(CaptureType::Video, std::chrono::milliseconds(20));
        push();
        VG_CHECK(!f.ctx.dispatchContinuous());
        f.enableContinuous();
    } });
    VG_CHECK(written == (Pts { boundary + 2, boundary + 3 }));

    // The schedule closes and opens again after the second clip was admitted, before its claim.
    std::tie(boundary, written) = pair({ .after_admission = [&] {
        f.ctx.publishScheduleGate(false);
        push();
        f.ctx.publishScheduleGate(true);
    } });
    VG_CHECK(written == (Pts { boundary + 2, boundary + 3 }));

    // Media is suspended after the second clip was admitted: the worker leaves it pending until the resume.
    std::tie(boundary, written) = pair({ .after_admission = [&] {
        f.ctx.suspendMedia(true);
        push();
        VG_CHECK(worker.step() == Step::Idle);
        f.ctx.media_barrier.resume();
    } });
    VG_CHECK(written == (Pts { boundary + 2, boundary + 3 }));

    // A closure, then a suspension, too short for the first clip to see: it completes, but the second starts fresh.
    std::tie(boundary, written) = pair({ .during_first = closeAndReopen, .between = push });
    VG_CHECK(written == (Pts { boundary + 2, boundary + 3 }));
    std::tie(boundary, written) = pair({ .during_first = suspendAndResume, .between = push });
    VG_CHECK(written == (Pts { boundary + 2, boundary + 3 }));

    // A Slot briefly not ready between the clips keeps them continuous.
    std::tie(boundary, written) = pair({ .between = [&] {
        f.slot.ready = false;
        push();
        VG_CHECK(!f.ctx.dispatchContinuous());
        VG_CHECK(worker.step() == Step::Idle);
        f.slot.ready = true;
    } });
    VG_CHECK(written == (Pts { boundary, boundary + 1 }));
}

VG_TEST_MAIN()
