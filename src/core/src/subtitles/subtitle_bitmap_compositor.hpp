#pragma once

#include "utsure/core/media/decoded_media.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

namespace utsure::core::subtitles::detail {

[[nodiscard]] bool is_rgba_frame_layout_supported(const media::DecodedVideoFrame &video_frame) noexcept;

void composite_bitmap_into_frame(
    media::DecodedVideoFrame &video_frame,
    const SubtitleBitmap &bitmap
);

}  // namespace utsure::core::subtitles::detail
