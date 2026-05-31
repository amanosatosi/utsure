#pragma once

#include "utsure/core/job/output_naming.hpp"
#include "utsure/core/media/audio_output.hpp"
#include "utsure/core/media/media_encoder.hpp"

#include <QString>

#include <optional>
#include <string>

struct DuplicateEncodeEntryState final {
    QString source_path{};
    QString source_name{};
    QString output_name_custom_text{};
    QString output_path{};
    bool output_path_manual_override{false};
    bool same_as_input{true};
    bool subtitle_enabled{false};
    QString subtitle_path{};
    bool subtitle_manual_override{false};
    utsure::core::media::OutputVideoCodec video_codec{utsure::core::media::OutputVideoCodec::h265};
    QString video_preset{"fast"};
    int video_crf{22};
    utsure::core::media::AudioOutputMode audio_mode{utsure::core::media::AudioOutputMode::encode_aac};
    int audio_bitrate_kbps{192};
    std::optional<int> selected_audio_stream_index{};
    bool audio_track_manual_override{false};
};

struct DuplicateEncodeEntryRequest final {
    DuplicateEncodeEntryState original{};
    utsure::core::job::OutputNamingRequest automatic_output_request{};
    utsure::core::job::OutputNamingTemplate naming_template{};
    int stored_sequence_number{0};
};

struct DuplicateEncodeEntryResult final {
    DuplicateEncodeEntryState duplicate{};
    std::string sequence_counter_key{};
    int persisted_sequence_number{0};
    bool sequence_counter_reserved{false};
    bool output_path_generation_failed{false};
    QString diagnostic{};
};

[[nodiscard]] DuplicateEncodeEntryResult duplicate_encode_entry(const DuplicateEncodeEntryRequest &request);
