#pragma once

#include "utsure/core/media/media_info.hpp"

#include <optional>
#include <vector>

namespace utsure::core::media {

[[nodiscard]] bool audio_stream_has_explicit_japanese_metadata(const AudioStreamInfo &audio_stream) noexcept;
[[nodiscard]] std::optional<int> select_preferred_audio_stream_index(
    const std::vector<AudioStreamInfo> &audio_streams
) noexcept;

}  // namespace utsure::core::media
