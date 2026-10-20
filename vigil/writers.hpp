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
#include "utils.hpp"

namespace vigil::writers {

using namespace vigil::constants;
using namespace vigil::types;
using namespace vigil::index_manager;
using namespace vigil::utils;

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

    // Runs the task to its terminal status: Canceled when nothing was recorded (the gate was already closed, or
    // no key frame arrived before the clip's wait, gate, or end), Completed, Failed, or Interrupted (a gate,
    // pause, stop, settings change, or input loss stopped a task that had started; its file is kept).
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

    // The writer stops when its channel's gate closes (rules disabled, or its own channel's recording settings
    // changed: a change to the other channel, to Sound, or to the watermarks does not stop it; docs/PLAN.md
    // sections 7.1 and 9) or when the Slot can no longer carry its task.
    inline bool shouldContinueWritingImpl() const noexcept
    {
        return this->ctxRef().file_channels.of(this->settings().type).isOpenFor(_settings_revision)
               && _slot.canContinue(this->settings().watermarks);
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
          _last_stats_time(std::chrono::steady_clock::now()), _reservation()
    {
        if (!_event)
            throw Exception("CaptureAdaptedWriter initialized with null event");

        if (_event->file_name.empty())
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception("CaptureAdaptedWriter initialized with empty output file name");
        }

        // Admission against the shared in-flight budget of the output filesystem (docs/PLAN.md section 9); the
        // reservation is held until this writer is destroyed, after the rename.
        if (!(_reservation = reserveBudget()))
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
        else if (this->event()->status.load(std::memory_order_acquire) == WriteEvent::Status::Canceled)
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_INFO,
                "CaptureAdaptedWriter recorded nothing, no file at path: ", _output_file_path);
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
    // The output filesystem's free space, minus what admitted tasks may still write there, must exceed this
    // capture's critical watermark; its own bytes are reserved in the same critical section.
    Context::InFlightBudgets::Reservation reserveBudget() const noexcept
    {
        struct statvfs stat;
        if (const auto ret = ::statvfs(_event->output_directory.c_str(), &stat); ret != 0)
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                "CaptureAdaptedWriter failed to get filesystem stats for path: ", _event->output_directory,
                ", code: ", ret);
            return {};
        }
        const auto filesystem = FilesystemIdOf(_event->output_directory);
        if (!filesystem)
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                "CaptureAdaptedWriter failed to identify the filesystem of path: ", _event->output_directory);
            return {};
        }
        const FileSize free_bytes = stat.f_bavail * stat.f_frsize;
        const auto &watermarks = this->settings().watermarks;
        auto &budgets = this->ctxRef().in_flight_budgets;
        auto reservation
            = budgets.tryReserve(*filesystem, watermarks.task_bytes, free_bytes, watermarks.critical_remain_bytes);
        if (!reservation)
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                "CaptureAdaptedWriter detected insufficient remaining space for path: ", _event->output_directory,
                ", free bytes: ", free_bytes, ", in flight: ", budgets.inFlight(*filesystem),
                ", required bytes: ", watermarks.critical_remain_bytes);
        return reservation;
    }

    // The running capture's own continuation check against its own critical watermark, once per batch period.
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
    Context::InFlightBudgets::Reservation _reservation;
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
    // The bounds of the waits on the media stream (docs/PLAN.md section 8.2): for a key frame, at the start of
    // a clip and at its end, and for any frame once the clip has started. Production uses the constants of
    // section 13; the tests inject shorter ones through setWaitBounds.
    struct WaitBounds
    {
        std::chrono::steady_clock::duration key_frame = kVideoKeyFrameWait;
        std::chrono::steady_clock::duration input_loss = kVideoInputLossWait;
    };

    template<typename... Args>
    VideoWriter(Args &&...args)
        : T(std::forward<Args>(args)...), _end_time(this->settings().video_clip_length == std::chrono::seconds::max()
                                                        ? std::chrono::steady_clock::time_point::max()
                                                        : this->startTime() + this->settings().video_clip_length),
          _fd(-1), _is_clip_finish(false), _thumbnail(), _waits()
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

    // Test seam: shorter bounds than the production constants. Call before the task runs.
    void setWaitBounds(WaitBounds bounds) noexcept
    {
        _waits = bounds;
    }

private:
    using Clock = std::chrono::steady_clock;

    static inline constexpr const std::chrono::milliseconds kVideoFrameWaitDelay = std::chrono::milliseconds(10);

private:
    // Creates the sink. Called with the first key frame in hand, so a clip that records nothing never leaves an
    // empty file behind.
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

    WriteEvent::Status failedWrite() noexcept
    {
        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "VideoWriter failed to write media frame to file at path ",
            this->getFilePath());
        return finishWith(WriteEvent::Status::Failed);
    }

    // Takes the first key frame out of the queue, dropping the frames before it; false when there is none yet.
    static bool TakeFirstKeyFrame(std::mutex &mutex, std::deque<MediaFrame> &queue, MediaFrame &frame) noexcept
    {
        const std::lock_guard lock(mutex);
        auto e = queue.begin();
        for (; e != queue.end() && !e->isKeyFrame(); ++e)
            ;
        if (e == queue.end())
        {
            queue.clear();
            return false;
        }
        e->transferTo(frame);
        queue.erase(queue.begin(), std::next(e));
        return true;
    }

    // Runs the clip (docs/PLAN.md section 8.2). Nothing is created before the first key frame: a clip that sees
    // none within the key frame wait, or whose gate closes or clip ends before one arrives, records nothing and
    // is Canceled rather than left as an empty file. From the first key frame the sink is created and frames
    // are written until the clip is finished (its length elapsed, or the adapter asks), when it ends on the next
    // key frame, which stays in the queue for the next clip, or at the key frame wait without one. No frame at
    // all for the input-loss wait ends a started clip as Interrupted, with what was written. Every wait checks
    // the gate, so a mode change, a pause, or a stop breaks it.
    WriteEvent::Status waitUntilWriteFinish() noexcept override
    {
        MediaFrame frame;
        auto &ctx_media_mutex = this->mediaMutex();
        auto &ctx_media_queue = this->mediaQueue();

        if (!this->shouldContinueWriting()) [[unlikely]]
            return WriteEvent::Status::Canceled; // no sink was created

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_DEBUG, "VideoWriter waiting for the starting key frame");
        for (const auto deadline = Clock::now() + _waits.key_frame;;)
        {
            if (TakeFirstKeyFrame(ctx_media_mutex, ctx_media_queue, frame))
                break;
            if (isClipFinished() || !this->shouldContinueWriting()) [[unlikely]]
                return WriteEvent::Status::Canceled; // nothing was recorded
            if (Clock::now() >= deadline) [[unlikely]]
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_WARN,
                    "VideoWriter saw no key frame within the key frame wait, nothing was recorded");
                return WriteEvent::Status::Canceled;
            }
            std::this_thread::sleep_for(kVideoFrameWaitDelay);
        }
        if (!startWrite()) [[unlikely]]
            return WriteEvent::Status::Failed;
        if (!writeMediaFrame(frame)) [[unlikely]]
            return failedWrite();
        _thumbnail = frame.frame;
        auto last_frame_time = Clock::now();

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_DEBUG, "VideoWriter started writing frames, fd: ", _fd);
        std::optional<Clock::time_point> closing_deadline; // the key frame wait, once the clip is finished
        do
        {
            ctx_media_mutex.lock();
            const auto it = ctx_media_queue.begin();
            const bool empty = it == ctx_media_queue.end();
            const auto now = Clock::now();
            if (isClipFinished()) [[unlikely]]
            {
                if (!closing_deadline)
                    closing_deadline = now + _waits.key_frame;
                if ((!empty && it->isKeyFrame()) || now >= *closing_deadline)
                {
                    ctx_media_mutex.unlock();
                    return finishWith(WriteEvent::Status::Completed); // the clip ends on the next key frame
                }
            }
            if (empty)
            {
                ctx_media_mutex.unlock();
                // A finished clip waits for its closing key frame or the key frame wait, not for input.
                if (!closing_deadline && now - last_frame_time >= _waits.input_loss) [[unlikely]]
                {
                    VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_WARN,
                        "VideoWriter received no frame within the input-loss wait, finishing the clip");
                    return finishWith(WriteEvent::Status::Interrupted);
                }
                std::this_thread::sleep_for(kVideoFrameWaitDelay);
                continue;
            }
            it->transferTo(frame);
            ctx_media_queue.erase(it);
            ctx_media_mutex.unlock();

            if (!writeMediaFrame(frame)) [[unlikely]]
                return failedWrite();
            last_frame_time = Clock::now();
        }
        while (this->shouldContinueWriting());

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
    WaitBounds _waits;
};

} // namespace vigil::writers

#endif
