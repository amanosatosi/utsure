#include "utsure/core/job/encode_job.hpp"

#include "../subtitles/subtitle_burn_in.hpp"
#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/media/media_inspector.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <exception>
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

}  // namespace

bool EncodeJobResult::succeeded() const noexcept {
    return encode_job_summary.has_value() && !error.has_value();
}

EncodeJobResult EncodeJobRunner::run(
    const EncodeJob &job,
    const media::DecodeNormalizationPolicy &decode_normalization_policy
) noexcept {
    try {
        const auto inspection_result = media::MediaInspector::inspect(job.input.main_source_path);
        if (!inspection_result.succeeded()) {
            return make_error(
                job,
                inspection_result.error->message,
                inspection_result.error->actionable_hint
            );
        }

        const auto decode_result = media::MediaDecoder::decode(job.input.main_source_path, decode_normalization_policy);
        if (!decode_result.succeeded()) {
            return make_error(
                job,
                decode_result.error->message,
                decode_result.error->actionable_hint
            );
        }

        media::DecodedMediaSource media_source_for_encode = *decode_result.decoded_media_source;
        std::int64_t subtitled_video_frame_count = 0;

        if (job.subtitles.has_value()) {
            auto subtitle_renderer = subtitles::create_default_subtitle_renderer();
            if (!subtitle_renderer) {
                return make_error(
                    job,
                    "Failed to initialize the default subtitle renderer.",
                    "Verify that the libassmod-backed subtitle renderer is available before burn-in."
                );
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
                .inspected_input_info = *inspection_result.media_source_info,
                .decode_normalization_policy = decode_result.decoded_media_source->normalization_policy,
                .decoded_video_frame_count = static_cast<std::int64_t>(decode_result.decoded_media_source->video_frames.size()),
                .decoded_audio_block_count = static_cast<std::int64_t>(decode_result.decoded_media_source->audio_blocks.size()),
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
