#pragma once

#include "utsure/core/media/decoded_media.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace utsure::core::media {

struct PreviewTrimRange final {
    std::int64_t trim_in_microseconds{0};
    std::optional<std::int64_t> trim_out_microseconds{};

    [[nodiscard]] bool has_trim() const noexcept {
        return trim_in_microseconds > 0 || trim_out_microseconds.has_value();
    }
};

[[nodiscard]] PreviewTrimRange normalize_preview_trim_range(
    std::int64_t trim_in_microseconds,
    std::optional<std::int64_t> trim_out_microseconds
);

[[nodiscard]] std::int64_t effective_trimmed_preview_frame_time(
    std::int64_t requested_time_microseconds,
    const PreviewTrimRange &trim_range
);

[[nodiscard]] std::vector<DecodedVideoFrame> trim_preview_video_frames(
    std::vector<DecodedVideoFrame> frames,
    const PreviewTrimRange &trim_range
);

[[nodiscard]] bool trimmed_preview_frames_cover_time(
    const std::vector<DecodedVideoFrame> &frames,
    std::int64_t requested_time_microseconds,
    const PreviewTrimRange &trim_range
);

[[nodiscard]] const DecodedVideoFrame *select_trimmed_preview_frame(
    const std::vector<DecodedVideoFrame> &frames,
    std::int64_t requested_time_microseconds,
    const PreviewTrimRange &trim_range
);

[[nodiscard]] std::vector<DecodedAudioSamples> trim_preview_audio_blocks(
    std::vector<DecodedAudioSamples> audio_blocks,
    const PreviewTrimRange &trim_range
);

}  // namespace utsure::core::media
