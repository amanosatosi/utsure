#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include "subtitle_bitmap_compositor.hpp"

#include <exception>
#include <utility>

namespace utsure::core::subtitles {

namespace {

SubtitleFrameComposeResult make_compose_error(std::string message, std::string actionable_hint) {
    return SubtitleFrameComposeResult{
        .subtitles_applied = false,
        .error = SubtitleFrameComposeError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

}  // namespace

bool SubtitleRenderSessionResult::succeeded() const noexcept {
    return session != nullptr && !error.has_value();
}

bool SubtitleRenderResult::succeeded() const noexcept {
    return rendered_frame.has_value() && !error.has_value();
}

bool SubtitleFrameComposeResult::succeeded() const noexcept {
    return !error.has_value();
}

SubtitleFrameComposeResult SubtitleRenderSession::compose_into_frame(
    media::DecodedVideoFrame &video_frame,
    const SubtitleRenderRequest &request
) noexcept {
    if (!detail::is_rgba_frame_layout_supported(video_frame)) {
        return make_compose_error(
            "Subtitle composition requires rgba8 decoded frames with a single plane.",
            "Keep the decoded video path normalized to single-plane rgba8 before requesting subtitle composition."
        );
    }

    SubtitleRenderResult render_result = render(request);
    if (!render_result.succeeded()) {
        return make_compose_error(
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
        return make_compose_error(
            "Subtitle composition failed while blending subtitle bitmaps into the decoded frame.",
            exception.what()
        );
    }

    return SubtitleFrameComposeResult{
        .subtitles_applied = true,
        .error = std::nullopt
    };
}

const char *to_string(const SubtitleBitmapPixelFormat pixel_format) noexcept {
    switch (pixel_format) {
    case SubtitleBitmapPixelFormat::rgba8_premultiplied:
        return "rgba8_premultiplied";
    case SubtitleBitmapPixelFormat::unknown:
    default:
        return "unknown";
    }
}

}  // namespace utsure::core::subtitles
