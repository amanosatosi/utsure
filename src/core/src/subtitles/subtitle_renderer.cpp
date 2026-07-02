#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include "utsure/core/subtitles/subtitle_frame_composer.hpp"

#include <cmath>
#include <limits>

namespace utsure::core::subtitles {

namespace {

constexpr double kSubtitleTimestampMillisecondEpsilon = 1e-6;

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

std::int64_t subtitle_timestamp_seconds_to_renderer_milliseconds(const double timestamp_seconds) noexcept {
    if (!std::isfinite(timestamp_seconds)) {
        return 0;
    }

    const double milliseconds = std::floor(
        timestamp_seconds * 1000.0 + kSubtitleTimestampMillisecondEpsilon
    );
    if (milliseconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (milliseconds <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }

    return static_cast<std::int64_t>(milliseconds);
}

}  // namespace utsure::core::subtitles
