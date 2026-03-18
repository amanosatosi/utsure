#include "utsure/core/job/encode_job.hpp"

#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/media/media_inspector.hpp"

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

    const auto encode_result = media::MediaEncoder::encode(
        *decode_result.decoded_media_source,
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
            .encoded_media_summary = *encode_result.encoded_media_summary
        },
        .error = std::nullopt
    };
}

}  // namespace utsure::core::job
