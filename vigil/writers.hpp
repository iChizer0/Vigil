#pragma once
#ifndef VIGIL_WRITERS_HPP
#define VIGIL_WRITERS_HPP

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include <sys/stat.h>
#include <sys/statvfs.h>

#include "constants.hpp"
#include "context.hpp"
#include "index_manager.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil::writers {

using namespace vigil::constants;
using namespace vigil::types;
using namespace vigil::index_manager;

namespace detail {

    template<class... Ts>
    struct Overloaded: Ts...
    {
        using Ts::operator()...;
    };

    template<class... Ts>
    Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace detail

// - MARK: Adaptable Writer

template<typename T>
class AdaptableWriter
{
public:
    AdaptableWriter(Context &ctx, std::shared_ptr<FileWriterSettings> settings, std::shared_ptr<WriteEvent> event)
        : _start_time(std::chrono::steady_clock::now()), _ctx(ctx), _settings(std::move(settings)),
          _event(std::move(event)), _operation(ctx.media_barrier)
    {
        if (!_settings)
            throw Exception("AdaptableWriter initialized with null settings");
        if (!_event)
            throw Exception("AdaptableWriter initialized with null event");
        setEventStatus(WriteEvent::Status::Writing);
    }

    virtual ~AdaptableWriter() noexcept = default;

    // Runs the task to its terminal status: Canceled when the gate was already closed before anything was
    // recorded, Completed, Failed, or Interrupted (a gate, pause, stop, or settings change stopped a task that
    // had started; its file is kept).
    const std::shared_ptr<WriteEvent> &operator()() noexcept
    {
        setEventStatus(waitUntilWriteFinish());
        return _event;
    }

    inline std::chrono::steady_clock::time_point startTime() const noexcept
    {
        return _start_time;
    }

    inline std::chrono::steady_clock::time_point endTime() const noexcept
    {
        return _start_time + _settings->interval;
    }

protected:
    inline std::mutex &mediaMutex() const noexcept
    {
        return static_cast<const T *>(this)->mediaMutexImpl();
    }

    inline std::deque<MediaFrame> &mediaQueue() noexcept
    {
        return static_cast<T *>(this)->mediaQueueImpl();
    }

    inline bool shouldFinishWriting() const noexcept
    {
        return static_cast<const T *>(this)->shouldFinishWritingImpl();
    }

    // False when media was paused at construction (the operation was refused) or since, or when the adapter's
    // own gate is closed. Writers check it before creating anything and between frames.
    inline bool shouldContinueWriting() const noexcept
    {
        return static_cast<bool>(_operation) && !_ctx.media_barrier.isPaused()
               && static_cast<const T *>(this)->shouldContinueWritingImpl();
    }

    inline Context &ctxRef() const noexcept
    {
        return _ctx;
    }

    inline const FileWriterSettings &settings() const noexcept
    {
        return *_settings;
    }

    inline void setEventStatus(WriteEvent::Status status) noexcept
    {
        _event->status.store(status, std::memory_order_release);
    }

    virtual WriteEvent::Status waitUntilWriteFinish() noexcept = 0;

    inline const std::shared_ptr<WriteEvent> &event() const noexcept
    {
        return _event;
    }

    inline std::filesystem::path getFilePath() const noexcept
    {
        return static_cast<const T *>(this)->getFilePathImpl();
    }

    void onThumbnailFrame(const MediaFrame::FrameType &frame) noexcept
    {
        static_cast<T *>(this)->onThumbnailFrameImpl(frame);
    }

private:
    std::chrono::steady_clock::time_point _start_time;
    Context &_ctx;
    std::shared_ptr<FileWriterSettings> _settings;
    std::shared_ptr<WriteEvent> _event;
    // Held for the whole writer lifetime, so it still covers the publication (thumbnail generation included)
    // that the adapter's destructor performs before this base is destroyed.
    Context::MediaBarrier::Operation _operation;
};

// - MARK: Slot Adapted Writer

template<typename T>
class SlotAdaptedWriter: public AdaptableWriter<SlotAdaptedWriter<T>>
{
public:
    template<typename... Args>
    SlotAdaptedWriter(T &slot, size_t settings_revision, Args &&...args)
        : AdaptableWriter<SlotAdaptedWriter<T>>(std::forward<Args>(args)...), _slot(slot),
          _settings_revision(settings_revision), _cache_file_info(this->event())
    {
        if (!initPathsAndFileName())
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception("SlotAdaptedWriter failed to initialize cache file paths and names");
        }
        _slot.writerCountRef().fetch_add(1, std::memory_order_acq_rel);
    }

    virtual ~SlotAdaptedWriter() noexcept
    {
        _slot.writerCountRef().fetch_sub(1, std::memory_order_acq_rel);
    }

    // After the task ran: the recorded file for the channel worker to publish, with its thumbnail generated into
    // the cache directory while this writer still holds its media operation (docs/PLAN.md section 8.3). Empty
    // when nothing is published: a Canceled task recorded nothing, and a Failed task's file is not usable. Called
    // once; the worker publishes after releasing the writer, so the rename and the FILE callback run outside the
    // operation.
    std::optional<CacheFileInfo> takePublication() noexcept
    {
        if (const auto sta = this->event()->status.load(std::memory_order_acquire);
            sta != WriteEvent::Status::Completed && sta != WriteEvent::Status::Interrupted)
            return std::nullopt;
        _slot.generateThumbnail(_cache_file_info);
        return std::optional<CacheFileInfo>(std::move(_cache_file_info));
    }

protected:
    friend AdaptableWriter<SlotAdaptedWriter<T>>;

    inline std::mutex &mediaMutexImpl() const noexcept
    {
        return this->ctxRef().slot_media_buffer.mutex;
    }

    inline std::deque<MediaFrame> &mediaQueueImpl() noexcept
    {
        return this->ctxRef().slot_media_buffer.queue;
    }

    inline std::filesystem::path getFilePathImpl() const noexcept
    {
        return _cache_file_info.path;
    }

    inline bool shouldFinishWritingImpl() const noexcept
    {
        return false;
    }

    // The writer stops when rules are disabled, when its own file channel's settings change (a change to the
    // other channel or to Sound does not stop it; docs/PLAN.md section 7.1), or when the Slot stops being ready.
    inline bool shouldContinueWritingImpl() const noexcept
    {
        return this->ctxRef().record_rule_enabled.load(std::memory_order_relaxed)
               && this->ctxRef().action_settings.channelRevision(this->settings().type) == _settings_revision
               && _slot.isReadyToWrite();
    }

    inline void onThumbnailFrameImpl(const MediaFrame::FrameType &frame) noexcept
    {
        if (!this->ctxRef().c_api.media.thumbnail_generate_cb)
            return;
        if (auto p = std::get_if<VideoFrame>(frame.get()); p) [[likely]]
            p->cloneTo(_cache_file_info.thumbnail);
    }

private:
    bool initPathsAndFileName()
    {
        const auto local_time = LocalTime(std::chrono::system_clock::now());
        auto tm = local_time.tm();
        std::stringstream ss;
        ss << std::put_time(&tm, kStorageFirstLevelDirNameTimeFormat);
        _cache_file_info.first_level_dir_name = ss.str();

        const auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        _cache_file_info.creation_time = std::chrono::file_clock::from_sys(tp);

        const auto duration = this->startTime().time_since_epoch();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);
        ss << ' ' << std::put_time(&tm, kStorageFileNamePrefixTimeFormat) << ' ' << std::setfill('0') << std::setw(6)
           << micros.count();

        const auto cache_dir = _slot.getRecordCachePath();
        if (!EnsureDirectoryExists(cache_dir, true, false, &this->ctxRef()))
            return false;
        auto fmt = std::string(this->settings().format);
        std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return std::tolower(c); });
        _cache_file_info.path = cache_dir / (std::string(this->settings().file_name_prefix) + ss.str() + '.' + fmt);

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_DEBUG, "Initialized writer with 1st level directory \"",
            _cache_file_info.first_level_dir_name, "\", cache file path ", _cache_file_info.path);
        return true;
    }

private:
    T &_slot;
    size_t _settings_revision; // of the writer's own file channel, loaded together with its settings
    CacheFileInfo _cache_file_info;
};

// - MARK: Capture Adapted Writer

template<typename T>
class CaptureAdaptedWriter: public AdaptableWriter<CaptureAdaptedWriter<T>>
{
public:
    template<typename... Args>
    CaptureAdaptedWriter(T &capture, std::shared_ptr<CaptureEvent> event, Args &&...args)
        : AdaptableWriter<CaptureAdaptedWriter<T>>(std::forward<Args>(args)...), _capture(capture),
          _event(std::move(event)), _output_file_path(), _output_cache_path(),
          _last_stats_time(std::chrono::steady_clock::time_point::min())
    {
        if (!_event)
            throw Exception("CaptureAdaptedWriter initialized with null event");

        if (_event->file_name.empty())
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception("CaptureAdaptedWriter initialized with empty output file name");
        }

        if (!isRemainSpaceEnough())
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception("CaptureAdaptedWriter detected insufficient remaining space");
        }

        if (!EnsureDirectoryExists(_event->output_directory, true, false, &this->ctxRef()))
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception(
                "CaptureAdaptedWriter failed to ensure output directory exists: " + _event->output_directory.string());
        }

        _output_file_path = _event->output_directory / _event->file_name;
        _output_cache_path = _event->output_directory / (std::string(".") + _event->file_name);
        if (std::error_code ec; std::filesystem::exists(_output_cache_path, ec))
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_INFO,
                "CaptureAdaptedWriter output cache already exists at path: ", _output_cache_path, ", overwriting");
            std::filesystem::remove(_output_cache_path, ec);
        }
    }

    virtual ~CaptureAdaptedWriter() noexcept
    {
        if (std::error_code ec; std::filesystem::exists(_output_cache_path, ec))
        {
            EnsureOwnerAndPermissions(_output_cache_path, kStorageDefaultUid, kStorageDefaultGid,
                fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::group_write
                    | fs::perms::others_read | fs::perms::others_write,
                &this->ctxRef());
            std::filesystem::rename(_output_cache_path, _output_file_path, ec);
            if (ec)
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                    "CaptureAdaptedWriter failed to rename cache file from path ", _output_cache_path,
                    " to final output path ", _output_file_path, ": ", ec.message());
                std::filesystem::remove_all(_output_cache_path, ec);
            }
        }
        else
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                "CaptureAdaptedWriter cache file does not exist at path: ", _output_cache_path);
        }
        _capture.handleCompletedCaptureEvent(_event);
    }

protected:
    friend AdaptableWriter<CaptureAdaptedWriter<T>>;

    inline std::mutex &mediaMutexImpl() const noexcept
    {
        return this->ctxRef().capture_media_buffer.mutex;
    }

    inline std::deque<MediaFrame> &mediaQueueImpl() noexcept
    {
        return this->ctxRef().capture_media_buffer.queue;
    }

    inline std::filesystem::path getFilePathImpl() const noexcept
    {
        return _output_cache_path;
    }

    inline bool shouldFinishWritingImpl() const noexcept
    {
        return _capture.shouldFinishWriting();
    }

    inline bool shouldContinueWritingImpl() const noexcept
    {
        return _capture.shouldContinueWriting() && isRemainSpaceEnough();
    }

    inline void onThumbnailFrameImpl(const MediaFrame::FrameType &) noexcept { }

private:
    bool isRemainSpaceEnough() const noexcept
    {
        if (auto now = std::chrono::steady_clock::now();
            now - _last_stats_time < std::chrono::seconds(kCaptureWaterMarkBatch)) [[likely]]
            return true;
        else
            _last_stats_time = now;

        {
            struct statvfs stat;
            if (auto ret = ::statvfs(_event->output_directory.c_str(), &stat); ret != 0)
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                    "CaptureAdaptedWriter failed to get filesystem stats for path: ", _event->output_directory,
                    ", code: ", ret);
                return false;
            }
            const FileSize free_bytes = stat.f_bavail * stat.f_frsize;
            const FileSize required_bytes = this->settings().watermarks.critical_remain_bytes;
            if (free_bytes < required_bytes)
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                    "CaptureAdaptedWriter detected insufficient remaining space for path: ", _event->output_directory,
                    ", free bytes: ", free_bytes, ", required bytes: ", required_bytes);
                return false;
            }
            return true;
        }
    }

private:
    T &_capture;
    std::shared_ptr<CaptureEvent> _event;
    std::filesystem::path _output_file_path;
    std::filesystem::path _output_cache_path;
    mutable std::chrono::steady_clock::time_point _last_stats_time;
};

// - MARK: Image Writer

template<typename T>
class ImageWriter final: public T
{
public:
    template<typename... Args>
    ImageWriter(Args &&...args) : T(std::forward<Args>(args)...)
    {
        if (!this->ctxRef().c_api.media.image_capture_cb)
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception("ImageWriter initialized without image_capture_cb set");
        }
    }

    ~ImageWriter() override = default;

private:
    WriteEvent::Status waitUntilWriteFinish() noexcept override
    {
        if (!this->shouldContinueWriting()) [[unlikely]]
            return WriteEvent::Status::Canceled; // nothing was captured
        if (const auto ret = this->ctxRef().c_api.media.image_capture_cb(this->getFilePath().c_str(),
                this->settings().format.c_str(), this->ctxRef().c_api.media.user_data);
            ret != 0)
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "ImageWriter failed to capture image to file at path ",
                this->getFilePath(), ": ", static_cast<int>(ret));
            return WriteEvent::Status::Failed;
        }
        return WriteEvent::Status::Completed;
    }
};

// - MARK: Video Writer

template<typename T>
class VideoWriter final: public T
{
public:
    template<typename... Args>
    VideoWriter(Args &&...args)
        : T(std::forward<Args>(args)...), _end_time(this->settings().video_clip_length == std::chrono::seconds::max()
                                                        ? std::chrono::steady_clock::time_point::max()
                                                        : this->startTime() + this->settings().video_clip_length),
          _fd(-1), _is_clip_finish(false), _thumbnail()
    {
        if (!this->ctxRef().c_api.media.video_file_create_cb || !this->ctxRef().c_api.media.video_file_finish_cb)
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception("VideoWriter initialized without video file create/finish callbacks set");
        }
        if (!this->ctxRef().c_api.media.video_file_write_audio_frame_cb
            || !this->ctxRef().c_api.media.video_file_write_video_frame_cb)
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception("VideoWriter initialized with audio frame write callback set or without video frame "
                            "write callback set");
        }
    }

    ~VideoWriter() override
    {
        endWrite(); // fallback: the normal paths finish the sink before returning
    }

private:
    static inline constexpr const std::chrono::milliseconds kVideoFrameWaitDelay = std::chrono::milliseconds(10);

private:
    // Creates the sink. Called once the pre-check passed, so a closed gate never leaves an empty file behind.
    bool startWrite() noexcept
    {
        int fd = -1;
        if (const auto ret = this->ctxRef().c_api.media.video_file_create_cb(&fd, this->getFilePath().c_str(),
                this->settings().format.c_str(), this->ctxRef().c_api.media.user_data);
            ret != 0 || fd < 0) [[unlikely]]
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "VideoWriter failed to create video file at path ",
                this->getFilePath(), ", code: ", static_cast<int>(ret));
            return false;
        }
        _fd = fd;
        return true;
    }

    // Ends the task with `status` after finishing the sink; a finish failure fails the task instead.
    WriteEvent::Status finishWith(WriteEvent::Status status) noexcept
    {
        return endWrite() == WriteEvent::Status::Failed ? WriteEvent::Status::Failed : status;
    }

    // Finishes the sink at most once; later calls are no-ops.
    WriteEvent::Status endWrite() noexcept
    {
        if (_fd < 0)
            return WriteEvent::Status::Completed;
        const int fd = std::exchange(_fd, -1);
        if (auto ret = this->ctxRef().c_api.media.video_file_finish_cb(fd, this->ctxRef().c_api.media.user_data);
            ret != 0) [[unlikely]]
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to finish video file with fd ", fd,
                ", code: ", static_cast<int>(ret));
            return WriteEvent::Status::Failed;
        }
        this->onThumbnailFrame(_thumbnail);
        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_DEBUG, "VideoWriter finished writing file at path ",
            this->getFilePath());
        return WriteEvent::Status::Completed;
    }

    WriteEvent::Status waitUntilWriteFinish() noexcept override
    {
        MediaFrame frame;
        auto &ctx_media_mutex = this->mediaMutex();
        auto &ctx_media_queue = this->mediaQueue();

#ifdef __VIGIL_WRITER_WRITE_FRAME_INTERNAL__
#warning "__VIGIL_WRITER_WRITE_FRAME_INTERNAL__ is already defined, undefining to avoid conflicts"
#undef __VIGIL_WRITER_WRITE_FRAME_INTERNAL__
#endif
#define __VIGIL_WRITER_WRITE_FRAME_INTERNAL__                                                                          \
    if (!writeMediaFrame(frame)) [[unlikely]]                                                                          \
    {                                                                                                                  \
        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "VideoWriter failed to write media frame to file at path ",     \
            this->getFilePath());                                                                                      \
        return finishWith(WriteEvent::Status::Failed);                                                                 \
    }

        if (!this->shouldContinueWriting()) [[unlikely]]
            return WriteEvent::Status::Canceled; // no sink was created
        if (!startWrite()) [[unlikely]]
            return WriteEvent::Status::Failed;

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_DEBUG, "VideoWriter waiting for starting key frame, fd: ", _fd);
        for (;;)
        {
            if (isClipFinished()) [[unlikely]]
                return finishWith(WriteEvent::Status::Completed);

            ctx_media_mutex.lock();
            auto s = ctx_media_queue.begin();
            auto e = s;
            for (; e != ctx_media_queue.end(); ++e)
            {
                if (e->isKeyFrame())
                    break;
            }
            if (e == ctx_media_queue.end())
            {
                ctx_media_queue.erase(s, e);
                ctx_media_mutex.unlock();
                if (!this->shouldContinueWriting()) [[unlikely]]
                    return finishWith(WriteEvent::Status::Interrupted);
                std::this_thread::sleep_for(kVideoFrameWaitDelay);
                continue;
            }
            e->transferTo(frame);
            ctx_media_queue.erase(s, std::next(e, 1));
            ctx_media_mutex.unlock();

            __VIGIL_WRITER_WRITE_FRAME_INTERNAL__;

            _thumbnail = frame.frame;

            break;
        }

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_DEBUG, "VideoWriter started writing frames, fd: ", _fd);
        do
        {
            ctx_media_mutex.lock();
            auto it = ctx_media_queue.begin();
            if (it == ctx_media_queue.end())
            {
                ctx_media_mutex.unlock();
                std::this_thread::sleep_for(kVideoFrameWaitDelay);
                continue;
            }
            if (isClipFinished() && it->isKeyFrame()) [[unlikely]]
            {
                ctx_media_mutex.unlock();
                return finishWith(WriteEvent::Status::Completed); // the clip ends on the next key frame
            }
            it->transferTo(frame);
            ctx_media_queue.erase(it);
            ctx_media_mutex.unlock();

            __VIGIL_WRITER_WRITE_FRAME_INTERNAL__;
        }
        while (this->shouldContinueWriting());

#undef __VIGIL_WRITER_WRITE_FRAME_INTERNAL__

        return finishWith(WriteEvent::Status::Interrupted);
    }

    inline bool isClipFinished() noexcept
    {
        if (!_is_clip_finish)
            _is_clip_finish = this->shouldFinishWriting() || (std::chrono::steady_clock::now() >= _end_time);
        return _is_clip_finish;
    }

    inline bool writeMediaFrame(const MediaFrame &media_frame) noexcept
    {
        if (!media_frame.frame)
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_WARN, "Media frame is empty, skipping write");
            return false;
        }
        return std::visit(
            detail::Overloaded { [&](const VideoFrame &vf) {
                                    const auto video_frame = vf.toRef();
                                    const auto ret = this->ctxRef().c_api.media.video_file_write_video_frame_cb(_fd,
                                        &video_frame, this->ctxRef().c_api.media.user_data);
                                    if (ret != 0)
                                    {
                                        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                                            "Failed to write video frame to file with fd ", _fd,
                                            ", code: ", static_cast<int>(ret));
                                        return false;
                                    }
                                    return true;
                                },
                [&](const AudioFrame &af) {
                    const auto audio_frame = af.toRef();
                    const auto ret = this->ctxRef().c_api.media.video_file_write_audio_frame_cb(_fd, &audio_frame,
                        this->ctxRef().c_api.media.user_data);
                    if (ret != 0)
                    {
                        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to write audio frame to file with fd ",
                            _fd, ", code: ", static_cast<int>(ret));
                        return false;
                    }
                    return true;
                } },
            *media_frame.frame);
    }

private:
    std::chrono::steady_clock::time_point _end_time;
    int _fd;
    bool _is_clip_finish;
    MediaFrame::FrameType _thumbnail;
};

} // namespace vigil::writers

#endif
