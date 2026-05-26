#pragma once

#include "utsure/core/media/audio_output.hpp"
#include "utsure/core/media/media_encoder.hpp"
#include "utsure/core/media/media_info.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace utsure::core::job {

enum class OutputNamingTokenType {
    selected_text = 0,
    source_folder_name,
    sequence_number,
    codec,
    resolution
};

struct OutputNamingToken final {
    OutputNamingTokenType type{OutputNamingTokenType::source_folder_name};
    bool enabled{true};
    int sequence_padding{2};
};

struct OutputNamingTemplate final {
    bool enabled{true};
    std::string separator{" - "};
    std::vector<OutputNamingToken> tokens{};
};

struct OutputNamingRequest final {
    std::filesystem::path source_path{};
    std::filesystem::path output_directory{};
    std::string custom_text{};
    std::string extension_hint{};
    media::OutputVideoCodec video_codec{media::OutputVideoCodec::h264};
    media::AudioEncodeSettings audio_settings{};
    bool source_audio_known{false};
    std::optional<media::AudioStreamInfo> source_audio_stream{};
    std::optional<int> output_width{};
    std::optional<int> output_height{};
};

struct OutputNamingResult final {
    std::filesystem::path output_path{};
    std::string file_name{};
    std::string source_folder_name{};
    std::string custom_text{};
    std::string video_codec_tag{};
    std::string audio_codec_tag{};
    std::string resolution_tag{};
    std::string extension{};
    int sequence_number{1};
    std::string sequence_counter_key{"default"};
};

struct OutputNamingReservationRequest final {
    OutputNamingRequest request{};
    OutputNamingTemplate naming_template{};
    int stored_sequence_number{0};
    std::vector<std::filesystem::path> excluded_output_paths{};
};

struct OutputNamingReservationResult final {
    OutputNamingResult result{};
    std::string sequence_counter_key{"default"};
    int assigned_sequence_number{0};
    int persisted_sequence_number{0};
};

class OutputNaming final {
public:
    [[nodiscard]] static OutputNamingTemplate default_template();
    [[nodiscard]] static const char *to_string(OutputNamingTokenType type) noexcept;
    [[nodiscard]] static std::optional<OutputNamingTokenType> token_type_from_string(std::string_view text);
    [[nodiscard]] static std::string sequence_counter_key(
        const OutputNamingRequest &request,
        const OutputNamingTemplate &naming_template
    );
    [[nodiscard]] static OutputNamingResult suggest(const OutputNamingRequest &request);
    [[nodiscard]] static OutputNamingResult suggest(
        const OutputNamingRequest &request,
        const OutputNamingTemplate &naming_template
    );
    [[nodiscard]] static OutputNamingResult suggest(
        const OutputNamingRequest &request,
        const OutputNamingTemplate &naming_template,
        int stored_sequence_number
    );
    [[nodiscard]] static OutputNamingReservationResult reserve_next(
        const OutputNamingRequest &request,
        const OutputNamingTemplate &naming_template,
        int stored_sequence_number
    );
    [[nodiscard]] static std::vector<OutputNamingResult> reserve_batch(const std::vector<OutputNamingRequest> &requests);
    [[nodiscard]] static std::vector<OutputNamingReservationResult> reserve_batch(
        const std::vector<OutputNamingReservationRequest> &requests
    );
};

}  // namespace utsure::core::job
