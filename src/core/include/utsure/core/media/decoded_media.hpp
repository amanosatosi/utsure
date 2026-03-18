#pragma once

#include "utsure/core/media/media_info.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace utsure::core::media {

enum class NormalizedVideoPixelFormat : std::uint8_t {
    unknown = 0,
    rgba8
};

enum class NormalizedAudioSampleFormat : std::uint8_t {
    unknown = 0,
    f32_planar
};

enum class TimestampOrigin : std::uint8_t {
    decoded_pts = 0,
    best_effort_pts,
    stream_cursor
};

struct DecodeNormalizationPolicy final {
    NormalizedVideoPixelFormat video_pixel_format{NormalizedVideoPixelFormat::rgba8};
    NormalizedAudioSampleFormat audio_sample_format{NormalizedAudioSampleFormat::f32_planar};
    int audio_block_samples{1024};
};

struct MediaTimestamp final {
    Rational source_time_base{};
    std::optional<std::int64_t> source_pts{};
    std::optional<std::int64_t> source_duration{};
    TimestampOrigin origin{TimestampOrigin::stream_cursor};
    std::int64_t start_microseconds{0};
    std::optional<std::int64_t> duration_microseconds{};
};

struct VideoPlane final {
    int line_stride_bytes{0};
    int visible_width{0};
    int visible_height{0};
    std::vector<std::uint8_t> bytes{};
};

struct DecodedVideoFrame final {
    int stream_index{-1};
    std::int64_t frame_index{0};
    MediaTimestamp timestamp{};
    int width{0};
    int height{0};
    Rational sample_aspect_ratio{1, 1};
    NormalizedVideoPixelFormat pixel_format{NormalizedVideoPixelFormat::unknown};
    std::vector<VideoPlane> planes{};
};

struct DecodedAudioSamples final {
    int stream_index{-1};
    std::int64_t block_index{0};
    MediaTimestamp timestamp{};
    int sample_rate{0};
    int channel_count{0};
    std::string channel_layout_name{"unknown"};
    NormalizedAudioSampleFormat sample_format{NormalizedAudioSampleFormat::unknown};
    int samples_per_channel{0};
    std::vector<std::vector<float>> channel_samples{};
};

struct DecodedMediaSource final {
    MediaSourceInfo source_info{};
    DecodeNormalizationPolicy normalization_policy{};
    std::vector<DecodedVideoFrame> video_frames{};
    std::vector<DecodedAudioSamples> audio_blocks{};
};

[[nodiscard]] const char *to_string(NormalizedVideoPixelFormat pixel_format) noexcept;
[[nodiscard]] const char *to_string(NormalizedAudioSampleFormat sample_format) noexcept;
[[nodiscard]] const char *to_string(TimestampOrigin timestamp_origin) noexcept;

}  // namespace utsure::core::media
