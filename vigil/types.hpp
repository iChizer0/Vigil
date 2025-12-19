#pragma once
#ifndef VIGIL_TYPES_HPP
#define VIGIL_TYPES_HPP

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <string>
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

class EvaluationResult final
{
public:
    EvaluationResult() : _matched(false), _timestamp(0), _id() { }

    ~EvaluationResult() = default;

    inline bool matched() const noexcept
    {
        return _matched;
    }

    inline int64_t timestamp() const noexcept
    {
        return _timestamp;
    }

    std::string id() const noexcept
    {
        return _id;
    }

    inline operator bool() const noexcept
    {
        return _matched;
    }

    inline void setMatched(bool matched) noexcept
    {
        _matched = matched;
    }

    inline void setTimestamp(int64_t ts) noexcept
    {
        _timestamp = ts;
    }

    inline void setId(const std::string &id) noexcept
    {
        _id = id;
    }

    inline void setId(std::string &id) noexcept
    {
        _id = std::move(id);
    }

    inline void reset() noexcept
    {
        _matched = false;
        _timestamp = 0;
        _id.clear();
    }

private:
    bool _matched;
    int64_t _timestamp;
    std::string _id;
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
    LocalTime(std::chrono::system_clock::time_point utc_now = std::chrono::system_clock::now()) noexcept
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

    static LocalTime now() noexcept
    {
        return LocalTime();
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
        return _local_seconds_weekly == other._local_seconds_weekly;
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
    long _local_seconds_weekly;
};

class TimeRange final
{
public:
    TimeRange(const LocalTime &start, const LocalTime &end) noexcept : _start(start), _end(end) { }

    TimeRange(const TimeRange &other) noexcept : _start(other._start), _end(other._end) { }

    ~TimeRange() = default;

    bool operator&(const LocalTime &time) const noexcept
    {
        if (_start <= _end)
            return time >= _start && time <= _end;
        else
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
    int class_id;
    float confidence;
    T object;
};

struct Classification final
{
    int class_id;
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

            edge_table[y_start].push_back({ .y_max = y_end, .x = x_at_start, .inv_slope = inv_slope });

            min_y = std::min(min_y, y_start);
            max_y = std::max(max_y, y_end);
        }

        std::vector<Edge> active_edges;

        for (int y = min_y; y < max_y; ++y)
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

private:
    alignas(native_bytes::value) std::array<size_t, x_size::value * y_size::value> _data;
    size_t _area;
};

using FileSize = int64_t;

struct FileIndex final
{
    std::string name;
    std::filesystem::file_time_type creation_time;

    explicit FileIndex(const std::string &name_, const std::filesystem::file_time_type &creation_time_) noexcept
        : name(name_), creation_time(creation_time_)
    {
    }

    explicit FileIndex(const std::filesystem::path &path, const char *name_time_formt) : name(path.filename().string())
    {
        // Check if could parse the name as time_point
        if (name_time_formt)
        {
            std::istringstream ss(name);
            std::tm tm = {};
            ss >> std::get_time(&tm, name_time_formt);
            if (ss.fail())
                throw Exception("Failed to parse time from filename: " + name);
            // Strictly check that the formated time matches the entire filename
            if (!ss.eof())
                throw Exception("Filename contains extra characters after time: " + name);
            if (tm.tm_year < 0)
                throw Exception("Parsed year is invalid (zero value) from filename: " + name);
            const auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
            creation_time = std::chrono::file_clock::from_sys(tp);
        }
        else
            creation_time = std::filesystem::file_time_type::clock::now();
    }

    FileIndex(const FileIndex &other) noexcept = default;
    FileIndex &operator=(const FileIndex &other) noexcept = default;

    FileIndex(FileIndex &&other) noexcept = default;
    FileIndex &operator=(FileIndex &&other) noexcept = default;

    ~FileIndex() = default;

    inline bool operator==(const FileIndex &other) const noexcept
    {
        return name == other.name;
    }

    inline std::strong_ordering operator<=>(const FileIndex &other) const noexcept
    {
        if (creation_time < other.creation_time)
            return std::strong_ordering::less;
        else if (creation_time > other.creation_time)
            return std::strong_ordering::greater;

        if (name < other.name)
            return std::strong_ordering::less;
        else if (name > other.name)
            return std::strong_ordering::greater;

        return std::strong_ordering::equal;
    }
};

struct CacheFileInfo final
{
    std::filesystem::path cache_file_path;
    std::string target_dir_name;
    std::filesystem::file_time_type creation_time;
};

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
            return "Error";
        case SlotState::NotFormattedOrFormatUnsupported:
            return "NotFormattedOrFormatUnsupported";
        case SlotState::Formatting:
            return "Formatting";
        case SlotState::NotMounted:
            return "NotMounted";
        case SlotState::Mounted:
            return "Mounted";
        case SlotState::Configured:
            return "Configured";
        case SlotState::Indexing:
            return "Indexing";
        case SlotState::Ready:
            return "Ready";
        default:
            return "Unknown";
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

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;

    NonCopyable(NonCopyable &&) noexcept = default;
    NonCopyable &operator=(NonCopyable &&) noexcept = default;
};

struct BaseFrame: private NonCopyable<BaseFrame>
{
    int64_t pts;
    void *data;
    size_t bytes_len;

    BaseFrame(int64_t pts_, void *data_, size_t bytes_len_) : pts(pts_), data(data_), bytes_len(bytes_len_)
    {
        if (data_ && bytes_len_ > 0)
        {
            if (data = new (std::nothrow) uint8_t[bytes_len_]; data)
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
    mutable std::shared_ptr<std::variant<VideoFrame, AudioFrame>> frame;

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

struct FileWriterSettings
{
    enum class CaptureType { Video, Image };
    CaptureType type;
    std::string format;
    std::chrono::milliseconds interval;
    std::string_view file_name_prefix;
    std::chrono::seconds video_clip_length;

    FileWriterSettings(std::string fmt = kRecordWriterDefaultFormat,
        std::chrono::milliseconds itv = kRecordWriterDefaultIntervalMs,
        std::string_view fnp = kRecordWriterPrefixDefault, std::chrono::seconds vcl = kVideoClipLengthDefault)
        : type(CaptureType::Video), format(std::move(fmt)), interval(itv), file_name_prefix(fnp), video_clip_length(vcl)
    {
        if (format == "mp4")
            type = CaptureType::Video;
        else if (format == "jpg" || format == "raw")
            type = CaptureType::Image;
        else
            throw Exception("Unsupported file writer format: " + format);
    }

    FileWriterSettings(const FileWriterSettings &other) noexcept = default;
    FileWriterSettings &operator=(const FileWriterSettings &other) noexcept = default;

    FileWriterSettings(FileWriterSettings &&other) noexcept = default;
    FileWriterSettings &operator=(FileWriterSettings &&other) noexcept = default;

    ~FileWriterSettings() = default;

    bool operator==(const FileWriterSettings &other) const noexcept
    {
        return type == other.type && format == other.format && interval == other.interval;
    }
};

using FileWriterSettingsWithRevision = std::pair<size_t, FileWriterSettings>;

} // namespace vigil::types

#endif
