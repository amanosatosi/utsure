#pragma once

#include "../../subtitles/subtitle_composition_diagnostics.hpp"

extern "C" {
#include <ass/ass.h>
}

#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace utsure::core::subtitles::detail::libassmod {

enum class AssImageRgbaValidationResult : std::uint8_t {
    empty = 0,
    drawable
};

struct DrawableAssImageRgba final {
    std::size_t bitmap_index{0};
    const ASS_ImageRGBA *image{nullptr};
};

[[nodiscard]] inline AssImageRgbaValidationResult validate_ass_image_rgba(
    const ASS_ImageRGBA &image,
    const std::size_t bitmap_index,
    const std::string &subtitle_path_string,
    const int session_instance_id
) {
    if (image.w <= 0 || image.h <= 0) {
        return AssImageRgbaValidationResult::empty;
    }

    const auto minimum_stride = static_cast<std::int64_t>(image.w) * 4LL;
    if (minimum_stride > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        std::ostringstream message;
        message << "libassmod produced a subtitle bitmap whose row size overflowed the host stride range"
                << " for '" << subtitle_path_string << "'"
                << " (session " << session_instance_id << ", bitmap " << bitmap_index << "): origin="
                << image.dst_x << ',' << image.dst_y
                << ", width=" << image.w << ", height=" << image.h << '.';
        throw std::runtime_error(message.str());
    }

    if (image.stride <= 0 || static_cast<std::int64_t>(image.stride) < minimum_stride) {
        std::ostringstream message;
        message << "libassmod produced an invalid RGBA subtitle bitmap stride"
                << " for '" << subtitle_path_string << "'"
                << " (session " << session_instance_id << ", bitmap " << bitmap_index << "): origin="
                << image.dst_x << ',' << image.dst_y
                << ", width=" << image.w << ", height=" << image.h
                << ", stride=" << image.stride << '.';
        throw std::runtime_error(message.str());
    }

    if (image.rgba == nullptr) {
        std::ostringstream message;
        message << "libassmod produced a subtitle bitmap with null RGBA bytes"
                << " for '" << subtitle_path_string << "'"
                << " (session " << session_instance_id << ", bitmap " << bitmap_index << "): origin="
                << image.dst_x << ',' << image.dst_y
                << ", width=" << image.w << ", height=" << image.h
                << ", stride=" << image.stride << '.';
        throw std::runtime_error(message.str());
    }

    return AssImageRgbaValidationResult::drawable;
}

[[nodiscard]] inline std::vector<DrawableAssImageRgba> collect_drawable_ass_image_rgba_nodes(
    const std::vector<ASS_ImageRGBA *> &image_nodes,
    const SubtitleRenderRequest &request,
    const std::string_view bitmap_mode,
    const std::string &subtitle_path_string,
    const int session_instance_id
) {
    std::vector<DrawableAssImageRgba> drawable_bitmaps{};
    drawable_bitmaps.reserve(image_nodes.size());
    for (std::size_t bitmap_index = 0; bitmap_index < image_nodes.size(); ++bitmap_index) {
        const ASS_ImageRGBA &image = *image_nodes[bitmap_index];
        const auto validation_result = validate_ass_image_rgba(
            image,
            bitmap_index,
            subtitle_path_string,
            session_instance_id
        );
        if (validation_result == AssImageRgbaValidationResult::empty) {
            maybe_log_skipped_empty_subtitle_bitmap_diagnostics(
                request,
                bitmap_index,
                image.dst_x,
                image.dst_y,
                image.w,
                image.h,
                image.stride,
                bitmap_mode
            );
            continue;
        }

        drawable_bitmaps.push_back(DrawableAssImageRgba{
            .bitmap_index = bitmap_index,
            .image = &image
        });
    }

    return drawable_bitmaps;
}

}  // namespace utsure::core::subtitles::detail::libassmod
