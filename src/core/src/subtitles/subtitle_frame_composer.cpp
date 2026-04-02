#include "utsure/core/subtitles/subtitle_frame_composer.hpp"

#include "subtitle_bitmap_compositor.hpp"

#include <sstream>

namespace utsure::core::subtitles {

namespace {

SubtitleFrameComposeResult make_error(std::string message, std::string actionable_hint) {
    return SubtitleFrameComposeResult{
        .subtitles_applied = false,
        .error = SubtitleFrameComposeError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

void emit_bitmap_diagnostics(
    const SubtitleRenderRequest &request,
    const media::DecodedVideoFrame &video_frame,
    const RenderedSubtitleFrame &rendered_frame
) {
    if (request.debug_context == nullptr ||
        !request.debug_context->log_bitmap_details ||
        !request.debug_context->log_callback) {
        return;
    }

    std::ostringstream frame_message;
    frame_message << "Subtitle composition diagnostics: frame_index="
                  << request.debug_context->decoded_frame_index
                  << ", decoded_pts=";
    if (request.debug_context->decoded_frame_pts.has_value()) {
        frame_message << *request.debug_context->decoded_frame_pts;
    } else {
        frame_message << "unknown";
    }
    frame_message << ", output_pts=";
    if (request.debug_context->output_pts.has_value()) {
        frame_message << *request.debug_context->output_pts;
    } else {
        frame_message << "unknown";
    }
    frame_message << ", subtitle_timestamp_us=" << request.timestamp_microseconds
                  << ", worker_id=" << request.debug_context->worker_id
                  << ", session_id=" << request.debug_context->session_id
                  << ", bitmap_count=" << rendered_frame.bitmaps.size()
                  << ", destination=" << video_frame.width << 'x' << video_frame.height
                  << ", destination_stride="
                  << (video_frame.planes.empty() ? 0 : video_frame.planes.front().line_stride_bytes);
    request.debug_context->log_callback(frame_message.str());

    for (std::size_t bitmap_index = 0; bitmap_index < rendered_frame.bitmaps.size(); ++bitmap_index) {
        const auto &bitmap = rendered_frame.bitmaps[bitmap_index];
        std::ostringstream bitmap_message;
        bitmap_message << "Subtitle bitmap[" << bitmap_index << "]: origin="
                       << bitmap.origin_x << ',' << bitmap.origin_y
                       << ", size=" << bitmap.width << 'x' << bitmap.height
                       << ", stride=" << bitmap.line_stride_bytes
                       << ", bytes=" << bitmap.bytes.size();
        request.debug_context->log_callback(bitmap_message.str());
    }
}

}  // namespace

SubtitleFrameComposeResult compose_subtitles_into_frame(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    const SubtitleRenderRequest &request
) noexcept {
    return subtitle_session.compose_into_frame(video_frame, request);
}

SubtitleFrameComposeResult compose_subtitles_into_frame_via_render_result(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    const SubtitleRenderRequest &request
) noexcept {
    if (!detail::is_rgba_frame_layout_supported(video_frame)) {
        return make_error(
            "Subtitle composition requires rgba8 decoded frames with a single plane.",
            "Keep the decoded video path normalized to single-plane rgba8 before requesting subtitle composition."
        );
    }

    SubtitleRenderResult render_result = subtitle_session.render(request);
    if (!render_result.succeeded()) {
        return make_error(
            render_result.error->message,
            render_result.error->actionable_hint
        );
    }

    emit_bitmap_diagnostics(request, video_frame, *render_result.rendered_frame);

    if (render_result.rendered_frame->bitmaps.empty()) {
        return SubtitleFrameComposeResult{
            .subtitles_applied = false,
            .error = std::nullopt
        };
    }

    try {
        for (const auto &bitmap : render_result.rendered_frame->bitmaps) {
            detail::composite_bitmap_into_frame(video_frame, bitmap);
        }
    } catch (const std::exception &exception) {
        return make_error(
            "Subtitle composition failed while blending subtitle bitmaps into the decoded frame.",
            exception.what()
        );
    }

    return SubtitleFrameComposeResult{
        .subtitles_applied = true,
        .error = std::nullopt
    };
}

SubtitleFrameComposeResult compose_subtitles_into_frame(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    const std::int64_t timestamp_microseconds
) noexcept {
    return compose_subtitles_into_frame(
        video_frame,
        subtitle_session,
        SubtitleRenderRequest{
            .timestamp_microseconds = timestamp_microseconds
        }
    );
}

}  // namespace utsure::core::subtitles
