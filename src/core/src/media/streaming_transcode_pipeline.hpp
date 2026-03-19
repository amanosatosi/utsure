#pragma once

#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/media/media_encoder.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace utsure::core::media::streaming {

struct PipelineQueueLimits final {
    std::size_t video_packet_queue_depth{16};
    std::size_t audio_packet_queue_depth{16};
    std::size_t decoded_video_frame_queue_depth{2};
    std::size_t composited_video_frame_queue_depth{2};
    std::size_t decoded_audio_block_queue_depth{8};
    std::size_t encoded_packet_queue_depth{16};
};

inline constexpr PipelineQueueLimits kDefaultPipelineQueueLimits{};

struct PipelineMemoryBudget final {
    PipelineQueueLimits queue_limits{};
    std::uint64_t normalized_rgba_frame_bytes{0};
    std::uint64_t encoder_yuv420_frame_bytes{0};
    std::uint64_t normalized_audio_block_bytes{0};
    std::uint64_t packet_queue_reserve_bytes{0};
    std::uint64_t estimated_peak_bytes{0};
};

struct StreamingTranscodeSummary final {
    timeline::TimelineCompositionSummary timeline_summary{};
    std::int64_t decoded_video_frame_count{0};
    std::int64_t decoded_audio_block_count{0};
    std::int64_t subtitled_video_frame_count{0};
    EncodedMediaSummary encoded_media_summary{};
};

struct StreamingTranscodeError final {
    std::string message{};
    std::string actionable_hint{};
};

struct StreamingTranscodeResult final {
    std::optional<StreamingTranscodeSummary> summary{};
    std::optional<StreamingTranscodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct StreamingTranscodeRequest final {
    const timeline::TimelinePlan *timeline_plan{nullptr};
    const std::optional<job::EncodeJobSubtitleSettings> *subtitle_settings{nullptr};
    MediaEncodeRequest media_encode_request{};
    DecodeNormalizationPolicy normalization_policy{};
    subtitles::SubtitleRenderer *subtitle_renderer{nullptr};
    PipelineQueueLimits queue_limits{kDefaultPipelineQueueLimits};
};

[[nodiscard]] std::optional<PipelineMemoryBudget> build_memory_budget(
    const timeline::TimelinePlan &timeline_plan,
    const DecodeNormalizationPolicy &normalization_policy,
    const PipelineQueueLimits &queue_limits = kDefaultPipelineQueueLimits,
    bool subtitles_present = false
);

class StreamingTranscoder final {
public:
    [[nodiscard]] static StreamingTranscodeResult transcode(const StreamingTranscodeRequest &request) noexcept;
};

}  // namespace utsure::core::media::streaming
