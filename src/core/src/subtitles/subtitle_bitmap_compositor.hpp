#pragma once

#include "utsure/core/media/decoded_media.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <cstdint>

namespace utsure::core::subtitles::detail {

struct PremultipliedRgbaBitmapView final {
    int origin_x{0};
    int origin_y{0};
    int width{0};
    int height{0};
    int line_stride_bytes{0};
    const std::uint8_t *bytes{nullptr};
};

[[nodiscard]] bool is_rgba_frame_layout_supported(const media::DecodedVideoFrame &video_frame) noexcept;

void composite_premultiplied_rgba_bitmap_into_frame(
    media::DecodedVideoFrame &video_frame,
    const PremultipliedRgbaBitmapView &bitmap
);

void composite_bitmap_into_frame(
    media::DecodedVideoFrame &video_frame,
    const SubtitleBitmap &bitmap
);

}  // namespace utsure::core::subtitles::detail
