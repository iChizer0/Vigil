// Characterization tests for the writers over a fake adapter and fake media callbacks: no Slot, no
// filesystem, no device (docs/PLAN.md section 7.2). They pin the sink call sequence (create, write, finish
// once), the terminal status of every callback failure, interruption, and the media operation every writer holds
// from construction to destruction.

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

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
using namespace vigil::writers;
using namespace vigil::tests;

namespace {

struct Fixture
{
    FakeMedia media;
    Context ctx;
    std::shared_ptr<WriteEvent> event;
    int64_t pts = 0;

    explicit Fixture(bool with_callbacks = true)
        : ctx(Config(media, with_callbacks)),
          event(std::make_shared<WriteEvent>(std::chrono::steady_clock::now(), int64_t { 1000 }, uint64_t { 42 }))
    {
        media.ctx = &ctx;
    }

    static vg_config_t Config(FakeMedia &media, bool with_callbacks)
    {
        auto cfg = MakeFakeConfig();
        cfg.media.user_data = &media;
        if (with_callbacks)
        {
            cfg.media.video_file_create_cb = &FakeMedia::Create;
            cfg.media.video_file_finish_cb = &FakeMedia::Finish;
            cfg.media.video_file_write_video_frame_cb = &FakeMedia::WriteVideo;
            cfg.media.video_file_write_audio_frame_cb = &FakeMedia::WriteAudio;
            cfg.media.image_capture_cb = &FakeMedia::Capture;
        }
        return cfg;
    }

    static std::shared_ptr<FileWriterSettings> Settings(CaptureType type)
    {
        const char *format = type == CaptureType::Video ? "MP4" : "JPG";
        return FileWriterSettings::create(type, format, std::chrono::milliseconds(5000), kRecordWriterPrefixDefault,
            kVideoClipLengthDefault, WaterMarks::from(format, 1, nullptr));
    }

    std::unique_ptr<VideoWriter<FakeAdapter>> video()
    {
        return std::make_unique<VideoWriter<FakeAdapter>>(std::ref(media), std::ref(ctx), Settings(CaptureType::Video),
            event);
    }

    std::unique_ptr<ImageWriter<FakeAdapter>> image()
    {
        return std::make_unique<ImageWriter<FakeAdapter>>(std::ref(media), std::ref(ctx), Settings(CaptureType::Image),
            event);
    }

    void push(bool key_frame)
    {
        uint8_t data[4] = { 1, 2, 3, 4 };
        const vg_video_frame_t frame { .pts = ++pts, .data = data, .bytes_len = sizeof data, .key_frame = key_frame };
        media.queue.emplace_back(frame);
    }

    void pushAudio()
    {
        uint8_t data[4] = { 5, 6, 7, 8 };
        const vg_audio_frame_t frame { .pts = ++pts, .data = data, .bytes_len = sizeof data };
        media.queue.emplace_back(frame);
    }
};

} // namespace

VG_TEST_CASE(video_writer_writes_from_the_first_key_frame_and_finishes_on_the_next_one)
{
    Fixture f;
    f.push(false); // before the first key frame: dropped
    f.push(true);
    f.push(false);
    f.push(false);
    f.pushAudio();
    f.push(true);
    f.media.finish_after_frames = 3;
    {
        auto writer = f.video();
        VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 1 }); // admitted at construction
        VG_CHECK(f.event->status.load() == WriteEvent::Status::Writing);
        VG_CHECK_EQ(f.media.creates, 0); // the task creates the sink after its pre-check

        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Completed);
        VG_CHECK_EQ(f.media.creates, 1);
        VG_CHECK_EQ(f.media.last_format, "MP4");
        VG_CHECK_EQ(f.media.last_path, "/fake/record.file");
        VG_CHECK_EQ(f.media.video_frames, 3);
        VG_CHECK_EQ(f.media.audio_frames, 1);
        VG_CHECK_EQ(f.media.finishes, 1);
        VG_CHECK_EQ(f.media.last_fd, 7);
        VG_CHECK_EQ(f.media.thumbnails, 1);
        VG_CHECK_EQ(f.media.active_while_writing, size_t { 1 });
        VG_CHECK_EQ(f.media.queue.size(), size_t { 1 }); // the closing key frame starts the next clip
        VG_CHECK(writer->boundaryKeyFrame() == f.media.queue.front().time_stamp); // and is reported as the boundary
    }
    VG_CHECK_EQ(f.media.finishes, 1); // the destructor does not finish the sink again
    VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
}

VG_TEST_CASE(video_writer_interruption_finishes_the_sink_exactly_once)
{
    // The gate closes while the writer waits for the first key frame (rules disabled, settings changed, or the
    // Slot stopping): nothing was recorded, so no sink was created and the task is Canceled, not an empty file
    // (Interrupted with an empty sink until 2026-10-20, Phase 5A-1).
    {
        Fixture f;
        f.push(false);
        f.media.stop_after_continue_checks = 1;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
            VG_CHECK_EQ(f.media.creates, 0);
            VG_CHECK_EQ(f.media.video_frames, 0);
            VG_CHECK_EQ(f.media.finishes, 0);
            VG_CHECK(f.media.queue.empty()); // the frames before a key frame are dropped as they are seen
        }
        VG_CHECK_EQ(f.media.finishes, 0);
        VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
    }
    // The gate is already closed when the task starts: no sink is created, and Canceled publishes nothing.
    {
        Fixture f;
        f.push(true);
        f.media.stop_after_frames = 0;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
            VG_CHECK_EQ(f.media.creates, 0);
            VG_CHECK_EQ(f.media.video_frames, 0);
        }
        VG_CHECK_EQ(f.media.finishes, 0);
    }
    // An interrupted clip whose finish fails is a failed task, so it is not published as usable.
    {
        Fixture f;
        f.push(true);
        f.media.stop_after_frames = 1;
        f.media.finish_result = EIO;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Failed);
            VG_CHECK_EQ(f.media.video_frames, 1);
            VG_CHECK_EQ(f.media.finishes, 1);
        }
        VG_CHECK_EQ(f.media.finishes, 1);
    }
    // Interrupted mid-clip: the frames written so far end up in a finished file.
    {
        Fixture f;
        f.push(true);
        f.push(false);
        f.push(false);
        f.media.stop_after_frames = 2;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Interrupted);
            VG_CHECK_EQ(f.media.video_frames, 2);
            VG_CHECK_EQ(f.media.finishes, 1);
        }
        VG_CHECK_EQ(f.media.finishes, 1);
    }
}

VG_TEST_CASE(video_writer_bounds_its_waits_for_key_frames_and_input)
{
    // docs/PLAN.md section 8.2 (Phase 5A-1): the waits for the first key frame, for the closing key frame, and
    // for any frame once the clip started are bounded (section 13: kVideoKeyFrameWait, kVideoInputLossWait;
    // injected here), and each outcome is defined.
    const VideoWriter<FakeAdapter>::WaitBounds bounds { std::chrono::milliseconds(60), std::chrono::milliseconds(60) };
    // No key frame within the wait: nothing is recorded, the task is Canceled, and no file exists.
    {
        Fixture f;
        f.push(false);
        f.push(false);
        auto writer = f.video();
        writer->setWaitBounds(bounds);
        const auto before = std::chrono::steady_clock::now();
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
        VG_CHECK(std::chrono::steady_clock::now() - before >= bounds.key_frame);
        VG_CHECK_EQ(f.media.creates, 0);
        VG_CHECK_EQ(f.media.finishes, 0);
        VG_CHECK(f.media.queue.empty());
    }
    // The clip ends (here the adapter asks) before any key frame: Canceled, no empty file (Completed with an
    // empty file until 2026-10-20).
    {
        Fixture f;
        f.push(false);
        f.media.finish_after_frames = 0;
        auto writer = f.video();
        writer->setWaitBounds(bounds);
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
        VG_CHECK_EQ(f.media.creates, 0);
    }
    // Input loss after the start: the clip is finished with what was written, as Interrupted.
    {
        Fixture f;
        f.push(true);
        auto writer = f.video();
        writer->setWaitBounds(bounds);
        const auto before = std::chrono::steady_clock::now();
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Interrupted);
        VG_CHECK(std::chrono::steady_clock::now() - before >= bounds.input_loss);
        VG_CHECK_EQ(f.media.creates, 1);
        VG_CHECK_EQ(f.media.video_frames, 1);
        VG_CHECK_EQ(f.media.finishes, 1);
        VG_CHECK_EQ(f.media.thumbnails, 1);
    }
    // The clip is finished but no closing key frame arrives: it ends at the wait, Completed, with the frames
    // written meanwhile; the next frame stays queued.
    {
        Fixture f;
        f.push(true);
        f.push(false);
        f.push(false);
        f.media.finish_after_frames = 1;
        auto writer = f.video();
        writer->setWaitBounds(bounds);
        const auto before = std::chrono::steady_clock::now();
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Completed);
        VG_CHECK(std::chrono::steady_clock::now() - before >= bounds.key_frame);
        VG_CHECK_EQ(f.media.video_frames, 3);
        VG_CHECK_EQ(f.media.finishes, 1);
        VG_CHECK(f.media.queue.empty());
        VG_CHECK(!writer->boundaryKeyFrame()); // no boundary key frame was left
    }
    // The closing key frame arrives: the clip ends on it at once and leaves it queued for the next clip.
    {
        Fixture f;
        f.push(true);
        f.push(false);
        f.push(true);
        f.media.finish_after_frames = 1;
        auto writer = f.video();
        writer->setWaitBounds(bounds);
        const auto before = std::chrono::steady_clock::now();
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Completed);
        VG_CHECK(std::chrono::steady_clock::now() - before < bounds.key_frame);
        VG_CHECK_EQ(f.media.video_frames, 2);
        VG_REQUIRE(f.media.queue.size() == size_t { 1 });
        VG_CHECK(f.media.queue.front().isKeyFrame());
    }
    // The gate breaks a wait before its bound: the production bounds stay in place here.
    {
        Fixture f;
        f.push(false);
        f.media.stop_after_continue_checks = 3;
        auto writer = f.video();
        const auto before = std::chrono::steady_clock::now();
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
        VG_CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
        VG_CHECK_EQ(f.media.creates, 0);
    }
}

VG_TEST_CASE(video_writer_failures_keep_the_activity_count_balanced)
{
    // The sink cannot be created: the task is Failed, nothing is finished, nothing stays counted.
    {
        Fixture f;
        f.push(true);
        f.media.create_result = EIO;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Failed);
            VG_CHECK_EQ(f.media.creates, 1);
            VG_CHECK_EQ(f.media.finishes, 0);
        }
        VG_CHECK_EQ(f.media.finishes, 0);
        VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 }); // leaked before 2026-10-19
    }
    // Missing callbacks fail the same way.
    {
        Fixture f(/* with_callbacks = */ false);
        VG_CHECK_THROWS(f.video());
        VG_CHECK(f.event->status.load() == WriteEvent::Status::Failed);
        VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
    }
    // A frame write failure finishes the sink and fails the task; the destructor does not finish again.
    {
        Fixture f;
        f.push(true);
        f.media.write_result = EIO;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Failed);
            VG_CHECK_EQ(f.media.video_frames, 1);
            VG_CHECK_EQ(f.media.finishes, 1);
        }
        VG_CHECK_EQ(f.media.finishes, 1);
        VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
    }
    // A finish failure fails the task, and the sink is not finished a second time.
    {
        Fixture f;
        f.push(true);
        f.push(true);
        f.media.finish_after_frames = 1;
        f.media.finish_result = EIO;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Failed);
            VG_CHECK_EQ(f.media.finishes, 1);
            VG_CHECK_EQ(f.media.thumbnails, 0);
        }
        VG_CHECK_EQ(f.media.finishes, 1); // twice before 2026-10-19
    }
}

VG_TEST_CASE(image_writer_captures_once_and_reports_every_failure)
{
    {
        Fixture f;
        auto writer = f.image();
        VG_CHECK(f.event->status.load() == WriteEvent::Status::Writing);
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Completed);
        VG_CHECK_EQ(f.media.captures, 1);
        VG_CHECK_EQ(f.media.last_format, "JPG");
        VG_CHECK_EQ(f.media.last_path, "/fake/record.file");
    }
    {
        Fixture f;
        f.media.capture_result = EIO;
        auto writer = f.image();
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Failed);
        VG_CHECK_EQ(f.media.captures, 1);
    }
    {
        Fixture f; // paused media: nothing is captured, and Canceled keeps the missing file unpublished
        f.ctx.media_barrier.pause(false);
        auto writer = f.image();
        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
        VG_CHECK_EQ(f.media.captures, 0);
    }
    {
        Fixture f(/* with_callbacks = */ false);
        VG_CHECK_THROWS(f.image());
        VG_CHECK(f.event->status.load() == WriteEvent::Status::Failed);
    }
}

VG_TEST_CASE(writers_hold_one_media_operation_from_construction_to_destruction)
{
    // Image capture is covered (it was not before 2026-10-19), and so is the adapter's destructor: the operation
    // leaves only after it. Since Phase 4C-2 the Slot generates the thumbnail in takePublication, still under
    // the operation, and publishes once the writer is released (tests/test_channel_worker.cpp).
    {
        Fixture f;
        {
            auto writer = f.image();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Completed);
            VG_CHECK_EQ(f.media.active_while_capturing, size_t { 1 });
        }
        VG_CHECK_EQ(f.media.active_at_destruction, size_t { 1 });
        VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
    }
    // Media paused before construction: admission is refused, no sink is created, the task is Canceled.
    {
        Fixture f;
        f.push(true);
        f.ctx.media_barrier.pause(false);
        {
            auto writer = f.video();
            VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
            VG_CHECK_EQ(f.media.creates, 0);
        }
        VG_CHECK_EQ(f.media.finishes, 0);
        VG_CHECK_EQ(f.media.queue.size(), size_t { 1 }); // nothing was consumed
    }
    // Media paused after admission (here from inside the create callback, which runs with the first key frame in
    // hand): the task is interrupted at its next check, the sink it created is finished, and the operation is
    // counted until the writer is destroyed.
    {
        Fixture f;
        f.push(true);
        f.media.pause_on_create = true;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Interrupted);
            VG_CHECK_EQ(f.media.creates, 1);
            VG_CHECK_EQ(f.media.video_frames, 1);
            VG_CHECK_EQ(f.media.finishes, 1);
            VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 1 });
        }
        VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
    }
}

VG_TEST_MAIN()
