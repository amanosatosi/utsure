#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace utsure::core::job {

enum class EncodeJobPreflightIssueSeverity : std::uint8_t {
    warning = 0,
    error
};

enum class EncodeJobPreflightIssueCode : std::uint8_t {
    invalid_main_source = 0,
    invalid_intro_source,
    invalid_outro_source,
    invalid_subtitle_source,
    invalid_output_path,
    invalid_video_settings,
    output_will_be_overwritten,
    timeline_validation_failed,
    subtitle_validation_failed
};

struct EncodeJobPreflightIssue final {
    EncodeJobPreflightIssueSeverity severity{EncodeJobPreflightIssueSeverity::error};
    EncodeJobPreflightIssueCode code{EncodeJobPreflightIssueCode::invalid_main_source};
    std::string message{};
    std::string actionable_hint{};
};

struct EncodeJobPreviewSummary final {
    std::size_t segment_count{0};
    std::vector<timeline::TimelineSegmentKind> segment_kinds{};
    media::MediaSourceInfo main_source_info{};
    media::Rational output_frame_rate{};
    bool output_audio_present{false};
    bool subtitles_enabled{false};
    timeline::SubtitleTimingMode subtitle_timing_mode{timeline::SubtitleTimingMode::main_segment_only};
    bool output_exists{false};
};

struct EncodeJobPreflightResult final {
    std::optional<EncodeJobPreviewSummary> preview_summary{};
    std::vector<EncodeJobPreflightIssue> issues{};

    [[nodiscard]] bool can_start_encode() const noexcept;
    [[nodiscard]] bool has_warnings() const noexcept;
    [[nodiscard]] bool requires_output_overwrite_confirmation() const noexcept;
};

class EncodeJobPreflight final {
public:
    [[nodiscard]] static EncodeJobPreflightResult inspect(const EncodeJob &job) noexcept;
};

[[nodiscard]] const char *to_string(EncodeJobPreflightIssueSeverity severity) noexcept;
[[nodiscard]] const char *to_string(EncodeJobPreflightIssueCode code) noexcept;
[[nodiscard]] std::string format_encode_job_preview(const EncodeJobPreviewSummary &preview_summary);

}  // namespace utsure::core::job
