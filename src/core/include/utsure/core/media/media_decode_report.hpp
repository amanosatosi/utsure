#pragma once

#include "utsure/core/media/decoded_media.hpp"

#include <cstddef>
#include <string>

namespace utsure::core::media {

struct MediaDecodeReportOptions final {
    std::size_t representative_video_frames{3};
    std::size_t representative_audio_blocks{3};
};

[[nodiscard]] std::string format_media_decode_report(
    const DecodedMediaSource &decoded_media_source,
    const MediaDecodeReportOptions &options = {}
);

}  // namespace utsure::core::media
