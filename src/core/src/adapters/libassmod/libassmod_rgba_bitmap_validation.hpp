#pragma once

#include "../../subtitles/subtitle_composition_diagnostics.hpp"

extern "C" {
#include <ass/ass.h>
}

#include <cstddef>
#include <cstdint>
#include <optional>
#include <limits>
#include <sstream>
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

[[nodiscard]] inline std::optional<std::uint64_t> estimate_ass_image_rgba_bytes(
    const ASS_ImageRGBA &image
) noexcept {
    if (image.w <= 0 || image.h <= 0 || image.stride <= 0) {
        return std::nullopt;
    }

    const auto minimum_stride = static_cast<std::int64_t>(image.w) * 4LL;
    if (minimum_stride <= 0 || static_cast<std::int64_t>(image.stride) < minimum_stride) {
        return std::nullopt;
    }

    const auto stride = static_cast<std::uint64_t>(image.stride);
    const auto height = static_cast<std::uint64_t>(image.h);
    if (height != 0U && stride > (std::numeric_limits<std::uint64_t>::max() / height)) {
        return std::nullopt;
    }

    return stride * height;
}

[[nodiscard]] inline std::optional<std::uint64_t> sum_ass_image_rgba_alpha(
    const ASS_ImageRGBA &image
) noexcept {
    constexpr std::uint64_t kMaximumDiagnosticAlphaScanBytes = 512ULL * 1024ULL * 1024ULL;

    const auto estimated_bytes = estimate_ass_image_rgba_bytes(image);
    if (!estimated_bytes.has_value() || *estimated_bytes > kMaximumDiagnosticAlphaScanBytes ||
        image.rgba == nullptr) {
        return std::nullopt;
    }

    std::uint64_t alpha_sum = 0;
    for (int row = 0; row < image.h; ++row) {
        const auto *source_row = image.rgba +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(image.stride);
        for (int column = 0; column < image.w; ++column) {
            alpha_sum += source_row[static_cast<std::size_t>(column) * 4U + 3U];
        }
    }

    return alpha_sum;
}

[[nodiscard]] inline std::string format_ass_image_rgba_node_diagnostics(
    const ASS_ImageRGBA &image,
    const std::size_t bitmap_index,
    const std::string_view bitmap_mode,
    const std::string_view phase
) {
    const auto alpha_sum = sum_ass_image_rgba_alpha(image);
    const auto estimated_bytes = estimate_ass_image_rgba_bytes(image);

    std::ostringstream message;
    message << "libassmod RGBA node[" << bitmap_index << "] " << phase
            << ": mode=" << bitmap_mode
            << ", type=" << image.type
            << ", origin=" << image.dst_x << ',' << image.dst_y
            << ", size=" << image.w << 'x' << image.h
            << ", stride=" << image.stride
            << ", rgba=" << static_cast<const void *>(image.rgba)
            << ", alpha_sum=";
    if (alpha_sum.has_value()) {
        message << *alpha_sum;
    } else {
        message << "unavailable";
    }

    message << ", estimated_bytes=";
    if (estimated_bytes.has_value()) {
        message << *estimated_bytes;
    } else {
        message << "unavailable";
    }

    return message.str();
}

inline void maybe_log_ass_image_rgba_nodes_after_render(
    const std::vector<ASS_ImageRGBA *> &image_nodes,
    const SubtitleRenderRequest &request,
    const std::string_view bitmap_mode
) {
    if (!should_log_subtitle_bitmap_diagnostics(request)) {
        return;
    }

    for (std::size_t bitmap_index = 0; bitmap_index < image_nodes.size(); ++bitmap_index) {
        if (image_nodes[bitmap_index] == nullptr) {
            request.debug_context->log_callback(
                "libassmod RGBA node[" + std::to_string(bitmap_index) +
                "] after_render: mode=" + std::string(bitmap_mode) + ", node=null"
            );
            continue;
        }

        request.debug_context->log_callback(
            format_ass_image_rgba_node_diagnostics(
                *image_nodes[bitmap_index],
                bitmap_index,
                bitmap_mode,
                "after_render"
            )
        );
    }
}

inline void maybe_log_ass_image_rgba_collection_decision(
    const SubtitleRenderRequest &request,
    const ASS_ImageRGBA &image,
    const std::size_t bitmap_index,
    const std::string_view bitmap_mode,
    const std::string_view decision,
    const std::string_view reason
) {
    if (!should_log_subtitle_bitmap_diagnostics(request)) {
        return;
    }

    request.debug_context->log_callback(
        format_ass_image_rgba_node_diagnostics(image, bitmap_index, bitmap_mode, decision) +
        ", decision_reason=" + std::string(reason)
    );
}

[[nodiscard]] inline AssImageRgbaValidationResult validate_ass_image_rgba(
    const ASS_ImageRGBA &image,
    const std::size_t,
    const std::string &,
    const int
) noexcept {
    if (image.w <= 0 || image.h <= 0) {
        return AssImageRgbaValidationResult::empty;
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
        if (image_nodes[bitmap_index] == nullptr) {
            continue;
        }

        const ASS_ImageRGBA &image = *image_nodes[bitmap_index];
        const AssImageRgbaValidationResult validation_result = validate_ass_image_rgba(
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
            maybe_log_ass_image_rgba_collection_decision(
                request,
                image,
                bitmap_index,
                bitmap_mode,
                "rejected",
                "empty"
            );
            continue;
        }

        maybe_log_ass_image_rgba_collection_decision(
            request,
            image,
            bitmap_index,
            bitmap_mode,
            "accepted",
            "trusted_libassmod_output"
        );
        drawable_bitmaps.push_back(DrawableAssImageRgba{
            .bitmap_index = bitmap_index,
            .image = &image
        });
    }

    return drawable_bitmaps;
}

}  // namespace utsure::core::subtitles::detail::libassmod
