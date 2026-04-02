#include "subtitles/subtitle_bitmap_compositor.hpp"

#include "utsure/core/media/decoded_media.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using utsure::core::media::DecodedVideoFrame;
using utsure::core::media::NormalizedVideoPixelFormat;
using utsure::core::media::VideoPlane;
using utsure::core::subtitles::SubtitleBitmap;
using utsure::core::subtitles::SubtitleBitmapPixelFormat;
using utsure::core::subtitles::detail::composite_bitmap_into_frame;
using utsure::core::subtitles::detail::composite_premultiplied_rgba_bitmap_into_frame;
using utsure::core::subtitles::detail::is_rgba_frame_layout_supported;
using utsure::core::subtitles::detail::PremultipliedRgbaBitmapView;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int run_premultiplied_assertion() {
    DecodedVideoFrame frame{
        .width = 2,
        .height = 1,
        .pixel_format = NormalizedVideoPixelFormat::rgba8,
        .planes = {VideoPlane{
            .line_stride_bytes = 8,
            .visible_width = 2,
            .visible_height = 1,
            .bytes = {
                10U, 20U, 30U, 64U,
                40U, 50U, 60U, 200U
            }
        }}
    };
    if (!is_rgba_frame_layout_supported(frame)) {
        return fail("The compositor helper did not accept a valid rgba8 frame layout.");
    }

    const SubtitleBitmap bitmap{
        .origin_x = 0,
        .origin_y = 0,
        .width = 2,
        .height = 1,
        .pixel_format = SubtitleBitmapPixelFormat::rgba8_premultiplied,
        .line_stride_bytes = 8,
        .bytes = {
            100U, 50U, 25U, 128U,
            255U, 0U, 0U, 255U
        }
    };

    composite_bitmap_into_frame(frame, bitmap);

    const std::vector<std::uint8_t> expected_bytes{
        104U, 59U, 39U, 159U,
        255U, 0U, 0U, 255U
    };
    if (frame.planes.front().bytes != expected_bytes) {
        return fail("Premultiplied RGBA subtitle composition produced unexpected bytes.");
    }

    std::cout << "frame.pixel0=104,59,39,159\n";
    std::cout << "frame.pixel1=255,0,0,255\n";
    return 0;
}

int run_clipped_direct_view_assertion() {
    DecodedVideoFrame frame{
        .width = 2,
        .height = 2,
        .pixel_format = NormalizedVideoPixelFormat::rgba8,
        .planes = {VideoPlane{
            .line_stride_bytes = 8,
            .visible_width = 2,
            .visible_height = 2,
            .bytes = {
                0U, 0U, 0U, 255U, 10U, 10U, 10U, 255U,
                20U, 20U, 20U, 255U, 30U, 30U, 30U, 255U
            }
        }}
    };

    const std::vector<std::uint8_t> bitmap_bytes{
        200U, 0U, 0U, 255U, 100U, 0U, 0U, 128U,
        0U, 200U, 0U, 255U, 0U, 100U, 0U, 128U
    };
    composite_premultiplied_rgba_bitmap_into_frame(
        frame,
        PremultipliedRgbaBitmapView{
            .origin_x = -1,
            .origin_y = 0,
            .width = 2,
            .height = 2,
            .line_stride_bytes = 8,
            .bytes = bitmap_bytes.data()
        }
    );

    const std::vector<std::uint8_t> expected_bytes{
        100U, 0U, 0U, 255U, 10U, 10U, 10U, 255U,
        0U, 100U, 0U, 255U, 30U, 30U, 30U, 255U
    };
    if (frame.planes.front().bytes != expected_bytes) {
        return fail("Direct premultiplied RGBA clipping produced unexpected bytes.");
    }

    std::cout << "direct_clip.frame.pixel0=100,0,0,255\n";
    std::cout << "direct_clip.frame.pixel2=0,100,0,255\n";
    return 0;
}

int run_invalid_bitmap_assertion() {
    DecodedVideoFrame frame{
        .width = 2,
        .height = 1,
        .pixel_format = NormalizedVideoPixelFormat::rgba8,
        .planes = {VideoPlane{
            .line_stride_bytes = 8,
            .visible_width = 2,
            .visible_height = 1,
            .bytes = {0U, 0U, 0U, 255U, 0U, 0U, 0U, 255U}
        }}
    };

    try {
        composite_premultiplied_rgba_bitmap_into_frame(
            frame,
            PremultipliedRgbaBitmapView{
                .origin_x = 0,
                .origin_y = 0,
                .width = 2,
                .height = 1,
                .line_stride_bytes = 4,
                .bytes = frame.planes.front().bytes.data()
            }
        );
    } catch (const std::exception &exception) {
        const std::string message(exception.what());
        if (message.find("invalid bitmap surface") == std::string::npos) {
            return fail("Invalid-bitmap assertion threw the wrong diagnostic message.");
        }

        std::cout << message << '\n';
        return 0;
    }

    return fail("Invalid-bitmap assertion unexpectedly succeeded.");
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return fail(
            "Usage: utsure_core_subtitle_bitmap_compositor_tests "
            "[--premultiplied|--direct-clipped|--invalid-bitmap]"
        );
    }

    const std::string_view mode(argv[1]);
    if (mode == "--premultiplied") {
        return run_premultiplied_assertion();
    }

    if (mode == "--direct-clipped") {
        return run_clipped_direct_view_assertion();
    }

    if (mode == "--invalid-bitmap") {
        return run_invalid_bitmap_assertion();
    }

    return fail("Unknown mode. Use --premultiplied, --direct-clipped, or --invalid-bitmap.");
}
