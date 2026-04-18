#pragma once

#include "subtitle_bitmap_compositor.hpp"

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

namespace utsure::core::subtitles::detail {

inline bool should_log_subtitle_frame_diagnostics(const SubtitleRenderRequest &request) {
    return request.debug_context != nullptr &&
        request.debug_context->log_frame_details &&
        static_cast<bool>(request.debug_context->log_callback);
}

inline bool should_log_subtitle_bitmap_diagnostics(const SubtitleRenderRequest &request) {
    return request.debug_context != nullptr &&
        request.debug_context->log_bitmap_details &&
        static_cast<bool>(request.debug_context->log_callback);
}

inline std::string format_subtitle_frame_diagnostics(
    const SubtitleRenderRequest &request,
    const media::DecodedVideoFrame &video_frame,
    const std::size_t bitmap_count,
    const std::string_view bitmap_mode
) {
    std::ostringstream message;
    message << "Subtitle composition diagnostics: frame_index=";
    if (request.debug_context != nullptr) {
        message << request.debug_context->decoded_frame_index;
    } else {
        message << "unknown";
    }

    message << ", decoded_pts=";
    if (request.debug_context != nullptr && request.debug_context->decoded_frame_pts.has_value()) {
        message << *request.debug_context->decoded_frame_pts;
    } else {
        message << "unknown";
    }

    message << ", output_pts=";
    if (request.debug_context != nullptr && request.debug_context->output_pts.has_value()) {
        message << *request.debug_context->output_pts;
    } else {
        message << "unknown";
    }

    message << ", subtitle_timestamp_us=" << request.timestamp_microseconds
            << ", worker_id=";
    if (request.debug_context != nullptr) {
        message << request.debug_context->worker_id;
    } else {
        message << "unknown";
    }

    message << ", session_id=";
    if (request.debug_context != nullptr) {
        message << request.debug_context->session_id;
    } else {
        message << "unknown";
    }

    message << ", bitmap_mode=" << bitmap_mode
            << ", bitmap_count=" << bitmap_count
            << ", destination=" << video_frame.width << 'x' << video_frame.height
            << ", destination_stride="
            << (video_frame.planes.empty() ? 0 : video_frame.planes.front().line_stride_bytes);
    return message.str();
}

inline std::string format_subtitle_bitmap_diagnostics(
    const std::size_t bitmap_index,
    const int origin_x,
    const int origin_y,
    const int width,
    const int height,
    const int stride,
    const std::string_view bitmap_mode
) {
    std::ostringstream message;
    message << "Subtitle bitmap[" << bitmap_index << "]: mode=" << bitmap_mode
            << ", origin=" << origin_x << ',' << origin_y
            << ", size=" << width << 'x' << height
            << ", stride=" << stride;
    return message.str();
}

inline std::string format_subtitle_compose_failure_context(
    const SubtitleRenderRequest &request,
    const media::DecodedVideoFrame &video_frame
) {
    return format_subtitle_frame_diagnostics(request, video_frame, 0U, "unknown");
}

inline void maybe_log_subtitle_frame_diagnostics(
    const SubtitleRenderRequest &request,
    const media::DecodedVideoFrame &video_frame,
    const std::size_t bitmap_count,
    const std::string_view bitmap_mode
) {
    if (!should_log_subtitle_frame_diagnostics(request)) {
        return;
    }

    request.debug_context->log_callback(
        format_subtitle_frame_diagnostics(request, video_frame, bitmap_count, bitmap_mode)
    );
}

inline void maybe_log_subtitle_bitmap_diagnostics(
    const SubtitleRenderRequest &request,
    const std::size_t bitmap_index,
    const int origin_x,
    const int origin_y,
    const int width,
    const int height,
    const int stride,
    const std::string_view bitmap_mode
) {
    if (!should_log_subtitle_bitmap_diagnostics(request)) {
        return;
    }

    request.debug_context->log_callback(
        format_subtitle_bitmap_diagnostics(bitmap_index, origin_x, origin_y, width, height, stride, bitmap_mode)
    );
}

inline std::string format_skipped_empty_subtitle_bitmap_diagnostics(
    const std::size_t bitmap_index,
    const int origin_x,
    const int origin_y,
    const int width,
    const int height,
    const int stride,
    const std::string_view bitmap_mode
) {
    std::ostringstream message;
    message << "Subtitle bitmap[" << bitmap_index << "] skipped as empty output: mode=" << bitmap_mode
            << ", origin=" << origin_x << ',' << origin_y
            << ", size=" << width << 'x' << height
            << ", stride=" << stride;
    return message.str();
}

inline void maybe_log_skipped_empty_subtitle_bitmap_diagnostics(
    const SubtitleRenderRequest &request,
    const std::size_t bitmap_index,
    const int origin_x,
    const int origin_y,
    const int width,
    const int height,
    const int stride,
    const std::string_view bitmap_mode
) {
    if (!should_log_subtitle_frame_diagnostics(request)) {
        return;
    }

    request.debug_context->log_callback(
        format_skipped_empty_subtitle_bitmap_diagnostics(
            bitmap_index,
            origin_x,
            origin_y,
            width,
            height,
            stride,
            bitmap_mode
        )
    );
}

}  // namespace utsure::core::subtitles::detail
