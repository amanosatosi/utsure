#include "adapters/libassmod/libassmod_rgba_bitmap_validation.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using utsure::core::subtitles::SubtitleCompositionDebugContext;
using utsure::core::subtitles::SubtitleRenderRequest;
using utsure::core::subtitles::detail::libassmod::collect_drawable_ass_image_rgba_nodes;
using utsure::core::subtitles::detail::libassmod::validate_ass_image_rgba;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

bool contains_text(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

bool messages_contain_text(const std::vector<std::string> &messages, std::string_view needle) {
    for (const auto &message : messages) {
        if (contains_text(message, needle)) {
            return true;
        }
    }

    return false;
}

int run_empty_bitmap_assertion(const std::string_view bitmap_mode) {
    std::vector<std::string> diagnostics{};
    SubtitleCompositionDebugContext debug_context{
        .decoded_frame_index = 6,
        .output_pts = 6,
        .subtitle_timestamp_microseconds = 250000,
        .worker_id = 0,
        .session_id = 7,
        .log_frame_details = true,
        .log_bitmap_details = true,
        .log_callback = [&diagnostics](const std::string &message) {
            diagnostics.push_back(message);
        }
    };

    ASS_ImageRGBA empty_bitmap{};
    empty_bitmap.dst_x = 24;
    empty_bitmap.dst_y = 132;
    empty_bitmap.w = 128;
    empty_bitmap.h = 0;
    empty_bitmap.stride = 0;
    empty_bitmap.rgba = nullptr;

    const auto drawable_bitmaps = collect_drawable_ass_image_rgba_nodes(
        std::vector<ASS_ImageRGBA *>{&empty_bitmap},
        SubtitleRenderRequest{
            .timestamp_microseconds = 250000,
            .debug_context = &debug_context
        },
        bitmap_mode,
        "sample.ass",
        7
    );

    if (!drawable_bitmaps.empty()) {
        return fail("Transient empty RGBA subtitle bitmaps should be skipped instead of returned as drawable.");
    }

    if (!messages_contain_text(diagnostics, "skipped as empty output") ||
        !messages_contain_text(diagnostics, std::string("mode=") + std::string(bitmap_mode))) {
        return fail("The libassmod empty-bitmap path did not emit the expected diagnostics log.");
    }

    std::cout << "bitmap_mode=" << bitmap_mode << '\n';
    std::cout << "empty_bitmap_skipped=yes\n";
    std::cout << "diagnostic_logged=yes\n";
    return 0;
}

template <typename Invoke>
int expect_runtime_error(Invoke invoke, std::string_view expected_message, std::string_view failure_label) {
    try {
        invoke();
    } catch (const std::exception &exception) {
        if (!contains_text(exception.what(), expected_message)) {
            std::cerr << "Expected error containing: " << expected_message << '\n';
            std::cerr << "Actual error: " << exception.what() << '\n';
            return 1;
        }

        return 0;
    }

    return fail(failure_label);
}

int run_invalid_positive_bitmap_assertion() {
    std::array<std::uint8_t, 32> pixels{};

    ASS_ImageRGBA bad_stride_bitmap{};
    bad_stride_bitmap.w = 4;
    bad_stride_bitmap.h = 2;
    bad_stride_bitmap.stride = 8;
    bad_stride_bitmap.rgba = pixels.data();

    const auto bad_stride_result = expect_runtime_error(
        [&bad_stride_bitmap]() {
            static_cast<void>(validate_ass_image_rgba(bad_stride_bitmap, 0U, "sample.ass", 9));
        },
        "invalid RGBA subtitle bitmap stride",
        "Positive-size bitmaps with truncated stride must fail validation."
    );
    if (bad_stride_result != 0) {
        return bad_stride_result;
    }

    ASS_ImageRGBA overflow_bitmap{};
    overflow_bitmap.w = (std::numeric_limits<int>::max() / 4) + 1;
    overflow_bitmap.h = 1;
    overflow_bitmap.stride = std::numeric_limits<int>::max();
    overflow_bitmap.rgba = pixels.data();

    const auto overflow_result = expect_runtime_error(
        [&overflow_bitmap]() {
            static_cast<void>(validate_ass_image_rgba(overflow_bitmap, 1U, "sample.ass", 9));
        },
        "row size overflowed the host stride range",
        "Positive-size bitmaps whose row size overflows the host stride range must fail validation."
    );
    if (overflow_result != 0) {
        return overflow_result;
    }

    ASS_ImageRGBA null_rgba_bitmap{};
    null_rgba_bitmap.w = 4;
    null_rgba_bitmap.h = 2;
    null_rgba_bitmap.stride = 16;
    null_rgba_bitmap.rgba = nullptr;

    const auto null_rgba_result = expect_runtime_error(
        [&null_rgba_bitmap]() {
            static_cast<void>(validate_ass_image_rgba(null_rgba_bitmap, 2U, "sample.ass", 9));
        },
        "null RGBA bytes",
        "Positive-size bitmaps with null RGBA storage must fail validation."
    );
    if (null_rgba_result != 0) {
        return null_rgba_result;
    }

    std::cout << "invalid_stride=fatal\n";
    std::cout << "row_overflow=fatal\n";
    std::cout << "null_rgba=fatal\n";
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return fail(
            "Usage: utsure_core_libassmod_subtitle_adapter_tests "
            "[--empty-copied|--empty-direct|--invalid-positive]"
        );
    }

    const std::string_view mode(argv[1]);
    if (mode == "--empty-copied") {
        return run_empty_bitmap_assertion("copied");
    }

    if (mode == "--empty-direct") {
        return run_empty_bitmap_assertion("direct");
    }

    if (mode == "--invalid-positive") {
        return run_invalid_positive_bitmap_assertion();
    }

    return fail("Unknown mode for utsure_core_libassmod_subtitle_adapter_tests.");
}
