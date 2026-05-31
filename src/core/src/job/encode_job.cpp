#include "utsure/core/job/encode_job.hpp"

#include "encode_job_working_set_guard.hpp"
#include "../runtime_anomaly_policy.hpp"
#include "../media/streaming_transcode_pipeline.hpp"
#include "utsure/core/filesystem/path_format.hpp"
#include "utsure/core/job/encode_job_metrics.hpp"
#include "utsure/core/job/output_naming.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace utsure::core::job {

namespace {

media::streaming::PipelineQueueLimits resolve_pipeline_queue_limits(const EncodeJob &job) {
    auto queue_limits = media::streaming::kDefaultPipelineQueueLimits;
    if (job.execution.video_frame_queue_depth_override.has_value()) {
        queue_limits.video_frame_queue_depth = *job.execution.video_frame_queue_depth_override;
    }

    return queue_limits;
}

media::streaming::StreamingRuntimeBehavior resolve_runtime_behavior(const EncodeJob &job) {
    return media::streaming::resolve_streaming_runtime_behavior(
        job.execution.threading,
        resolve_pipeline_queue_limits(job)
    );
}

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

    if (job.thumbnail_preroll.has_value() && job.thumbnail_preroll->enabled) {
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

void notify_final_progress(
    EncodeJobTelemetry &telemetry,
    std::string message,
    const std::int64_t encoded_video_frames = 0,
    const std::int64_t encoded_video_duration_us = 0,
    const std::int64_t encode_elapsed_us = 0
) {
    if (telemetry.observer == nullptr) {
        return;
    }

    const auto final_metrics =
        calculate_final_encode_metrics(encoded_video_frames, encoded_video_duration_us, encode_elapsed_us);
    telemetry.observer->on_progress(EncodeJobProgress{
        .stage = EncodeJobStage::completed,
        .current_step = telemetry.total_steps,
        .total_steps = telemetry.total_steps,
        .message = std::move(message),
        .overall_fraction = 1.0,
        .stage_fraction = 1.0,
        .encoded_video_frames = encoded_video_frames > 0
            ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(encoded_video_frames))
            : std::nullopt,
        .encoded_video_duration_us = encoded_video_duration_us > 0
            ? std::optional<std::int64_t>(encoded_video_duration_us)
            : std::nullopt,
        .encoded_fps = final_metrics.average_efps,
        .encoded_speed = final_metrics.average_speed
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

void notify_log_safely(
    EncodeJobTelemetry &telemetry,
    const EncodeJobLogLevel level,
    std::string message
) noexcept {
    try {
        notify_log(telemetry, level, std::move(message));
    } catch (...) {
    }
}

bool same_path_or_equivalent(const std::filesystem::path &left, const std::filesystem::path &right) {
    if (left.lexically_normal() == right.lexically_normal()) {
        return true;
    }

    std::error_code error{};
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    return !error && equivalent;
}

struct Crc32FinalizeResult final {
    std::filesystem::path output_path{};
    std::optional<std::string> warning{};
};

Crc32FinalizeResult finalize_crc32_suffix(
    EncodeJobTelemetry &telemetry,
    const std::filesystem::path &output_path
) {
    notify_log(telemetry, EncodeJobLogLevel::info, "Calculating CRC32 for completed output...");

    std::string crc_error{};
    const auto crc32_hex = OutputNaming::calculate_file_crc32_hex(output_path, &crc_error);
    if (!crc32_hex.has_value()) {
        const std::string warning = "CRC32 suffix was not added. " + crc_error;
        notify_log(
            telemetry,
            EncodeJobLogLevel::warning,
            warning
        );
        return Crc32FinalizeResult{.output_path = output_path, .warning = warning};
    }

    std::string crc_target_error{};
    const auto available_crc_output_path =
        OutputNaming::choose_available_crc32_suffix_path(output_path, *crc32_hex, &crc_target_error);
    if (!available_crc_output_path.has_value()) {
        const std::string warning = "CRC32 suffix was not added for target '" +
            filesystem::path_to_utf8_string(OutputNaming::append_or_replace_crc32_suffix(output_path, *crc32_hex)) +
            "'. " + crc_target_error;
        notify_log(telemetry, EncodeJobLogLevel::warning, warning);
        return Crc32FinalizeResult{.output_path = output_path, .warning = warning};
    }

    const std::filesystem::path crc_output_path = *available_crc_output_path;
    const std::filesystem::path direct_crc_output_path =
        OutputNaming::append_or_replace_crc32_suffix(output_path, *crc32_hex);
    if (crc_output_path.lexically_normal() != direct_crc_output_path.lexically_normal()) {
        notify_log(
            telemetry,
            EncodeJobLogLevel::warning,
            "CRC32 target filename already exists; using fallback output path '" +
                filesystem::path_to_utf8_string(crc_output_path) + "'."
        );
    }
    if (same_path_or_equivalent(output_path, crc_output_path)) {
        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            "CRC32 suffix already matched final output name: [" + *crc32_hex + "]."
        );
        return Crc32FinalizeResult{.output_path = output_path};
    }

    std::error_code rename_error{};
    std::filesystem::rename(output_path, crc_output_path, rename_error);
    if (rename_error) {
        const std::string warning =
            "CRC32 suffix was not added because the completed output could not be renamed from '" +
            filesystem::path_to_utf8_string(output_path) + "' to '" +
            filesystem::path_to_utf8_string(crc_output_path) + "': " + rename_error.message();
        notify_log(
            telemetry,
            EncodeJobLogLevel::warning,
            warning
        );
        return Crc32FinalizeResult{.output_path = output_path, .warning = warning};
    }

    notify_log(
        telemetry,
        EncodeJobLogLevel::info,
        "CRC32 suffix added: [" + *crc32_hex + "]."
    );
    return Crc32FinalizeResult{.output_path = crc_output_path};
}

EncodeJobResult make_error(
    const EncodeJob &job,
    const std::string &message,
    const std::string &actionable_hint,
    EncodeJobTelemetry *telemetry = nullptr,
    const bool canceled = false
) {
    if (telemetry != nullptr) {
        notify_log_safely(*telemetry, EncodeJobLogLevel::error, message);
        if (!actionable_hint.empty()) {
            notify_log_safely(*telemetry, EncodeJobLogLevel::error, "Hint: " + actionable_hint);
        }
    }

    return EncodeJobResult{
        .encode_job_summary = std::nullopt,
        .error = EncodeJobError{
            .main_source_path = filesystem::path_to_utf8_string(job.input.main_source_path),
            .output_path = filesystem::path_to_utf8_string(job.output.output_path),
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
        .main_source_trim_in_us = job.input.main_source_trim_in_us,
        .main_source_trim_out_us = job.input.main_source_trim_out_us,
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
        filesystem::path_to_utf8_string(source_path) + "'.";
}

std::string format_encode_log_message(const EncodeJob &job) {
    return "Encoding the streaming output as " + std::string(media::to_string(job.output.video.codec)) +
        " with preset '" + job.output.video.preset + "', CRF " + std::to_string(job.output.video.crf) +
        ", and audio mode '" + std::string(media::to_string(job.output.audio.mode)) + "'.";
}

std::string format_encode_runtime_log_message(const EncodeJob &job) {
    const auto runtime_behavior = resolve_runtime_behavior(job);
    return "Encoding runtime request: CPU mode " +
        std::string(media::to_string(job.execution.threading.cpu_usage_mode)) +
        ", encoder threads " +
        media::streaming::format_encoder_threading_summary(runtime_behavior, job.output.video.codec) +
        ", video workers " + std::to_string(runtime_behavior.video_processing_worker_count) +
        ", subtitle workers " + std::to_string(runtime_behavior.subtitle_processing_worker_count) +
        ", video queue " + std::to_string(runtime_behavior.video_frame_queue_depth) + " frames" +
        ", subtitle bitmap mode " + runtime_behavior.subtitle_bitmap_mode +
        ", subtitle composition mode " + runtime_behavior.subtitle_composition_mode +
        ", subtitle diagnostics " + runtime_behavior.subtitle_diagnostics_mode +
        ", priority " + std::string(to_display_string(job.execution.process_priority)) + '.';
}

std::string format_elapsed_microseconds(const std::int64_t elapsed_us) {
    if (elapsed_us <= 0) {
        return "unknown";
    }

    const auto total_milliseconds = elapsed_us / 1000;
    const auto milliseconds = total_milliseconds % 1000;
    const auto total_seconds = total_milliseconds / 1000;
    const auto seconds = total_seconds % 60;
    const auto total_minutes = total_seconds / 60;
    const auto minutes = total_minutes % 60;
    const auto hours = total_minutes / 60;

    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(2) << hours << ':'
           << std::setw(2) << minutes << ':'
           << std::setw(2) << seconds << '.'
           << std::setw(3) << milliseconds;
    return stream.str();
}

std::string format_decimal_or_dash(const std::optional<double> value, const int precision) {
    if (!value.has_value() || !std::isfinite(*value) || *value < 0.0) {
        return "-";
    }

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << *value;
    return stream.str();
}

std::string format_completion_metrics_log(
    const std::int64_t encoded_video_frames,
    const std::int64_t encoded_video_duration_us,
    const std::int64_t encode_elapsed_us
) {
    const auto metrics =
        calculate_final_encode_metrics(encoded_video_frames, encoded_video_duration_us, encode_elapsed_us);
    return "Encode completed: " + std::to_string(std::max<std::int64_t>(encoded_video_frames, 0)) +
        " frames in " + format_elapsed_microseconds(encode_elapsed_us) +
        ", average " + format_decimal_or_dash(metrics.average_efps, 1) + " EFPS, " +
        format_decimal_or_dash(metrics.average_speed, 2) + "x speed.";
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
    case EncodeJobLogLevel::warning:
        return "warning";
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

        if (job.thumbnail_preroll.has_value() && job.thumbnail_preroll->enabled) {
            notify_progress(
                telemetry,
                EncodeJobStage::burning_in_subtitles,
                "Preparing thumbnail pre-roll."
            );
            notify_log(
                telemetry,
                EncodeJobLogLevel::info,
                "Preparing the thumbnail pre-roll image and same-stem ASS overlay."
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
                .thumbnail_preroll_settings = &job.thumbnail_preroll,
                .media_encode_request = build_media_encode_request(job),
                .normalization_policy = options.decode_normalization_policy,
                .subtitle_renderer = subtitle_renderer.get(),
                .queue_limits = resolve_pipeline_queue_limits(job),
                .progress_callback = [&telemetry](const media::streaming::StreamingEncodeProgress &progress) {
                    notify_encode_progress(telemetry, progress);
                },
                .log_callback = [&telemetry](const std::string &message) {
                    notify_log(telemetry, EncodeJobLogLevel::info, message);
                },
                .warning_callback = [&telemetry](const std::string &message) {
                    notify_log(telemetry, EncodeJobLogLevel::warning, message);
                }
            }
        );
        if (!streaming_result.succeeded()) {
            return make_error(
                job,
                streaming_result.error->message,
                streaming_result.error->actionable_hint,
                &telemetry,
                streaming_result.error->canceled
            );
        }

        auto completed_summary = *streaming_result.summary;
        auto completed_job = job;
        std::vector<std::string> completion_warnings{};
        if (job.output.append_crc32_suffix) {
            const Crc32FinalizeResult crc_result = finalize_crc32_suffix(
                telemetry,
                completed_summary.encoded_media_summary.output_path
            );
            completed_summary.encoded_media_summary.output_path = crc_result.output_path;
            if (crc_result.warning.has_value()) {
                completion_warnings.push_back(*crc_result.warning);
            }
            completed_job.output.output_path = completed_summary.encoded_media_summary.output_path;
        }

        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            "Encode job completed successfully. Output written to '" +
                filesystem::path_to_utf8_string(completed_summary.encoded_media_summary.output_path) + "'."
        );
        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            format_completion_metrics_log(
                completed_summary.encoded_media_summary.encoded_video_frame_count,
                completed_summary.timeline_summary.output_duration_microseconds,
                completed_summary.performance_metrics.total_elapsed_microseconds
            )
        );
        notify_final_progress(
            telemetry,
            "Encode completed successfully.",
            completed_summary.encoded_media_summary.encoded_video_frame_count,
            completed_summary.timeline_summary.output_duration_microseconds,
            completed_summary.performance_metrics.total_elapsed_microseconds
        );

        return EncodeJobResult{
            .encode_job_summary = EncodeJobSummary{
                .job = completed_job,
                .inspected_input_info = timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info,
                .timeline_summary = completed_summary.timeline_summary,
                .decode_normalization_policy = options.decode_normalization_policy,
                .decoded_video_frame_count = completed_summary.decoded_video_frame_count,
                .decoded_audio_block_count = completed_summary.decoded_audio_block_count,
                .subtitled_video_frame_count = completed_summary.subtitled_video_frame_count,
                .streaming_runtime = EncodeJobStreamingRuntimeSummary{
                    .detected_logical_core_count =
                        completed_summary.runtime_behavior.detected_logical_core_count,
                    .effective_logical_core_count =
                        completed_summary.runtime_behavior.effective_logical_core_count,
                    .cpu_usage_mode = completed_summary.runtime_behavior.cpu_usage_mode,
                    .selected_video_decoder_thread_count =
                        completed_summary.runtime_behavior.selected_video_decoder_thread_count,
                    .selected_video_decoder_thread_type =
                        completed_summary.runtime_behavior.selected_video_decoder_thread_type,
                    .selected_video_encoder_thread_count =
                        completed_summary.runtime_behavior.selected_video_encoder_thread_count,
                    .selected_video_encoder_thread_type =
                        completed_summary.runtime_behavior.selected_video_encoder_thread_type,
                    .video_processing_worker_count =
                        completed_summary.runtime_behavior.video_processing_worker_count,
                    .subtitle_processing_worker_count =
                        completed_summary.runtime_behavior.subtitle_processing_worker_count,
                    .video_frame_queue_depth =
                        completed_summary.runtime_behavior.video_frame_queue_depth,
                    .decoded_audio_block_queue_depth =
                        completed_summary.runtime_behavior.decoded_audio_block_queue_depth,
                    .subtitle_bitmap_mode =
                        completed_summary.runtime_behavior.subtitle_bitmap_mode,
                    .subtitle_composition_mode =
                        completed_summary.runtime_behavior.subtitle_composition_mode,
                    .subtitle_diagnostics_mode =
                        completed_summary.runtime_behavior.subtitle_diagnostics_mode,
                    .video_decode_microseconds =
                        completed_summary.performance_metrics.video_decode.total_microseconds,
                    .video_process_microseconds =
                        completed_summary.performance_metrics.video_process.total_microseconds,
                    .subtitle_compose_microseconds =
                        completed_summary.performance_metrics.subtitle_compose.total_microseconds,
                    .video_encode_microseconds =
                        completed_summary.performance_metrics.video_encode.total_microseconds,
                    .total_elapsed_microseconds =
                        completed_summary.performance_metrics.total_elapsed_microseconds,
                    .average_output_fps =
                        completed_summary.performance_metrics.average_output_fps
                },
                .encoded_media_summary = completed_summary.encoded_media_summary,
                .warnings = std::move(completion_warnings)
            },
            .error = std::nullopt
        };
    } catch (const runtime_policy::RuntimeAnomalyError &exception) {
        return make_error(
            job,
            exception.what(),
            "classification=" + std::string(runtime_policy::to_string(exception.classification())),
            &telemetry
        );
    } catch (const std::exception &exception) {
        if (std::string_view(exception.what()) == kEncodeJobCanceledException) {
            return make_error(
                job,
                std::string(kEncodeJobCanceledMessage),
                "The active encode was canceled by the user. Any partial output opened by this encode was removed automatically.",
                &telemetry,
                true
            );
        }

        return make_error(
            job,
            runtime_policy::format_operation_message(
                runtime_policy::RuntimeAnomalyClass::unsafe_or_corrupt,
                "encode",
                "Encode job raised an unclassified runtime failure."
            ),
            exception.what(),
            &telemetry
        );
    } catch (...) {
        return make_error(
            job,
            runtime_policy::format_operation_message(
                runtime_policy::RuntimeAnomalyClass::unsafe_or_corrupt,
                "encode",
                "Encode job raised a non-standard runtime failure."
            ),
            "An unknown exception escaped the encode core boundary.",
            &telemetry
        );
    }
}

}  // namespace utsure::core::job
