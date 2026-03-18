#include "utsure/core/subtitles/subtitle_renderer.hpp"

namespace utsure::core::subtitles {

bool SubtitleRenderSessionResult::succeeded() const noexcept {
    return session != nullptr && !error.has_value();
}

bool SubtitleRenderResult::succeeded() const noexcept {
    return rendered_frame.has_value() && !error.has_value();
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
