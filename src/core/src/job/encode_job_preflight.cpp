#include "utsure/core/job/encode_job_preflight.hpp"

#include "encode_job_working_set_guard.hpp"
#include "utsure/core/media/media_inspector.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace utsure::core::job {

namespace {

using utsure::core::subtitles::SubtitleRenderRequest;
using utsure::core::subtitles::SubtitleRenderSessionCreateRequest;

bool issues_contain_error(const std::vector<EncodeJobPreflightIssue> &issues) {
    return std::any_of(
        issues.begin(),
        issues.end(),
        [](const EncodeJobPreflightIssue &issue) {
            return issue.severity == EncodeJobPreflightIssueSeverity::error;
        }
    );
}

void append_issue(
    std::vector<EncodeJobPreflightIssue> &issues,
    const EncodeJobPreflightIssueSeverity severity,
    const EncodeJobPreflightIssueCode code,
    std::string message,
    std::string actionable_hint
) {
    issues.push_back(EncodeJobPreflightIssue{
        .severity = severity,
        .code = code,
        .message = std::move(message),
        .actionable_hint = std::move(actionable_hint)
    });
}

std::string normalize_path_key(const std::filesystem::path &path) {
    if (path.empty()) {
        return {};
    }

    std::error_code error;
    auto absolute_path = std::filesystem::absolute(path, error);
    if (error) {
        absolute_path = path;
    }

    auto normalized_path = absolute_path.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(
        normalized_path.begin(),
        normalized_path.end(),
        normalized_path.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
#endif
    return normalized_path;
}

bool paths_match(
    const std::filesystem::path &left,
    const std::filesystem::path &right
) {
    if (left.empty() || right.empty()) {
        return false;
    }

    std::error_code error;
    if (std::filesystem::exists(left, error) && !error) {
        std::error_code other_error;
        if (std::filesystem::exists(right, other_error) && !other_error) {
            std::error_code equivalent_error;
            const bool equivalent = std::filesystem::equivalent(left, right, equivalent_error);
            if (!equivalent_error) {
                return equivalent;
            }
        }
    }

    return normalize_path_key(left) == normalize_path_key(right);
}

void validate_existing_file(
    const std::filesystem::path &path,
    const EncodeJobPreflightIssueCode issue_code,
    const std::string &label,
    std::vector<EncodeJobPreflightIssue> &issues
) {
    if (path.empty()) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            issue_code,
            "The " + label + " path is empty.",
            "Choose a valid " + label + " file before starting the encode."
        );
        return;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error || !exists) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            issue_code,
            "The " + label + " file '" + path.lexically_normal().string() + "' was not found.",
            "Check the selected path and make sure the file still exists."
        );
        return;
    }

    error.clear();
    if (!std::filesystem::is_regular_file(path, error) || error) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            issue_code,
            "The " + label + " path '" + path.lexically_normal().string() + "' is not a regular file.",
            "Select a readable file instead of a directory or special path."
        );
    }
}

void validate_video_settings(
    const EncodeJob &job,
    std::vector<EncodeJobPreflightIssue> &issues
) {
    if (job.output.video.preset.empty()) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_video_settings,
            "The video preset is empty.",
            "Choose one of the supported presets before starting the encode."
        );
    }

    if (job.output.video.crf < 0 || job.output.video.crf > 51) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_video_settings,
            "The CRF value " + std::to_string(job.output.video.crf) + " is outside the supported 0-51 range.",
            "Choose a CRF between 0 and 51 before starting the encode."
        );
    }
}

void validate_audio_settings(
    const EncodeJob &job,
    std::vector<EncodeJobPreflightIssue> &issues
) {
    if ((job.output.audio.mode == media::AudioOutputMode::auto_select ||
         job.output.audio.mode == media::AudioOutputMode::encode_aac) &&
        job.output.audio.bitrate_kbps <= 0) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_audio_settings,
            "The audio bitrate must be positive.",
            "Choose a bitrate such as 192 kbps before starting the encode."
        );
    }

    if (job.output.audio.sample_rate_hz.has_value() && *job.output.audio.sample_rate_hz <= 0) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_audio_settings,
            "The selected output audio sample rate is not valid.",
            "Choose Auto or a positive sample rate."
        );
    }

    if (job.output.audio.channel_count.has_value() && *job.output.audio.channel_count <= 0) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_audio_settings,
            "The selected output audio channel count is not valid.",
            "Choose Auto or a positive channel count."
        );
    }
}

void validate_output_path(
    const EncodeJob &job,
    std::vector<EncodeJobPreflightIssue> &issues,
    bool &output_exists
) {
    const auto &output_path = job.output.output_path;
    output_exists = false;

    if (output_path.empty()) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_output_path,
            "The output path is empty.",
            "Choose a destination file before starting the encode."
        );
        return;
    }

    if (output_path.filename().empty()) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_output_path,
            "The output path '" + output_path.lexically_normal().string() + "' does not include a file name.",
            "Choose a full output file path, such as an .mp4 file."
        );
    }

    if (output_path.extension().empty()) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_output_path,
            "The output path '" + output_path.lexically_normal().string() + "' does not include a file extension.",
            "Use a file name with an extension such as .mp4."
        );
    }

    std::error_code error;
    if (std::filesystem::exists(output_path, error) && !error) {
        if (std::filesystem::is_directory(output_path, error) && !error) {
            append_issue(
                issues,
                EncodeJobPreflightIssueSeverity::error,
                EncodeJobPreflightIssueCode::invalid_output_path,
                "The output path '" + output_path.lexically_normal().string() + "' points to a directory.",
                "Choose a file path instead of a folder."
            );
            return;
        }

        output_exists = true;
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::warning,
            EncodeJobPreflightIssueCode::output_will_be_overwritten,
            "The output file already exists and will be overwritten.",
            "Confirm the overwrite before starting the encode, or choose a different output path."
        );
    }

    const auto conflicts_with_output = [&](const std::optional<std::filesystem::path> &candidate_path) {
        return candidate_path.has_value() && paths_match(output_path, *candidate_path);
    };

    if (paths_match(output_path, job.input.main_source_path) ||
        conflicts_with_output(job.input.intro_source_path) ||
        conflicts_with_output(job.input.outro_source_path) ||
        (job.subtitles.has_value() && paths_match(output_path, job.subtitles->subtitle_path))) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::invalid_output_path,
            "The output path matches one of the selected input files.",
            "Choose a separate output file so the source media and subtitle inputs are not overwritten."
        );
    }
}

timeline::TimelineAssemblyRequest build_timeline_request(const EncodeJob &job) {
    return timeline::TimelineAssemblyRequest{
        .intro_source_path = job.input.intro_source_path,
        .main_source_path = job.input.main_source_path,
        .outro_source_path = job.input.outro_source_path,
        .subtitles_present = job.subtitles.has_value(),
        .subtitle_timing_mode = job.subtitles.has_value()
            ? job.subtitles->timing_mode
            : timeline::SubtitleTimingMode::main_segment_only
    };
}

void validate_subtitle_session(
    const EncodeJob &job,
    const timeline::TimelinePlan &timeline_plan,
    std::vector<EncodeJobPreflightIssue> &issues
) {
    if (!job.subtitles.has_value()) {
        return;
    }

    const auto &main_segment_info = timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info;
    if (!main_segment_info.primary_video_stream.has_value()) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::subtitle_validation_failed,
            "Subtitle validation requires a readable main video stream.",
            "Fix the main source media issue before validating subtitle burn-in."
        );
        return;
    }

    auto subtitle_renderer = subtitles::create_default_subtitle_renderer();
    if (!subtitle_renderer) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::subtitle_validation_failed,
            "The default subtitle renderer could not be created for preflight validation.",
            "Verify that the libassmod-backed subtitle renderer is available in this build."
        );
        return;
    }

    const auto &video_stream = *main_segment_info.primary_video_stream;
    auto session_result = subtitle_renderer->create_session(SubtitleRenderSessionCreateRequest{
        .subtitle_path = job.subtitles->subtitle_path,
        .format_hint = job.subtitles->format_hint,
        .canvas_width = video_stream.width,
        .canvas_height = video_stream.height,
        .sample_aspect_ratio = video_stream.sample_aspect_ratio
    });
    if (!session_result.succeeded()) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::subtitle_validation_failed,
            "Failed to validate the subtitle file before encode. " + session_result.error->message,
            session_result.error->actionable_hint
        );
        return;
    }

    const auto render_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 0
    });
    if (!render_result.succeeded()) {
        append_issue(
            issues,
            EncodeJobPreflightIssueSeverity::error,
            EncodeJobPreflightIssueCode::subtitle_validation_failed,
            "Failed to render the subtitle file during preflight validation. " + render_result.error->message,
            render_result.error->actionable_hint
        );
    }
}

std::string format_rational(const media::Rational &value) {
    if (!value.is_valid()) {
        return "unknown";
    }

    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

}  // namespace

bool EncodeJobPreflightResult::can_start_encode() const noexcept {
    return !issues_contain_error(issues);
}

bool EncodeJobPreflightResult::has_warnings() const noexcept {
    return std::any_of(
        issues.begin(),
        issues.end(),
        [](const EncodeJobPreflightIssue &issue) {
            return issue.severity == EncodeJobPreflightIssueSeverity::warning;
        }
    );
}

bool EncodeJobPreflightResult::requires_output_overwrite_confirmation() const noexcept {
    return std::any_of(
        issues.begin(),
        issues.end(),
        [](const EncodeJobPreflightIssue &issue) {
            return issue.severity == EncodeJobPreflightIssueSeverity::warning &&
                issue.code == EncodeJobPreflightIssueCode::output_will_be_overwritten;
        }
    );
}

const char *to_string(const EncodeJobPreflightIssueSeverity severity) noexcept {
    switch (severity) {
    case EncodeJobPreflightIssueSeverity::warning:
        return "warning";
    case EncodeJobPreflightIssueSeverity::error:
        return "error";
    default:
        return "unknown";
    }
}

const char *to_string(const EncodeJobPreflightIssueCode code) noexcept {
    switch (code) {
    case EncodeJobPreflightIssueCode::invalid_main_source:
        return "invalid_main_source";
    case EncodeJobPreflightIssueCode::invalid_intro_source:
        return "invalid_intro_source";
    case EncodeJobPreflightIssueCode::invalid_outro_source:
        return "invalid_outro_source";
    case EncodeJobPreflightIssueCode::invalid_subtitle_source:
        return "invalid_subtitle_source";
    case EncodeJobPreflightIssueCode::invalid_output_path:
        return "invalid_output_path";
    case EncodeJobPreflightIssueCode::invalid_video_settings:
        return "invalid_video_settings";
    case EncodeJobPreflightIssueCode::invalid_audio_settings:
        return "invalid_audio_settings";
    case EncodeJobPreflightIssueCode::output_will_be_overwritten:
        return "output_will_be_overwritten";
    case EncodeJobPreflightIssueCode::working_set_limit_exceeded:
        return "working_set_limit_exceeded";
    case EncodeJobPreflightIssueCode::timeline_validation_failed:
        return "timeline_validation_failed";
    case EncodeJobPreflightIssueCode::subtitle_validation_failed:
        return "subtitle_validation_failed";
    default:
        return "unknown";
    }
}

std::string format_encode_job_preview(const EncodeJobPreviewSummary &preview_summary) {
    std::ostringstream preview;

    preview << preview_summary.segment_count << " segment";
    if (preview_summary.segment_count != 1U) {
        preview << 's';
    }
    preview << " (";

    for (std::size_t index = 0; index < preview_summary.segment_kinds.size(); ++index) {
        if (index > 0U) {
            preview << ", ";
        }
        preview << timeline::to_string(preview_summary.segment_kinds[index]);
    }
    preview << ")";

    if (preview_summary.main_source_info.primary_video_stream.has_value()) {
        const auto &video_stream = *preview_summary.main_source_info.primary_video_stream;
        preview << " | " << video_stream.width << 'x' << video_stream.height;
    }

    preview << " | " << format_rational(preview_summary.output_frame_rate) << " fps";
    preview << " | audio " << (preview_summary.output_audio_present ? "yes" : "no");
    preview << "\nSource audio: " << preview_summary.source_audio_summary;
    preview << "\nOutput audio: " << preview_summary.output_audio_summary;
    preview << " | subtitles ";
    if (preview_summary.subtitles_enabled) {
        preview << timeline::to_string(preview_summary.subtitle_timing_mode);
    } else {
        preview << "off";
    }
    preview << " | overwrite " << (preview_summary.output_exists ? "yes" : "no");
    preview << "\nEncoding runtime: encoder threads " << preview_summary.encoder_threading_summary;
    preview << " | video queue " << preview_summary.video_frame_queue_depth << " frames";
    preview << " | priority " << to_display_string(preview_summary.process_priority);

    return preview.str();
}

EncodeJobPreflightResult EncodeJobPreflight::inspect(const EncodeJob &job) noexcept {
    try {
        std::vector<EncodeJobPreflightIssue> issues{};

        validate_existing_file(
            job.input.main_source_path,
            EncodeJobPreflightIssueCode::invalid_main_source,
            "main source",
            issues
        );

        if (job.input.intro_source_path.has_value()) {
            validate_existing_file(
                *job.input.intro_source_path,
                EncodeJobPreflightIssueCode::invalid_intro_source,
                "intro clip",
                issues
            );
        }

        if (job.input.outro_source_path.has_value()) {
            validate_existing_file(
                *job.input.outro_source_path,
                EncodeJobPreflightIssueCode::invalid_outro_source,
                "outro clip",
                issues
            );
        }

        if (job.subtitles.has_value()) {
            validate_existing_file(
                job.subtitles->subtitle_path,
                EncodeJobPreflightIssueCode::invalid_subtitle_source,
                "subtitle",
                issues
            );
        }

        validate_video_settings(job, issues);
        validate_audio_settings(job, issues);

        bool output_exists = false;
        validate_output_path(job, issues, output_exists);

        if (issues_contain_error(issues)) {
            return EncodeJobPreflightResult{
                .preview_summary = std::nullopt,
                .issues = std::move(issues)
            };
        }

        const auto timeline_result = timeline::TimelineAssembler::assemble(build_timeline_request(job));
        if (!timeline_result.succeeded()) {
            append_issue(
                issues,
                EncodeJobPreflightIssueSeverity::error,
                EncodeJobPreflightIssueCode::timeline_validation_failed,
                "The selected clips are not compatible for this encode job. " + timeline_result.error->message,
                timeline_result.error->actionable_hint
            );

            return EncodeJobPreflightResult{
                .preview_summary = std::nullopt,
                .issues = std::move(issues)
            };
        }

        const auto &timeline_plan = *timeline_result.timeline_plan;
        validate_subtitle_session(job, timeline_plan, issues);

        const auto resolved_audio_output = media::resolve_audio_output_plan(media::AudioOutputResolveRequest{
            .output_path = job.output.output_path,
            .settings = job.output.audio,
            .segment_count = timeline_plan.segments.size(),
            .main_source_audio_stream = timeline_plan.segments[timeline_plan.main_segment_index]
                .inspected_source_info.primary_audio_stream.has_value()
                    ? &*timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info.primary_audio_stream
                    : nullptr
        });
        if (resolved_audio_output.requested_copy_blocker.has_value()) {
            append_issue(
                issues,
                EncodeJobPreflightIssueSeverity::error,
                EncodeJobPreflightIssueCode::invalid_audio_settings,
                "The selected audio copy mode cannot be used for this output.",
                *resolved_audio_output.requested_copy_blocker + " Use Auto or AAC instead."
            );
        }

        if (const auto working_set_failure = working_set_guard::check(
                timeline_plan,
                job.subtitles,
                {}
            ); working_set_failure.has_value()) {
            append_issue(
                issues,
                EncodeJobPreflightIssueSeverity::error,
                EncodeJobPreflightIssueCode::working_set_limit_exceeded,
                working_set_failure->message,
                working_set_failure->actionable_hint
            );
        }

        EncodeJobPreviewSummary preview_summary{
            .segment_count = timeline_plan.segments.size(),
            .segment_kinds = {},
            .main_source_info = timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info,
            .output_frame_rate = timeline_plan.output_frame_rate,
            .output_audio_present = resolved_audio_output.output_present,
            .source_audio_summary = media::format_source_audio_summary(
                timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info.primary_audio_stream
            ),
            .output_audio_summary = media::format_resolved_audio_output_summary(resolved_audio_output),
            .encoder_threading_summary = media::streaming::format_encoder_threading_summary(
                media::streaming::resolve_streaming_runtime_behavior(),
                job.output.video.codec
            ),
            .video_frame_queue_depth = media::streaming::kDefaultPipelineQueueLimits.video_frame_queue_depth,
            .process_priority = job.execution.process_priority,
            .subtitles_enabled = job.subtitles.has_value(),
            .subtitle_timing_mode = job.subtitles.has_value()
                ? job.subtitles->timing_mode
                : timeline::SubtitleTimingMode::main_segment_only,
            .output_exists = output_exists
        };
        preview_summary.segment_kinds.reserve(timeline_plan.segments.size());
        for (const auto &segment : timeline_plan.segments) {
            preview_summary.segment_kinds.push_back(segment.kind);
        }

        return EncodeJobPreflightResult{
            .preview_summary = std::move(preview_summary),
            .issues = std::move(issues)
        };
    } catch (const std::exception &exception) {
        return EncodeJobPreflightResult{
            .preview_summary = std::nullopt,
            .issues = {
                EncodeJobPreflightIssue{
                    .severity = EncodeJobPreflightIssueSeverity::error,
                    .code = EncodeJobPreflightIssueCode::timeline_validation_failed,
                    .message = "Encode preflight aborted because an unexpected exception was raised.",
                    .actionable_hint = exception.what()
                }
            }
        };
    }
}

}  // namespace utsure::core::job
