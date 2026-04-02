#include "subtitle_bitmap_compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace utsure::core::subtitles::detail {

namespace {

std::string describe_destination_surface(const media::DecodedVideoFrame &video_frame) {
    std::ostringstream message;
    message << "destination=" << video_frame.width << 'x' << video_frame.height
            << ", planes=" << video_frame.planes.size()
            << ", stride=" << (video_frame.planes.empty() ? 0 : video_frame.planes.front().line_stride_bytes);
    return message.str();
}

std::string describe_bitmap_surface(const PremultipliedRgbaBitmapView &bitmap) {
    std::ostringstream message;
    message << "origin=" << bitmap.origin_x << ',' << bitmap.origin_y
            << ", size=" << bitmap.width << 'x' << bitmap.height
            << ", stride=" << bitmap.line_stride_bytes
            << ", bytes=" << (bitmap.bytes != nullptr ? "present" : "null");
    return message.str();
}

}  // namespace

std::size_t required_rgba_buffer_size(
    const int width,
    const int height,
    const int stride_bytes,
    const char *label
) {
    const std::int64_t minimum_stride = static_cast<std::int64_t>(width) * 4LL;
    if (width <= 0 || height <= 0 ||
        minimum_stride > static_cast<std::int64_t>(std::numeric_limits<int>::max()) ||
        stride_bytes <= 0 ||
        static_cast<std::int64_t>(stride_bytes) < minimum_stride) {
        std::ostringstream message;
        message << "Premultiplied RGBA composition received an invalid " << label
                << " surface: width=" << width
                << ", height=" << height
                << ", stride=" << stride_bytes << '.';
        throw std::runtime_error(message.str());
    }

    const std::uint64_t row_extent = static_cast<std::uint64_t>(width) * 4ULL;
    const std::uint64_t buffer_size =
        static_cast<std::uint64_t>(stride_bytes) * static_cast<std::uint64_t>(height - 1) + row_extent;
    if (buffer_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::ostringstream message;
        message << "Premultiplied RGBA composition overflowed the " << label
                << " buffer size calculation: width=" << width
                << ", height=" << height
                << ", stride=" << stride_bytes << '.';
        throw std::runtime_error(message.str());
    }

    return static_cast<std::size_t>(buffer_size);
}

bool is_rgba_frame_layout_supported(const media::DecodedVideoFrame &video_frame) noexcept {
    return video_frame.pixel_format == media::NormalizedVideoPixelFormat::rgba8 &&
        video_frame.planes.size() == 1 &&
        video_frame.width > 0 &&
        video_frame.height > 0;
}

void validate_rgba_frame_surface(const media::DecodedVideoFrame &video_frame, const char *context) {
    if (!is_rgba_frame_layout_supported(video_frame)) {
        throw std::runtime_error(
            std::string(context) + " requires a valid destination frame layout: " +
            describe_destination_surface(video_frame)
        );
    }

    const auto &plane = video_frame.planes.front();
    const auto required_plane_bytes = required_rgba_buffer_size(
        video_frame.width,
        video_frame.height,
        plane.line_stride_bytes,
        "destination"
    );
    if (plane.bytes.size() < required_plane_bytes) {
        throw std::runtime_error(
            std::string(context) + " received a truncated destination frame buffer: " +
            describe_destination_surface(video_frame)
        );
    }
}

void composite_premultiplied_rgba_bitmap_into_frame(
    media::DecodedVideoFrame &video_frame,
    const PremultipliedRgbaBitmapView &bitmap
) {
    validate_rgba_frame_surface(video_frame, "Premultiplied RGBA composition");

    auto &plane = video_frame.planes.front();
    required_rgba_buffer_size(bitmap.width, bitmap.height, bitmap.line_stride_bytes, "bitmap");
    if (bitmap.bytes == nullptr) {
        throw std::runtime_error(
            "Premultiplied RGBA composition received a truncated frame or bitmap buffer. " +
            describe_destination_surface(video_frame) + "; bitmap=" + describe_bitmap_surface(bitmap)
        );
    }

    const std::int64_t bitmap_left = static_cast<std::int64_t>(bitmap.origin_x);
    const std::int64_t bitmap_top = static_cast<std::int64_t>(bitmap.origin_y);
    const std::int64_t bitmap_right = bitmap_left + static_cast<std::int64_t>(bitmap.width);
    const std::int64_t bitmap_bottom = bitmap_top + static_cast<std::int64_t>(bitmap.height);
    const std::int64_t clipped_left64 = std::max<std::int64_t>(0, bitmap_left);
    const std::int64_t clipped_top64 = std::max<std::int64_t>(0, bitmap_top);
    const std::int64_t clipped_right64 = std::min<std::int64_t>(video_frame.width, bitmap_right);
    const std::int64_t clipped_bottom64 = std::min<std::int64_t>(video_frame.height, bitmap_bottom);
    if (clipped_left64 >= clipped_right64 || clipped_top64 >= clipped_bottom64) {
        return;
    }

    const int clipped_left = static_cast<int>(clipped_left64);
    const int clipped_top = static_cast<int>(clipped_top64);
    const int clipped_right = static_cast<int>(clipped_right64);
    const int clipped_bottom = static_cast<int>(clipped_bottom64);

    for (int destination_y = clipped_top; destination_y < clipped_bottom; ++destination_y) {
        const int source_y = destination_y - bitmap.origin_y;
        if (source_y < 0 || source_y >= bitmap.height) {
            throw std::runtime_error(
                "Premultiplied RGBA composition produced an invalid source row index. " +
                describe_destination_surface(video_frame) + "; bitmap=" + describe_bitmap_surface(bitmap)
            );
        }

        auto *destination_row = plane.bytes.data() +
            static_cast<std::size_t>(destination_y) * static_cast<std::size_t>(plane.line_stride_bytes);
        const auto *source_row = bitmap.bytes +
            static_cast<std::size_t>(source_y) * static_cast<std::size_t>(bitmap.line_stride_bytes);

        for (int destination_x = clipped_left; destination_x < clipped_right; ++destination_x) {
            const int source_x = destination_x - bitmap.origin_x;
            if (source_x < 0 || source_x >= bitmap.width) {
                throw std::runtime_error(
                    "Premultiplied RGBA composition produced an invalid source column index. " +
                    describe_destination_surface(video_frame) + "; bitmap=" + describe_bitmap_surface(bitmap)
                );
            }

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

void composite_bitmap_into_frame(
    media::DecodedVideoFrame &video_frame,
    const SubtitleBitmap &bitmap
) {
    if (bitmap.pixel_format != SubtitleBitmapPixelFormat::rgba8_premultiplied) {
        throw std::runtime_error("Only premultiplied rgba8 subtitle bitmaps are supported for burn-in.");
    }

    const auto required_bitmap_bytes = required_rgba_buffer_size(
        bitmap.width,
        bitmap.height,
        bitmap.line_stride_bytes,
        "bitmap"
    );
    if (bitmap.bytes.size() < required_bitmap_bytes) {
        std::ostringstream message;
        message << "Premultiplied RGBA composition received a truncated copied bitmap buffer: origin="
                << bitmap.origin_x << ',' << bitmap.origin_y
                << ", size=" << bitmap.width << 'x' << bitmap.height
                << ", stride=" << bitmap.line_stride_bytes
                << ", bytes=" << bitmap.bytes.size()
                << ", required=" << required_bitmap_bytes << '.';
        throw std::runtime_error(message.str());
    }

    composite_premultiplied_rgba_bitmap_into_frame(
        video_frame,
        PremultipliedRgbaBitmapView{
            .origin_x = bitmap.origin_x,
            .origin_y = bitmap.origin_y,
            .width = bitmap.width,
            .height = bitmap.height,
            .line_stride_bytes = bitmap.line_stride_bytes,
            .bytes = bitmap.bytes.data()
        }
    );
}

}  // namespace utsure::core::subtitles::detail
