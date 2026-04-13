#pragma once

#include "utsure/core/media/media_info.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace utsure::core::media {

enum class AudioOutputMode : std::uint8_t {
    auto_select = 0,
    copy_source,
    encode_aac,
    disable
};

enum class OutputAudioCodec : std::uint8_t {
    aac = 0
};

enum class ResolvedAudioOutputMode : std::uint8_t {
    disabled = 0,
    copy_source,
    encode_aac
};

struct AudioEncodeSettings final {
    AudioOutputMode mode{AudioOutputMode::auto_select};
    OutputAudioCodec codec{OutputAudioCodec::aac};
    int bitrate_kbps{192};
    std::optional<int> sample_rate_hz{};
    std::optional<int> channel_count{};
};

struct ResolvedAudioOutputPlan final {
    AudioOutputMode requested_mode{AudioOutputMode::auto_select};
    ResolvedAudioOutputMode resolved_mode{ResolvedAudioOutputMode::disabled};
    bool source_audio_present{false};
    bool output_present{false};
    bool copy_is_safe{false};
    std::string source_codec_name{};
    std::string output_codec_name{};
    int bitrate_kbps{0};
    int sample_rate_hz{0};
    int channel_count{0};
    std::string channel_layout_name{"unknown"};
    Rational time_base{};
    std::string decision_summary{};
    std::optional<std::string> requested_copy_blocker{};
};

struct AudioOutputResolveRequest final {
    std::filesystem::path output_path{};
    AudioEncodeSettings settings{};
    std::size_t segment_count{0};
    bool main_source_trimmed{false};
    const AudioStreamInfo *main_source_audio_stream{nullptr};
};

[[nodiscard]] ResolvedAudioOutputPlan resolve_audio_output_plan(const AudioOutputResolveRequest &request);
[[nodiscard]] std::string format_source_audio_summary(const std::optional<AudioStreamInfo> &audio_stream);
[[nodiscard]] std::string format_resolved_audio_output_summary(const ResolvedAudioOutputPlan &plan);
[[nodiscard]] const char *to_string(AudioOutputMode mode) noexcept;
[[nodiscard]] const char *to_string(OutputAudioCodec codec) noexcept;
[[nodiscard]] const char *to_string(ResolvedAudioOutputMode mode) noexcept;

}  // namespace utsure::core::media
