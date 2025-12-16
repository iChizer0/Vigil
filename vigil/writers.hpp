#pragma once
#ifndef VIGIL_WRITERS_HPP
#define VIGIL_WRITERS_HPP

#include <atomic>
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

// - MARK: Base Writer

template<typename T>
class BaseWriter
{
public:
    BaseWriter(FileWriterSettingsWithRevision settings, T &slot)
        : _settings(std::move(settings)), _slot(slot), _start_time(std::chrono::steady_clock::now()), _cache_file_info()
    {
        if (!initPathsAndFileName())
            throw Exception("BaseWriter failed to initialize paths and file name");

        // After successful initialization, increment writer count
        _slot.writerCountRef().fetch_add(1, std::memory_order_acq_rel);
    }

    virtual ~BaseWriter() noexcept
    {
        _slot.addToCacheFileSyncQueue(_cache_file_info);
        _slot.writerCountRef().fetch_sub(1, std::memory_order_acq_rel);
    }

    bool tryFinishWrite() // Returns false to continue writing, true to finish write
    {
        if (!shouldContinueWriting())
            return true; // Stop requested or storage is ejected
        return onLoop();
    }

    inline std::chrono::steady_clock::time_point startTime() const noexcept
    {
        return _start_time;
    }

    inline std::chrono::milliseconds interval() const noexcept
    {
        return _settings.second.interval;
    }

protected:
    inline bool shouldContinueWriting() const noexcept
    {
        return !this->ctxRef().media_paused.load(std::memory_order_relaxed)
               && this->ctxRef().writer_settings_revision.load(std::memory_order_relaxed) == _settings.first
               && _slot.isReadyToWrite();
    }

    inline Context &ctxRef() const noexcept
    {
        return _slot.ctxRef();
    }

    inline const std::filesystem::path &cacheFilePath() const noexcept
    {
        return _cache_file_info.cache_file_path;
    }

    inline const std::string &format() const noexcept
    {
        return _settings.second.format;
    }

    virtual bool onLoop() = 0; // Return true to indicate write is complete

private:
    static inline constexpr const char *kFirstLevelDirNameTimeFormat = kStorageFirstLevelDirNameTimeFormat;
    static inline constexpr const char *kFileNamePrefixTimeFormat = "%H-%M-%S";

    bool initPathsAndFileName()
    {
        const auto local_time = LocalTime(std::chrono::system_clock::now());
        auto tm = local_time.tm();
        std::stringstream ss;
        ss << std::put_time(&tm, kFirstLevelDirNameTimeFormat);
        _cache_file_info.target_dir_name = ss.str();

        const auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        _cache_file_info.creation_time = std::chrono::file_clock::from_sys(tp);

        const auto duration = _start_time.time_since_epoch();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);
        ss << " " << std::put_time(&tm, kFileNamePrefixTimeFormat) << " " << micros.count() << "."
           << _settings.second.format;

        const auto cache_dir = _slot.getRecordCachePath();
        if (!IndexManager::ensureDirectoryExist(cache_dir, ctxRef()))
            return false;
        _cache_file_info.cache_file_path
            = _slot.getRecordCachePath() / (std::string(_settings.second.file_name_prefix) + ss.str());

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_DEBUG, "Initialized writer with target directory '",
            _cache_file_info.target_dir_name, "', cache file path '", _cache_file_info.cache_file_path, "'");
        return true;
    }

private:
    FileWriterSettingsWithRevision _settings;
    T &_slot;

    std::chrono::steady_clock::time_point _start_time;
    CacheFileInfo _cache_file_info;
};

// - MARK: Image Writer

template<typename T>
class ImageWriter final: public BaseWriter<T>
{
public:
    ImageWriter(FileWriterSettingsWithRevision settings, T &slot) : BaseWriter<T>(std::move(settings), slot)
    {
        if (!this->ctxRef().c_api.media.image_capture_cb)
            throw Exception("ImageWriter initialized without image_capture_cb set");
    }

    ~ImageWriter() override = default;

private:
    bool onLoop() override
    {
        const auto ret = this->ctxRef().c_api.media.image_capture_cb(this->cacheFilePath().c_str(),
            this->format().c_str(), this->ctxRef().c_api.media.user_data);
        if (ret != 0)
        {
            VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "ImageWriter failed to capture image to file at path ",
                this->cacheFilePath(), ": ", static_cast<int>(ret));
            return true; // Finish write on error
        }
        return true; // Finish write after single image capture
    }
};

// - MARK: Video Writer

template<typename T>
class VideoWriter final: public BaseWriter<T>
{
public:
    VideoWriter(FileWriterSettingsWithRevision settings, T &slot)
        : BaseWriter<T>(settings, slot), _video_clip_length(settings.second.video_clip_length), _fd(-1),
          _is_clip_start(true), _is_clip_finish(false)
    {
        if (!this->ctxRef().c_api.media.video_file_create_cb || !this->ctxRef().c_api.media.video_file_finish_cb)
            throw Exception("VideoWriter initialized without video file create/finish callbacks set");
        if (!this->ctxRef().c_api.media.video_file_write_audio_frame_cb
            || !this->ctxRef().c_api.media.video_file_write_video_frame_cb)
            throw Exception("VideoWriter initialized with audio frame write callback set or without video frame "
                            "write callback set");

        if (auto ret = this->ctxRef().c_api.media.video_file_create_cb(&_fd, this->cacheFilePath().c_str(),
                this->format().c_str(), this->ctxRef().c_api.media.user_data);
            ret != 0)
            throw Exception("VideoWriter failed to create video file at path " + this->cacheFilePath().string() + ": "
                            + std::to_string(static_cast<int>(ret)));
    }

    ~VideoWriter() override
    {
        if (_fd >= 0)
        {
            if (auto ret = this->ctxRef().c_api.media.video_file_finish_cb(_fd, this->ctxRef().c_api.media.user_data);
                ret != 0)
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to finish video file with fd ", _fd,
                    ", code: ", static_cast<int>(ret));
            _fd = -1;
        }
    }

private:
    static inline constexpr const std::chrono::milliseconds kVideoFrameWaitDelay = std::chrono::milliseconds(10);

private:
    bool onLoop() override
    {
        MediaFrame frame;
        do
        {
            this->ctxRef().media_mutex.lock();
            if (this->ctxRef().media_frame_buffer.empty())
            {
                this->ctxRef().media_mutex.unlock();
                std::this_thread::sleep_for(kVideoFrameWaitDelay);
                continue;
            }
            const auto &media_frame = this->ctxRef().media_frame_buffer.top();
            if (isClipFinished() && media_frame.isKeyFrame())
            {
                this->ctxRef().media_mutex.unlock();
                return true; // Finish current clip on next key frame
            }
            if (_is_clip_start)
            {
                if (!media_frame.isKeyFrame())
                {
                    this->ctxRef().media_frame_buffer.pop();
                    this->ctxRef().media_mutex.unlock();
                    continue; // Discard frames until key frame is found
                }
                _is_clip_start = false;
            }
            media_frame.moveTo(frame);
            this->ctxRef().media_frame_buffer.pop();
            this->ctxRef().media_mutex.unlock();
            break;
        }
        while (this->shouldContinueWriting());
        return !writeMediaFrame(frame);
    }

    inline bool isClipFinished() noexcept
    {
        if (!_is_clip_finish)
            _is_clip_finish = (std::chrono::steady_clock::now() - this->startTime()) >= _video_clip_length;
        return _is_clip_finish;
    }

    inline bool writeMediaFrame(const MediaFrame &media_frame)
    {
        if (std::holds_alternative<VideoFrame>(media_frame.frame))
        {
            const auto &vf = std::get<VideoFrame>(media_frame.frame);
            auto video_frame = vf.to();
            const auto ret = this->ctxRef().c_api.media.video_file_write_video_frame_cb(_fd, &video_frame,
                this->ctxRef().c_api.media.user_data);
            if (ret != 0)
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to write video frame to file with fd ", _fd,
                    ", code: ", static_cast<int>(ret));
                return false;
            }
            return true;
        }

        if (std::holds_alternative<AudioFrame>(media_frame.frame))
        {
            const auto &af = std::get<AudioFrame>(media_frame.frame);
            auto audio_frame = af.to();
            const auto ret = this->ctxRef().c_api.media.video_file_write_audio_frame_cb(_fd, &audio_frame,
                this->ctxRef().c_api.media.user_data);
            if (ret != 0)
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to write audio frame to file with fd ", _fd,
                    ", code: ", static_cast<int>(ret));
                return false;
            }
            return true;
        }

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_WARN, "Media frame is neither video nor audio frame, skipping write");
        return false;
    }

private:
    std::chrono::seconds _video_clip_length;
    int _fd;
    bool _is_clip_start;
    bool _is_clip_finish;
};

// - MARK: Passive Video Writer

template<typename T>
class PassiveVideoWriter final: public BaseWriter<T>
{
public:
    PassiveVideoWriter(FileWriterSettingsWithRevision &&settings, T &slot)
        : BaseWriter<T>(settings, slot), _video_clip_length(settings.second.video_clip_length), _fd(-1),
          _is_clip_start(true), _is_clip_finish(false)
    {
        if (!this->ctxRef().c_api.media.video_file_create_cb || !this->ctxRef().c_api.media.video_file_finish_cb)
            throw Exception("PassiveVideoWriter initialized without video file create/finish callbacks set");
        if (!this->ctxRef().c_api.media.video_file_write_audio_frame_cb
            || !this->ctxRef().c_api.media.video_file_write_video_frame_cb)
            throw Exception("PassiveVideoWriter initialized with audio frame write callback set or without video frame "
                            "write callback set");

        if (auto ret = this->ctxRef().c_api.media.video_file_create_cb(&_fd, this->cacheFilePath().c_str(),
                this->format().c_str(), this->ctxRef().c_api.media.user_data);
            ret != 0)
            throw Exception("PassiveVideoWriter failed to create video file at path " + this->cacheFilePath().string()
                            + ": " + std::to_string(static_cast<int>(ret)));
    }

    ~PassiveVideoWriter() override
    {
        if (_fd >= 0)
        {
            if (auto ret = this->ctxRef().c_api.media.video_file_finish_cb(_fd, this->ctxRef().c_api.media.user_data);
                ret != 0)
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to finish video file with fd ", _fd,
                    ", code: ", static_cast<int>(ret));
            _fd = -1;
        }
    }

private:
    static inline constexpr const std::chrono::milliseconds kVideoFrameWaitDelay = std::chrono::milliseconds(10);

private:
    bool onLoop() override
    {
        MediaFrame frame;
        do
        {
            this->ctxRef().media_mutex.lock();
            if (this->ctxRef().media_frame_buffer.empty())
            {
                this->ctxRef().media_mutex.unlock();
                std::this_thread::sleep_for(kVideoFrameWaitDelay);
                continue;
            }
            const auto &media_frame = this->ctxRef().media_frame_buffer.top();
            if (media_frame.time_stamp <= _last_frame_time) // No newer frame
            {
                this->ctxRef().media_mutex.unlock();
                std::this_thread::sleep_for(kVideoFrameWaitDelay);
                continue;
            }
            _last_frame_time = media_frame.time_stamp;
            if (isClipFinished() && media_frame.isKeyFrame()) // Finished or need to finish clip
            {
                this->ctxRef().media_mutex.unlock();
                return true; // Finish current clip on next key frame
            }
            if (_is_clip_start)
            {
                if (!media_frame.isKeyFrame())
                {
                    this->ctxRef().media_mutex.unlock();
                    std::this_thread::sleep_for(kVideoFrameWaitDelay);
                    continue;
                }
                _is_clip_start = false;
            }
            media_frame.copyTo(frame);
            this->ctxRef().media_mutex.unlock();
            break;
        }
        while (this->shouldContinueWriting());
        return !writeMediaFrame(frame);
    }

    inline bool isClipFinished() noexcept
    {
        if (!_is_clip_finish)
            _is_clip_finish = (std::chrono::steady_clock::now() - this->startTime()) >= _video_clip_length;
        return _is_clip_finish;
    }

    inline bool writeMediaFrame(const MediaFrame &media_frame)
    {
        if (std::holds_alternative<VideoFrame>(media_frame.frame))
        {
            const auto &vf = std::get<VideoFrame>(media_frame.frame);
            auto video_frame = vf.to();
            const auto ret = this->ctxRef().c_api.media.video_file_write_video_frame_cb(_fd, &video_frame,
                this->ctxRef().c_api.media.user_data);
            if (ret != 0)
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to write video frame to file with fd ", _fd,
                    ", code: ", static_cast<int>(ret));
                return false;
            }
            return true;
        }

        if (std::holds_alternative<AudioFrame>(media_frame.frame))
        {
            const auto &af = std::get<AudioFrame>(media_frame.frame);
            auto audio_frame = af.to();
            const auto ret = this->ctxRef().c_api.media.video_file_write_audio_frame_cb(_fd, &audio_frame,
                this->ctxRef().c_api.media.user_data);
            if (ret != 0)
            {
                VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_ERROR, "Failed to write audio frame to file with fd ", _fd,
                    ", code: ", static_cast<int>(ret));
                return false;
            }
            return true;
        }

        VG_LOG(this->ctxRef().log, VG_LOG_LEVEL_WARN, "Media frame is neither video nor audio frame, skipping write");
        return false;
    }

private:
    std::chrono::seconds _video_clip_length;
    int _fd;
    bool _is_clip_start;
    bool _is_clip_finish;
    std::chrono::steady_clock::time_point _last_frame_time;
};

} // namespace vigil::writers

#endif
