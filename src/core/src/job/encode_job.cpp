#include "utsure/core/job/encode_job.hpp"

#include "encode_job_working_set_guard.hpp"
#include "../media/streaming_transcode_pipeline.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace utsure::core::job {

namespace {

struct EncodeJobTelemetry final {
    EncodeJobObserver *observer{nullptr};
    int total_steps{0};
    int current_step{0};
};

int calculate_total_steps(const EncodeJob &job) {
    int total_steps = 1 + 1 + 1;
    total_steps += 1;

    if (job.input.intro_source_path.has_value()) {
        ++total_steps;
    }

    if (job.input.outro_source_path.has_value()) {
        ++total_steps;
    }

    if (job.subtitles.has_value()) {
        ++total_steps;
    }

    return total_steps;
}

double clamp_fraction(const double value) {
    return std::clamp(value, 0.0, 1.0);
}

void notify_progress(
    EncodeJobTelemetry &telemetry,
    const EncodeJobStage stage,
    std::string message
) {
    if (telemetry.observer == nullptr) {
        return;
    }

    ++telemetry.current_step;
    telemetry.observer->on_progress(EncodeJobProgress{
        .stage = stage,
        .current_step = telemetry.current_step,
        .total_steps = telemetry.total_steps,
        .message = std::move(message)
    });
}

void notify_encode_progress(
    EncodeJobTelemetry &telemetry,
    const media::streaming::StreamingEncodeProgress &streaming_progress
) {
    if (telemetry.observer == nullptr) {
        return;
    }

    const double stage_fraction = clamp_fraction(streaming_progress.stage_fraction);
    const double overall_fraction = telemetry.total_steps > 0
        ? clamp_fraction(
            (static_cast<double>(std::max(telemetry.current_step - 1, 0)) + stage_fraction) /
            static_cast<double>(telemetry.total_steps)
        )
        : stage_fraction;

    telemetry.observer->on_progress(EncodeJobProgress{
        .stage = EncodeJobStage::encoding_output,
        .current_step = telemetry.current_step,
        .total_steps = telemetry.total_steps,
        .message = "Encoding output...",
        .overall_fraction = overall_fraction,
        .stage_fraction = stage_fraction,
        .encoded_video_frames = streaming_progress.encoded_video_frames > 0
            ? std::optional<std::uint64_t>(streaming_progress.encoded_video_frames)
            : std::nullopt,
        .total_video_frames = streaming_progress.total_video_frames > 0
            ? std::optional<std::uint64_t>(streaming_progress.total_video_frames)
            : std::nullopt,
        .encoded_video_duration_us = streaming_progress.encoded_video_duration_us > 0
            ? std::optional<std::int64_t>(streaming_progress.encoded_video_duration_us)
            : std::nullopt,
        .total_video_duration_us = streaming_progress.total_video_duration_us > 0
            ? std::optional<std::int64_t>(streaming_progress.total_video_duration_us)
            : std::nullopt,
        .encoded_fps = streaming_progress.encoded_fps
    });
}

void notify_final_progress(EncodeJobTelemetry &telemetry, std::string message) {
    if (telemetry.observer == nullptr) {
        return;
    }

    telemetry.observer->on_progress(EncodeJobProgress{
        .stage = EncodeJobStage::completed,
        .current_step = telemetry.total_steps,
        .total_steps = telemetry.total_steps,
        .message = std::move(message),
        .overall_fraction = 1.0
    });
}

void notify_log(
    EncodeJobTelemetry &telemetry,
    const EncodeJobLogLevel level,
    std::string message
) {
    if (telemetry.observer == nullptr) {
        return;
    }

    telemetry.observer->on_log(EncodeJobLogMessage{
        .level = level,
        .message = std::move(message)
    });
}

EncodeJobResult make_error(
    const EncodeJob &job,
    const std::string &message,
    const std::string &actionable_hint,
    EncodeJobTelemetry *telemetry = nullptr,
    const bool canceled = false
) {
    if (telemetry != nullptr) {
        notify_log(*telemetry, EncodeJobLogLevel::error, message);
        if (!actionable_hint.empty()) {
            notify_log(*telemetry, EncodeJobLogLevel::error, "Hint: " + actionable_hint);
        }
    }

    return EncodeJobResult{
        .encode_job_summary = std::nullopt,
        .error = EncodeJobError{
            .main_source_path = job.input.main_source_path.lexically_normal().string(),
            .output_path = job.output.output_path.lexically_normal().string(),
            .message = message,
            .actionable_hint = actionable_hint,
            .canceled = canceled
        }
    };
}

media::MediaEncodeRequest build_media_encode_request(const EncodeJob &job) {
    return media::MediaEncodeRequest{
        .output_path = job.output.output_path,
        .video_settings = {
            .codec = job.output.video.codec,
            .preset = job.output.video.preset,
            .crf = job.output.video.crf
        },
        .audio_settings = job.output.audio,
        .threading = job.execution.threading
    };
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

std::string format_segment_log_message(
    const timeline::TimelineSegmentKind kind,
    const std::filesystem::path &source_path
) {
    return "Decoding the " + std::string(timeline::to_string(kind)) + " segment from '" +
        source_path.lexically_normal().string() + "'.";
}

std::string format_encode_log_message(const EncodeJob &job) {
    return "Encoding the streaming output as " + std::string(media::to_string(job.output.video.codec)) +
        " with preset '" + job.output.video.preset + "', CRF " + std::to_string(job.output.video.crf) +
        ", and audio mode '" + std::string(media::to_string(job.output.audio.mode)) + "'.";
}

std::string format_encode_runtime_log_message(const EncodeJob &job) {
    const auto runtime_behavior = media::streaming::resolve_streaming_runtime_behavior(job.execution.threading);
    return "Encoding runtime request: CPU mode " +
        std::string(media::to_string(job.execution.threading.cpu_usage_mode)) +
        ", encoder threads " +
        media::streaming::format_encoder_threading_summary(runtime_behavior, job.output.video.codec) +
        ", video workers " + std::to_string(runtime_behavior.video_processing_worker_count) +
        ", video queue " + std::to_string(runtime_behavior.video_frame_queue_depth) +
        " frames, priority " + std::string(to_display_string(job.execution.process_priority)) + '.';
}

}  // namespace

void EncodeJobObserver::on_progress(const EncodeJobProgress & /*progress*/) {}

void EncodeJobObserver::on_log(const EncodeJobLogMessage & /*message*/) {}

bool EncodeJobResult::succeeded() const noexcept {
    return encode_job_summary.has_value() && !error.has_value();
}

const char *to_string(const EncodeJobStage stage) noexcept {
    switch (stage) {
    case EncodeJobStage::assembling_timeline:
        return "assembling_timeline";
    case EncodeJobStage::decoding_segment:
        return "decoding_segment";
    case EncodeJobStage::burning_in_subtitles:
        return "burning_in_subtitles";
    case EncodeJobStage::composing_timeline:
        return "composing_timeline";
    case EncodeJobStage::encoding_output:
        return "encoding_output";
    case EncodeJobStage::completed:
        return "completed";
    default:
        return "unknown";
    }
}

const char *to_string(const EncodeJobLogLevel level) noexcept {
    switch (level) {
    case EncodeJobLogLevel::info:
        return "info";
    case EncodeJobLogLevel::error:
        return "error";
    default:
        return "unknown";
    }
}

const char *to_string(const EncodeJobProcessPriority priority) noexcept {
    switch (priority) {
    case EncodeJobProcessPriority::high:
        return "high";
    case EncodeJobProcessPriority::above_normal:
        return "above_normal";
    case EncodeJobProcessPriority::normal:
        return "normal";
    case EncodeJobProcessPriority::below_normal:
        return "below_normal";
    case EncodeJobProcessPriority::low:
        return "low";
    default:
        return "unknown";
    }
}

const char *to_display_string(const EncodeJobProcessPriority priority) noexcept {
    switch (priority) {
    case EncodeJobProcessPriority::high:
        return "High";
    case EncodeJobProcessPriority::above_normal:
        return "Above Normal";
    case EncodeJobProcessPriority::normal:
        return "Normal";
    case EncodeJobProcessPriority::below_normal:
        return "Below Normal";
    case EncodeJobProcessPriority::low:
        return "Low";
    default:
        return "Unknown";
    }
}

EncodeJobResult EncodeJobRunner::run(const EncodeJob &job, const EncodeJobRunOptions &options) noexcept {
    EncodeJobTelemetry telemetry{
        .observer = options.observer,
        .total_steps = calculate_total_steps(job),
        .current_step = 0
    };

    try {
        notify_progress(
            telemetry,
            EncodeJobStage::assembling_timeline,
            "Inspecting the selected clips and assembling the output timeline."
        );
        notify_log(telemetry, EncodeJobLogLevel::info, "Assembling the encode timeline.");

        const auto timeline_assembly_result = timeline::TimelineAssembler::assemble(build_timeline_request(job));
        if (!timeline_assembly_result.succeeded()) {
            return make_error(
                job,
                timeline_assembly_result.error->message,
                timeline_assembly_result.error->actionable_hint,
                &telemetry
            );
        }

        const auto &timeline_plan = *timeline_assembly_result.timeline_plan;
        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            "Timeline assembled with " + std::to_string(timeline_plan.segments.size()) + " segment(s)."
        );

        if (const auto working_set_failure = working_set_guard::check(
                timeline_plan,
                job.subtitles,
                options.decode_normalization_policy
            ); working_set_failure.has_value()) {
            return make_error(
                job,
                working_set_failure->message,
                working_set_failure->actionable_hint,
                &telemetry
            );
        }

        std::unique_ptr<subtitles::SubtitleRenderer> subtitle_renderer{};

        const auto ensure_subtitle_renderer = [&]() -> std::optional<EncodeJobResult> {
            if (subtitle_renderer) {
                return std::nullopt;
            }

            subtitle_renderer = subtitles::create_default_subtitle_renderer();
            if (subtitle_renderer) {
                return std::nullopt;
            }

            return make_error(
                job,
                "Failed to initialize the default subtitle renderer.",
                "Verify that the libassmod-backed subtitle renderer is available before burn-in.",
                &telemetry
            );
        };

        for (const auto &segment_plan : timeline_plan.segments) {
            notify_progress(
                telemetry,
                EncodeJobStage::decoding_segment,
                "Decoding the " + std::string(timeline::to_string(segment_plan.kind)) + " segment."
            );
            notify_log(telemetry, EncodeJobLogLevel::info, format_segment_log_message(segment_plan.kind, segment_plan.source_path));
        }

        if (job.subtitles.has_value()) {
            notify_progress(
                telemetry,
                EncodeJobStage::burning_in_subtitles,
                "Rendering and compositing subtitles per frame during streaming encode."
            );
            notify_log(
                telemetry,
                EncodeJobLogLevel::info,
                "Preparing the subtitle renderer for streaming frame composition."
            );

            if (auto renderer_error = ensure_subtitle_renderer(); renderer_error.has_value()) {
                return *renderer_error;
            }
        }

        notify_progress(
            telemetry,
            EncodeJobStage::composing_timeline,
            "Streaming intro/main/outro segments through the bounded-memory pipeline."
        );
        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            "Streaming demux, decode, subtitle/composite, encode, and mux stages with bounded queues."
        );

        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            format_encode_log_message(job)
        );
        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            format_encode_runtime_log_message(job)
        );
        notify_progress(
            telemetry,
            EncodeJobStage::encoding_output,
            "Encoding and muxing the final output file incrementally."
        );

        const auto streaming_result = media::streaming::StreamingTranscoder::transcode(
            media::streaming::StreamingTranscodeRequest{
                .timeline_plan = &timeline_plan,
                .subtitle_settings = &job.subtitles,
                .media_encode_request = build_media_encode_request(job),
                .normalization_policy = options.decode_normalization_policy,
                .subtitle_renderer = subtitle_renderer.get(),
                .progress_callback = [&telemetry](const media::streaming::StreamingEncodeProgress &progress) {
                    notify_encode_progress(telemetry, progress);
                },
                .log_callback = [&telemetry](const std::string &message) {
                    notify_log(telemetry, EncodeJobLogLevel::info, message);
                }
            }
        );
        if (!streaming_result.succeeded()) {
            return make_error(
                job,
                streaming_result.error->message,
                streaming_result.error->actionable_hint,
                &telemetry
            );
        }

        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            "Encode job completed successfully. Output written to '" +
                streaming_result.summary->encoded_media_summary.output_path.lexically_normal().string() + "'."
        );
        notify_final_progress(telemetry, "Encode completed successfully.");

        return EncodeJobResult{
            .encode_job_summary = EncodeJobSummary{
                .job = job,
                .inspected_input_info = timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info,
                .timeline_summary = streaming_result.summary->timeline_summary,
                .decode_normalization_policy = options.decode_normalization_policy,
                .decoded_video_frame_count = streaming_result.summary->decoded_video_frame_count,
                .decoded_audio_block_count = streaming_result.summary->decoded_audio_block_count,
                .subtitled_video_frame_count = streaming_result.summary->subtitled_video_frame_count,
                .encoded_media_summary = streaming_result.summary->encoded_media_summary
            },
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        if (std::string_view(exception.what()) == kEncodeJobCanceledException) {
            return make_error(
                job,
                std::string(kEncodeJobCanceledMessage),
                "The active encode was canceled by the user. Any partial output may need to be deleted manually.",
                &telemetry,
                true
            );
        }

        return make_error(
            job,
            "Encode job aborted because an unexpected exception was raised.",
            exception.what(),
            &telemetry
        );
    }
}

}  // namespace utsure::core::job
