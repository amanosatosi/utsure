#pragma once

#include "utsure/core/media/audio_output.hpp"
#include "utsure/core/media/media_encoder.hpp"
#include "utsure/core/media/media_info.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace utsure::core::job {

struct OutputNamingRequest final {
    std::filesystem::path source_path{};
    std::filesystem::path output_directory{};
    std::string custom_text{};
    std::string extension_hint{};
    media::OutputVideoCodec video_codec{media::OutputVideoCodec::h264};
    media::AudioEncodeSettings audio_settings{};
    bool source_audio_known{false};
    std::optional<media::AudioStreamInfo> source_audio_stream{};
};

struct OutputNamingResult final {
    std::filesystem::path output_path{};
    std::string file_name{};
    std::string source_folder_name{};
    std::string custom_text{};
    std::string video_codec_tag{};
    std::string audio_codec_tag{};
    std::string extension{};
    int sequence_number{1};
};

class OutputNaming final {
public:
    [[nodiscard]] static OutputNamingResult suggest(const OutputNamingRequest &request);
};

}  // namespace utsure::core::job
