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
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "constants.hpp"
#include "context.hpp"
#include "index_manager.hpp"
#include "log.hpp"
#include "types.hpp"

namespace vigil::writers {

using namespace vigil::constants;
using namespace vigil::types;
using namespace vigil::index_manager;

// - MARK: Adaptable Writer

template<typename T>
class AdaptableWriter
{
public:
    AdaptableWriter(Context &ctx, FileWriterSettingsWithRevision settings, std::shared_ptr<WriteEvent> event)
        : _start_time(std::chrono::steady_clock::now()), _ctx(ctx), _settings(std::move(settings)),
          _event(std::move(event))
    {
        if (!_event)
            throw Exception("AdaptableWriter initialized with null event");
        setEventStatus(WriteEvent::Status::Writing);
    }

    virtual ~AdaptableWriter() noexcept = default;

    const std::shared_ptr<WriteEvent> &operator()() noexcept
    {
        if (!shouldContinueWriting())
        {
            setEventStatus(WriteEvent::Status::Interrupted);
            return _event;
        }
        setEventStatus(waitUntilWriteFinish());
        return _event;
    }

    inline std::chrono::steady_clock::time_point startTime() const noexcept
    {
        return _start_time;
    }

    inline std::chrono::steady_clock::time_point endTime() const noexcept
    {
        return _start_time + _settings.second.interval;
    }

    inline std::mutex &mediaMutex() const noexcept
    {
        return static_cast<const T *>(this)->mediaMutexImpl();
    }

    inline std::deque<MediaFrame> &mediaBuffer() noexcept
    {
        return static_cast<T *>(this)->mediaBufferImpl();
    }

    inline bool shouldContinueWriting() const noexcept
    {
        return !_ctx.media_paused.load(std::memory_order_relaxed)
               && static_cast<const T *>(this)->shouldContinueWritingImpl();
    }

    inline Context &ctxRef() const noexcept
    {
        return _ctx;
    }

    inline const FileWriterSettingsWithRevision &settings() const noexcept
    {
        return _settings;
    }

    inline void setEventStatus(WriteEvent::Status status) noexcept
    {
        _event->status.store(status, std::memory_order_release);
    }

    virtual WriteEvent::Status waitUntilWriteFinish() noexcept = 0; // Return true to indicate write is complete

protected:
    inline const std::shared_ptr<WriteEvent> &event() const noexcept
    {
        return _event;
    }

    inline std::filesystem::path getFilePath() const noexcept
    {
        return static_cast<const T *>(this)->getFilePathImpl();
    }

private:
    std::chrono::steady_clock::time_point _start_time;
    Context &_ctx;
    FileWriterSettingsWithRevision _settings;
    std::shared_ptr<WriteEvent> _event;
};

// - MARK: Slot Adapted Writer

template<typename T>
class SlotAdaptedWriter: public AdaptableWriter<SlotAdaptedWriter<T>>
{
public:
    template<typename... Args>
    SlotAdaptedWriter(T &slot, Args &&...args)
        : AdaptableWriter<SlotAdaptedWriter<T>>(std::forward<Args>(args)...), _slot(slot),
          _cache_file_info(this->event())
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
        _slot.addToCacheFileSyncQueue(_cache_file_info);
        _slot.writerCountRef().fetch_sub(1, std::memory_order_acq_rel);
        this->ctxRef().clearRuleEvent();
    }

protected:
    friend AdaptableWriter<SlotAdaptedWriter<T>>;

    inline std::mutex &mediaMutexImpl() const noexcept
    {
        return this->ctxRef().media_mutex;
    }

    inline std::deque<MediaFrame> &mediaBufferImpl() noexcept
    {
        return this->ctxRef().media_buffer;
    }

    inline std::filesystem::path getFilePathImpl() const noexcept
    {
        return _cache_file_info.path;
    }

    inline bool shouldContinueWritingImpl() const noexcept
    {
        return this->ctxRef().writer_settings_revision.load(std::memory_order_relaxed) == this->settings().first
               && _slot.isReadyToWrite();
    }

private:
    bool initPathsAndFileName()
    {
        const auto local_time = LocalTime(std::chrono::system_clock::now());
        auto tm = local_time.tm();
        std::stringstream ss;
        ss << std::put_time(&tm, kFirstLevelDirNameTimeFormat);
        _cache_file_info.first_level_dir_name = ss.str();

        const auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        _cache_file_info.creation_time = std::chrono::file_clock::from_sys(tp);

        const auto duration = this->startTime().time_since_epoch();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);
        ss << " " << std::put_time(&tm, kFileNamePrefixTimeFormat) << " " << micros.count() << "."
           << this->settings().second.format;

        const auto cache_dir = _slot.getRecordCachePath();
        if (!IndexManager::ensureDirectoryExist(cache_dir, this->ctxRef()))
            return false;
        auto fmt = std::string(this->settings().second.file_name_prefix);
        std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return std::tolower(c); });
        _cache_file_info.path = cache_dir / (fmt + ss.str());

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_DEBUG, "Initialized writer with 1st level directory '",
            _cache_file_info.first_level_dir_name, "', cache file path '", _cache_file_info.path, "'");
        return true;
    }

private:
    static inline constexpr const char *kFirstLevelDirNameTimeFormat = kStorageFirstLevelDirNameTimeFormat;
    static inline constexpr const char *kFileNamePrefixTimeFormat = "%H-%M-%S";

private:
    T &_slot;
    CacheFileInfo _cache_file_info;
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
        const auto ret = this->ctxRef().c_api.media.image_capture_cb(this->getFilePath().c_str(),
            this->settings().second.format.c_str(), this->ctxRef().c_api.media.user_data);
        if (ret != 0)
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
        : T(std::forward<Args>(args)...), _video_clip_length(this->settings().second.video_clip_length), _fd(-1),
          _is_clip_start(true), _is_clip_finish(false)
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

        startWrite();
    }

    ~VideoWriter() override
    {
        endWrite();
    }

private:
    static inline constexpr const std::chrono::milliseconds kVideoFrameWaitDelay = std::chrono::milliseconds(10);

private:
    void startWrite()
    {
        if (auto ret = this->ctxRef().c_api.media.video_file_create_cb(&_fd, this->getFilePath().c_str(),
                this->settings().second.format.c_str(), this->ctxRef().c_api.media.user_data);
            ret != 0 || _fd < 0)
        {
            this->setEventStatus(WriteEvent::Status::Failed);
            throw Exception("VideoWriter failed to create video file at path " + this->getFilePath().string() + ": "
                            + std::to_string(static_cast<int>(ret)));
        }
    }

    WriteEvent::Status endWrite() noexcept
    {
        if (_fd >= 0)
        {
            if (auto ret = this->ctxRef().c_api.media.video_file_finish_cb(_fd, this->ctxRef().c_api.media.user_data);
                ret != 0)
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to finish video file with fd ", _fd,
                    ", code: ", static_cast<int>(ret));
                return WriteEvent::Status::Failed;
            }
            _fd = -1;
        }
        return WriteEvent::Status::Completed;
    }

    WriteEvent::Status waitUntilWriteFinish() noexcept override
    {
        MediaFrame frame;
        auto &ctx_media_mutex = this->mediaMutex();
        auto &ctx_media_buffer = this->mediaBuffer();

        do
        {
            ctx_media_mutex.lock();
            auto it = ctx_media_buffer.begin();
            if (it == ctx_media_buffer.end())
            {
                ctx_media_mutex.unlock();
                std::this_thread::sleep_for(kVideoFrameWaitDelay);
                continue;
            }
            if (isClipFinished() && it->isKeyFrame())
            {
                ctx_media_mutex.unlock();
                return endWrite(); // Finish current clip on next key frame
            }
            if (_is_clip_start)
            {
                if (!it->isKeyFrame())
                {
                    ctx_media_buffer.pop_front();
                    ctx_media_mutex.unlock();
                    continue; // Discard frames until key frame is found
                }
                _is_clip_start = false;
            }
            it->transferTo(frame);
            ctx_media_buffer.erase(it);
            ctx_media_mutex.unlock();
            if (!writeMediaFrame(frame)) [[unlikely]]
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR,
                    "VideoWriter failed to write media frame to file at path ", this->getFilePath());
                endWrite();
                return WriteEvent::Status::Failed;
            }
        }
        while (this->shouldContinueWriting());

        return WriteEvent::Status::Interrupted;
    }

    inline bool isClipFinished() noexcept
    {
        if (!_is_clip_finish)
            _is_clip_finish = (std::chrono::steady_clock::now() - this->startTime()) >= _video_clip_length;
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
            [&](auto &&arg) -> bool {
                using FrameType = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<FrameType, VideoFrame>)
                {
                    const auto video_frame = arg.toRef();
                    const auto ret = this->ctxRef().c_api.media.video_file_write_video_frame_cb(_fd, &video_frame,
                        this->ctxRef().c_api.media.user_data);
                    if (ret != 0)
                    {
                        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to write video frame to file with fd ",
                            _fd, ", code: ", static_cast<int>(ret));
                        return false;
                    }
                    return true;
                }
                else if constexpr (std::is_same_v<FrameType, AudioFrame>)
                {
                    const auto audio_frame = arg.toRef();
                    const auto ret = this->ctxRef().c_api.media.video_file_write_audio_frame_cb(_fd, &audio_frame,
                        this->ctxRef().c_api.media.user_data);
                    if (ret != 0)
                    {
                        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to write audio frame to file with fd ",
                            _fd, ", code: ", static_cast<int>(ret));
                        return false;
                    }
                    return true;
                }
                else
                {
                    VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_WARN,
                        "Media frame is neither video nor audio frame, skipping write");
                    return false;
                }
            },
            *media_frame.frame);
    }

private:
    std::chrono::seconds _video_clip_length;
    int _fd;
    bool _is_clip_start;
    bool _is_clip_finish;
};

} // namespace vigil::writers

#endif
