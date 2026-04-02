#include "utsure/core/subtitles/subtitle_frame_composer.hpp"

#include "subtitle_composition_diagnostics.hpp"
#include "subtitle_bitmap_compositor.hpp"

#include <exception>

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
    try {
        detail::validate_rgba_frame_surface(video_frame, "Subtitle composition");
    } catch (const std::exception &exception) {
        return make_error(
            "Subtitle composition requires rgba8 decoded frames with a single plane.",
            exception.what()
        );
    }

    SubtitleRenderResult render_result = subtitle_session.render(request);
    if (!render_result.succeeded()) {
        return make_error(
            render_result.error->message,
            render_result.error->actionable_hint
        );
    }

    detail::maybe_log_subtitle_frame_diagnostics(
        request,
        video_frame,
        render_result.rendered_frame->bitmaps.size(),
        "copied"
    );

    if (render_result.rendered_frame->bitmaps.empty()) {
        return SubtitleFrameComposeResult{
            .subtitles_applied = false,
            .error = std::nullopt
        };
    }

    try {
        for (std::size_t bitmap_index = 0; bitmap_index < render_result.rendered_frame->bitmaps.size(); ++bitmap_index) {
            const auto &bitmap = render_result.rendered_frame->bitmaps[bitmap_index];
            detail::maybe_log_subtitle_bitmap_diagnostics(
                request,
                bitmap_index,
                bitmap.origin_x,
                bitmap.origin_y,
                bitmap.width,
                bitmap.height,
                bitmap.line_stride_bytes,
                "copied"
            );
            detail::composite_bitmap_into_frame(video_frame, bitmap);
        }
    } catch (const std::exception &exception) {
        return make_error(
            "Subtitle composition failed while blending subtitle bitmaps into the decoded frame.",
            std::string(exception.what()) + " Context: " +
                detail::format_subtitle_compose_failure_context(request, video_frame)
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
