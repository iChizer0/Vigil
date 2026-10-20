// Characterization tests for the file channel worker over a fake Slot adapter and fake media (docs/PLAN.md
// section 7.2; matrix W1): claim, write, publish outside the media operation and before the hold, complete; the
// failure cooldown and the consecutive failure limit with injected constants; Canceled and Interrupted do not
// hold; a stale task never touches a newer one through the loop; the thread body with its stop; two workers over
// one Slot, each on its own channel with its own budget and failure count, serving both channels at once; and
// the admission at dispatch that binds a task to the registered Slot with its budget (section 8.1).

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
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

// The worker's view of a Slot, over FakeMedia writers, with the registered admission of FakeSlotAdmission (the
// ready flag, the free space, the registration). The flags are atomics because the thread cases flip them while
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
        const std::lock_guard lock(publications_mutex);
        publications.push_back(
            Publication { info.event, info.path, info.event->status.load(), ctx.file_channels.holdsTask(),
                ctx.media_barrier.activeOperations(), ctx.in_flight_budgets.inFlight(kFilesystem) });
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

    // Publishes a snapshot with both channels enabled, which no configuration can do before Phase 4D.
    void enableBoth(std::chrono::milliseconds photo_interval, std::chrono::milliseconds video_interval)
    {
        publish(settingsOf(CaptureType::Image, photo_interval), settingsOf(CaptureType::Video, video_interval));
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
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Failed); // completed before the stop
    VG_CHECK(photo.current() == nullptr && photo.last() == task);
    VG_CHECK_EQ(f.media.captures, 4);
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
    // The task's own bytes are in flight from its acceptance until its file is published (docs/PLAN.md section
    // 9), whatever the outcome: success, failure, cancellation, interruption, and retirement before a claim all
    // release them.
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
    // Two workers over one Slot, one per channel (docs/PLAN.md section 7.2): neither claims from the other's
    // channel, each task reserves its own bytes against the aggregate mark (section 9), and a channel's failures
    // count for that channel alone. The Video worker steps inside the Photo capture callback, so the two
    // reservations overlap deterministically.
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
    // Closing the Slot's admission retires the tasks of both channels that no worker claimed (docs/PLAN.md
    // section 9), while a worker's start retires nothing; then the Photo worker completes a capture while the
    // Video worker is still waiting for the key frame that closes its clip (matrix A1: Video busy while Photo
    // triggers), and the stop ends both holds.
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

VG_TEST_MAIN()
