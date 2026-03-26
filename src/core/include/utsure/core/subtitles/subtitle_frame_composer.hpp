#pragma once

#include "utsure/core/media/decoded_media.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace utsure::core::subtitles {

struct SubtitleFrameComposeError final {
    std::string message{};
    std::string actionable_hint{};
};

struct SubtitleFrameComposeResult final {
    bool subtitles_applied{false};
    std::optional<SubtitleFrameComposeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

[[nodiscard]] SubtitleFrameComposeResult compose_subtitles_into_frame(
    media::DecodedVideoFrame &video_frame,
    SubtitleRenderSession &subtitle_session,
    std::int64_t timestamp_microseconds
) noexcept;

}  // namespace utsure::core::subtitles
