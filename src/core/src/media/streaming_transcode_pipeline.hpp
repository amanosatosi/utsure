#pragma once

#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/media/media_encoder.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace utsure::core::media::streaming {

struct PipelineQueueLimits final {
    std::size_t video_frame_queue_depth{70};
    std::size_t decoded_audio_block_queue_depth{8};
    std::int64_t startup_audio_preroll_microseconds{250000};
};

inline constexpr PipelineQueueLimits kDefaultPipelineQueueLimits{};

struct StreamingRuntimeBehavior final {
    std::uint32_t detected_logical_core_count{0};
    std::uint32_t effective_logical_core_count{1};
    CpuUsageMode cpu_usage_mode{CpuUsageMode::auto_select};
    int selected_video_decoder_thread_count{0};
    int selected_video_decoder_thread_type{0};
    int selected_video_encoder_thread_count{0};
    int selected_video_encoder_thread_type{0};
    std::size_t video_processing_worker_count{1};
    std::size_t video_frame_queue_depth{0};
    std::size_t decoded_audio_block_queue_depth{0};
};

struct StreamingStageTiming final {
    std::uint64_t sample_count{0};
    std::uint64_t total_microseconds{0};
};

struct StreamingPerformanceMetrics final {
    StreamingStageTiming video_decode{};
    StreamingStageTiming video_process{};
    StreamingStageTiming subtitle_compose{};
    StreamingStageTiming video_encode{};
    std::int64_t total_elapsed_microseconds{0};
    double average_output_fps{0.0};
};

struct PipelineMemoryBudget final {
    PipelineQueueLimits queue_limits{};
    std::uint64_t normalized_rgba_frame_bytes{0};
    std::uint64_t subtitle_scratch_bytes{0};
    std::uint64_t encoder_yuv420_frame_bytes{0};
    std::uint64_t normalized_audio_block_bytes{0};
    std::uint64_t audio_encoder_carry_bytes{0};
    std::uint64_t estimated_peak_bytes{0};
};

struct StreamingTranscodeSummary final {
    timeline::TimelineCompositionSummary timeline_summary{};
    std::int64_t decoded_video_frame_count{0};
    std::int64_t decoded_audio_block_count{0};
    std::int64_t subtitled_video_frame_count{0};
    StreamingRuntimeBehavior runtime_behavior{};
    StreamingPerformanceMetrics performance_metrics{};
    EncodedMediaSummary encoded_media_summary{};
};

struct StreamingTranscodeError final {
    std::string message{};
    std::string actionable_hint{};
    bool canceled{false};
};

struct StreamingTranscodeResult final {
    std::optional<StreamingTranscodeSummary> summary{};
    std::optional<StreamingTranscodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct StreamingEncodeProgress final {
    double stage_fraction{0.0};
    std::uint64_t encoded_video_frames{0};
    std::uint64_t total_video_frames{0};
    std::int64_t encoded_video_duration_us{0};
    std::int64_t total_video_duration_us{0};
    std::optional<double> encoded_fps{};
};

struct StreamingTranscodeRequest final {
    const timeline::TimelinePlan *timeline_plan{nullptr};
    const std::optional<job::EncodeJobSubtitleSettings> *subtitle_settings{nullptr};
    MediaEncodeRequest media_encode_request{};
    DecodeNormalizationPolicy normalization_policy{};
    subtitles::SubtitleRenderer *subtitle_renderer{nullptr};
    PipelineQueueLimits queue_limits{kDefaultPipelineQueueLimits};
    std::function<void(const StreamingEncodeProgress &progress)> progress_callback{};
    std::function<void(const std::string &message)> log_callback{};
};

[[nodiscard]] std::optional<PipelineMemoryBudget> build_memory_budget(
    const timeline::TimelinePlan &timeline_plan,
    const DecodeNormalizationPolicy &normalization_policy,
    const PipelineQueueLimits &queue_limits = kDefaultPipelineQueueLimits,
    bool subtitles_present = false
);
[[nodiscard]] StreamingRuntimeBehavior resolve_streaming_runtime_behavior(
    TranscodeThreadingSettings threading = {},
    const PipelineQueueLimits &queue_limits = kDefaultPipelineQueueLimits,
    std::optional<std::uint32_t> logical_core_count_override = std::nullopt
) noexcept;
[[nodiscard]] std::string format_encoder_threading_summary(
    const StreamingRuntimeBehavior &behavior,
    OutputVideoCodec codec = OutputVideoCodec::h264
);

class StreamingTranscoder final {
public:
    [[nodiscard]] static StreamingTranscodeResult transcode(const StreamingTranscodeRequest &request) noexcept;
};

}  // namespace utsure::core::media::streaming
