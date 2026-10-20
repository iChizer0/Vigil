#pragma once
#ifndef VIGIL_TESTS_FAKE_MEDIA_HPP
#define VIGIL_TESTS_FAKE_MEDIA_HPP

// The fake media sink and the writer adapter that the writer and channel worker tests run on: no Slot, no
// filesystem, no device. FakeMedia records every callback with a knob for each outcome; FakeAdapter is an
// AdaptableWriter with the same hooks as SlotAdaptedWriter, including the publication hand-over the channel
// worker relies on.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "vigil/context.hpp"
#include "vigil/types.hpp"
#include "vigil/writers.hpp"

namespace vigil::tests {

using namespace vigil::types;
using namespace vigil::writers;

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
    bool pause_on_create = false;     // pauses media from inside the create callback, after admission
    std::function<void()> on_capture; // runs inside the image capture callback, before it returns

    int creates = 0;
    int finishes = 0;
    int captures = 0;
    int video_frames = 0;
    int audio_frames = 0;
    int thumbnails = 0;
    int last_fd = -1;
    std::string last_path;
    std::string last_format;
    size_t active_while_writing = 0;         // active media operations seen from inside the write callback
    size_t active_while_capturing = 0;       // same, from inside the image capture callback
    size_t active_at_destruction = SIZE_MAX; // same, from the adapter's destructor
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
        if (m.pause_on_create)
            m.ctx->media_barrier.pause(false);
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
        m.active_while_writing = m.ctx->media_barrier.activeOperations();
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
        m.active_while_capturing = m.ctx->media_barrier.activeOperations();
        if (m.on_capture)
            m.on_capture();
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

    ~FakeAdapter() override
    {
        _media.active_at_destruction = this->ctxRef().media_barrier.activeOperations();
    }

    // What SlotAdaptedWriter hands the channel worker after the task ran: the recorded file when the task is
    // publishable (Completed or Interrupted), nothing otherwise.
    std::optional<CacheFileInfo> takePublication() noexcept
    {
        if (const auto sta = this->event()->status.load(std::memory_order_acquire);
            sta != WriteEvent::Status::Completed && sta != WriteEvent::Status::Interrupted)
            return std::nullopt;
        std::optional<CacheFileInfo> publication(std::in_place, this->event());
        publication->path = getFilePathImpl();
        return publication;
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

} // namespace vigil::tests

#endif
