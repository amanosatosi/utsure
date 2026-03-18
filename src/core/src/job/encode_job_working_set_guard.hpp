#pragma once

#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

namespace utsure::core::job::working_set_guard {

struct WorkingSetLimitFailure final {
    std::uint64_t estimated_peak_bytes{0};
    std::string message{};
    std::string actionable_hint{};
};

inline constexpr std::uint64_t kMaxEstimatedPeakBytes = 3ULL * 1024ULL * 1024ULL * 1024ULL;

namespace detail {

inline bool checked_add_u64(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t &result
) {
    if (left > (std::numeric_limits<std::uint64_t>::max() - right)) {
        return false;
    }

    result = left + right;
    return true;
}

inline bool checked_mul_u64(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t &result
) {
    if (left == 0U || right == 0U) {
        result = 0U;
        return true;
    }

    if (left > (std::numeric_limits<std::uint64_t>::max() / right)) {
        return false;
    }

    result = left * right;
    return true;
}

inline std::optional<long double> duration_seconds(
    const media::TimestampInfo &timestamps,
    const std::optional<std::int64_t> &container_duration_microseconds
) {
    if (timestamps.duration_pts.has_value() &&
        *timestamps.duration_pts > 0 &&
        timestamps.time_base.is_valid() &&
        timestamps.time_base.numerator > 0 &&
        timestamps.time_base.denominator > 0) {
        return static_cast<long double>(*timestamps.duration_pts) *
            static_cast<long double>(timestamps.time_base.numerator) /
            static_cast<long double>(timestamps.time_base.denominator);
    }

    if (container_duration_microseconds.has_value() && *container_duration_microseconds > 0) {
        return static_cast<long double>(*container_duration_microseconds) / 1000000.0L;
    }

    return std::nullopt;
}

inline std::optional<std::uint64_t> estimate_video_frame_count(
    const media::MediaSourceInfo &source_info
) {
    if (!source_info.primary_video_stream.has_value()) {
        return 0U;
    }

    const auto &video_stream = *source_info.primary_video_stream;
    if (video_stream.frame_count.has_value() && *video_stream.frame_count > 0) {
        return static_cast<std::uint64_t>(*video_stream.frame_count);
    }

    if (!video_stream.average_frame_rate.is_valid() ||
        video_stream.average_frame_rate.numerator <= 0 ||
        video_stream.average_frame_rate.denominator <= 0) {
        return std::nullopt;
    }

    const auto seconds = duration_seconds(video_stream.timestamps, source_info.container_duration_microseconds);
    if (!seconds.has_value() || *seconds <= 0.0L) {
        return std::nullopt;
    }

    const long double frame_count = *seconds *
        static_cast<long double>(video_stream.average_frame_rate.numerator) /
        static_cast<long double>(video_stream.average_frame_rate.denominator);
    if (frame_count <= 0.0L ||
        frame_count > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }

    return static_cast<std::uint64_t>(std::ceil(frame_count));
}

inline std::optional<std::uint64_t> estimate_video_bytes(
    const media::MediaSourceInfo &source_info,
    const media::DecodeNormalizationPolicy &normalization_policy
) {
    if (!source_info.primary_video_stream.has_value()) {
        return 0U;
    }

    if (normalization_policy.video_pixel_format != media::NormalizedVideoPixelFormat::rgba8) {
        return std::nullopt;
    }

    const auto &video_stream = *source_info.primary_video_stream;
    if (video_stream.width <= 0 || video_stream.height <= 0) {
        return std::nullopt;
    }

    const auto frame_count = estimate_video_frame_count(source_info);
    if (!frame_count.has_value()) {
        return std::nullopt;
    }

    std::uint64_t bytes_per_frame = 0;
    if (!checked_mul_u64(static_cast<std::uint64_t>(video_stream.width), 4U, bytes_per_frame)) {
        return std::nullopt;
    }

    if (!checked_mul_u64(bytes_per_frame, static_cast<std::uint64_t>(video_stream.height), bytes_per_frame)) {
        return std::nullopt;
    }

    std::uint64_t total_video_bytes = 0;
    if (!checked_mul_u64(*frame_count, bytes_per_frame, total_video_bytes)) {
        return std::nullopt;
    }

    return total_video_bytes;
}

inline std::optional<std::uint64_t> estimate_audio_bytes(
    const media::MediaSourceInfo &source_info,
    const media::DecodeNormalizationPolicy &normalization_policy
) {
    if (!source_info.primary_audio_stream.has_value()) {
        return 0U;
    }

    if (normalization_policy.audio_sample_format != media::NormalizedAudioSampleFormat::f32_planar) {
        return std::nullopt;
    }

    const auto &audio_stream = *source_info.primary_audio_stream;
    if (audio_stream.sample_rate <= 0 || audio_stream.channel_count <= 0) {
        return std::nullopt;
    }

    const auto seconds = duration_seconds(audio_stream.timestamps, source_info.container_duration_microseconds);
    if (!seconds.has_value() || *seconds <= 0.0L) {
        return std::nullopt;
    }

    const long double samples_per_channel = *seconds * static_cast<long double>(audio_stream.sample_rate);
    if (samples_per_channel <= 0.0L ||
        samples_per_channel > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }

    std::uint64_t total_samples = static_cast<std::uint64_t>(std::ceil(samples_per_channel));
    std::uint64_t total_audio_bytes = 0;
    if (!checked_mul_u64(total_samples, static_cast<std::uint64_t>(audio_stream.channel_count), total_audio_bytes)) {
        return std::nullopt;
    }

    if (!checked_mul_u64(total_audio_bytes, sizeof(float), total_audio_bytes)) {
        return std::nullopt;
    }

    return total_audio_bytes;
}

inline std::optional<std::uint64_t> estimate_segment_bytes(
    const media::MediaSourceInfo &source_info,
    const media::DecodeNormalizationPolicy &normalization_policy
) {
    const auto video_bytes = estimate_video_bytes(source_info, normalization_policy);
    const auto audio_bytes = estimate_audio_bytes(source_info, normalization_policy);
    if (!video_bytes.has_value() || !audio_bytes.has_value()) {
        return std::nullopt;
    }

    std::uint64_t total_segment_bytes = 0;
    if (!checked_add_u64(*video_bytes, *audio_bytes, total_segment_bytes)) {
        return std::nullopt;
    }

    return total_segment_bytes;
}

inline std::string format_bytes(const std::uint64_t bytes) {
    constexpr long double kBytesPerMiB = 1024.0L * 1024.0L;
    constexpr long double kBytesPerGiB = 1024.0L * 1024.0L * 1024.0L;

    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(2);
    if (bytes >= (1024ULL * 1024ULL * 1024ULL)) {
        formatted << (static_cast<long double>(bytes) / kBytesPerGiB) << " GiB";
    } else {
        formatted << (static_cast<long double>(bytes) / kBytesPerMiB) << " MiB";
    }

    return formatted.str();
}

}  // namespace detail

inline std::optional<WorkingSetLimitFailure> check(
    const timeline::TimelinePlan &timeline_plan,
    const std::optional<EncodeJobSubtitleSettings> &subtitle_settings,
    const media::DecodeNormalizationPolicy &normalization_policy
) {
    if (timeline_plan.segments.empty() || timeline_plan.main_segment_index >= timeline_plan.segments.size()) {
        return std::nullopt;
    }

    std::uint64_t total_segment_bytes = 0;
    std::uint64_t main_segment_bytes = 0;

    for (std::size_t index = 0; index < timeline_plan.segments.size(); ++index) {
        const auto segment_bytes =
            detail::estimate_segment_bytes(timeline_plan.segments[index].inspected_source_info, normalization_policy);
        if (!segment_bytes.has_value()) {
            return std::nullopt;
        }

        if (!detail::checked_add_u64(total_segment_bytes, *segment_bytes, total_segment_bytes)) {
            return std::nullopt;
        }

        if (index == timeline_plan.main_segment_index) {
            main_segment_bytes = *segment_bytes;
        }
    }

    std::uint64_t estimated_peak_bytes = total_segment_bytes;
    if (!detail::checked_add_u64(estimated_peak_bytes, total_segment_bytes, estimated_peak_bytes) ||
        !detail::checked_add_u64(estimated_peak_bytes, total_segment_bytes, estimated_peak_bytes)) {
        return std::nullopt;
    }

    if (subtitle_settings.has_value()) {
        const std::uint64_t subtitle_target_bytes =
            subtitle_settings->timing_mode == timeline::SubtitleTimingMode::full_output_timeline
                ? total_segment_bytes
                : main_segment_bytes;
        if (!detail::checked_add_u64(estimated_peak_bytes, subtitle_target_bytes, estimated_peak_bytes)) {
            return std::nullopt;
        }
    }

    if (estimated_peak_bytes <= kMaxEstimatedPeakBytes) {
        return std::nullopt;
    }

    return WorkingSetLimitFailure{
        .estimated_peak_bytes = estimated_peak_bytes,
        .message =
            "The selected encode job is estimated to require about " +
            detail::format_bytes(estimated_peak_bytes) +
            " of in-memory decoded media, which exceeds the current safety limit of " +
            detail::format_bytes(kMaxEstimatedPeakBytes) + ".",
        .actionable_hint =
            "The current pipeline still decodes and composes full clips in memory. "
            "Use a shorter clip, lower the resolution, split the job into smaller sections, "
            "or wait for the planned streaming pipeline work."
    };
}

}  // namespace utsure::core::job::working_set_guard
