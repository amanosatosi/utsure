#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include "utsure/core/subtitles/subtitle_frame_composer.hpp"

namespace utsure::core::subtitles {

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
    return compose_subtitles_into_frame_via_render_result(video_frame, *this, request);
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
