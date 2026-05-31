#include "utsure/core/job/resize.hpp"

#include <iostream>
#include <string_view>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

bool expect_dimensions(
    const utsure::core::job::ResizeSourceDimensions &source,
    const utsure::core::job::EncodeResizeSettings &settings,
    const int expected_width,
    const int expected_height
) {
    const auto result = utsure::core::job::calculate_resize_dimensions(source, settings);
    return result.succeeded() &&
        result.dimensions->width == expected_width &&
        result.dimensions->height == expected_height;
}

int run_resize_assertions() {
    using utsure::core::job::EncodeResizeMode;
    using utsure::core::job::EncodeResizeSettings;
    using utsure::core::job::ResizeSourceDimensions;
    using utsure::core::media::Rational;

    const ResizeSourceDimensions source_16x9{
        .width = 1920,
        .height = 1080,
        .sample_aspect_ratio = Rational{1, 1}
    };
    if (!expect_dimensions(source_16x9, EncodeResizeSettings{.mode = EncodeResizeMode::target_height, .target_height = 720}, 1280, 720)) {
        return fail("1920x1080 to 720p did not resolve to 1280x720.");
    }
    if (!expect_dimensions(source_16x9, EncodeResizeSettings{.mode = EncodeResizeMode::target_height, .target_height = 540}, 960, 540)) {
        return fail("1920x1080 to 540p did not resolve to 960x540.");
    }
    if (!expect_dimensions(source_16x9, EncodeResizeSettings{.mode = EncodeResizeMode::target_height, .target_height = 480}, 854, 480)) {
        return fail("1920x1080 to 480p did not resolve to an encoder-safe 854x480 result.");
    }

    const ResizeSourceDimensions source_4x3{
        .width = 1440,
        .height = 1080,
        .sample_aspect_ratio = Rational{1, 1}
    };
    if (!expect_dimensions(source_4x3, EncodeResizeSettings{.mode = EncodeResizeMode::target_height, .target_height = 480}, 640, 480)) {
        return fail("4:3 source to 480p did not resolve to 640x480.");
    }
    if (!expect_dimensions(source_4x3, EncodeResizeSettings{.mode = EncodeResizeMode::target_height, .target_height = 540}, 720, 540)) {
        return fail("4:3 source to 540p did not resolve to 720x540.");
    }

    const ResizeSourceDimensions ultrawide{
        .width = 2560,
        .height = 1080,
        .sample_aspect_ratio = Rational{1, 1}
    };
    if (!expect_dimensions(ultrawide, EncodeResizeSettings{.mode = EncodeResizeMode::target_height, .target_height = 540}, 1280, 540)) {
        return fail("Ultrawide source did not preserve source aspect ratio.");
    }

    const ResizeSourceDimensions small_source{
        .width = 640,
        .height = 360,
        .sample_aspect_ratio = Rational{1, 1}
    };
    if (!expect_dimensions(small_source, EncodeResizeSettings{.mode = EncodeResizeMode::target_height, .target_height = 720}, 640, 360)) {
        return fail("No-upscale resize did not keep a smaller source unchanged.");
    }

    const ResizeSourceDimensions odd_source{
        .width = 1919,
        .height = 1079,
        .sample_aspect_ratio = Rational{1, 1}
    };
    if (!expect_dimensions(odd_source, EncodeResizeSettings{}, 1918, 1078)) {
        return fail("Source/no-resize did not round odd dimensions to encoder-safe even values.");
    }

    const auto invalid = utsure::core::job::calculate_resize_dimensions(
        source_16x9,
        EncodeResizeSettings{.mode = EncodeResizeMode::target_height, .target_height = -1}
    );
    if (invalid.succeeded()) {
        return fail("Invalid target height unexpectedly succeeded.");
    }

    std::cout << "resize.presets=ok\n";
    return 0;
}

}  // namespace

int main() {
    return run_resize_assertions();
}
