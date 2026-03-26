#include "utsure/core/subtitles/subtitle_frame_composer.hpp"

#include "subtitle_bitmap_compositor.hpp"

#include <exception>
#include <utility>

namespace utsure::core::subtitles {

namespace {

SubtitleFrameComposeResult make_error(std::string message, std::string actionable_hint) {
    return SubtitleFrameComposeResult{
        .subtitles_applied = false,
        .error = SubtitleFrameComposeError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

}  // namespace

bool SubtitleFrameComposeResult::succeeded() const noexcept {
    return !error.has_value();
}

SubtitleFrameComposeResult compose_subtitles_into_frame(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    const std::int64_t timestamp_microseconds
) noexcept {
    if (!detail::is_rgba_frame_layout_supported(video_frame)) {
        return make_error(
            "Subtitle composition requires rgba8 decoded frames with a single plane.",
            "Keep the decoded video path normalized to single-plane rgba8 before requesting subtitle composition."
        );
    }

    SubtitleRenderResult render_result = subtitle_session.render(SubtitleRenderRequest{
        .timestamp_microseconds = timestamp_microseconds
    });
    if (!render_result.succeeded()) {
        return make_error(
            render_result.error->message,
            render_result.error->actionable_hint
        );
    }

    if (render_result.rendered_frame->bitmaps.empty()) {
        return SubtitleFrameComposeResult{
            .subtitles_applied = false,
            .error = std::nullopt
        };
    }

    try {
        for (const auto &bitmap : render_result.rendered_frame->bitmaps) {
            detail::composite_bitmap_into_frame(video_frame, bitmap);
        }
    } catch (const std::exception &exception) {
        return make_error(
            "Subtitle composition failed while blending subtitle bitmaps into the decoded frame.",
            exception.what()
        );
    }

    return SubtitleFrameComposeResult{
        .subtitles_applied = true,
        .error = std::nullopt
    };
}

}  // namespace utsure::core::subtitles
