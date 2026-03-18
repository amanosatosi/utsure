#include "utsure/core/job/encode_job.hpp"

#include "../subtitles/subtitle_burn_in.hpp"
#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/media/media_inspector.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <exception>
#include <vector>
#include <utility>

namespace utsure::core::job {

namespace {

EncodeJobResult make_error(
    const EncodeJob &job,
    std::string message,
    std::string actionable_hint
) {
    return EncodeJobResult{
        .encode_job_summary = std::nullopt,
        .error = EncodeJobError{
            .main_source_path = job.input.main_source_path.lexically_normal().string(),
            .output_path = job.output.output_path.lexically_normal().string(),
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
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
    const media::MediaDecodeError &error
) {
    return make_error(
        job,
        "Failed to decode the " + std::string(timeline::to_string(kind)) + " segment. " + error.message,
        error.actionable_hint
    );
}

}  // namespace

bool EncodeJobResult::succeeded() const noexcept {
    return encode_job_summary.has_value() && !error.has_value();
}

EncodeJobResult EncodeJobRunner::run(
    const EncodeJob &job,
    const media::DecodeNormalizationPolicy &decode_normalization_policy
) noexcept {
    try {
        const auto timeline_assembly_result = timeline::TimelineAssembler::assemble(build_timeline_request(job));
        if (!timeline_assembly_result.succeeded()) {
            return make_error(
                job,
                timeline_assembly_result.error->message,
                timeline_assembly_result.error->actionable_hint
            );
        }

        const auto &timeline_plan = *timeline_assembly_result.timeline_plan;
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
                "Verify that the libassmod-backed subtitle renderer is available before burn-in."
            );
        };

        for (const auto &segment_plan : timeline_plan.segments) {
            const auto decode_result = media::MediaDecoder::decode(
                segment_plan.source_path,
                decode_normalization_policy
            );
            if (!decode_result.succeeded()) {
                return make_segment_decode_error(job, segment_plan.kind, *decode_result.error);
            }

            media::DecodedMediaSource decoded_segment = *decode_result.decoded_media_source;

            if (job.subtitles.has_value() &&
                job.subtitles->timing_mode == timeline::SubtitleTimingMode::main_segment_only &&
                segment_plan.kind == timeline::TimelineSegmentKind::main) {
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
                        burn_in_result.error->actionable_hint
                    );
                }

                decoded_segment = std::move(burn_in_result.output->decoded_media_source);
                subtitled_video_frame_count = burn_in_result.output->subtitled_video_frame_count;
            }

            decoded_segments.push_back(std::move(decoded_segment));
        }

        const auto timeline_composition_result = timeline::TimelineComposer::compose(timeline_plan, decoded_segments);
        if (!timeline_composition_result.succeeded()) {
            return make_error(
                job,
                timeline_composition_result.error->message,
                timeline_composition_result.error->actionable_hint
            );
        }

        media::DecodedMediaSource media_source_for_encode =
            timeline_composition_result.output->decoded_media_source;

        if (job.subtitles.has_value() &&
            job.subtitles->timing_mode == timeline::SubtitleTimingMode::full_output_timeline) {
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
                    burn_in_result.error->actionable_hint
                );
            }

            media_source_for_encode = std::move(burn_in_result.output->decoded_media_source);
            subtitled_video_frame_count = burn_in_result.output->subtitled_video_frame_count;
        }

        const auto encode_result = media::MediaEncoder::encode(
            media_source_for_encode,
            build_media_encode_request(job)
        );
        if (!encode_result.succeeded()) {
            return make_error(
                job,
                encode_result.error->message,
                encode_result.error->actionable_hint
            );
        }

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
            exception.what()
        );
    }
}

}  // namespace utsure::core::job
