#pragma once

#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <cstdint>

namespace utsure::core::subtitles {

[[nodiscard]] SubtitleFrameComposeResult compose_subtitles_into_frame(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    const SubtitleRenderRequest &request
) noexcept;

[[nodiscard]] SubtitleFrameComposeResult compose_subtitles_into_frame_via_render_result(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    const SubtitleRenderRequest &request
) noexcept;

[[nodiscard]] SubtitleFrameComposeResult compose_subtitles_into_frame(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    std::int64_t timestamp_microseconds
) noexcept;

}  // namespace utsure::core::subtitles
