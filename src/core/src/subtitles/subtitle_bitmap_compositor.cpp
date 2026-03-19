#include "subtitle_bitmap_compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace utsure::core::subtitles::detail {

namespace {

std::size_t minimum_rgba_buffer_size(
    const int width,
    const int height,
    const int stride_bytes
) {
    if (width <= 0 || height <= 0 || stride_bytes < (width * 4)) {
        throw std::runtime_error("Premultiplied RGBA composition received an invalid surface shape.");
    }

    return static_cast<std::size_t>(stride_bytes) * static_cast<std::size_t>(height - 1) +
        static_cast<std::size_t>(width * 4);
}

}  // namespace

bool is_rgba_frame_layout_supported(const media::DecodedVideoFrame &video_frame) noexcept {
    return video_frame.pixel_format == media::NormalizedVideoPixelFormat::rgba8 &&
        video_frame.planes.size() == 1 &&
        video_frame.width > 0 &&
        video_frame.height > 0;
}

void composite_bitmap_into_frame(
    media::DecodedVideoFrame &video_frame,
    const SubtitleBitmap &bitmap
) {
    if (bitmap.pixel_format != SubtitleBitmapPixelFormat::rgba8_premultiplied) {
        throw std::runtime_error("Only premultiplied rgba8 subtitle bitmaps are supported for burn-in.");
    }

    auto &plane = video_frame.planes.front();
    const auto required_plane_bytes = minimum_rgba_buffer_size(
        video_frame.width,
        video_frame.height,
        plane.line_stride_bytes
    );
    const auto required_bitmap_bytes = minimum_rgba_buffer_size(
        bitmap.width,
        bitmap.height,
        bitmap.line_stride_bytes
    );
    if (plane.bytes.size() < required_plane_bytes || bitmap.bytes.size() < required_bitmap_bytes) {
        throw std::runtime_error("Premultiplied RGBA composition received a truncated frame or bitmap buffer.");
    }

    const int clipped_left = std::max(0, bitmap.origin_x);
    const int clipped_top = std::max(0, bitmap.origin_y);
    const int clipped_right = std::min(video_frame.width, bitmap.origin_x + bitmap.width);
    const int clipped_bottom = std::min(video_frame.height, bitmap.origin_y + bitmap.height);
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
        return;
    }

    for (int destination_y = clipped_top; destination_y < clipped_bottom; ++destination_y) {
        const int source_y = destination_y - bitmap.origin_y;
        auto *destination_row = plane.bytes.data() +
            static_cast<std::size_t>(destination_y) * static_cast<std::size_t>(plane.line_stride_bytes);
        const auto *source_row = bitmap.bytes.data() +
            static_cast<std::size_t>(source_y) * static_cast<std::size_t>(bitmap.line_stride_bytes);

        for (int destination_x = clipped_left; destination_x < clipped_right; ++destination_x) {
            const int source_x = destination_x - bitmap.origin_x;
            const auto source_offset = static_cast<std::size_t>(source_x) * 4U;
            const auto destination_offset = static_cast<std::size_t>(destination_x) * 4U;

            const std::uint8_t source_alpha = source_row[source_offset + 3U];
            if (source_alpha == 0U) {
                continue;
            }

            const std::uint8_t inverse_alpha = static_cast<std::uint8_t>(255U - source_alpha);
            // Subtitle tiles are premultiplied RGBA8888, so blend RGB in premultiplied space and
            // update destination alpha with the same source-over equation.
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                destination_row[destination_offset + channel] = static_cast<std::uint8_t>(
                    std::min<std::uint16_t>(
                        255U,
                        static_cast<std::uint16_t>(source_row[source_offset + channel]) +
                            static_cast<std::uint16_t>(
                                (static_cast<std::uint16_t>(destination_row[destination_offset + channel]) *
                                 inverse_alpha) /
                                255U
                            )
                    )
                );
            }

            destination_row[destination_offset + 3U] = static_cast<std::uint8_t>(
                std::min<std::uint16_t>(
                    255U,
                    static_cast<std::uint16_t>(source_alpha) +
                        static_cast<std::uint16_t>(
                            (static_cast<std::uint16_t>(destination_row[destination_offset + 3U]) *
                             inverse_alpha) /
                            255U
                        )
                )
            );
        }
    }
}

}  // namespace utsure::core::subtitles::detail
