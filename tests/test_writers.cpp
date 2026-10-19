// Characterization tests for the writers over a fake adapter and fake media callbacks: no Slot, no
// filesystem, no device (docs/PLAN.md section 7.2). They pin the sink call sequence (create, write, finish
// once), the terminal status of every callback failure, interruption, and the balanced media activity count.

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/types.hpp"
#include "vigil/writers.hpp"

#include "check.hpp"
#include "fake_config.hpp"

using namespace vigil;
using namespace vigil::types;
using namespace vigil::constants;
using namespace vigil::writers;
using namespace vigil::tests;

namespace {

// The fake media sink and the frame queue a writer reads from, with a knob for every callback outcome.
struct FakeMedia
{
    std::mutex mutex;
    std::deque<MediaFrame> queue;

    vg_sta_e create_result = 0;
    vg_sta_e finish_result = 0;
    vg_sta_e write_result = 0;
    vg_sta_e capture_result = 0;
    // The clip is "finished" once this many frames were written; writing "should stop" after this many frames,
    // or after this many continue checks (the first check is the writer's pre-check before anything is created).
    size_t finish_after_frames = SIZE_MAX;
    size_t stop_after_frames = SIZE_MAX;
    size_t stop_after_continue_checks = SIZE_MAX;
    size_t continue_checks = 0;

    int creates = 0;
    int finishes = 0;
    int captures = 0;
    int video_frames = 0;
    int audio_frames = 0;
    int thumbnails = 0;
    int last_fd = -1;
    std::string last_path;
    std::string last_format;
    size_t active_while_writing = 0; // media_writers_active as seen from inside the write callback
    Context *ctx = nullptr;

    size_t framesWritten() const noexcept
    {
        return static_cast<size_t>(video_frames + audio_frames);
    }

    static FakeMedia &of(void *user_data)
    {
        return *static_cast<FakeMedia *>(user_data);
    }

    static vg_sta_e Create(int *fd, const char *path, const char *format, void *user_data)
    {
        auto &m = of(user_data);
        ++m.creates;
        m.last_path = path;
        m.last_format = format;
        if (m.create_result != 0)
            return m.create_result;
        *fd = 7;
        return 0;
    }

    static vg_sta_e Finish(int fd, void *user_data)
    {
        auto &m = of(user_data);
        ++m.finishes;
        m.last_fd = fd;
        return m.finish_result;
    }

    static vg_sta_e WriteVideo(int fd, const vg_video_frame_t *, void *user_data)
    {
        auto &m = of(user_data);
        ++m.video_frames;
        m.last_fd = fd;
        m.active_while_writing = m.ctx->media_writers_active.load();
        return m.write_result;
    }

    static vg_sta_e WriteAudio(int fd, const vg_audio_frame_t *, void *user_data)
    {
        auto &m = of(user_data);
        ++m.audio_frames;
        m.last_fd = fd;
        return m.write_result;
    }

    static vg_sta_e Capture(const char *path, const char *format, void *user_data)
    {
        auto &m = of(user_data);
        ++m.captures;
        m.last_path = path;
        m.last_format = format;
        return m.capture_result;
    }
};

// A writer adapter over FakeMedia with the same hooks as SlotAdaptedWriter.
class FakeAdapter: public AdaptableWriter<FakeAdapter>
{
public:
    FakeAdapter(FakeMedia &media, Context &ctx, std::shared_ptr<FileWriterSettings> settings,
        std::shared_ptr<WriteEvent> event)
        : AdaptableWriter<FakeAdapter>(ctx, std::move(settings), std::move(event)), _media(media)
    {
    }

protected:
    friend AdaptableWriter<FakeAdapter>;

    std::mutex &mediaMutexImpl() const noexcept
    {
        return _media.mutex;
    }

    std::deque<MediaFrame> &mediaQueueImpl() noexcept
    {
        return _media.queue;
    }

    std::filesystem::path getFilePathImpl() const noexcept
    {
        return "/fake/record.file";
    }

    bool shouldFinishWritingImpl() const noexcept
    {
        return _media.framesWritten() >= _media.finish_after_frames;
    }

    bool shouldContinueWritingImpl() const noexcept
    {
        return ++_media.continue_checks <= _media.stop_after_continue_checks
               && _media.framesWritten() < _media.stop_after_frames;
    }

    void onThumbnailFrameImpl(const MediaFrame::FrameType &) noexcept
    {
        ++_media.thumbnails;
    }

private:
    FakeMedia &_media;
};

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
        VG_CHECK_EQ(f.ctx.media_writers_active.load(), size_t { 1 });
        VG_CHECK(f.event->status.load() == WriteEvent::Status::Writing);
        VG_CHECK_EQ(f.media.creates, 1);
        VG_CHECK_EQ(f.media.last_format, "MP4");
        VG_CHECK_EQ(f.media.last_path, "/fake/record.file");

        VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Completed);
        VG_CHECK_EQ(f.media.video_frames, 3);
        VG_CHECK_EQ(f.media.audio_frames, 1);
        VG_CHECK_EQ(f.media.finishes, 1);
        VG_CHECK_EQ(f.media.last_fd, 7);
        VG_CHECK_EQ(f.media.thumbnails, 1);
        VG_CHECK_EQ(f.media.active_while_writing, size_t { 1 });
        VG_CHECK_EQ(f.media.queue.size(), size_t { 1 }); // the closing key frame starts the next clip
    }
    VG_CHECK_EQ(f.media.finishes, 1); // the destructor does not finish the sink again
    VG_CHECK_EQ(f.ctx.media_writers_active.load(), size_t { 0 });
}

VG_TEST_CASE(video_writer_interruption_finishes_the_sink_exactly_once)
{
    // Interrupted while waiting for the first key frame (rules disabled, settings changed, or the Slot
    // stopping): the sink is finished before the task reports, not left to the destructor.
    {
        Fixture f;
        f.push(false);
        f.media.stop_after_continue_checks = 1;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Interrupted);
            VG_CHECK_EQ(f.media.video_frames, 0);
            VG_CHECK_EQ(f.media.finishes, 1);
        }
        VG_CHECK_EQ(f.media.finishes, 1);
        VG_CHECK_EQ(f.ctx.media_writers_active.load(), size_t { 0 });
    }
    // The gate is already closed when the task starts: nothing is written, the untouched sink is finished, and
    // Canceled keeps it unpublished.
    {
        Fixture f;
        f.push(true);
        f.media.stop_after_frames = 0;
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
            VG_CHECK_EQ(f.media.video_frames, 0);
            VG_CHECK_EQ(f.media.finishes, 1);
        }
        VG_CHECK_EQ(f.media.finishes, 1);
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
    // A media pause before the task starts cancels it the same way.
    {
        Fixture f;
        f.push(true);
        f.ctx.media_paused.store(true);
        {
            auto writer = f.video();
            VG_CHECK((*writer)()->status.load() == WriteEvent::Status::Canceled);
            VG_CHECK_EQ(f.media.video_frames, 0);
            VG_CHECK_EQ(f.media.finishes, 1);
        }
        VG_CHECK_EQ(f.media.finishes, 1);
    }
}

VG_TEST_CASE(video_writer_failures_keep_the_activity_count_balanced)
{
    // The sink cannot be created: the constructor throws, the event is Failed, nothing stays counted.
    {
        Fixture f;
        f.media.create_result = EIO;
        VG_CHECK_THROWS(f.video());
        VG_CHECK(f.event->status.load() == WriteEvent::Status::Failed);
        VG_CHECK_EQ(f.media.creates, 1);
        VG_CHECK_EQ(f.media.finishes, 0);
        VG_CHECK_EQ(f.ctx.media_writers_active.load(), size_t { 0 }); // leaked before 2026-10-19
    }
    // Missing callbacks fail the same way.
    {
        Fixture f(/* with_callbacks = */ false);
        VG_CHECK_THROWS(f.video());
        VG_CHECK(f.event->status.load() == WriteEvent::Status::Failed);
        VG_CHECK_EQ(f.ctx.media_writers_active.load(), size_t { 0 });
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
        VG_CHECK_EQ(f.ctx.media_writers_active.load(), size_t { 0 });
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
        VG_CHECK_EQ(f.ctx.media_writers_active.load(), size_t { 0 }); // image capture is not counted (Phase 2B)
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
        f.ctx.media_paused.store(true);
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

VG_TEST_MAIN()
