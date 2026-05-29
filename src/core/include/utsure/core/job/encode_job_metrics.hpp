#pragma once

#include <cstdint>
#include <optional>

namespace utsure::core::job {

struct FinalEncodeMetrics final {
    std::optional<double> average_efps{};
    std::optional<double> average_speed{};
};

[[nodiscard]] inline FinalEncodeMetrics calculate_final_encode_metrics(
    const std::int64_t encoded_video_frames,
    const std::int64_t encoded_video_duration_us,
    const std::int64_t encode_elapsed_us
) noexcept {
    FinalEncodeMetrics metrics{};
    if (encode_elapsed_us <= 0) {
        return metrics;
    }

    const double elapsed_seconds = static_cast<double>(encode_elapsed_us) / 1000000.0;
    if (encoded_video_frames > 0) {
        metrics.average_efps = static_cast<double>(encoded_video_frames) / elapsed_seconds;
    }

    if (encoded_video_duration_us > 0) {
        metrics.average_speed =
            (static_cast<double>(encoded_video_duration_us) / 1000000.0) / elapsed_seconds;
    }

    return metrics;
}

}  // namespace utsure::core::job
