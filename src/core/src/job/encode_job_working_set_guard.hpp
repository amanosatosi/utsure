#pragma once

#include "../media/streaming_transcode_pipeline.hpp"
#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <cstddef>
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

    const auto pipeline_budget = media::streaming::build_memory_budget(
        timeline_plan,
        normalization_policy,
        media::streaming::kDefaultPipelineQueueLimits,
        subtitle_settings.has_value()
    );
    if (!pipeline_budget.has_value()) {
        return std::nullopt;
    }

    const auto estimated_peak_bytes = pipeline_budget->estimated_peak_bytes;

    if (estimated_peak_bytes <= kMaxEstimatedPeakBytes) {
        return std::nullopt;
    }

    return WorkingSetLimitFailure{
        .estimated_peak_bytes = estimated_peak_bytes,
        .message =
            "The selected encode job is estimated to peak at about " +
            detail::format_bytes(estimated_peak_bytes) +
            " with the streaming pipeline queue depths, which exceeds the current safety limit of " +
            detail::format_bytes(kMaxEstimatedPeakBytes) + ".",
        .actionable_hint =
            "Lower the resolution, reduce the pipeline queue depths, or raise the safety limit "
            "only after verifying that the target system has enough memory headroom."
    };
}

}  // namespace utsure::core::job::working_set_guard
