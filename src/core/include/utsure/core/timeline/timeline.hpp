#pragma once

#include "utsure/core/media/decoded_media.hpp"
#include "utsure/core/media/media_info.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace utsure::core::timeline {

enum class TimelineSegmentKind : std::uint8_t {
    intro = 0,
    main,
    outro
};

enum class SubtitleTimingMode : std::uint8_t {
    main_segment_only = 0,
    full_output_timeline
};

struct TimelineAssemblyRequest final {
    std::optional<std::filesystem::path> intro_source_path{};
    std::filesystem::path main_source_path{};
    std::optional<std::int64_t> main_source_trim_in_us{};
    std::optional<std::int64_t> main_source_trim_out_us{};
    std::optional<std::filesystem::path> outro_source_path{};
    bool subtitles_present{false};
    SubtitleTimingMode subtitle_timing_mode{SubtitleTimingMode::main_segment_only};
};

struct TimelineSegmentPlan final {
    TimelineSegmentKind kind{TimelineSegmentKind::main};
    std::filesystem::path source_path{};
    media::MediaSourceInfo inspected_source_info{};
    std::int64_t source_trim_in_microseconds{0};
    std::optional<std::int64_t> source_trim_out_microseconds{};
    bool subtitles_enabled{false};

    [[nodiscard]] bool has_source_trim() const noexcept {
        return source_trim_in_microseconds > 0 || source_trim_out_microseconds.has_value();
    }
};

// Maps between three deliberately separate domains for one source stream:
// raw FFmpeg stream PTS, source/media time relative to the chosen media origin,
// and zero-based output time relative to the requested trim start.
struct SourceTimelineMapping final {
    // The common media origin expressed in this stream's time base. For audio this
    // is not necessarily the audio stream's first-sample PTS.
    std::int64_t source_origin_pts{0};
    media::Rational stream_time_base{};
    std::int64_t trim_start_microseconds{0};
    std::optional<std::int64_t> trim_end_microseconds{};

    [[nodiscard]] std::int64_t stream_pts_for_source_time(
        std::int64_t source_time_microseconds
    ) const;
    [[nodiscard]] std::int64_t source_time_for_stream_pts(
        std::int64_t source_pts
    ) const;
    [[nodiscard]] std::int64_t source_time_for_output_time(
        std::int64_t output_time_microseconds
    ) const;
    [[nodiscard]] bool source_interval_overlaps_encode_range(
        std::int64_t event_start_microseconds,
        std::int64_t event_end_microseconds
    ) const noexcept;
};

struct TimelineOutputVideoShape final {
    int width{0};
    int height{0};
    media::Rational sample_aspect_ratio{1, 1};
};

struct TimelinePlan final {
    std::vector<TimelineSegmentPlan> segments{};
    std::size_t main_segment_index{0};
    media::Rational output_video_time_base{};
    media::Rational output_frame_rate{};
    std::optional<TimelineOutputVideoShape> output_video_shape{};
    std::optional<media::AudioStreamInfo> output_audio_stream{};
};

struct TimelineSegmentSummary final {
    TimelineSegmentKind kind{TimelineSegmentKind::main};
    std::filesystem::path source_path{};
    std::int64_t start_microseconds{0};
    std::int64_t duration_microseconds{0};
    std::int64_t video_frame_count{0};
    std::int64_t audio_block_count{0};
    bool subtitles_enabled{false};
    bool inserted_silence{false};
};

struct TimelineCompositionSummary final {
    std::vector<TimelineSegmentSummary> segments{};
    media::Rational output_video_time_base{};
    media::Rational output_frame_rate{};
    std::optional<media::Rational> output_audio_time_base{};
    std::int64_t output_duration_microseconds{0};
    std::int64_t output_video_frame_count{0};
    std::int64_t output_audio_block_count{0};
};

struct TimelineAssemblyError final {
    std::string message{};
    std::string actionable_hint{};
};

struct TimelineAssemblyResult final {
    std::optional<TimelinePlan> timeline_plan{};
    std::optional<TimelineAssemblyError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct TimelineCompositionError final {
    std::string message{};
    std::string actionable_hint{};
};

struct TimelineCompositionOutput final {
    media::DecodedMediaSource decoded_media_source{};
    TimelineCompositionSummary timeline_summary{};
};

struct TimelineCompositionResult final {
    std::optional<TimelineCompositionOutput> output{};
    std::optional<TimelineCompositionError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

class TimelineAssembler final {
public:
    [[nodiscard]] static TimelineAssemblyResult assemble(const TimelineAssemblyRequest &request) noexcept;
};

class TimelineComposer final {
public:
    [[nodiscard]] static TimelineCompositionResult compose(
        const TimelinePlan &timeline_plan,
        const std::vector<media::DecodedMediaSource> &decoded_segments
    ) noexcept;
};

[[nodiscard]] const char *to_string(TimelineSegmentKind kind) noexcept;
[[nodiscard]] const char *to_string(SubtitleTimingMode mode) noexcept;

}  // namespace utsure::core::timeline
