#include "utsure/core/subtitles/subtitle_frame_composer.hpp"

namespace utsure::core::subtitles {

SubtitleFrameComposeResult compose_subtitles_into_frame(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    const std::int64_t timestamp_microseconds
) noexcept {
    return subtitle_session.compose_into_frame(video_frame, SubtitleRenderRequest{
        .timestamp_microseconds = timestamp_microseconds
    });
}

}  // namespace utsure::core::subtitles
