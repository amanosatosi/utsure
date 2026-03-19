#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace utsure::core::media {

struct Rational final {
    std::int64_t numerator{0};
    std::int64_t denominator{0};

    [[nodiscard]] bool is_valid() const noexcept;
};

struct TimestampInfo final {
    Rational time_base{};
    std::optional<std::int64_t> start_pts{};
    std::optional<std::int64_t> duration_pts{};
};

struct VideoStreamInfo final {
    int stream_index{-1};
    std::string codec_name{};
    int width{0};
    int height{0};
    Rational sample_aspect_ratio{1, 1};
    std::string pixel_format_name{"unknown"};
    Rational average_frame_rate{};
    TimestampInfo timestamps{};
    std::optional<std::int64_t> frame_count{};
};

struct AudioStreamInfo final {
    int stream_index{-1};
    std::string codec_name{};
    std::string sample_format_name{"unknown"};
    int sample_rate{0};
    int channel_count{0};
    std::string channel_layout_name{"unknown"};
    TimestampInfo timestamps{};
    std::optional<std::int64_t> frame_count{};
};

struct MediaSourceInfo final {
    std::string input_name{};
    std::string container_format_name{};
    std::optional<std::int64_t> container_duration_microseconds{};
    std::optional<VideoStreamInfo> primary_video_stream{};
    std::optional<AudioStreamInfo> primary_audio_stream{};
};

}  // namespace utsure::core::media
