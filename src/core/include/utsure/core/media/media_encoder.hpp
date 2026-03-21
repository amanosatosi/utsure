#pragma once

#include "utsure/core/media/audio_output.hpp"
#include "utsure/core/media/decoded_media.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace utsure::core::media {

enum class OutputVideoCodec : std::uint8_t {
    h264 = 0,
    h265
};

enum class CpuUsageMode : std::uint8_t {
    auto_select = 0,
    conservative,
    aggressive
};

struct TranscodeThreadingSettings final {
    CpuUsageMode cpu_usage_mode{CpuUsageMode::auto_select};
    std::optional<int> decoder_thread_count_override{};
    std::optional<int> encoder_thread_count_override{};
};

struct VideoEncodeSettings final {
    OutputVideoCodec codec{OutputVideoCodec::h264};
    std::string preset{"medium"};
    int crf{23};
};

struct MediaEncodeRequest final {
    std::filesystem::path output_path{};
    VideoEncodeSettings video_settings{};
    AudioEncodeSettings audio_settings{};
    TranscodeThreadingSettings threading{};
};

struct EncodedMediaSummary final {
    std::filesystem::path output_path{};
    VideoEncodeSettings video_settings{};
    ResolvedAudioOutputPlan resolved_audio_output{};
    MediaSourceInfo output_info{};
    std::int64_t encoded_video_frame_count{0};
};

struct MediaEncodeError final {
    std::string output_path{};
    std::string message{};
    std::string actionable_hint{};
};

struct MediaEncodeResult final {
    std::optional<EncodedMediaSummary> encoded_media_summary{};
    std::optional<MediaEncodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

class MediaEncoder final {
public:
    [[nodiscard]] static MediaEncodeResult encode(
        const DecodedMediaSource &decoded_media_source,
        const MediaEncodeRequest &request
    ) noexcept;
};

[[nodiscard]] const char *to_string(OutputVideoCodec codec) noexcept;
[[nodiscard]] const char *to_string(CpuUsageMode mode) noexcept;
[[nodiscard]] int resolve_requested_ffmpeg_thread_count(
    const TranscodeThreadingSettings &settings,
    std::uint32_t logical_core_count
) noexcept;

}  // namespace utsure::core::media
