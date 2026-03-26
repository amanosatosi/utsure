#pragma once

#include "utsure/core/media/media_encoder.hpp"
#include "utsure/core/media/media_info.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace utsure::core::job {

enum class EncodeJobStage : std::uint8_t {
    assembling_timeline = 0,
    decoding_segment,
    burning_in_subtitles,
    composing_timeline,
    encoding_output,
    completed
};

enum class EncodeJobLogLevel : std::uint8_t {
    info = 0,
    error
};

enum class EncodeJobProcessPriority : std::uint8_t {
    high = 0,
    above_normal,
    normal,
    below_normal,
    low
};

struct EncodeJobProgress final {
    EncodeJobStage stage{EncodeJobStage::assembling_timeline};
    int current_step{0};
    int total_steps{0};
    std::string message{};
    std::optional<double> overall_fraction{};
    std::optional<double> stage_fraction{};
    std::optional<std::uint64_t> encoded_video_frames{};
    std::optional<std::uint64_t> total_video_frames{};
    std::optional<std::int64_t> encoded_video_duration_us{};
    std::optional<std::int64_t> total_video_duration_us{};
    std::optional<double> encoded_fps{};
};

struct EncodeJobLogMessage final {
    EncodeJobLogLevel level{EncodeJobLogLevel::info};
    std::string message{};
};

class EncodeJobObserver {
public:
    virtual ~EncodeJobObserver() = default;

    virtual void on_progress(const EncodeJobProgress &progress);
    virtual void on_log(const EncodeJobLogMessage &message);
};

struct EncodeJobInput final {
    std::optional<std::filesystem::path> intro_source_path{};
    std::filesystem::path main_source_path{};
    std::optional<std::filesystem::path> outro_source_path{};
};

struct EncodeJobSubtitleSettings final {
    std::filesystem::path subtitle_path{};
    std::string format_hint{"ass"};
    timeline::SubtitleTimingMode timing_mode{timeline::SubtitleTimingMode::main_segment_only};
};

struct EncodeJobVideoOutputSettings final {
    media::OutputVideoCodec codec{media::OutputVideoCodec::h264};
    std::string preset{"medium"};
    int crf{23};
};

struct EncodeJobOutputSettings final {
    std::filesystem::path output_path{};
    EncodeJobVideoOutputSettings video{};
    media::AudioEncodeSettings audio{};
};

struct EncodeJobExecutionSettings final {
    EncodeJobProcessPriority process_priority{EncodeJobProcessPriority::below_normal};
    media::TranscodeThreadingSettings threading{};
};

struct EncodeJob final {
    EncodeJobInput input{};
    std::optional<EncodeJobSubtitleSettings> subtitles{};
    EncodeJobOutputSettings output{};
    EncodeJobExecutionSettings execution{};
};

struct EncodeJobSummary final {
    EncodeJob job{};
    media::MediaSourceInfo inspected_input_info{};
    timeline::TimelineCompositionSummary timeline_summary{};
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
    bool canceled{false};
};

struct EncodeJobResult final {
    std::optional<EncodeJobSummary> encode_job_summary{};
    std::optional<EncodeJobError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct EncodeJobRunOptions final {
    media::DecodeNormalizationPolicy decode_normalization_policy{};
    EncodeJobObserver *observer{nullptr};
};

class EncodeJobRunner final {
public:
    [[nodiscard]] static EncodeJobResult run(const EncodeJob &job, const EncodeJobRunOptions &options = {}) noexcept;
};

inline constexpr std::string_view kEncodeJobCanceledException{"utsure.encode_job.canceled"};
inline constexpr std::string_view kEncodeJobCanceledMessage{"Encode canceled by user."};

[[nodiscard]] const char *to_string(EncodeJobStage stage) noexcept;
[[nodiscard]] const char *to_string(EncodeJobLogLevel level) noexcept;
[[nodiscard]] const char *to_string(EncodeJobProcessPriority priority) noexcept;
[[nodiscard]] const char *to_display_string(EncodeJobProcessPriority priority) noexcept;

}  // namespace utsure::core::job
