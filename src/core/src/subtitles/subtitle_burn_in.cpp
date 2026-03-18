#include "subtitle_burn_in.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

namespace utsure::core::subtitles::burn_in {

namespace {

SubtitleBurnInResult make_error(std::string message, std::string actionable_hint) {
    return SubtitleBurnInResult{
        .output = std::nullopt,
        .error = SubtitleBurnInError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

bool is_rgba_frame_layout_supported(const media::DecodedVideoFrame &video_frame) {
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
    if (plane.line_stride_bytes <= 0 || bitmap.line_stride_bytes < (bitmap.width * 4)) {
        throw std::runtime_error("Subtitle burn-in received an invalid plane or bitmap stride.");
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

            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const std::uint8_t source_value = source_row[source_offset + channel];
                const std::uint8_t destination_value = destination_row[destination_offset + channel];
                const std::uint16_t blended_value = static_cast<std::uint16_t>(source_value) +
                    static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(destination_value) * inverse_alpha + 127U) / 255U
                    );
                destination_row[destination_offset + channel] =
                    static_cast<std::uint8_t>(std::min<std::uint16_t>(255U, blended_value));
            }

            destination_row[destination_offset + 3U] = 255U;
        }
    }
}

}  // namespace

bool SubtitleBurnInResult::succeeded() const noexcept {
    return output.has_value() && !error.has_value();
}

SubtitleBurnInResult apply(
    const media::DecodedMediaSource &decoded_media_source,
    SubtitleRenderer &subtitle_renderer,
    const job::EncodeJobSubtitleSettings &subtitle_settings
) noexcept {
    if (decoded_media_source.video_frames.empty()) {
        return make_error(
            "Subtitle burn-in requires at least one decoded video frame.",
            "Decode a source video stream before requesting subtitle burn-in."
        );
    }

    const auto &first_frame = decoded_media_source.video_frames.front();
    if (!is_rgba_frame_layout_supported(first_frame)) {
        return make_error(
            "Subtitle burn-in requires rgba8 decoded frames with a single plane.",
            "Keep the decoded main-source path normalized to rgba8 before subtitle composition."
        );
    }

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = subtitle_settings.subtitle_path,
        .format_hint = subtitle_settings.format_hint,
        .canvas_width = first_frame.width,
        .canvas_height = first_frame.height,
        .sample_aspect_ratio = first_frame.sample_aspect_ratio
    };

    SubtitleRenderSessionResult session_result = subtitle_renderer.create_session(session_request);
    if (!session_result.succeeded()) {
        return make_error(
            session_result.error->message,
            session_result.error->actionable_hint
        );
    }

    media::DecodedMediaSource composited_media_source = decoded_media_source;
    std::int64_t subtitled_video_frame_count = 0;

    for (auto &video_frame : composited_media_source.video_frames) {
        if (!is_rgba_frame_layout_supported(video_frame)) {
            return make_error(
                "Subtitle burn-in encountered an unsupported decoded frame layout.",
                "Keep all video frames normalized as single-plane rgba8 before subtitle composition."
            );
        }

        SubtitleRenderResult render_result = session_result.session->render(SubtitleRenderRequest{
            .timestamp_microseconds = video_frame.timestamp.start_microseconds
        });
        if (!render_result.succeeded()) {
            return make_error(
                render_result.error->message,
                render_result.error->actionable_hint
            );
        }

        if (render_result.rendered_frame->bitmaps.empty()) {
            continue;
        }

        for (const auto &bitmap : render_result.rendered_frame->bitmaps) {
            try {
                composite_bitmap_into_frame(video_frame, bitmap);
            } catch (const std::exception &exception) {
                return make_error(
                    "Subtitle burn-in failed while compositing subtitle bitmaps into decoded video frames.",
                    exception.what()
                );
            }
        }

        ++subtitled_video_frame_count;
    }

    return SubtitleBurnInResult{
        .output = SubtitleBurnInOutput{
            .decoded_media_source = std::move(composited_media_source),
            .subtitled_video_frame_count = subtitled_video_frame_count
        },
        .error = std::nullopt
    };
}

}  // namespace utsure::core::subtitles::burn_in
