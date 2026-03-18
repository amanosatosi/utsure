#pragma once

#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/media/decoded_media.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace utsure::core::subtitles::burn_in {

struct SubtitleBurnInOutput final {
    media::DecodedMediaSource decoded_media_source{};
    std::int64_t subtitled_video_frame_count{0};
};

struct SubtitleBurnInError final {
    std::string message{};
    std::string actionable_hint{};
};

struct SubtitleBurnInResult final {
    std::optional<SubtitleBurnInOutput> output{};
    std::optional<SubtitleBurnInError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

[[nodiscard]] SubtitleBurnInResult apply(
    const media::DecodedMediaSource &decoded_media_source,
    SubtitleRenderer &subtitle_renderer,
    const job::EncodeJobSubtitleSettings &subtitle_settings
) noexcept;

}  // namespace utsure::core::subtitles::burn_in
