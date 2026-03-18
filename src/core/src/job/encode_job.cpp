#include "utsure/core/job/encode_job.hpp"

#include "encode_job_working_set_guard.hpp"
#include "../subtitles/subtitle_burn_in.hpp"
#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/media/media_inspector.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

void notify_final_progress(EncodeJobTelemetry &telemetry, std::string message) {
    if (telemetry.observer == nullptr) {
        return;
    }

    telemetry.observer->on_progress(EncodeJobProgress{
        .stage = EncodeJobStage::completed,
        .current_step = telemetry.total_steps,
        .total_steps = telemetry.total_steps,
        .message = std::move(message)
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
    EncodeJobTelemetry *telemetry = nullptr
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
            .actionable_hint = actionable_hint
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
        }
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

EncodeJobResult make_segment_decode_error(
    const EncodeJob &job,
    const timeline::TimelineSegmentKind kind,
    const media::MediaDecodeError &error,
    EncodeJobTelemetry *telemetry = nullptr
) {
    return make_error(
        job,
        "Failed to decode the " + std::string(timeline::to_string(kind)) + " segment. " + error.message,
        error.actionable_hint,
        telemetry
    );
}

std::string format_segment_log_message(
    const timeline::TimelineSegmentKind kind,
    const std::filesystem::path &source_path
) {
    return "Decoding the " + std::string(timeline::to_string(kind)) + " segment from '" +
        source_path.lexically_normal().string() + "'.";
}

std::string format_encode_log_message(const EncodeJob &job) {
    return "Encoding the composed output as " + std::string(media::to_string(job.output.video.codec)) +
        " with preset '" + job.output.video.preset + "' and CRF " + std::to_string(job.output.video.crf) + ".";
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

        std::vector<media::DecodedMediaSource> decoded_segments{};
        decoded_segments.reserve(timeline_plan.segments.size());
        std::int64_t subtitled_video_frame_count = 0;
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

            const auto decode_result = media::MediaDecoder::decode(
                segment_plan.source_path,
                options.decode_normalization_policy
            );
            if (!decode_result.succeeded()) {
                return make_segment_decode_error(job, segment_plan.kind, *decode_result.error, &telemetry);
            }

            media::DecodedMediaSource decoded_segment = *decode_result.decoded_media_source;

            if (job.subtitles.has_value() &&
                job.subtitles->timing_mode == timeline::SubtitleTimingMode::main_segment_only &&
                segment_plan.kind == timeline::TimelineSegmentKind::main) {
                notify_progress(
                    telemetry,
                    EncodeJobStage::burning_in_subtitles,
                    "Burning subtitles into the main segment."
                );
                notify_log(
                    telemetry,
                    EncodeJobLogLevel::info,
                    "Applying subtitle burn-in to the main segment."
                );

                if (auto renderer_error = ensure_subtitle_renderer(); renderer_error.has_value()) {
                    return *renderer_error;
                }

                const auto burn_in_result = subtitles::burn_in::apply(
                    decoded_segment,
                    *subtitle_renderer,
                    *job.subtitles
                );
                if (!burn_in_result.succeeded()) {
                    return make_error(
                        job,
                        burn_in_result.error->message,
                        burn_in_result.error->actionable_hint,
                        &telemetry
                    );
                }

                decoded_segment = std::move(burn_in_result.output->decoded_media_source);
                subtitled_video_frame_count = burn_in_result.output->subtitled_video_frame_count;
            }

            decoded_segments.push_back(std::move(decoded_segment));
        }

        notify_progress(
            telemetry,
            EncodeJobStage::composing_timeline,
            "Composing the decoded segments into one output timeline."
        );
        notify_log(telemetry, EncodeJobLogLevel::info, "Composing the decoded intro/main/outro timeline.");

        const auto timeline_composition_result = timeline::TimelineComposer::compose(timeline_plan, decoded_segments);
        if (!timeline_composition_result.succeeded()) {
            return make_error(
                job,
                timeline_composition_result.error->message,
                timeline_composition_result.error->actionable_hint,
                &telemetry
            );
        }

        media::DecodedMediaSource media_source_for_encode =
            timeline_composition_result.output->decoded_media_source;

        if (job.subtitles.has_value() &&
            job.subtitles->timing_mode == timeline::SubtitleTimingMode::full_output_timeline) {
            notify_progress(
                telemetry,
                EncodeJobStage::burning_in_subtitles,
                "Burning subtitles into the composed output timeline."
            );
            notify_log(
                telemetry,
                EncodeJobLogLevel::info,
                "Applying subtitle burn-in to the composed output timeline."
            );

            if (auto renderer_error = ensure_subtitle_renderer(); renderer_error.has_value()) {
                return *renderer_error;
            }

            const auto burn_in_result = subtitles::burn_in::apply(
                media_source_for_encode,
                *subtitle_renderer,
                *job.subtitles
            );
            if (!burn_in_result.succeeded()) {
                return make_error(
                    job,
                    burn_in_result.error->message,
                    burn_in_result.error->actionable_hint,
                    &telemetry
                );
            }

            media_source_for_encode = std::move(burn_in_result.output->decoded_media_source);
            subtitled_video_frame_count = burn_in_result.output->subtitled_video_frame_count;
        }

        notify_progress(
            telemetry,
            EncodeJobStage::encoding_output,
            "Encoding the final output file."
        );
        notify_log(telemetry, EncodeJobLogLevel::info, format_encode_log_message(job));

        const auto encode_result = media::MediaEncoder::encode(
            media_source_for_encode,
            build_media_encode_request(job)
        );
        if (!encode_result.succeeded()) {
            return make_error(
                job,
                encode_result.error->message,
                encode_result.error->actionable_hint,
                &telemetry
            );
        }

        notify_log(
            telemetry,
            EncodeJobLogLevel::info,
            "Encode job completed successfully. Output written to '" +
                encode_result.encoded_media_summary->output_path.lexically_normal().string() + "'."
        );
        notify_final_progress(telemetry, "Encode completed successfully.");

        return EncodeJobResult{
            .encode_job_summary = EncodeJobSummary{
                .job = job,
                .inspected_input_info = timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info,
                .timeline_summary = timeline_composition_result.output->timeline_summary,
                .decode_normalization_policy = media_source_for_encode.normalization_policy,
                .decoded_video_frame_count = static_cast<std::int64_t>(media_source_for_encode.video_frames.size()),
                .decoded_audio_block_count = static_cast<std::int64_t>(media_source_for_encode.audio_blocks.size()),
                .subtitled_video_frame_count = subtitled_video_frame_count,
                .encoded_media_summary = *encode_result.encoded_media_summary
            },
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_error(
            job,
            "Encode job aborted because an unexpected exception was raised.",
            exception.what(),
            &telemetry
        );
    }
}

}  // namespace utsure::core::job
