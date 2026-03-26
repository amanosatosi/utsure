#include "subtitle_burn_in.hpp"

#include "utsure/core/subtitles/subtitle_frame_composer.hpp"

#include <utility>

namespace utsure::core::subtitles::burn_in {

namespace {

SubtitleBurnInResult make_error(std::string message, std::string actionable_hint) {
    return SubtitleBurnInResult{
        .output = std::nullopt,
        .error = SubtitleBurnInError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

}  // namespace

bool SubtitleBurnInResult::succeeded() const noexcept {
    return output.has_value() && !error.has_value();
}

SubtitleBurnInResult apply(
    const media::DecodedMediaSource &decoded_media_source,
    SubtitleRenderer &subtitle_renderer,
    const job::EncodeJobSubtitleSettings &subtitle_settings
) noexcept {
    if (decoded_media_source.video_frames.empty()) {
        return make_error(
            "Subtitle burn-in requires at least one decoded video frame.",
            "Decode a source video stream before requesting subtitle burn-in."
        );
    }

    const auto &first_frame = decoded_media_source.video_frames.front();
    if (!detail::is_rgba_frame_layout_supported(first_frame)) {
        return make_error(
            "Subtitle burn-in requires rgba8 decoded frames with a single plane.",
            "Keep the decoded main-source path normalized to rgba8 before subtitle composition."
        );
    }

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = subtitle_settings.subtitle_path,
        .format_hint = subtitle_settings.format_hint,
        .canvas_width = first_frame.width,
        .canvas_height = first_frame.height,
        .sample_aspect_ratio = first_frame.sample_aspect_ratio
    };

    SubtitleRenderSessionResult session_result = subtitle_renderer.create_session(session_request);
    if (!session_result.succeeded()) {
        return make_error(
            session_result.error->message,
            session_result.error->actionable_hint
        );
    }

    media::DecodedMediaSource composited_media_source = decoded_media_source;
    std::int64_t subtitled_video_frame_count = 0;

    for (auto &video_frame : composited_media_source.video_frames) {
        const auto compose_result = compose_subtitles_into_frame(
            video_frame,
            *session_result.session,
            video_frame.timestamp.start_microseconds
        );
        if (!compose_result.succeeded()) {
            return make_error(
                compose_result.error->message,
                compose_result.error->actionable_hint
            );
        }

        if (!compose_result.subtitles_applied) {
            continue;
        }

        ++subtitled_video_frame_count;
    }

    return SubtitleBurnInResult{
        .output = SubtitleBurnInOutput{
            .decoded_media_source = std::move(composited_media_source),
            .subtitled_video_frame_count = subtitled_video_frame_count
        },
        .error = std::nullopt
    };
}

}  // namespace utsure::core::subtitles::burn_in
