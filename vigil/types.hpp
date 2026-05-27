#pragma once
#ifndef VIGIL_TYPES_HPP
#define VIGIL_TYPES_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <ios>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "constants.hpp"
#include "vigil_ctypes.h"

namespace vigil::types {

using namespace vigil::constants;

class Exception: public std::exception
{
public:
    Exception(const std::string &msg) : _msg(msg) { }

    ~Exception() = default;

    virtual const char *what() const noexcept override
    {
        return _msg.c_str();
    }

private:
    std::string _msg;
};

struct WriteEvent
{
    enum class Status {
        Pending,
        Writing,
        Completed,
        Failed,
        Interrupted,
        Canceled,
    };

    static inline const char *StatusString(Status status) noexcept
    {
        switch (status)
        {
            case Status::Pending:
                return "PENDING";
            case Status::Writing:
                return "WRITING";
            case Status::Completed:
                return "COMPLETED";
            case Status::Failed:
                return "FAILED";
            case Status::Interrupted:
                return "INTERRUPTED";
            case Status::Canceled:
                return "CANCELED";
            default:
                return "UNKNOWN";
        }
    }

    alignas(kHardwareDestructiveInterferenceSize) mutable std::atomic<Status> status;
    const std::chrono::steady_clock::time_point timepoint;
    const int64_t timestamp;

    WriteEvent() : status(Status::Pending), timepoint(), timestamp(0) { }

    WriteEvent(std::chrono::steady_clock::time_point tp, int64_t ts)
        : status(Status::Pending), timepoint(tp), timestamp(ts)
    {
    }

    ~WriteEvent() = default;
};

struct EventOwner
{
    vg_rule_type_e type;
    std::string id;
    int64_t timestamp;

    EventOwner(vg_rule_type_e t, const std::string &i, int64_t ts) : type(t), id(i), timestamp(ts) { }

    ~EventOwner() = default;
};

template<typename T>
class Range final
{
public:
    Range() = default;

    Range(const T &lb, const T &ub) : _lb(lb), _ub(ub) { }

    ~Range() = default;

    inline bool operator&(const T &value) const
    {
        return value >= _lb && value <= _ub;
    }

    inline bool operator&(const Range<T> &other) const
    {
        return !(other._ub < _lb || other._lb > _ub);
    }

    inline const T &lb() const noexcept
    {
        return _lb;
    }

    inline const T &ub() const noexcept
    {
        return _ub;
    }

    inline bool operator==(const Range<T> &other) const noexcept
    {
        return _lb == other._lb && _ub == other._ub;
    }

private:
    T _lb;
    T _ub;
};

class LocalTime final
{
public:
    LocalTime(std::chrono::system_clock::time_point utc_now) noexcept
    {
        auto utc_now_t = std::chrono::system_clock::to_time_t(utc_now);
        _local_tm = localtime_r(&utc_now_t, &_local_tm_storage);

        updateWeeklySeconds();
    }

    LocalTime(int day, int hour, int minute, int second) noexcept
    {
        _local_tm_storage = {};
        _local_tm_storage.tm_wday = day;
        _local_tm_storage.tm_hour = hour;
        _local_tm_storage.tm_min = minute;
        _local_tm_storage.tm_sec = second;
        _local_tm = &_local_tm_storage;

        updateWeeklySeconds();
    }

    ~LocalTime() = default;

    static inline LocalTime now() noexcept
    {
        return LocalTime(std::chrono::system_clock::now());
    }

    std::tm tm() const noexcept
    {
        return _local_tm_storage;
    }

    std::strong_ordering operator<=>(const LocalTime &other) const noexcept
    {
        if (_local_seconds_weekly < other._local_seconds_weekly)
            return std::strong_ordering::less;
        else if (_local_seconds_weekly > other._local_seconds_weekly)
            return std::strong_ordering::greater;
        else
            return std::strong_ordering::equal;
    }

    bool operator==(const LocalTime &other) const noexcept
    {
        return std::memcmp(&_local_tm_storage, &other._local_tm_storage, sizeof(std::tm)) == 0;
    }

private:
    void updateWeeklySeconds() noexcept
    {
        _local_seconds_weekly = (_local_tm->tm_wday * 24 * 3600L) + (_local_tm->tm_hour * 3600L)
                                + (_local_tm->tm_min * 60L) + _local_tm->tm_sec;
    }

private:
    std::tm _local_tm_storage;
    std::tm *_local_tm;
    int64_t _local_seconds_weekly;
};

class TimeRange final
{
public:
    TimeRange(const LocalTime &start, const LocalTime &end) noexcept : _start(start), _end(end) { }

    TimeRange(const TimeRange &other) noexcept : _start(other._start), _end(other._end) { }

    ~TimeRange() = default;

    bool operator&(const LocalTime &time) const noexcept
    {
        if (_start < _end)
            return time >= _start && time <= _end;
        else // Looped when equal or start > end
            return time >= _start || time <= _end;
    }

    LocalTime start() const noexcept
    {
        return _start;
    }

    LocalTime end() const noexcept
    {
        return _end;
    }

    bool operator==(const TimeRange &other) const noexcept
    {
        return _start == other._start && _end == other._end;
    }

private:
    LocalTime _start;
    LocalTime _end;
};

template<typename T>
struct Point final
{
    using value_type = T;
    T x;
    T y;

    bool operator==(const Point<T> &other) const noexcept
    {
        return x == other.x && y == other.y;
    }
};

template<typename T>
struct BoundingBox final
{
    using value_type = T;
    Point<T> top_left;
    Point<T> bottom_right;
};

template<typename T>
struct Detection final
{
    using value_type = T::value_type;
    std::string class_name;
    float confidence;
    T object;
};

struct Classification final
{
    std::string class_name;
    float confidence;
};

using DetectionResult = Detection<BoundingBox<uint8_t>>;
using DetectionResultVec = std::vector<DetectionResult>;
using ClassificationResult = Classification;
using ClassificationResultVec = std::vector<ClassificationResult>;
using InferenceResult = std::pair<int64_t, std::variant<DetectionResultVec, ClassificationResultVec>>;

template<typename T>
struct Polygon final
{
    using value_type = T;
    std::vector<Point<T>> points;

    bool operator==(const Polygon<T> &other) const noexcept
    {
        return points == other.points;
    }
};

template<typename T>
struct ROI final
{
    using value_type = T;
    Polygon<T> polygon;

    bool operator==(const ROI<T> &other) const noexcept
    {
        return polygon.points == other.polygon.points;
    }
};

template<size_t W, size_t H>
class Mask final
{
public:
    static_assert(W > 0 && H > 0, "Width and height must be positive");
    using width = std::integral_constant<size_t, W>;
    using height = std::integral_constant<size_t, H>;

    using native_bytes = std::integral_constant<size_t, sizeof(size_t)>;
    using native_bits = std::integral_constant<size_t, native_bytes::value * 8>;
    using native_shift = std::integral_constant<size_t, std::countr_zero(native_bits::value)>;
    using native_mask = std::integral_constant<size_t, native_bits::value - 1>;

    using x_size = std::integral_constant<size_t, (W + native_bits::value - 1) / native_bits::value>;
    using y_size = std::integral_constant<size_t, H>;

public:
    template<typename T, std::enable_if_t<std::is_integral_v<T>, bool> = true>
    static std::unique_ptr<Mask<W, H>> from(const BoundingBox<T> &bbox)
    {
        auto p = std::make_unique<Mask<W, H>>();
        if (!p)
            return {};
        auto x1 = std::clamp(bbox.top_left.x, static_cast<T>(0), static_cast<T>(width::value - 1));
        auto y1 = std::clamp(bbox.top_left.y, static_cast<T>(0), static_cast<T>(height::value - 1));
        auto x2 = std::clamp(bbox.bottom_right.x, static_cast<T>(0), static_cast<T>(width::value - 1));
        auto y2 = std::clamp(bbox.bottom_right.y, static_cast<T>(0), static_cast<T>(height::value - 1));

        for (size_t y = y1; y <= y2; ++y)
        {
            for (size_t x = x1; x <= x2; ++x)
            {
                p->_data[(y * x_size::value) + (x >> native_shift::value)] |= static_cast<size_t>(1)
                                                                              << (x & native_mask::value);
            }
        }
        p->_area = x2 >= x1 && y2 >= y1 ? (x2 - x1 + 1) * (y2 - y1 + 1) : 0;

        return p;
    }

    template<typename T, std::enable_if_t<std::is_integral_v<T>, bool> = true>
    static std::unique_ptr<Mask<W, H>> from(const Polygon<T> &polygon)
    {
        auto p = std::make_unique<Mask<W, H>>();
        if (!p || polygon.points.size() < 3)
            return p;

        struct Edge final
        {
            int y_max;
            float x;
            float inv_slope;
        };

        std::vector<std::vector<Edge>> edge_table(height::value);
        T min_y = height::value - 1;
        T max_y = 0;
        for (size_t i = 0; i < polygon.points.size(); ++i)
        {
            const auto &p1 = polygon.points[i];
            const auto &p2 = polygon.points[(i + 1) % polygon.points.size()];
            if (p1.y == p2.y) // Ignore horizontal edges
                continue;

            const auto &start_pt = (p1.y < p2.y) ? p1 : p2;
            const auto &end_pt = (p1.y < p2.y) ? p2 : p1;

            T y_start = std::clamp(static_cast<T>(start_pt.y), static_cast<T>(0), static_cast<T>(height::value - 1));
            T y_end = std::clamp(static_cast<T>(end_pt.y), static_cast<T>(0), static_cast<T>(height::value - 1));
            if (y_start >= y_end)
                continue;

            float inv_slope = static_cast<float>(end_pt.x - start_pt.x) / static_cast<float>(end_pt.y - start_pt.y);
            float x_at_start = static_cast<float>(start_pt.x) + (y_start - start_pt.y) * inv_slope;

            edge_table[y_start].push_back(
                { .y_max = static_cast<int>(y_end), .x = x_at_start, .inv_slope = inv_slope });

            min_y = std::min(min_y, y_start);
            max_y = std::max(max_y, y_end);
        }

        std::vector<Edge> active_edges;

        for (int y = min_y; y < static_cast<int>(max_y); ++y)
        {
            // Add new edges from the edge table to the active list
            if (!edge_table[y].empty())
                active_edges.insert(active_edges.end(), edge_table[y].begin(), edge_table[y].end());

            // Remove edges that are no longer active
            active_edges.erase(std::remove_if(active_edges.begin(), active_edges.end(),
                                   [y](const Edge &edge) { return y >= edge.y_max; }),
                active_edges.end());

            // Sort active edges by their current x-intercept
            std::sort(active_edges.begin(), active_edges.end(), [](const Edge &a, const Edge &b) { return a.x < b.x; });

            // Fill pixels between pairs of intersections
            for (size_t i = 0; i + 1 < active_edges.size(); i += 2)
            {
                // Get the start and end x-coordinates for the span
                int x_start = std::round(active_edges[i].x);
                int x_end = std::round(active_edges[i + 1].x);

                x_start = std::clamp(x_start, 0, static_cast<int>(width::value - 1));
                x_end = std::clamp(x_end, 0, static_cast<int>(width::value - 1));

                for (int x = x_start; x < x_end; ++x)
                {
                    p->_data[(y * x_size::value) + (x >> native_shift::value)]
                        |= (static_cast<size_t>(1) << (x & native_mask::value));
                }
            }

            // Update x-coordinates for the next scanline
            for (auto &edge: active_edges)
            {
                edge.x += edge.inv_slope;
            }
        }

        size_t total_area = 0;
        for (const auto &word: p->_data)
        {
            total_area += std::popcount(word);
        }
        p->_area = total_area;

        return p;
    }

    template<typename T, std::enable_if_t<std::is_floating_point_v<T>, bool> = true>
    static std::unique_ptr<Mask<W, H>> from(const Polygon<T> &polygon)
    {
        using P = int;
        auto integral_polygon = Polygon<P> {};
        integral_polygon.points.reserve(polygon.points.size());
        for (const auto &pt: polygon.points)
        {
            P x = static_cast<int>(
                std::clamp(pt.x * static_cast<T>(width::value), static_cast<T>(0), static_cast<T>(width::value - 1)));
            P y = static_cast<int>(
                std::clamp(pt.y * static_cast<T>(height::value), static_cast<T>(0), static_cast<T>(height::value - 1)));
            integral_polygon.points.push_back({ x, y });
        }
        return from(integral_polygon);
    }

    static std::unique_ptr<Mask<W, H>> from(bool initial_value)
    {
        auto p = std::make_unique<Mask<W, H>>();
        if (!p)
            return {};
        if (initial_value)
        {
            std::fill(p->_data.begin(), p->_data.end(), std::numeric_limits<size_t>::max());
            p->_area = width::value * height::value;
        }
        return p;
    }

public:
    Mask() noexcept : _data {}, _area(0) { }

    ~Mask() = default;

    size_t area() const noexcept
    {
        return _area;
    }

    bool at(size_t x, size_t y) const noexcept
    {
        if (x >= width::value || y >= height::value) [[unlikely]]
            return false;
        return (_data[(y * x_size::value) + (x >> native_shift::value)] >> (x & native_mask::value) & 0x01) != 0;
    }

    Mask<W, H> &operator&=(const Mask<W, H> &other) noexcept
    {
        size_t area = 0;
        for (size_t i = 0; i < _data.size(); ++i)
        {
            auto &r = _data[i];
            auto v = r & other._data[i];
            area += std::popcount(v);
            r = v;
        }
        _area = area;
        return *this;
    }

    Mask<W, H> &operator|=(const Mask<W, H> &other) noexcept
    {
        size_t area = 0;
        for (size_t i = 0; i < _data.size(); ++i)
        {
            auto &r = _data[i];
            auto v = r | other._data[i];
            area += std::popcount(v);
            r = v;
        }
        _area = area;
        return *this;
    }

    Mask<W, H> &operator^=(const Mask<W, H> &other) noexcept
    {
        size_t area = 0;
        for (size_t i = 0; i < _data.size(); ++i)
        {
            auto &r = _data[i];
            auto v = r ^ other._data[i];
            area += std::popcount(v);
            r = v;
        }
        _area = area;
        return *this;
    }

    float operator&(const Mask<W, H> &other) const noexcept
    {
        if (_area == 0 || other._area == 0) [[unlikely]]
            return 0.0f;
        size_t inter = 0;
        for (size_t i = 0; i < _data.size(); ++i)
        {
            auto v = _data[i] & other._data[i];
            inter += std::popcount(v);
        }
        return static_cast<float>(inter) / static_cast<float>(_area);
    }

    std::string encoded() const
    {
        // For debugging, format int_str(height),int_str(width),hex_str(data_bytes...)
        std::string result;
        result.reserve(32 + _data.size() * native_bytes::value * 2);

        result += std::to_string(height::value);
        result += ',';
        result += std::to_string(width::value);
        result += ',';

        static constexpr char hex_chars[] = "0123456789abcdef";
        for (const auto &word: _data)
        {
            for (size_t i = 0; i < native_bytes::value; ++i)
            {
                uint8_t byte = static_cast<uint8_t>((word >> (i * 8)) & 0xFF);
                result += hex_chars[(byte >> 4) & 0x0F];
                result += hex_chars[byte & 0x0F];
            }
        }

        return result;
    }

private:
    alignas(native_bytes::value) std::array<size_t, x_size::value * y_size::value> _data;
    size_t _area;
};

using FileSize = int64_t;

enum class SlotState : int {
    Error = 1,
    NotFormattedOrFormatUnsupported = 2,
    Formatting = 3,
    NotMounted = 4,
    Mounted = 5,
    Configured = 6,
    Indexing = 7,
    Ready = 8,
};

inline std::string_view SlotStateString(SlotState state) noexcept
{
    switch (state)
    {
        case SlotState::Error:
            return "ERROR";
        case SlotState::NotFormattedOrFormatUnsupported:
            return "NOT_FORMATTED_OR_FORMAT_UNSUPPORTED";
        case SlotState::Formatting:
            return "FORMATTING";
        case SlotState::NotMounted:
            return "NOT_MOUNTED";
        case SlotState::Mounted:
            return "MOUNTED";
        case SlotState::Configured:
            return "CONFIGURED";
        case SlotState::Indexing:
            return "INDEXING";
        case SlotState::Ready:
            return "READY";
        default:
            return "UNKNOWN";
    }
}

struct SlotStats final
{
    SlotStats(FileSize size_bytes = 0, FileSize free_bytes = 0) : size_bytes(size_bytes), free_bytes(free_bytes) { }

    ~SlotStats() = default;

    FileSize size_bytes;
    FileSize free_bytes;
};

template<typename T>
struct NonCopyable
{
public:
    NonCopyable(const NonCopyable &) = delete;
    NonCopyable &operator=(const NonCopyable &) = delete;

    NonCopyable(NonCopyable &&) noexcept = default;
    NonCopyable &operator=(NonCopyable &&) noexcept = default;

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
};

struct ThumbnailBuffer final: public NonCopyable<ThumbnailBuffer>
{
    void *data;
    size_t size;
    const char *format;

    ThumbnailBuffer() noexcept : data(nullptr), size(0), format(nullptr) { }

    ~ThumbnailBuffer() noexcept
    {
        if (data)
        {
            delete[] static_cast<uint8_t *>(data);
            data = nullptr;
        }
    }

    inline operator bool() const noexcept
    {
        return data != nullptr && size > 0;
    }

    void transferTo(ThumbnailBuffer &other) noexcept
    {
        std::swap(other.data, data);
        std::swap(other.size, size);
        std::swap(other.format, format);
    }

    ThumbnailBuffer(ThumbnailBuffer &&other) noexcept : data(nullptr), size(0), format()
    {
        other.transferTo(*this);
    }

    ThumbnailBuffer &operator=(ThumbnailBuffer &&other) noexcept
    {
        if (this != &other)
            other.transferTo(*this);
        return *this;
    }
};

struct BaseFrame: public NonCopyable<BaseFrame>
{
    int64_t pts;
    void *data;
    size_t bytes_len;

    BaseFrame(int64_t pts_, void *data_, size_t bytes_len_) : pts(pts_), data(data_), bytes_len(bytes_len_)
    {
        if (data_ && bytes_len_ > 0)
        {
            if (data = new uint8_t[bytes_len_]; data)
                std::memcpy(data, data_, bytes_len_);
        }
    }

    ~BaseFrame() noexcept
    {
        if (data)
        {
            delete[] static_cast<uint8_t *>(data);
            data = nullptr;
        }
    }

    BaseFrame(BaseFrame &&other) noexcept : pts(other.pts), data(other.data), bytes_len(other.bytes_len)
    {
        other.data = nullptr;
        other.bytes_len = 0;
    }

    BaseFrame &operator=(BaseFrame &&other) noexcept
    {
        if (this != &other)
        {
            std::swap(pts, other.pts);
            std::swap(data, other.data);
            std::swap(bytes_len, other.bytes_len);
        }
        return *this;
    }
};

struct VideoFrame: public BaseFrame
{
    int key_frame;

    VideoFrame(const vg_video_frame_t &frame)
        : BaseFrame(frame.pts, frame.data, frame.bytes_len), key_frame(frame.key_frame)
    {
    }

    ~VideoFrame() noexcept = default;

    inline bool isKeyFrame() const noexcept
    {
        return key_frame == kKeyFrameTypeId;
    }

    vg_video_frame_t toRef() const noexcept
    {
        return vg_video_frame_t {
            .pts = pts,
            .data = data,
            .bytes_len = bytes_len,
            .key_frame = key_frame,
        };
    }

    bool cloneTo(ThumbnailBuffer &buffer) noexcept
    {
        if (data && bytes_len > 0)
        {
            if (buffer.data)
                return false;
            if (buffer.data = new (std::nothrow) uint8_t[bytes_len]; buffer.data)
            {
                std::memcpy(buffer.data, data, bytes_len);
                buffer.size = bytes_len;
                buffer.format = kRecordWriterDefaultFormat;
                return true;
            }
            return false;
        }
        return false;
    }

    VideoFrame(VideoFrame &&other) noexcept = default;
    VideoFrame &operator=(VideoFrame &&other) noexcept = default;

private:
    static inline const constexpr int kKeyFrameTypeId = 1;
};

struct AudioFrame: public BaseFrame
{
    AudioFrame(const vg_audio_frame_t &frame) : BaseFrame(frame.pts, frame.data, frame.bytes_len) { }

    ~AudioFrame() noexcept = default;

    vg_audio_frame_t toRef() const noexcept
    {
        return vg_audio_frame_t {
            .pts = pts,
            .data = data,
            .bytes_len = bytes_len,
        };
    }

    AudioFrame(AudioFrame &&other) noexcept = default;
    AudioFrame &operator=(AudioFrame &&other) noexcept = default;
};

struct MediaFrame
{
    std::chrono::steady_clock::time_point time_stamp;
    using FrameType = std::shared_ptr<std::variant<VideoFrame, AudioFrame>>;
    mutable FrameType frame;

    MediaFrame() : time_stamp(), frame() { }

    MediaFrame(const vg_video_frame_t &vf)
        : time_stamp(std::chrono::steady_clock::now()),
          frame(std::make_shared<std::variant<VideoFrame, AudioFrame>>(std::in_place_type<VideoFrame>, vf))
    {
    }

    MediaFrame(const vg_audio_frame_t &af)
        : time_stamp(std::chrono::steady_clock::now()),
          frame(std::make_shared<std::variant<VideoFrame, AudioFrame>>(std::in_place_type<AudioFrame>, af))
    {
    }

    ~MediaFrame() = default;

    inline bool isKeyFrame() const noexcept
    {
        if (!frame)
            return false;
        if (const auto vf = std::get_if<VideoFrame>(frame.get()); vf)
            return vf->isKeyFrame();
        return false;
    }

    inline void transferTo(MediaFrame &other) const noexcept
    {
        other.time_stamp = time_stamp;
        other.frame = std::move(frame);
    }

    MediaFrame(const MediaFrame &) = default;
    MediaFrame &operator=(const MediaFrame &) = default;

    MediaFrame(MediaFrame &&) noexcept = default;
    MediaFrame &operator=(MediaFrame &&) noexcept = default;

    struct CompareOlder
    {
        inline bool operator()(const MediaFrame &a, const MediaFrame &b) const noexcept
        {
            return a.time_stamp > b.time_stamp;
        }
    };
};

struct CacheFileInfo final
{
    std::filesystem::path path;
    std::string first_level_dir_name;
    std::filesystem::file_time_type creation_time;
    std::shared_ptr<WriteEvent> event;
    ThumbnailBuffer thumbnail;

    CacheFileInfo() : path(), first_level_dir_name(), creation_time(), event(), thumbnail() { }

    CacheFileInfo(std::shared_ptr<WriteEvent> evt)
        : path(), first_level_dir_name(), creation_time(), event(evt), thumbnail()
    {
    }

    ~CacheFileInfo() = default;
};

enum class CaptureType { Video, Image, Unknown };

inline CaptureType CaptureTypeFrom(const std::string &fmt) noexcept
{
    if (fmt == "MP4")
        return CaptureType::Video;
    else if (fmt == "JPG" || fmt == "RAW")
        return CaptureType::Image;
    else
        return CaptureType::Unknown;
}

struct WaterMarks
{
    FileSize critical_remain_bytes;
    FileSize high_remain_bytes;
    FileSize recycle_bytes;

    typedef int64_t (*media_max_bytes_estimate)(const char *format, size_t batch, void *user_data);

    static inline constexpr WaterMarks from(const std::string &format, size_t batch, const vg_config_media_t *media_capi) noexcept
    {
        size_t i = 0;
        for (; i < kFormatMaxBytesMap.size(); ++i)
        {
            if (kFormatMaxBytesMap[i].first == format)
                break;
        }
        FileSize limit = kFormatMaxBytesMap[i].second;
        batch = std::max(batch, static_cast<size_t>(1));
        limit *= batch;
        if (media_capi && media_capi->max_bytes_estimate)
        {
            if (const auto res = media_capi->max_bytes_estimate(format.c_str(), batch, media_capi->user_data); res > 0)
                limit = static_cast<FileSize>(res);
        }
        return WaterMarks {
            .critical_remain_bytes = static_cast<FileSize>(kStorageCriticalWaterMarkFactor * limit),
            .high_remain_bytes = static_cast<FileSize>(kStorageHighWaterMarkFactor * limit),
            .recycle_bytes = static_cast<FileSize>(kStorageRecycleFactor * limit),
        };
    }

    bool operator==(const WaterMarks &other) const noexcept
    {
        return critical_remain_bytes == other.critical_remain_bytes && high_remain_bytes == other.high_remain_bytes
               && recycle_bytes == other.recycle_bytes;
    }

    template<typename T, std::enable_if_t<std::is_unsigned_v<T>, bool> = true>
    WaterMarks &operator+=(const T &scalar) noexcept
    {
        critical_remain_bytes += scalar;
        high_remain_bytes += scalar;
        return *this;
    }

private:
    static inline constexpr const std::array<std::pair<std::string_view, FileSize>, 4> kFormatMaxBytesMap = {
        std::make_pair<std::string_view, FileSize>("MP4", 3 * 1024 * 1024),            // 3 MB per second of video
        std::make_pair<std::string_view, FileSize>("JPG", 10 * 1024 * 1024),           // 10 MB per image
        std::make_pair<std::string_view, FileSize>("RAW", 20 * 1024 * 1024),           // 20 MB per image
        std::make_pair<std::string_view, FileSize>("", kStorageMinFileSystemFreeBytes) // Fallback
    };
};

struct FileWriterSettings
{
    const CaptureType type;
    const std::string format;
    const std::chrono::milliseconds interval;
    const std::string_view file_name_prefix;
    const std::chrono::seconds video_clip_length;
    const WaterMarks watermarks;

    ~FileWriterSettings() = default;

    bool operator==(const FileWriterSettings &other) const noexcept
    {
        return type == other.type && format == other.format && interval == other.interval
               && file_name_prefix == other.file_name_prefix && video_clip_length == other.video_clip_length
               && watermarks == other.watermarks;
    }

    static std::shared_ptr<FileWriterSettings> create(CaptureType type, const std::string &fmt,
        std::chrono::milliseconds itv, std::string_view fnp, std::chrono::seconds vcl, WaterMarks wm) noexcept
    {
        return std::shared_ptr<FileWriterSettings>(new (std::nothrow) FileWriterSettings(type, fmt, itv, fnp, vcl, wm));
    }

private:
    FileWriterSettings(CaptureType tp, const std::string &fmt, std::chrono::milliseconds itv, std::string_view fnp,
        std::chrono::seconds vcl, WaterMarks wm) noexcept
        : type(tp), format(fmt), interval(itv), file_name_prefix(fnp), video_clip_length(vcl), watermarks(wm)
    {
    }
};

struct GPIOHelpers final
{
    static inline std::optional<std::string_view> StateToString(vg_gpio_pin_mod_e state) noexcept
    {
        switch (state)
        {
            case VG_GPIO_PIN_MODE_DISABLED:
                return "DISABLED";
            case VG_GPIO_PIN_MODE_INP_FLOATING:
                return "FLOATING";
            case VG_GPIO_PIN_MODE_INP_PULLUP:
                return "PULL_UP";
            case VG_GPIO_PIN_MODE_INP_PULLDOWN:
                return "PULL_DOWN";
            default:
                return std::nullopt;
        }
    }

    static inline std::optional<vg_gpio_pin_mod_e> StringToState(std::string_view state_str) noexcept
    {
        if (state_str == "DISABLED")
            return VG_GPIO_PIN_MODE_DISABLED;
        else if (state_str == "FLOATING")
            return VG_GPIO_PIN_MODE_INP_FLOATING;
        else if (state_str == "PULL_UP")
            return VG_GPIO_PIN_MODE_INP_PULLUP;
        else if (state_str == "PULL_DOWN")
            return VG_GPIO_PIN_MODE_INP_PULLDOWN;
        else
            return std::nullopt;
    }

    static inline std::optional<std::string_view> LevelToString(vg_gpio_pin_lvl_e level) noexcept
    {
        switch (level)
        {
            case VG_GPIO_PIN_LVL_LOW:
                return "LOW";
            case VG_GPIO_PIN_LVL_HIGH:
                return "HIGH";
            default:
                return std::nullopt;
        }
    }

    static inline std::optional<vg_gpio_pin_lvl_e> StringToLevel(std::string_view level_str) noexcept
    {
        if (level_str == "LOW")
            return VG_GPIO_PIN_LVL_LOW;
        else if (level_str == "HIGH")
            return VG_GPIO_PIN_LVL_HIGH;
        else
            return std::nullopt;
    }

    static inline std::optional<std::string_view> EventToString(vg_gpio_pin_evt_e event) noexcept
    {
        switch (event)
        {
            case VG_GPIO_PIN_EVT_RISING:
                return "RISING";
            case VG_GPIO_PIN_EVT_FALLING:
                return "FALLING";
            default:
                return std::nullopt;
        }
    }

    static inline std::optional<vg_gpio_pin_evt_e> StringToEvent(std::string_view event_str) noexcept
    {
        if (event_str == "RISING")
            return VG_GPIO_PIN_EVT_RISING;
        else if (event_str == "FALLING")
            return VG_GPIO_PIN_EVT_FALLING;
        else
            return std::nullopt;
    }
};

struct CaptureEvent
{
    const std::filesystem::path output_directory;
    const std::shared_ptr<FileWriterSettings> settings;
    const std::shared_ptr<WriteEvent> event;
    const std::string file_name;

    CaptureEvent(const std::filesystem::path &od, const std::shared_ptr<FileWriterSettings> &fws,
        std::shared_ptr<WriteEvent> evt, std::string fn)
        : output_directory(od), settings(fws), event(std::move(evt)), file_name(std::move(fn))
    {
        if (output_directory.empty())
            throw Exception("Output directory cannot be empty");
        if (!settings)
            throw Exception("File writer settings cannot be null");
        if (!event)
            throw Exception("Write event cannot be null");
        if (file_name.empty())
            throw Exception("File name cannot be empty");

        {
            std::ostringstream ss;
            ss << std::hex << event->timestamp << event->timepoint.time_since_epoch().count();
            _id = ss.str();
        }
    }

    const std::string &id() const noexcept
    {
        return _id;
    }

    ~CaptureEvent() = default;

private:
    std::string _id;
};

} // namespace vigil::types

#endif
