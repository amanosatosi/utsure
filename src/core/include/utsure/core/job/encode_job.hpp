#pragma once

#include "utsure/core/media/media_encoder.hpp"
#include "utsure/core/media/media_info.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace utsure::core::job {

struct EncodeJobInput final {
    std::filesystem::path main_source_path{};
};

struct EncodeJobSubtitleSettings final {
    std::filesystem::path subtitle_path{};
    std::string format_hint{"ass"};
};

struct EncodeJobVideoOutputSettings final {
    media::OutputVideoCodec codec{media::OutputVideoCodec::h264};
    std::string preset{"medium"};
    int crf{23};
};

struct EncodeJobOutputSettings final {
    std::filesystem::path output_path{};
    EncodeJobVideoOutputSettings video{};
};

struct EncodeJob final {
    EncodeJobInput input{};
    std::optional<EncodeJobSubtitleSettings> subtitles{};
    EncodeJobOutputSettings output{};
};

struct EncodeJobSummary final {
    EncodeJob job{};
    media::MediaSourceInfo inspected_input_info{};
    media::DecodeNormalizationPolicy decode_normalization_policy{};
    std::int64_t decoded_video_frame_count{0};
    std::int64_t decoded_audio_block_count{0};
    std::int64_t subtitled_video_frame_count{0};
    media::EncodedMediaSummary encoded_media_summary{};
};

struct EncodeJobError final {
    std::string main_source_path{};
    std::string output_path{};
    std::string message{};
    std::string actionable_hint{};
};

struct EncodeJobResult final {
    std::optional<EncodeJobSummary> encode_job_summary{};
    std::optional<EncodeJobError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

class EncodeJobRunner final {
public:
    [[nodiscard]] static EncodeJobResult run(
        const EncodeJob &job,
        const media::DecodeNormalizationPolicy &decode_normalization_policy = {}
    ) noexcept;
};

}  // namespace utsure::core::job
