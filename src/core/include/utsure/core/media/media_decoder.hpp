#pragma once

#include "utsure/core/media/decoded_media.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace utsure::core::media {

struct MediaDecodeError final {
    std::string input_path{};
    std::string message{};
    std::string actionable_hint{};
};

struct MediaDecodeResult final {
    std::optional<DecodedMediaSource> decoded_media_source{};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct VideoFrameDecodeResult final {
    std::optional<DecodedVideoFrame> video_frame{};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct DecodeStreamSelection final {
    bool decode_video{true};
    bool decode_audio{true};
};

class MediaDecoder final {
public:
    [[nodiscard]] static MediaDecodeResult decode(
        const std::filesystem::path &input_path,
        const DecodeNormalizationPolicy &normalization_policy = {},
        const DecodeStreamSelection &stream_selection = {}
    ) noexcept;

    [[nodiscard]] static VideoFrameDecodeResult decode_video_frame_at_time(
        const std::filesystem::path &input_path,
        std::int64_t requested_time_microseconds,
        const DecodeNormalizationPolicy &normalization_policy = {}
    ) noexcept;
};

}  // namespace utsure::core::media
