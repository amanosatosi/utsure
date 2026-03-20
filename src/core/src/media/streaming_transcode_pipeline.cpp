#include "streaming_transcode_pipeline.hpp"

#include "ffmpeg_media_support.hpp"
#include "../subtitles/subtitle_bitmap_compositor.hpp"
#include "utsure/core/media/media_inspector.hpp"

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace utsure::core::media::streaming {

namespace {

using ffmpeg_support::FormatContextHandle;

struct CodecContextDeleter final {
    void operator()(AVCodecContext *codec_context) const noexcept {
        if (codec_context == nullptr) {
            return;
        }

        avcodec_free_context(&codec_context);
    }
};

using CodecContextHandle = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

struct FrameDeleter final {
    void operator()(AVFrame *frame) const noexcept {
        if (frame == nullptr) {
            return;
        }

        av_frame_free(&frame);
    }
};

using FrameHandle = std::unique_ptr<AVFrame, FrameDeleter>;

struct PacketDeleter final {
    void operator()(AVPacket *packet) const noexcept {
        if (packet == nullptr) {
            return;
        }

        av_packet_free(&packet);
    }
};

using PacketHandle = std::unique_ptr<AVPacket, PacketDeleter>;

struct CodecParametersDeleter final {
    void operator()(AVCodecParameters *parameters) const noexcept {
        if (parameters == nullptr) {
            return;
        }

        avcodec_parameters_free(&parameters);
    }
};

using CodecParametersHandle = std::unique_ptr<AVCodecParameters, CodecParametersDeleter>;

struct OutputFormatContextDeleter final {
    void operator()(AVFormatContext *format_context) const noexcept {
        if (format_context == nullptr) {
            return;
        }

        if (format_context->pb != nullptr &&
            format_context->oformat != nullptr &&
            (format_context->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&format_context->pb);
        }

        avformat_free_context(format_context);
    }
};

using OutputFormatContextHandle = std::unique_ptr<AVFormatContext, OutputFormatContextDeleter>;

struct SwsContextDeleter final {
    void operator()(SwsContext *scale_context) const noexcept {
        if (scale_context == nullptr) {
            return;
        }

        sws_freeContext(scale_context);
    }
};

using SwsContextHandle = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct SwrContextDeleter final {
    void operator()(SwrContext *resample_context) const noexcept {
        if (resample_context == nullptr) {
            return;
        }

        swr_free(&resample_context);
    }
};

using SwrContextHandle = std::unique_ptr<SwrContext, SwrContextDeleter>;

constexpr std::uint64_t kInFlightRgbaSurfaceCount = 1ULL;
constexpr std::uint64_t kEncoderWorkingSurfaceCount = 2ULL;

template <typename T>
class BoundedQueue final {
public:
    explicit BoundedQueue(const std::size_t max_depth)
        : max_depth_(max_depth) {
        if (max_depth_ == 0U) {
            throw std::runtime_error("Bounded pipeline queues require a positive max depth.");
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return queue_.empty();
    }

    [[nodiscard]] bool full() const noexcept {
        return queue_.size() >= max_depth_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return queue_.size();
    }

    [[nodiscard]] const T &front() const {
        if (queue_.empty()) {
            throw std::runtime_error("A streaming pipeline queue front was requested while empty.");
        }

        return queue_.front();
    }

    void push(T value) {
        if (full()) {
            throw std::runtime_error("A streaming pipeline queue exceeded its configured depth.");
        }

        queue_.push_back(std::move(value));
    }

    T pop() {
        if (queue_.empty()) {
            throw std::runtime_error("A streaming pipeline queue was popped while empty.");
        }

        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }

private:
    std::size_t max_depth_{0};
    std::deque<T> queue_{};
};

struct TimestampSeed final {
    std::int64_t source_pts{0};
    TimestampOrigin origin{TimestampOrigin::stream_cursor};
};

struct VideoOutputPlan final {
    int width{0};
    int height{0};
    Rational time_base{};
    Rational average_frame_rate{};
    Rational sample_aspect_ratio{1, 1};
    std::int64_t frame_duration_pts{1};
    std::int64_t frame_duration_microseconds{0};
};

struct AudioOutputPlan final {
    ResolvedAudioOutputPlan resolved{};
    int sample_rate{0};
    int channel_count{0};
    Rational time_base{};
    std::string channel_layout_name{"unknown"};
    int bitrate_kbps{0};

    [[nodiscard]] bool encodes_audio() const noexcept {
        return resolved.resolved_mode == ResolvedAudioOutputMode::encode_aac;
    }

    [[nodiscard]] bool copies_audio() const noexcept {
        return resolved.resolved_mode == ResolvedAudioOutputMode::copy_source;
    }
};

struct AudioCopyTemplate final {
    CodecParametersHandle codec_parameters{};
    Rational time_base{};
    std::int64_t start_pts{0};
};

struct SegmentDecoderResources final {
    FormatContextHandle format_context{};
    CodecContextHandle video_decoder{};
    AVStream *video_stream{nullptr};
    CodecContextHandle audio_decoder{};
    AVStream *audio_stream{nullptr};
};

struct SegmentProcessResult final {
    timeline::TimelineSegmentSummary segment_summary{};
    std::int64_t decoded_video_frame_count{0};
    std::int64_t decoded_audio_block_count{0};
    std::int64_t subtitled_video_frame_count{0};
};

struct ResolvedVideoFrameTiming final {
    std::int64_t output_pts{0};
    std::optional<std::int64_t> output_duration_pts{};
};

struct PendingVideoFrameOutput final {
    DecodedVideoFrame frame{};
    ResolvedVideoFrameTiming timing{};
};

struct EncoderSelection final {
    const char *encoder_name{""};
};

struct EstimatedVideoProgressTotals final {
    std::uint64_t total_video_frames{0};
    std::int64_t total_video_duration_pts{0};
    std::int64_t total_video_duration_us{0};
};

StreamingTranscodeResult make_error(std::string message, std::string actionable_hint) {
    return StreamingTranscodeResult{
        .summary = std::nullopt,
        .error = StreamingTranscodeError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

AVRational to_av_rational(const Rational &value) {
    return AVRational{
        .num = static_cast<int>(value.numerator),
        .den = static_cast<int>(value.denominator)
    };
}

bool rational_is_positive(const Rational &value) {
    return value.is_valid() && value.numerator > 0 && value.denominator > 0;
}

double clamp_progress_fraction(const double value) {
    return std::clamp(value, 0.0, 1.0);
}

bool rationals_equal(const Rational &left, const Rational &right) {
    if (!left.is_valid() || !right.is_valid()) {
        return false;
    }

    return (left.numerator * right.denominator) == (right.numerator * left.denominator);
}

std::int64_t rescale_value(
    const std::int64_t value,
    const Rational &source_time_base,
    const Rational &target_time_base
) {
    return av_rescale_q(value, to_av_rational(source_time_base), to_av_rational(target_time_base));
}

std::int64_t rescale_to_microseconds(const std::int64_t value, const Rational &time_base) {
    return av_rescale_q(value, to_av_rational(time_base), AV_TIME_BASE_Q);
}

bool checked_add_u64(const std::uint64_t left, const std::uint64_t right, std::uint64_t &result) {
    if (left > (std::numeric_limits<std::uint64_t>::max() - right)) {
        return false;
    }

    result = left + right;
    return true;
}

bool checked_mul_u64(const std::uint64_t left, const std::uint64_t right, std::uint64_t &result) {
    if (left == 0U || right == 0U) {
        result = 0U;
        return true;
    }

    if (left > (std::numeric_limits<std::uint64_t>::max() / right)) {
        return false;
    }

    result = left * right;
    return true;
}

std::optional<std::uint64_t> compute_rgba_frame_bytes(const timeline::TimelinePlan &timeline_plan) {
    if (timeline_plan.segments.empty() || timeline_plan.main_segment_index >= timeline_plan.segments.size()) {
        return std::nullopt;
    }

    const auto &main_info = timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info;
    if (!main_info.primary_video_stream.has_value()) {
        return std::nullopt;
    }

    const auto &video_stream = *main_info.primary_video_stream;
    if (video_stream.width <= 0 || video_stream.height <= 0) {
        return std::nullopt;
    }

    std::uint64_t bytes = 0;
    if (!checked_mul_u64(static_cast<std::uint64_t>(video_stream.width), 4U, bytes)) {
        return std::nullopt;
    }

    if (!checked_mul_u64(bytes, static_cast<std::uint64_t>(video_stream.height), bytes)) {
        return std::nullopt;
    }

    return bytes;
}

std::optional<std::uint64_t> compute_yuv420_frame_bytes(const timeline::TimelinePlan &timeline_plan) {
    const auto rgba_frame_bytes = compute_rgba_frame_bytes(timeline_plan);
    if (!rgba_frame_bytes.has_value()) {
        return std::nullopt;
    }

    return (*rgba_frame_bytes * 3U) / 8U;
}

std::optional<std::uint64_t> compute_audio_block_bytes(
    const timeline::TimelinePlan &timeline_plan,
    const DecodeNormalizationPolicy &normalization_policy
) {
    if (!timeline_plan.output_audio_stream.has_value()) {
        return 0U;
    }

    if (normalization_policy.audio_sample_format != NormalizedAudioSampleFormat::f32_planar ||
        normalization_policy.audio_block_samples <= 0) {
        return std::nullopt;
    }

    const auto &audio_stream = *timeline_plan.output_audio_stream;
    if (audio_stream.channel_count <= 0) {
        return std::nullopt;
    }

    std::uint64_t bytes = 0;
    if (!checked_mul_u64(
            static_cast<std::uint64_t>(normalization_policy.audio_block_samples),
            static_cast<std::uint64_t>(audio_stream.channel_count),
            bytes)) {
        return std::nullopt;
    }

    if (!checked_mul_u64(bytes, sizeof(float), bytes)) {
        return std::nullopt;
    }

    return bytes;
}

}  // namespace

std::optional<PipelineMemoryBudget> build_memory_budget(
    const timeline::TimelinePlan &timeline_plan,
    const DecodeNormalizationPolicy &normalization_policy,
    const PipelineQueueLimits &queue_limits,
    const bool subtitles_present
) {
    const auto rgba_frame_bytes = compute_rgba_frame_bytes(timeline_plan);
    const auto yuv420_frame_bytes = compute_yuv420_frame_bytes(timeline_plan);
    const auto audio_block_bytes = compute_audio_block_bytes(timeline_plan, normalization_policy);
    if (!rgba_frame_bytes.has_value() || !yuv420_frame_bytes.has_value() || !audio_block_bytes.has_value()) {
        return std::nullopt;
    }

    std::uint64_t estimated_peak_bytes = 0;
    std::uint64_t rgba_surfaces_bytes = 0;
    if (!checked_mul_u64(*rgba_frame_bytes, kInFlightRgbaSurfaceCount, rgba_surfaces_bytes) ||
        !checked_add_u64(estimated_peak_bytes, rgba_surfaces_bytes, estimated_peak_bytes)) {
        return std::nullopt;
    }

    std::uint64_t subtitle_scratch_bytes = 0;
    if (subtitles_present &&
        (!checked_mul_u64(*rgba_frame_bytes, 1U, subtitle_scratch_bytes) ||
         !checked_add_u64(estimated_peak_bytes, subtitle_scratch_bytes, estimated_peak_bytes))) {
        return std::nullopt;
    }

    std::uint64_t encoder_frame_bytes = 0;
    if (!checked_mul_u64(*yuv420_frame_bytes, kEncoderWorkingSurfaceCount, encoder_frame_bytes) ||
        !checked_add_u64(estimated_peak_bytes, encoder_frame_bytes, estimated_peak_bytes)) {
        return std::nullopt;
    }

    std::uint64_t queued_audio_bytes = 0;
    if (!checked_mul_u64(
            *audio_block_bytes,
            static_cast<std::uint64_t>(queue_limits.decoded_audio_block_queue_depth),
            queued_audio_bytes) ||
        !checked_add_u64(estimated_peak_bytes, queued_audio_bytes, estimated_peak_bytes)) {
        return std::nullopt;
    }

    std::uint64_t audio_encoder_carry_bytes = 0;
    if (!checked_mul_u64(*audio_block_bytes, 1U, audio_encoder_carry_bytes) ||
        !checked_add_u64(estimated_peak_bytes, audio_encoder_carry_bytes, estimated_peak_bytes)) {
        return std::nullopt;
    }

    return PipelineMemoryBudget{
        .queue_limits = queue_limits,
        .normalized_rgba_frame_bytes = *rgba_frame_bytes,
        .subtitle_scratch_bytes = subtitle_scratch_bytes,
        .encoder_yuv420_frame_bytes = *yuv420_frame_bytes,
        .normalized_audio_block_bytes = *audio_block_bytes,
        .audio_encoder_carry_bytes = audio_encoder_carry_bytes,
        .estimated_peak_bytes = estimated_peak_bytes
    };
}

bool StreamingTranscodeResult::succeeded() const noexcept {
    return summary.has_value() && !error.has_value();
}

namespace {

FrameHandle allocate_frame() {
    FrameHandle frame(av_frame_alloc());
    if (!frame) {
        throw std::runtime_error("Failed to allocate an FFmpeg frame.");
    }

    return frame;
}

PacketHandle allocate_packet() {
    PacketHandle packet(av_packet_alloc());
    if (!packet) {
        throw std::runtime_error("Failed to allocate an FFmpeg packet.");
    }

    return packet;
}

TimestampSeed choose_timestamp_seed(const AVFrame &frame, const std::int64_t fallback_source_pts) {
    if (frame.pts != AV_NOPTS_VALUE) {
        return TimestampSeed{
            .source_pts = frame.pts,
            .origin = TimestampOrigin::decoded_pts
        };
    }

    if (frame.best_effort_timestamp != AV_NOPTS_VALUE) {
        return TimestampSeed{
            .source_pts = frame.best_effort_timestamp,
            .origin = TimestampOrigin::best_effort_pts
        };
    }

    return TimestampSeed{
        .source_pts = fallback_source_pts,
        .origin = TimestampOrigin::stream_cursor
    };
}

std::optional<std::int64_t> choose_packet_timestamp_seed(const AVPacket &packet) {
    if (packet.pts != AV_NOPTS_VALUE) {
        return packet.pts;
    }

    if (packet.dts != AV_NOPTS_VALUE) {
        return packet.dts;
    }

    return std::nullopt;
}

Rational choose_sample_aspect_ratio(const AVFrame &frame, const AVStream &stream) {
    const Rational frame_sample_aspect_ratio = ffmpeg_support::to_rational(frame.sample_aspect_ratio);
    if (rational_is_positive(frame_sample_aspect_ratio)) {
        return frame_sample_aspect_ratio;
    }

    const Rational stream_sample_aspect_ratio = ffmpeg_support::to_rational(stream.sample_aspect_ratio);
    if (rational_is_positive(stream_sample_aspect_ratio)) {
        return stream_sample_aspect_ratio;
    }

    return Rational{1, 1};
}

std::optional<std::int64_t> infer_video_frame_duration_pts(const VideoStreamInfo &video_stream_info) {
    if (!video_stream_info.timestamps.time_base.is_valid() ||
        !video_stream_info.average_frame_rate.is_valid() ||
        video_stream_info.average_frame_rate.numerator <= 0 ||
        video_stream_info.average_frame_rate.denominator <= 0) {
        return std::nullopt;
    }

    const AVRational frame_rate = to_av_rational(video_stream_info.average_frame_rate);
    const auto frame_duration_pts = av_rescale_q(
        1,
        av_inv_q(frame_rate),
        to_av_rational(video_stream_info.timestamps.time_base)
    );
    if (frame_duration_pts <= 0) {
        return std::nullopt;
    }

    return frame_duration_pts;
}

std::int64_t compute_frame_duration_pts(const Rational &frame_rate, const Rational &time_base) {
    if (!rational_is_positive(frame_rate) || !rational_is_positive(time_base)) {
        throw std::runtime_error("The output timeline does not expose a usable frame rate and time base.");
    }

    const auto frame_duration_pts = av_rescale_q(1, av_inv_q(to_av_rational(frame_rate)), to_av_rational(time_base));
    if (frame_duration_pts <= 0) {
        throw std::runtime_error("The output frame cadence could not be expressed in the output time base.");
    }

    return frame_duration_pts;
}

std::optional<std::int64_t> estimate_segment_duration_pts(
    const timeline::TimelineSegmentPlan &segment_plan,
    const Rational &output_video_time_base,
    const Rational &output_frame_rate
) {
    if (!segment_plan.inspected_source_info.primary_video_stream.has_value()) {
        return std::nullopt;
    }

    const auto &video_stream = *segment_plan.inspected_source_info.primary_video_stream;
    if (video_stream.timestamps.duration_pts.has_value() &&
        *video_stream.timestamps.duration_pts > 0 &&
        rational_is_positive(video_stream.timestamps.time_base)) {
        const auto duration_pts = rescale_value(
            *video_stream.timestamps.duration_pts,
            video_stream.timestamps.time_base,
            output_video_time_base
        );
        if (duration_pts > 0) {
            return duration_pts;
        }
    }

    if (video_stream.frame_count.has_value() &&
        *video_stream.frame_count > 0 &&
        rational_is_positive(output_frame_rate)) {
        const auto duration_pts = av_rescale_q(
            *video_stream.frame_count,
            av_inv_q(to_av_rational(output_frame_rate)),
            to_av_rational(output_video_time_base)
        );
        if (duration_pts > 0) {
            return duration_pts;
        }
    }

    if (segment_plan.inspected_source_info.container_duration_microseconds.has_value() &&
        *segment_plan.inspected_source_info.container_duration_microseconds > 0) {
        const auto duration_pts = av_rescale_q(
            *segment_plan.inspected_source_info.container_duration_microseconds,
            AV_TIME_BASE_Q,
            to_av_rational(output_video_time_base)
        );
        if (duration_pts > 0) {
            return duration_pts;
        }
    }

    return std::nullopt;
}

EstimatedVideoProgressTotals estimate_video_progress_totals(const timeline::TimelinePlan &timeline_plan) {
    EstimatedVideoProgressTotals totals{};
    bool all_segment_frame_counts_known = true;
    bool all_segment_durations_known = true;

    for (const auto &segment_plan : timeline_plan.segments) {
        if (!segment_plan.inspected_source_info.primary_video_stream.has_value()) {
            all_segment_frame_counts_known = false;
            all_segment_durations_known = false;
            continue;
        }

        const auto &video_stream = *segment_plan.inspected_source_info.primary_video_stream;
        if (video_stream.frame_count.has_value() && *video_stream.frame_count > 0) {
            totals.total_video_frames += static_cast<std::uint64_t>(*video_stream.frame_count);
        } else {
            all_segment_frame_counts_known = false;
        }

        const auto estimated_segment_duration_pts = estimate_segment_duration_pts(
            segment_plan,
            timeline_plan.output_video_time_base,
            timeline_plan.output_frame_rate
        );
        if (estimated_segment_duration_pts.has_value()) {
            totals.total_video_duration_pts += *estimated_segment_duration_pts;
        } else {
            all_segment_durations_known = false;
        }
    }

    if (!all_segment_durations_known) {
        totals.total_video_duration_pts = 0;
    }

    if (!all_segment_frame_counts_known) {
        totals.total_video_frames = 0;
    }

    if (totals.total_video_duration_pts <= 0 &&
        totals.total_video_frames > 0 &&
        rational_is_positive(timeline_plan.output_frame_rate)) {
        totals.total_video_duration_pts = av_rescale_q(
            static_cast<std::int64_t>(totals.total_video_frames),
            av_inv_q(to_av_rational(timeline_plan.output_frame_rate)),
            to_av_rational(timeline_plan.output_video_time_base)
        );
    }

    if (totals.total_video_duration_pts > 0) {
        totals.total_video_duration_us = rescale_to_microseconds(
            totals.total_video_duration_pts,
            timeline_plan.output_video_time_base
        );
    }

    if (totals.total_video_frames == 0 &&
        totals.total_video_duration_pts > 0 &&
        rational_is_positive(timeline_plan.output_frame_rate)) {
        // When container metadata does not expose an exact frame count, use the planned output duration
        // and authoritative output frame rate to derive a stable estimate for progress reporting.
        const auto estimated_total_frames = av_rescale_q_rnd(
            totals.total_video_duration_pts,
            to_av_rational(timeline_plan.output_video_time_base),
            av_inv_q(to_av_rational(timeline_plan.output_frame_rate)),
            AV_ROUND_NEAR_INF
        );
        if (estimated_total_frames > 0) {
            totals.total_video_frames = static_cast<std::uint64_t>(estimated_total_frames);
        }
    }

    return totals;
}

class StreamingEncodeProgressEmitter final {
public:
    StreamingEncodeProgressEmitter(
        EstimatedVideoProgressTotals totals,
        std::function<void(const StreamingEncodeProgress &)> callback
    )
        : totals_(std::move(totals)),
          callback_(std::move(callback)),
          start_time_(Clock::now()),
          last_emit_time_(start_time_) {}

    void record_frame_written(
        const std::uint64_t encoded_video_frames,
        const std::int64_t encoded_video_duration_us
    ) {
        latest_encoded_video_frames_ = encoded_video_frames;
        latest_encoded_video_duration_us_ = encoded_video_duration_us;

        if (!callback_ || encoded_video_frames == 0U) {
            return;
        }

        if (encoded_video_frames == 1U ||
            encoded_video_frames >= (last_emitted_frame_count_ + kFrameEmitInterval)) {
            emit_progress(false);
        }
    }

    void finish(
        const std::uint64_t encoded_video_frames,
        const std::int64_t encoded_video_duration_us
    ) {
        latest_encoded_video_frames_ = encoded_video_frames;
        latest_encoded_video_duration_us_ = encoded_video_duration_us;

        if (!callback_) {
            return;
        }

        emit_progress(true);
    }

private:
    using Clock = std::chrono::steady_clock;

    static constexpr std::uint64_t kFrameEmitInterval = 12U;
    static constexpr double kFpsSmoothingAlpha = 0.25;

    [[nodiscard]] double stage_fraction() const {
        if (totals_.total_video_duration_us > 0 && latest_encoded_video_duration_us_ > 0) {
            return clamp_progress_fraction(
                static_cast<double>(latest_encoded_video_duration_us_) /
                static_cast<double>(totals_.total_video_duration_us)
            );
        }

        if (totals_.total_video_frames > 0 && latest_encoded_video_frames_ > 0U) {
            return clamp_progress_fraction(
                static_cast<double>(latest_encoded_video_frames_) /
                static_cast<double>(totals_.total_video_frames)
            );
        }

        return 0.0;
    }

    [[nodiscard]] std::optional<double> encoded_fps(const Clock::time_point now) {
        if (latest_encoded_video_frames_ == 0U) {
            return std::nullopt;
        }

        const auto total_elapsed_seconds = std::chrono::duration<double>(now - start_time_).count();
        if (total_elapsed_seconds <= 0.0) {
            return std::nullopt;
        }

        const auto emit_elapsed_seconds = std::chrono::duration<double>(now - last_emit_time_).count();
        const auto emitted_frame_delta =
            latest_encoded_video_frames_ >= last_emitted_frame_count_
                ? (latest_encoded_video_frames_ - last_emitted_frame_count_)
                : 0U;
        if (emitted_frame_delta > 0U && emit_elapsed_seconds > 0.0) {
            const double instant_fps =
                static_cast<double>(emitted_frame_delta) / emit_elapsed_seconds;
            smoothed_fps_ = smoothed_fps_.has_value()
                ? (((1.0 - kFpsSmoothingAlpha) * *smoothed_fps_) + (kFpsSmoothingAlpha * instant_fps))
                : std::optional<double>(instant_fps);
        }

        const double average_fps =
            static_cast<double>(latest_encoded_video_frames_) / total_elapsed_seconds;
        if (smoothed_fps_.has_value()) {
            return *smoothed_fps_;
        }

        return average_fps > 0.0
            ? std::optional<double>(average_fps)
            : std::nullopt;
    }

    void emit_progress(const bool force_final) {
        const auto now = Clock::now();
        callback_(StreamingEncodeProgress{
            .stage_fraction = force_final ? 1.0 : stage_fraction(),
            .encoded_video_frames = latest_encoded_video_frames_,
            .total_video_frames = totals_.total_video_frames,
            .encoded_video_duration_us = latest_encoded_video_duration_us_,
            .total_video_duration_us = totals_.total_video_duration_us,
            .encoded_fps = encoded_fps(now)
        });

        last_emit_time_ = now;
        last_emitted_frame_count_ = latest_encoded_video_frames_;
    }

    EstimatedVideoProgressTotals totals_{};
    std::function<void(const StreamingEncodeProgress &)> callback_{};
    Clock::time_point start_time_{};
    Clock::time_point last_emit_time_{};
    std::uint64_t last_emitted_frame_count_{0};
    std::uint64_t latest_encoded_video_frames_{0};
    std::int64_t latest_encoded_video_duration_us_{0};
    std::optional<double> smoothed_fps_{};
};

FormatContextHandle open_format_context(const std::string &input_path_string) {
    AVFormatContext *raw_format_context = nullptr;
    const auto open_result = avformat_open_input(&raw_format_context, input_path_string.c_str(), nullptr, nullptr);
    if (open_result < 0) {
        throw std::runtime_error(
            "Failed to open media input '" + input_path_string + "'. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(open_result)
        );
    }

    FormatContextHandle format_context(raw_format_context);
    const auto stream_info_result = avformat_find_stream_info(format_context.get(), nullptr);
    if (stream_info_result < 0) {
        throw std::runtime_error(
            "Failed to read stream information from '" + input_path_string + "'. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(stream_info_result)
        );
    }

    return format_context;
}

CodecContextHandle open_decoder_context(AVStream &stream) {
    if (stream.codecpar == nullptr) {
        throw std::runtime_error("The selected stream does not expose codec parameters.");
    }

    const AVCodec *decoder = avcodec_find_decoder(stream.codecpar->codec_id);
    if (decoder == nullptr) {
        throw std::runtime_error(
            "No decoder is available for stream index " + std::to_string(stream.index) + "."
        );
    }

    CodecContextHandle codec_context(avcodec_alloc_context3(decoder));
    if (!codec_context) {
        throw std::runtime_error("Failed to allocate an FFmpeg decoder context.");
    }

    const auto parameters_result = avcodec_parameters_to_context(codec_context.get(), stream.codecpar);
    if (parameters_result < 0) {
        throw std::runtime_error(
            "Failed to copy decoder parameters for stream index " + std::to_string(stream.index) +
            ". FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(parameters_result)
        );
    }

    codec_context->pkt_timebase = stream.time_base;

    const auto open_result = avcodec_open2(codec_context.get(), decoder, nullptr);
    if (open_result < 0) {
        throw std::runtime_error(
            "Failed to open the decoder for stream index " + std::to_string(stream.index) +
            ". FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(open_result)
        );
    }

    return codec_context;
}

CodecParametersHandle clone_codec_parameters(const AVCodecParameters &source_parameters) {
    CodecParametersHandle cloned_parameters(avcodec_parameters_alloc());
    if (!cloned_parameters) {
        throw std::runtime_error("Failed to allocate cloned FFmpeg codec parameters.");
    }

    const auto copy_result = avcodec_parameters_copy(cloned_parameters.get(), &source_parameters);
    if (copy_result < 0) {
        throw std::runtime_error(
            "Failed to clone FFmpeg codec parameters for streaming stream copy. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(copy_result)
        );
    }

    return cloned_parameters;
}

SegmentDecoderResources open_segment_resources(
    const timeline::TimelineSegmentPlan &segment_plan,
    const bool decode_audio
) {
    const auto input_path_string = segment_plan.source_path.lexically_normal().string();
    auto format_context = open_format_context(input_path_string);

    const auto &video_stream_info = *segment_plan.inspected_source_info.primary_video_stream;
    if (video_stream_info.stream_index < 0 ||
        video_stream_info.stream_index >= static_cast<int>(format_context->nb_streams)) {
        throw std::runtime_error(
            "The selected video stream index is not present in '" + input_path_string + "'."
        );
    }

    auto *video_stream = format_context->streams[video_stream_info.stream_index];
    CodecContextHandle video_decoder = open_decoder_context(*video_stream);

    CodecContextHandle audio_decoder{};
    AVStream *audio_stream = nullptr;
    if (segment_plan.inspected_source_info.primary_audio_stream.has_value()) {
        const auto &audio_stream_info = *segment_plan.inspected_source_info.primary_audio_stream;
        if (audio_stream_info.stream_index < 0 ||
            audio_stream_info.stream_index >= static_cast<int>(format_context->nb_streams)) {
            throw std::runtime_error(
                "The selected audio stream index is not present in '" + input_path_string + "'."
            );
        }

        audio_stream = format_context->streams[audio_stream_info.stream_index];
        if (decode_audio) {
            audio_decoder = open_decoder_context(*audio_stream);
        }
    }

    return SegmentDecoderResources{
        .format_context = std::move(format_context),
        .video_decoder = std::move(video_decoder),
        .video_stream = video_stream,
        .audio_decoder = std::move(audio_decoder),
        .audio_stream = audio_stream
    };
}

std::optional<AudioCopyTemplate> build_audio_copy_template(const timeline::TimelineSegmentPlan &segment_plan) {
    if (!segment_plan.inspected_source_info.primary_audio_stream.has_value()) {
        return std::nullopt;
    }

    const auto input_path_string = segment_plan.source_path.lexically_normal().string();
    auto format_context = open_format_context(input_path_string);
    const auto &audio_stream_info = *segment_plan.inspected_source_info.primary_audio_stream;
    if (audio_stream_info.stream_index < 0 ||
        audio_stream_info.stream_index >= static_cast<int>(format_context->nb_streams)) {
        throw std::runtime_error(
            "The selected audio stream index is not present in '" + input_path_string + "'."
        );
    }

    auto *audio_stream = format_context->streams[audio_stream_info.stream_index];
    if (audio_stream == nullptr || audio_stream->codecpar == nullptr) {
        throw std::runtime_error("The streaming audio copy path requires source codec parameters.");
    }

    const std::int64_t start_pts =
        audio_stream->start_time != AV_NOPTS_VALUE
            ? audio_stream->start_time
            : audio_stream_info.timestamps.start_pts.value_or(0);

    return AudioCopyTemplate{
        .codec_parameters = clone_codec_parameters(*audio_stream->codecpar),
        .time_base = ffmpeg_support::to_rational(audio_stream->time_base),
        .start_pts = start_pts
    };
}

void send_packet_or_throw(AVCodecContext &codec_context, AVPacket *packet) {
    const auto send_result = avcodec_send_packet(&codec_context, packet);
    if (send_result < 0) {
        throw std::runtime_error(
            "Failed to send a packet to an FFmpeg codec context. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(send_result)
        );
    }
}

DecodedVideoFrame normalize_video_frame(
    const AVFrame &source_frame,
    const AVStream &stream,
    const int stream_index,
    const std::int64_t frame_index,
    const DecodeNormalizationPolicy &normalization_policy,
    SwsContextHandle &scale_context,
    int &scale_width,
    int &scale_height,
    AVPixelFormat &scale_source_pixel_format,
    std::int64_t &next_fallback_source_pts,
    const std::int64_t fallback_duration_pts
) {
    const auto timestamp_seed = choose_timestamp_seed(source_frame, next_fallback_source_pts);
    next_fallback_source_pts = timestamp_seed.source_pts + fallback_duration_pts;
    const auto stream_time_base = ffmpeg_support::to_rational(stream.time_base);
    const auto source_duration = source_frame.duration > 0
        ? std::optional<std::int64_t>(source_frame.duration)
        : std::nullopt;

    const auto source_pixel_format = static_cast<AVPixelFormat>(source_frame.format);
    if (source_pixel_format == AV_PIX_FMT_NONE) {
        throw std::runtime_error("The video decoder returned a frame without a usable pixel format.");
    }

    if (!scale_context ||
        scale_width != source_frame.width ||
        scale_height != source_frame.height ||
        scale_source_pixel_format != source_pixel_format) {
        SwsContext *raw_scale_context = sws_getContext(
            source_frame.width,
            source_frame.height,
            source_pixel_format,
            source_frame.width,
            source_frame.height,
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr
        );

        if (raw_scale_context == nullptr) {
            throw std::runtime_error("Failed to create an FFmpeg scaling context for streaming decode normalization.");
        }

        scale_context.reset(raw_scale_context);
        scale_width = source_frame.width;
        scale_height = source_frame.height;
        scale_source_pixel_format = source_pixel_format;
    }

    auto normalized_frame = allocate_frame();
    normalized_frame->format = AV_PIX_FMT_RGBA;
    normalized_frame->width = source_frame.width;
    normalized_frame->height = source_frame.height;

    const auto buffer_result = av_frame_get_buffer(normalized_frame.get(), 1);
    if (buffer_result < 0) {
        throw std::runtime_error(
            "Failed to allocate the normalized streaming video frame buffer. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(buffer_result)
        );
    }

    const auto scale_result = sws_scale(
        scale_context.get(),
        source_frame.data,
        source_frame.linesize,
        0,
        source_frame.height,
        normalized_frame->data,
        normalized_frame->linesize
    );
    if (scale_result <= 0) {
        throw std::runtime_error("FFmpeg did not produce normalized video output for a decoded streaming frame.");
    }

    VideoPlane plane{
        .line_stride_bytes = normalized_frame->linesize[0],
        .visible_width = source_frame.width,
        .visible_height = source_frame.height,
        .bytes = std::vector<std::uint8_t>(
            static_cast<std::size_t>(normalized_frame->linesize[0]) * static_cast<std::size_t>(source_frame.height)
        )
    };

    for (int row = 0; row < source_frame.height; ++row) {
        std::memcpy(
            plane.bytes.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(normalized_frame->linesize[0]),
            normalized_frame->data[0] + static_cast<std::size_t>(row) * static_cast<std::size_t>(normalized_frame->linesize[0]),
            static_cast<std::size_t>(normalized_frame->linesize[0])
        );
    }

    return DecodedVideoFrame{
        .stream_index = stream_index,
        .frame_index = frame_index,
        .timestamp = MediaTimestamp{
            .source_time_base = stream_time_base,
            .source_pts = timestamp_seed.source_pts,
            .source_duration = source_duration,
            .origin = timestamp_seed.origin,
            .start_microseconds = rescale_to_microseconds(
                timestamp_seed.source_pts,
                stream_time_base
            ),
            .duration_microseconds = source_duration.has_value()
                ? std::optional<std::int64_t>(rescale_to_microseconds(*source_duration, stream_time_base))
                : std::nullopt
        },
        .width = source_frame.width,
        .height = source_frame.height,
        .sample_aspect_ratio = choose_sample_aspect_ratio(source_frame, stream),
        .pixel_format = normalization_policy.video_pixel_format,
        .planes = {std::move(plane)}
    };
}

std::vector<std::vector<float>> resample_audio_frame(
    SwrContext &resample_context,
    const AVFrame *decoded_frame,
    const int channel_count
) {
    const int input_samples = decoded_frame != nullptr ? decoded_frame->nb_samples : 0;
    const int output_capacity = swr_get_out_samples(&resample_context, input_samples);
    if (output_capacity < 0) {
        throw std::runtime_error("FFmpeg failed to estimate normalized streaming audio output capacity.");
    }

    std::vector<std::vector<float>> converted_channels(
        static_cast<std::size_t>(channel_count),
        std::vector<float>(static_cast<std::size_t>(output_capacity))
    );
    std::vector<std::uint8_t *> output_data(static_cast<std::size_t>(channel_count), nullptr);
    for (int channel_index = 0; channel_index < channel_count; ++channel_index) {
        output_data[static_cast<std::size_t>(channel_index)] =
            reinterpret_cast<std::uint8_t *>(converted_channels[static_cast<std::size_t>(channel_index)].data());
    }

    std::vector<const std::uint8_t *> input_data{};
    const std::uint8_t *const *input_data_pointer = nullptr;
    if (decoded_frame != nullptr && input_samples > 0) {
        const auto sample_format = static_cast<AVSampleFormat>(decoded_frame->format);
        const int input_planes = av_sample_fmt_is_planar(sample_format) ? channel_count : 1;
        input_data.resize(static_cast<std::size_t>(std::max(1, input_planes)), nullptr);
        for (int plane_index = 0; plane_index < input_planes; ++plane_index) {
            input_data[static_cast<std::size_t>(plane_index)] = decoded_frame->extended_data[plane_index];
        }
        input_data_pointer = input_data.data();
    }

    const auto converted_samples = swr_convert(
        &resample_context,
        output_data.data(),
        output_capacity,
        input_data_pointer,
        input_samples
    );
    if (converted_samples < 0) {
        throw std::runtime_error(
            "Failed to normalize decoded streaming audio samples. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(converted_samples)
        );
    }

    for (auto &channel_samples : converted_channels) {
        channel_samples.resize(static_cast<std::size_t>(converted_samples));
    }

    return converted_channels;
}

void append_channel_samples(
    std::vector<std::vector<float>> &pending_channels,
    const std::vector<std::vector<float>> &converted_channels
) {
    if (pending_channels.empty()) {
        pending_channels.resize(converted_channels.size());
    }

    for (std::size_t channel_index = 0; channel_index < converted_channels.size(); ++channel_index) {
        auto &pending = pending_channels[channel_index];
        const auto &converted = converted_channels[channel_index];
        pending.insert(pending.end(), converted.begin(), converted.end());
    }
}

DecodedAudioSamples build_audio_block(
    const AudioOutputPlan &audio_output_plan,
    const std::int64_t block_index,
    const std::int64_t samples_written_before_block,
    const int block_sample_count,
    const std::vector<std::vector<float>> &block_channels
) {
    return DecodedAudioSamples{
        .stream_index = -1,
        .block_index = block_index,
        .timestamp = MediaTimestamp{
            .source_time_base = audio_output_plan.time_base,
            .source_pts = samples_written_before_block,
            .source_duration = block_sample_count,
            .origin = TimestampOrigin::stream_cursor,
            .start_microseconds = rescale_to_microseconds(samples_written_before_block, audio_output_plan.time_base),
            .duration_microseconds = rescale_to_microseconds(block_sample_count, audio_output_plan.time_base)
        },
        .sample_rate = audio_output_plan.sample_rate,
        .channel_count = audio_output_plan.channel_count,
        .channel_layout_name = audio_output_plan.channel_layout_name,
        .sample_format = NormalizedAudioSampleFormat::f32_planar,
        .samples_per_channel = block_sample_count,
        .channel_samples = block_channels
    };
}

bool maybe_composite_subtitles(
    DecodedVideoFrame &video_frame,
    subtitles::SubtitleRenderSession *subtitle_session,
    const std::optional<job::EncodeJobSubtitleSettings> &subtitle_settings,
    const bool subtitles_enabled,
    const std::int64_t subtitle_timestamp_microseconds
) {
    if (!subtitle_settings.has_value() || !subtitles_enabled) {
        return false;
    }

    if (subtitle_session == nullptr) {
        throw std::runtime_error("Streaming subtitle burn-in requires an active subtitle render session.");
    }

    if (!subtitles::detail::is_rgba_frame_layout_supported(video_frame)) {
        throw std::runtime_error("Streaming subtitle burn-in requires rgba8 decoded frames with a single plane.");
    }

    const auto render_result = subtitle_session->render(subtitles::SubtitleRenderRequest{
        .timestamp_microseconds = subtitle_timestamp_microseconds
    });
    if (!render_result.succeeded()) {
        throw std::runtime_error(
            "Subtitle rendering failed during streaming burn-in. " + render_result.error->message +
            " Hint: " + render_result.error->actionable_hint
        );
    }

    if (render_result.rendered_frame->bitmaps.empty()) {
        return false;
    }

    for (const auto &bitmap : render_result.rendered_frame->bitmaps) {
        subtitles::detail::composite_bitmap_into_frame(video_frame, bitmap);
    }

    return true;
}

EncoderSelection select_encoder(const OutputVideoCodec codec) {
    switch (codec) {
    case OutputVideoCodec::h264:
        return EncoderSelection{.encoder_name = "libx264"};
    case OutputVideoCodec::h265:
        return EncoderSelection{.encoder_name = "libx265"};
    default:
        throw std::runtime_error("Unsupported output video codec selection.");
    }
}

OutputFormatContextHandle create_output_context(const std::string &output_path_string) {
    AVFormatContext *raw_format_context = nullptr;
    const auto format_result = avformat_alloc_output_context2(
        &raw_format_context,
        nullptr,
        nullptr,
        output_path_string.c_str()
    );
    if (format_result < 0 || raw_format_context == nullptr) {
        throw std::runtime_error(
            "Failed to allocate an output format context for '" + output_path_string + "'. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(format_result)
        );
    }

    return OutputFormatContextHandle(raw_format_context);
}

void open_output_file(AVFormatContext &format_context, const std::filesystem::path &output_path) {
    if ((format_context.oformat->flags & AVFMT_NOFILE) != 0) {
        return;
    }

    const auto parent_path = output_path.parent_path();
    if (!parent_path.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent_path, filesystem_error);
        if (filesystem_error) {
            throw std::runtime_error(
                "Failed to create the output directory '" + parent_path.string() + "': " +
                filesystem_error.message()
            );
        }
    }

    const auto output_path_string = output_path.string();
    const auto open_result = avio_open(&format_context.pb, output_path_string.c_str(), AVIO_FLAG_WRITE);
    if (open_result < 0) {
        throw std::runtime_error(
            "Failed to open the output file '" + output_path_string + "'. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(open_result)
        );
    }
}

CodecContextHandle create_video_encoder_context(
    AVFormatContext &output_context,
    AVStream &video_stream,
    const MediaEncodeRequest &request,
    const VideoOutputPlan &plan
) {
    const auto selection = select_encoder(request.video_settings.codec);
    const AVCodec *encoder = avcodec_find_encoder_by_name(selection.encoder_name);
    if (encoder == nullptr) {
        throw std::runtime_error(
            "The requested software encoder backend '" + std::string(selection.encoder_name) + "' is not available."
        );
    }

    CodecContextHandle codec_context(avcodec_alloc_context3(encoder));
    if (!codec_context) {
        throw std::runtime_error("Failed to allocate an FFmpeg video encoder context.");
    }

    codec_context->codec_id = encoder->id;
    codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_context->width = plan.width;
    codec_context->height = plan.height;
    codec_context->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_context->time_base = to_av_rational(plan.time_base);
    codec_context->framerate = to_av_rational(plan.average_frame_rate);
    codec_context->sample_aspect_ratio = to_av_rational(plan.sample_aspect_ratio);

    if ((output_context.oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (request.video_settings.preset.empty()) {
        throw std::runtime_error("The encoder preset must not be empty.");
    }

    if (request.video_settings.crf < 0 || request.video_settings.crf > 51) {
        throw std::runtime_error("The encoder CRF must be between 0 and 51.");
    }

    const auto preset_result = av_opt_set(codec_context->priv_data, "preset", request.video_settings.preset.c_str(), 0);
    if (preset_result < 0) {
        throw std::runtime_error(
            "Failed to apply the encoder preset '" + request.video_settings.preset + "'. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(preset_result)
        );
    }

    const auto crf_result = av_opt_set_int(codec_context->priv_data, "crf", request.video_settings.crf, 0);
    if (crf_result < 0) {
        throw std::runtime_error(
            "Failed to apply the encoder CRF value. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(crf_result)
        );
    }

    const auto open_result = avcodec_open2(codec_context.get(), encoder, nullptr);
    if (open_result < 0) {
        throw std::runtime_error(
            "Failed to open the requested video encoder backend. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(open_result)
        );
    }

    video_stream.time_base = codec_context->time_base;
    video_stream.avg_frame_rate = codec_context->framerate;
    video_stream.sample_aspect_ratio = codec_context->sample_aspect_ratio;

    const auto parameters_result = avcodec_parameters_from_context(video_stream.codecpar, codec_context.get());
    if (parameters_result < 0) {
        throw std::runtime_error(
            "Failed to copy encoded video stream parameters. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(parameters_result)
        );
    }

    return codec_context;
}

template <typename T>
struct CodecSupportedConfigView final {
    const T *values{nullptr};
    int count{0};
};

template <typename T>
CodecSupportedConfigView<T> get_supported_codec_config(
    const AVCodecContext &codec_context,
    const AVCodecConfig config,
    const char *config_name
) {
    const void *raw_values = nullptr;
    int value_count = 0;
    const auto query_result =
        avcodec_get_supported_config(&codec_context, nullptr, config, 0, &raw_values, &value_count);
    if (query_result < 0) {
        throw std::runtime_error(
            "Failed to query the streaming audio encoder supported " + std::string(config_name) +
            ". FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(query_result)
        );
    }

    return CodecSupportedConfigView<T>{
        .values = static_cast<const T *>(raw_values),
        .count = value_count
    };
}

AVSampleFormat choose_audio_encoder_sample_format(const AVCodecContext &codec_context) {
    const auto supported_sample_formats = get_supported_codec_config<AVSampleFormat>(
        codec_context,
        AV_CODEC_CONFIG_SAMPLE_FORMAT,
        "sample formats"
    );
    if (supported_sample_formats.values == nullptr || supported_sample_formats.count <= 0) {
        return AV_SAMPLE_FMT_FLTP;
    }

    for (int index = 0; index < supported_sample_formats.count; ++index) {
        if (supported_sample_formats.values[index] == AV_SAMPLE_FMT_FLTP) {
            return supported_sample_formats.values[index];
        }
    }

    throw std::runtime_error(
        "The default audio encoder does not support planar float audio input for the streaming pipeline."
    );
}

bool audio_encoder_supports_sample_rate(const AVCodecContext &codec_context, const int sample_rate) {
    const auto supported_sample_rates = get_supported_codec_config<int>(
        codec_context,
        AV_CODEC_CONFIG_SAMPLE_RATE,
        "sample rates"
    );
    if (supported_sample_rates.values == nullptr || supported_sample_rates.count <= 0) {
        return true;
    }

    for (int index = 0; index < supported_sample_rates.count; ++index) {
        if (supported_sample_rates.values[index] == sample_rate) {
            return true;
        }
    }

    return false;
}

AVChannelLayout make_default_audio_channel_layout(const int channel_count) {
    if (channel_count <= 0) {
        throw std::runtime_error("The streaming pipeline requires a positive output audio channel count.");
    }

    AVChannelLayout channel_layout{};
    av_channel_layout_default(&channel_layout, channel_count);
    if (channel_layout.nb_channels != channel_count) {
        throw std::runtime_error("The streaming pipeline could not derive a default output audio channel layout.");
    }

    return channel_layout;
}

bool audio_encoder_supports_channel_layout(
    const AVCodecContext &codec_context,
    const AVChannelLayout &requested_channel_layout
) {
    const auto supported_channel_layouts = get_supported_codec_config<AVChannelLayout>(
        codec_context,
        AV_CODEC_CONFIG_CHANNEL_LAYOUT,
        "channel layouts"
    );
    if (supported_channel_layouts.values == nullptr || supported_channel_layouts.count <= 0) {
        return true;
    }

    for (int index = 0; index < supported_channel_layouts.count; ++index) {
        const auto &supported_layout = supported_channel_layouts.values[index];
        if (av_channel_layout_compare(&supported_layout, &requested_channel_layout) == 0 ||
            supported_layout.nb_channels == requested_channel_layout.nb_channels) {
            return true;
        }
    }

    return false;
}

bool audio_encoder_supports_short_frame_samples(const AVCodecContext &codec_context) {
    if (codec_context.frame_size <= 0) {
        return true;
    }

    if (codec_context.codec == nullptr) {
        return false;
    }

    return (codec_context.codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0 ||
        (codec_context.codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
}

void configure_audio_copy_stream(
    AVStream &audio_stream,
    const AudioCopyTemplate &copy_template
) {
    if (!copy_template.codec_parameters) {
        throw std::runtime_error("The streaming audio copy path requires source codec parameters.");
    }

    const auto parameters_result = avcodec_parameters_copy(audio_stream.codecpar, copy_template.codec_parameters.get());
    if (parameters_result < 0) {
        throw std::runtime_error(
            "Failed to copy source audio stream parameters for stream copy. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(parameters_result)
        );
    }

    audio_stream.codecpar->codec_tag = 0;
    audio_stream.time_base = to_av_rational(copy_template.time_base);
}

CodecContextHandle create_audio_encoder_context(
    AVFormatContext &output_context,
    AVStream &audio_stream,
    const AudioOutputPlan &plan
) {
    const AVCodec *encoder = avcodec_find_encoder_by_name("aac");
    if (encoder == nullptr) {
        throw std::runtime_error(
            "The default AAC audio encoder is not available for the streaming pipeline."
        );
    }

    if (avformat_query_codec(output_context.oformat, encoder->id, FF_COMPLIANCE_NORMAL) == 0) {
        throw std::runtime_error(
            "The selected output container does not support AAC audio in the streaming pipeline."
        );
    }

    CodecContextHandle codec_context(avcodec_alloc_context3(encoder));
    if (!codec_context) {
        throw std::runtime_error("Failed to allocate an FFmpeg audio encoder context.");
    }

    if (!audio_encoder_supports_sample_rate(*codec_context, plan.sample_rate)) {
        throw std::runtime_error(
            "The default AAC audio encoder does not support the required sample rate " +
            std::to_string(plan.sample_rate) + "."
        );
    }

    const AVSampleFormat sample_format = choose_audio_encoder_sample_format(*codec_context);
    AVChannelLayout requested_channel_layout = make_default_audio_channel_layout(plan.channel_count);
    if (!audio_encoder_supports_channel_layout(*codec_context, requested_channel_layout)) {
        av_channel_layout_uninit(&requested_channel_layout);
        throw std::runtime_error(
            "The default AAC audio encoder does not support the required channel count " +
            std::to_string(plan.channel_count) + "."
        );
    }

    codec_context->codec_id = encoder->id;
    codec_context->codec_type = AVMEDIA_TYPE_AUDIO;
    codec_context->sample_fmt = sample_format;
    codec_context->sample_rate = plan.sample_rate;
    codec_context->time_base = to_av_rational(plan.time_base);
    codec_context->bit_rate = static_cast<std::int64_t>(plan.bitrate_kbps) * 1000LL;
    const auto channel_layout_copy_result =
        av_channel_layout_copy(&codec_context->ch_layout, &requested_channel_layout);
    av_channel_layout_uninit(&requested_channel_layout);
    if (channel_layout_copy_result < 0) {
        throw std::runtime_error(
            "Failed to configure the streaming audio encoder channel layout. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(channel_layout_copy_result)
        );
    }

    if ((output_context.oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    const auto open_result = avcodec_open2(codec_context.get(), encoder, nullptr);
    if (open_result < 0) {
        throw std::runtime_error(
            "Failed to open the default AAC audio encoder. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(open_result)
        );
    }

    audio_stream.time_base = codec_context->time_base;
    const auto parameters_result = avcodec_parameters_from_context(audio_stream.codecpar, codec_context.get());
    if (parameters_result < 0) {
        throw std::runtime_error(
            "Failed to copy encoded audio stream parameters. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(parameters_result)
        );
    }

    return codec_context;
}

class StreamingOutputSession final {
public:
    StreamingOutputSession(
        const MediaEncodeRequest &request,
        const VideoOutputPlan &video_plan,
        const ResolvedAudioOutputPlan &resolved_audio_output,
        const std::optional<AudioOutputPlan> &audio_plan,
        std::optional<AudioCopyTemplate> audio_copy_template
    )
        : request_(request),
          video_plan_(video_plan),
          resolved_audio_output_(resolved_audio_output),
          audio_plan_(audio_plan),
          audio_copy_template_(std::move(audio_copy_template)) {
        const auto output_path = request.output_path.lexically_normal();
        const auto output_path_string = output_path.string();
        if (output_path_string.empty()) {
            throw std::runtime_error("The output path must not be empty.");
        }

        if ((video_plan.width % 2) != 0 || (video_plan.height % 2) != 0) {
            throw std::runtime_error("The software encoder backends require even frame dimensions for yuv420p output.");
        }

        output_context_ = create_output_context(output_path_string);
        AVStream *raw_video_stream = avformat_new_stream(output_context_.get(), nullptr);
        if (raw_video_stream == nullptr) {
            throw std::runtime_error("Failed to create the output video stream.");
        }

        video_stream_ = raw_video_stream;
        video_codec_context_ = create_video_encoder_context(*output_context_, *video_stream_, request, video_plan);

        if (audio_plan_.has_value()) {
            AVStream *raw_audio_stream = avformat_new_stream(output_context_.get(), nullptr);
            if (raw_audio_stream == nullptr) {
                throw std::runtime_error("Failed to create the output audio stream.");
            }

            audio_stream_ = raw_audio_stream;
            if (audio_plan_->encodes_audio()) {
                audio_codec_context_ = create_audio_encoder_context(*output_context_, *audio_stream_, *audio_plan_);
            } else if (audio_plan_->copies_audio()) {
                if (!audio_copy_template_.has_value()) {
                    throw std::runtime_error("The streaming audio copy path is missing its source stream template.");
                }

                configure_audio_copy_stream(*audio_stream_, *audio_copy_template_);
            }
        }

        open_output_file(*output_context_, output_path);

        const auto header_result = avformat_write_header(output_context_.get(), nullptr);
        if (header_result < 0) {
            throw std::runtime_error(
                "Failed to write the output container header. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(header_result)
            );
        }

        reusable_video_receive_packet_ = allocate_packet();
        if (audio_codec_context_) {
            reusable_audio_receive_packet_ = allocate_packet();
        }
    }

    [[nodiscard]] bool audio_enabled() const noexcept {
        return audio_stream_ != nullptr;
    }

    void push_frame(const DecodedVideoFrame &decoded_frame) {
        auto encoded_frame = build_encoded_video_frame(decoded_frame);
        const auto send_result = avcodec_send_frame(video_codec_context_.get(), encoded_frame.get());
        if (send_result < 0) {
            throw std::runtime_error(
                "Failed to send a frame to the video encoder. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(send_result)
            );
        }

        ++encoded_video_frame_count_;
        drain_video_encoder();
    }

    void push_audio_block(const DecodedAudioSamples &audio_block) {
        if (!audio_codec_context_ || !audio_plan_.has_value() || !audio_plan_->encodes_audio() || audio_stream_ == nullptr) {
            throw std::runtime_error("The streaming output session was asked to encode audio without an audio stream.");
        }

        validate_audio_block_for_output(audio_block);
        append_pending_audio_block(audio_block);
        drain_ready_audio_blocks_into_encoder(false);
    }

    void copy_audio_packet(const AVPacket &source_packet) {
        if (!audio_plan_.has_value() || !audio_plan_->copies_audio() || audio_stream_ == nullptr ||
            !audio_copy_template_.has_value()) {
            throw std::runtime_error("The streaming output session was asked to copy audio without a copy stream.");
        }

        auto packet = allocate_packet();
        const auto ref_result = av_packet_ref(packet.get(), &source_packet);
        if (ref_result < 0) {
            throw std::runtime_error(
                "Failed to clone a source audio packet for stream copy. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(ref_result)
            );
        }

        if (packet->pts != AV_NOPTS_VALUE) {
            packet->pts -= audio_copy_template_->start_pts;
        }
        if (packet->dts != AV_NOPTS_VALUE) {
            packet->dts -= audio_copy_template_->start_pts;
        }

        av_packet_rescale_ts(
            packet.get(),
            to_av_rational(audio_copy_template_->time_base),
            audio_stream_->time_base
        );
        packet->stream_index = audio_stream_->index;
        packet->pos = -1;

        const auto write_result = av_interleaved_write_frame(output_context_.get(), packet.get());
        av_packet_unref(packet.get());
        if (write_result < 0) {
            throw std::runtime_error(
                "Failed to mux a copied audio packet. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(write_result)
            );
        }
    }

    EncodedMediaSummary finish() {
        if (!finalized_) {
            const auto flush_video_result = avcodec_send_frame(video_codec_context_.get(), nullptr);
            if (flush_video_result < 0) {
                throw std::runtime_error(
                    "Failed to flush the video encoder. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(flush_video_result)
                );
            }

            drain_video_encoder();
            if (audio_codec_context_) {
                drain_ready_audio_blocks_into_encoder(true);
                const auto flush_audio_result = avcodec_send_frame(audio_codec_context_.get(), nullptr);
                if (flush_audio_result < 0) {
                    throw std::runtime_error(
                        "Failed to flush the audio encoder. FFmpeg reported: " +
                        ffmpeg_support::ffmpeg_error_to_string(flush_audio_result)
                    );
                }

                drain_audio_encoder();
            }

            const auto trailer_result = av_write_trailer(output_context_.get());
            if (trailer_result < 0) {
                throw std::runtime_error(
                    "Failed to finalize the output container. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(trailer_result)
                );
            }

            if (output_context_->pb != nullptr && (output_context_->oformat->flags & AVFMT_NOFILE) == 0) {
                avio_closep(&output_context_->pb);
            }

            finalized_ = true;
        }

        const auto inspection_result = MediaInspector::inspect(request_.output_path);
        if (!inspection_result.succeeded()) {
            throw std::runtime_error(
                "The encoded output could not be inspected after write. " + inspection_result.error->message +
                " Hint: " + inspection_result.error->actionable_hint
            );
        }

        return EncodedMediaSummary{
            .output_path = request_.output_path.lexically_normal(),
            .video_settings = request_.video_settings,
            .resolved_audio_output = resolved_audio_output_,
            .output_info = *inspection_result.media_source_info,
            .encoded_video_frame_count = encoded_video_frame_count_
        };
    }

private:
    FrameHandle build_encoded_video_frame(const DecodedVideoFrame &decoded_frame) {
        if (!subtitles::detail::is_rgba_frame_layout_supported(decoded_frame)) {
            throw std::runtime_error("The streaming encoder currently only supports rgba8 decoded frames.");
        }

        if (!scale_context_) {
            SwsContext *raw_scale_context = sws_getContext(
                video_plan_.width,
                video_plan_.height,
                AV_PIX_FMT_RGBA,
                video_plan_.width,
                video_plan_.height,
                video_codec_context_->pix_fmt,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr
            );
            if (raw_scale_context == nullptr) {
                throw std::runtime_error("Failed to create the streaming encode scaling context.");
            }

            scale_context_.reset(raw_scale_context);
        }

        auto encoded_frame = allocate_frame();
        encoded_frame->format = video_codec_context_->pix_fmt;
        encoded_frame->width = video_codec_context_->width;
        encoded_frame->height = video_codec_context_->height;

        const auto buffer_result = av_frame_get_buffer(encoded_frame.get(), 1);
        if (buffer_result < 0) {
            throw std::runtime_error(
                "Failed to allocate the encoded streaming video frame buffer. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(buffer_result)
            );
        }

        const std::uint8_t *source_data[4] = {
            decoded_frame.planes.front().bytes.data(),
            nullptr,
            nullptr,
            nullptr
        };
        const int source_linesize[4] = {
            decoded_frame.planes.front().line_stride_bytes,
            0,
            0,
            0
        };

        const auto scale_result = sws_scale(
            scale_context_.get(),
            source_data,
            source_linesize,
            0,
            video_plan_.height,
            encoded_frame->data,
            encoded_frame->linesize
        );
        if (scale_result <= 0) {
            throw std::runtime_error("FFmpeg failed to convert a streaming frame into the encoder pixel format.");
        }

        encoded_frame->pts = decoded_frame.timestamp.source_pts.value_or(0);
        encoded_frame->duration = decoded_frame.timestamp.source_duration.value_or(video_plan_.frame_duration_pts);
        return encoded_frame;
    }

    FrameHandle build_encoded_audio_frame(const DecodedAudioSamples &audio_block) {
        if (!audio_codec_context_ || !audio_plan_.has_value()) {
            throw std::runtime_error("The streaming output session cannot build an audio frame without an audio plan.");
        }

        if (audio_block.sample_format != NormalizedAudioSampleFormat::f32_planar) {
            throw std::runtime_error("The streaming audio encoder currently only supports planar float audio blocks.");
        }

        if (audio_block.sample_rate != audio_plan_->sample_rate ||
            audio_block.channel_count != audio_plan_->channel_count) {
            throw std::runtime_error("The streaming audio block shape does not match the output audio plan.");
        }

        if (audio_block.samples_per_channel <= 0) {
            throw std::runtime_error("The streaming audio encoder received an empty audio block.");
        }

        if (audio_block.channel_samples.size() != static_cast<std::size_t>(audio_plan_->channel_count)) {
            throw std::runtime_error("The streaming audio block channel buffer count does not match the output plan.");
        }

        const int frame_sample_count = audio_block.samples_per_channel;
        if (audio_codec_context_->frame_size > 0 && frame_sample_count > audio_codec_context_->frame_size) {
            throw std::runtime_error(
                "The streaming audio block size exceeds the audio encoder frame size and is not supported."
            );
        }

        if (audio_codec_context_->frame_size > 0 &&
            frame_sample_count != audio_codec_context_->frame_size &&
            !audio_encoder_supports_short_frame_samples(*audio_codec_context_)) {
            throw std::runtime_error(
                "The streaming audio encoder requires fixed-size frames and does not accept a short final audio block."
            );
        }

        auto encoded_frame = allocate_frame();
        encoded_frame->format = audio_codec_context_->sample_fmt;
        encoded_frame->sample_rate = audio_codec_context_->sample_rate;
        encoded_frame->nb_samples = frame_sample_count;
        const auto layout_copy_result =
            av_channel_layout_copy(&encoded_frame->ch_layout, &audio_codec_context_->ch_layout);
        if (layout_copy_result < 0) {
            throw std::runtime_error(
                "Failed to copy the audio encoder channel layout into an output frame. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(layout_copy_result)
            );
        }

        const auto buffer_result = av_frame_get_buffer(encoded_frame.get(), 0);
        if (buffer_result < 0) {
            throw std::runtime_error(
                "Failed to allocate the encoded streaming audio frame buffer. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(buffer_result)
            );
        }

        const auto writable_result = av_frame_make_writable(encoded_frame.get());
        if (writable_result < 0) {
            throw std::runtime_error(
                "Failed to make the encoded streaming audio frame writable. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(writable_result)
            );
        }

        for (int channel_index = 0; channel_index < audio_plan_->channel_count; ++channel_index) {
            const auto &channel_samples = audio_block.channel_samples[static_cast<std::size_t>(channel_index)];
            if (channel_samples.size() != static_cast<std::size_t>(audio_block.samples_per_channel)) {
                throw std::runtime_error(
                    "The streaming audio block does not contain the expected sample count for every channel."
                );
            }

            auto *destination =
                reinterpret_cast<float *>(encoded_frame->extended_data[static_cast<std::size_t>(channel_index)]);
            std::copy_n(channel_samples.begin(), frame_sample_count, destination);
        }

        encoded_frame->pts = audio_block.timestamp.source_pts.value_or(0);
        encoded_frame->duration = audio_block.samples_per_channel;
        return encoded_frame;
    }

    void validate_audio_block_for_output(const DecodedAudioSamples &audio_block) const {
        if (!audio_plan_.has_value()) {
            throw std::runtime_error("The streaming output session cannot validate audio without an audio plan.");
        }

        if (audio_block.sample_format != NormalizedAudioSampleFormat::f32_planar) {
            throw std::runtime_error("The streaming audio encoder currently only supports planar float audio blocks.");
        }

        if (audio_block.sample_rate != audio_plan_->sample_rate ||
            audio_block.channel_count != audio_plan_->channel_count) {
            throw std::runtime_error("The streaming audio block shape does not match the output audio plan.");
        }

        if (audio_block.samples_per_channel <= 0) {
            throw std::runtime_error("The streaming audio encoder received an empty audio block.");
        }

        if (audio_block.channel_samples.size() != static_cast<std::size_t>(audio_plan_->channel_count)) {
            throw std::runtime_error("The streaming audio block channel buffer count does not match the output plan.");
        }

        for (int channel_index = 0; channel_index < audio_plan_->channel_count; ++channel_index) {
            if (audio_block.channel_samples[static_cast<std::size_t>(channel_index)].size() !=
                static_cast<std::size_t>(audio_block.samples_per_channel)) {
                throw std::runtime_error(
                    "The streaming audio block does not contain the expected sample count for every channel."
                );
            }
        }
    }

    void append_pending_audio_block(const DecodedAudioSamples &audio_block) {
        const auto block_start_pts = audio_block.timestamp.source_pts.value_or(0);
        if (!pending_audio_start_pts_.has_value()) {
            pending_audio_start_pts_ = block_start_pts;
        } else {
            const auto expected_next_pts = *pending_audio_start_pts_ + pending_audio_sample_count_;
            if (block_start_pts != expected_next_pts) {
                throw std::runtime_error(
                    "The streaming audio blocks are not contiguous on the output audio timeline."
                );
            }
        }

        if (pending_audio_channels_.empty()) {
            pending_audio_channels_.resize(static_cast<std::size_t>(audio_plan_->channel_count));
        }

        append_channel_samples(pending_audio_channels_, audio_block.channel_samples);
        pending_audio_sample_count_ += audio_block.samples_per_channel;
    }

    DecodedAudioSamples take_pending_audio_block(const int samples_to_emit) {
        if (!audio_plan_.has_value() || !pending_audio_start_pts_.has_value() || samples_to_emit <= 0) {
            throw std::runtime_error("The streaming audio encoder was asked to emit an invalid pending audio block.");
        }

        if (pending_audio_sample_count_ < samples_to_emit) {
            throw std::runtime_error("The streaming audio encoder pending buffer does not contain enough samples.");
        }

        std::vector<std::vector<float>> block_channels{};
        block_channels.reserve(static_cast<std::size_t>(audio_plan_->channel_count));
        for (auto &pending_channel : pending_audio_channels_) {
            if (pending_channel.size() < static_cast<std::size_t>(samples_to_emit)) {
                throw std::runtime_error("The streaming audio encoder pending channel data is incomplete.");
            }

            block_channels.emplace_back(
                pending_channel.begin(),
                pending_channel.begin() + static_cast<std::ptrdiff_t>(samples_to_emit)
            );
            pending_channel.erase(
                pending_channel.begin(),
                pending_channel.begin() + static_cast<std::ptrdiff_t>(samples_to_emit)
            );
        }

        const auto block_start_pts = *pending_audio_start_pts_;
        pending_audio_sample_count_ -= samples_to_emit;
        if (pending_audio_sample_count_ == 0) {
            pending_audio_start_pts_ = std::nullopt;
            pending_audio_channels_.clear();
        } else {
            *pending_audio_start_pts_ += samples_to_emit;
        }

        return DecodedAudioSamples{
            .stream_index = audio_stream_ != nullptr ? audio_stream_->index : -1,
            .block_index = encoded_audio_block_count_,
            .timestamp = MediaTimestamp{
                .source_time_base = audio_plan_->time_base,
                .source_pts = block_start_pts,
                .source_duration = samples_to_emit,
                .origin = TimestampOrigin::stream_cursor,
                .start_microseconds = rescale_to_microseconds(block_start_pts, audio_plan_->time_base),
                .duration_microseconds = rescale_to_microseconds(samples_to_emit, audio_plan_->time_base)
            },
            .sample_rate = audio_plan_->sample_rate,
            .channel_count = audio_plan_->channel_count,
            .channel_layout_name = audio_plan_->channel_layout_name,
            .sample_format = NormalizedAudioSampleFormat::f32_planar,
            .samples_per_channel = samples_to_emit,
            .channel_samples = std::move(block_channels)
        };
    }

    void submit_audio_block_to_encoder(const DecodedAudioSamples &audio_block) {
        auto encoded_frame = build_encoded_audio_frame(audio_block);
        const auto send_result = avcodec_send_frame(audio_codec_context_.get(), encoded_frame.get());
        if (send_result < 0) {
            throw std::runtime_error(
                "Failed to send an audio block to the audio encoder. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(send_result)
            );
        }

        ++encoded_audio_block_count_;
        drain_audio_encoder();
    }

    void drain_ready_audio_blocks_into_encoder(const bool final_drain) {
        if (!audio_codec_context_ || !audio_plan_.has_value()) {
            return;
        }

        const int encoder_frame_size = audio_codec_context_->frame_size;
        if (encoder_frame_size <= 0) {
            while (pending_audio_sample_count_ > 0) {
                submit_audio_block_to_encoder(take_pending_audio_block(static_cast<int>(pending_audio_sample_count_)));
            }
            return;
        }

        while (pending_audio_sample_count_ >= encoder_frame_size) {
            submit_audio_block_to_encoder(take_pending_audio_block(encoder_frame_size));
        }

        if (final_drain && pending_audio_sample_count_ > 0) {
            if (!audio_encoder_supports_short_frame_samples(*audio_codec_context_)) {
                throw std::runtime_error(
                    "The streaming audio encoder requires fixed-size frames and does not support a short final frame."
                );
            }

            submit_audio_block_to_encoder(
                take_pending_audio_block(static_cast<int>(pending_audio_sample_count_))
            );
        }
    }

    void mux_encoded_packet(
        AVPacket &packet,
        AVStream &stream,
        const Rational &encoder_time_base
    ) {
        av_packet_rescale_ts(
            &packet,
            to_av_rational(encoder_time_base),
            stream.time_base
        );
        packet.stream_index = stream.index;

        const auto write_result = av_interleaved_write_frame(output_context_.get(), &packet);
        av_packet_unref(&packet);
        if (write_result < 0) {
            throw std::runtime_error(
                "Failed to mux an encoded streaming packet. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(write_result)
            );
        }
    }

    void drain_video_encoder() {
        while (true) {
            const auto receive_result =
                avcodec_receive_packet(video_codec_context_.get(), reusable_video_receive_packet_.get());
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive an encoded streaming packet. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            mux_encoded_packet(*reusable_video_receive_packet_, *video_stream_, video_plan_.time_base);
        }
    }

    void drain_audio_encoder() {
        while (true) {
            const auto receive_result =
                avcodec_receive_packet(audio_codec_context_.get(), reusable_audio_receive_packet_.get());
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive an encoded streaming audio packet. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            mux_encoded_packet(*reusable_audio_receive_packet_, *audio_stream_, audio_plan_->time_base);
        }
    }

    MediaEncodeRequest request_{};
    VideoOutputPlan video_plan_{};
    ResolvedAudioOutputPlan resolved_audio_output_{};
    std::optional<AudioOutputPlan> audio_plan_{};
    std::optional<AudioCopyTemplate> audio_copy_template_{};
    OutputFormatContextHandle output_context_{};
    CodecContextHandle video_codec_context_{};
    CodecContextHandle audio_codec_context_{};
    AVStream *video_stream_{nullptr};
    AVStream *audio_stream_{nullptr};
    PacketHandle reusable_video_receive_packet_{};
    PacketHandle reusable_audio_receive_packet_{};
    SwsContextHandle scale_context_{};
    std::vector<std::vector<float>> pending_audio_channels_{};
    std::optional<std::int64_t> pending_audio_start_pts_{};
    std::int64_t pending_audio_sample_count_{0};
    std::int64_t encoded_video_frame_count_{0};
    std::int64_t encoded_audio_block_count_{0};
    bool finalized_{false};
};

VideoOutputPlan build_video_output_plan(const timeline::TimelinePlan &timeline_plan) {
    if (timeline_plan.segments.empty() || timeline_plan.main_segment_index >= timeline_plan.segments.size()) {
        throw std::runtime_error("The streaming pipeline requires a valid main timeline segment.");
    }

    const auto &main_segment_info = timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info;
    if (!main_segment_info.primary_video_stream.has_value()) {
        throw std::runtime_error("The streaming pipeline requires a main video stream.");
    }

    const auto &main_video_stream = *main_segment_info.primary_video_stream;
    if (main_video_stream.width <= 0 || main_video_stream.height <= 0) {
        throw std::runtime_error("The streaming pipeline requires a valid output resolution.");
    }

    const auto frame_duration_pts = compute_frame_duration_pts(
        timeline_plan.output_frame_rate,
        timeline_plan.output_video_time_base
    );

    return VideoOutputPlan{
        .width = main_video_stream.width,
        .height = main_video_stream.height,
        .time_base = timeline_plan.output_video_time_base,
        .average_frame_rate = timeline_plan.output_frame_rate,
        .sample_aspect_ratio = rational_is_positive(main_video_stream.sample_aspect_ratio)
            ? main_video_stream.sample_aspect_ratio
            : Rational{1, 1},
        .frame_duration_pts = frame_duration_pts,
        .frame_duration_microseconds = rescale_to_microseconds(frame_duration_pts, timeline_plan.output_video_time_base)
    };
}

std::optional<AudioOutputPlan> build_audio_output_plan(const ResolvedAudioOutputPlan &resolved_audio_output) {
    if (!resolved_audio_output.output_present) {
        return std::nullopt;
    }

    if (resolved_audio_output.sample_rate_hz <= 0 || resolved_audio_output.channel_count <= 0) {
        throw std::runtime_error("The streaming pipeline requires a usable output audio sample rate and channel count.");
    }

    Rational time_base = resolved_audio_output.time_base;
    if (!rational_is_positive(time_base)) {
        time_base = Rational{
            .numerator = 1,
            .denominator = resolved_audio_output.sample_rate_hz
        };
    }

    if (!rational_is_positive(time_base)) {
        throw std::runtime_error("The streaming pipeline requires a usable output audio time base.");
    }

    return AudioOutputPlan{
        .resolved = resolved_audio_output,
        .sample_rate = resolved_audio_output.sample_rate_hz,
        .channel_count = resolved_audio_output.channel_count,
        .time_base = time_base,
        .channel_layout_name = resolved_audio_output.channel_layout_name,
        .bitrate_kbps = resolved_audio_output.bitrate_kbps
    };
}

DecodedAudioSamples make_output_audio_block(
    const AudioOutputPlan &output_audio_plan,
    const DecodeNormalizationPolicy &normalization_policy,
    const std::int64_t block_index,
    const std::int64_t output_pts,
    const int samples_per_channel,
    const bool silent,
    const std::vector<std::vector<float>> &channel_samples
) {
    std::vector<std::vector<float>> output_channels{};
    if (silent) {
        output_channels.resize(
            static_cast<std::size_t>(output_audio_plan.channel_count),
            std::vector<float>(static_cast<std::size_t>(samples_per_channel), 0.0F)
        );
    } else {
        output_channels = channel_samples;
    }

    return DecodedAudioSamples{
        .stream_index = -1,
        .block_index = block_index,
        .timestamp = MediaTimestamp{
            .source_time_base = output_audio_plan.time_base,
            .source_pts = output_pts,
            .source_duration = samples_per_channel,
            .origin = TimestampOrigin::stream_cursor,
            .start_microseconds = rescale_to_microseconds(output_pts, output_audio_plan.time_base),
            .duration_microseconds = rescale_to_microseconds(
                samples_per_channel,
                output_audio_plan.time_base
            )
        },
        .sample_rate = output_audio_plan.sample_rate,
        .channel_count = output_audio_plan.channel_count,
        .channel_layout_name = output_audio_plan.channel_layout_name,
        .sample_format = normalization_policy.audio_sample_format,
        .samples_per_channel = samples_per_channel,
        .channel_samples = std::move(output_channels)
    };
}

std::vector<std::vector<float>> copy_audio_block_prefix(
    const std::vector<std::vector<float>> &channel_samples,
    const int samples_per_channel
) {
    std::vector<std::vector<float>> prefix{};
    prefix.reserve(channel_samples.size());
    for (const auto &channel : channel_samples) {
        if (channel.size() < static_cast<std::size_t>(samples_per_channel)) {
            throw std::runtime_error("The streaming pipeline could not trim an audio block because its samples were incomplete.");
        }

        prefix.emplace_back(channel.begin(), channel.begin() + samples_per_channel);
    }

    return prefix;
}

ResolvedVideoFrameTiming resolve_video_frame_timing_for_segment(
    const timeline::TimelineSegmentKind kind,
    const DecodedVideoFrame &frame,
    const VideoOutputPlan &video_output_plan,
    const Rational &output_video_time_base,
    const std::int64_t segment_output_start_pts,
    std::optional<std::int64_t> &first_source_pts_in_output_time_base,
    std::optional<std::int64_t> &previous_source_pts,
    Rational &previous_source_time_base
) {
    if (!subtitles::detail::is_rgba_frame_layout_supported(frame)) {
        throw std::runtime_error(
            "The " + std::string(timeline::to_string(kind)) +
            " segment contains an unsupported decoded video frame layout."
        );
    }

    if (frame.width != video_output_plan.width || frame.height != video_output_plan.height) {
        throw std::runtime_error(
            "The " + std::string(timeline::to_string(kind)) +
            " segment decoded into a resolution that does not match the main segment."
        );
    }

    if (rational_is_positive(video_output_plan.sample_aspect_ratio) &&
        !rationals_equal(frame.sample_aspect_ratio, video_output_plan.sample_aspect_ratio)) {
        throw std::runtime_error(
            "The " + std::string(timeline::to_string(kind)) +
            " segment decoded with a sample aspect ratio that does not match the main segment."
        );
    }

    if (!rational_is_positive(frame.timestamp.source_time_base) || !frame.timestamp.source_pts.has_value()) {
        throw std::runtime_error(
            "The " + std::string(timeline::to_string(kind)) +
            " segment contains a decoded frame with unusable timing data."
        );
    }

    const auto current_source_pts_in_output_time_base = rescale_value(
        *frame.timestamp.source_pts,
        frame.timestamp.source_time_base,
        output_video_time_base
    );
    if (!first_source_pts_in_output_time_base.has_value()) {
        first_source_pts_in_output_time_base = current_source_pts_in_output_time_base;
    }

    if (previous_source_pts.has_value() &&
        av_compare_ts(
            *frame.timestamp.source_pts,
            to_av_rational(frame.timestamp.source_time_base),
            *previous_source_pts,
            to_av_rational(previous_source_time_base)
        ) <= 0) {
        throw std::runtime_error(
            "The " + std::string(timeline::to_string(kind)) +
            " segment timestamps do not advance monotonically on the decoded timeline."
        );
    }

    previous_source_pts = *frame.timestamp.source_pts;
    previous_source_time_base = frame.timestamp.source_time_base;

    std::optional<std::int64_t> output_duration_pts{};
    if (frame.timestamp.source_duration.has_value() && *frame.timestamp.source_duration > 0) {
        const auto converted_duration = rescale_value(
            *frame.timestamp.source_duration,
            frame.timestamp.source_time_base,
            output_video_time_base
        );
        if (converted_duration > 0) {
            output_duration_pts = converted_duration;
        }
    }

    return ResolvedVideoFrameTiming{
        .output_pts =
            segment_output_start_pts + (current_source_pts_in_output_time_base - *first_source_pts_in_output_time_base),
        .output_duration_pts = output_duration_pts
    };
}

SegmentProcessResult process_segment(
    const timeline::TimelinePlan &timeline_plan,
    const timeline::TimelineSegmentPlan &segment_plan,
    const std::optional<job::EncodeJobSubtitleSettings> &subtitle_settings,
    subtitles::SubtitleRenderSession *subtitle_session,
    const VideoOutputPlan &video_output_plan,
    const DecodeNormalizationPolicy &normalization_policy,
    const PipelineQueueLimits &queue_limits,
    const std::optional<AudioOutputPlan> &audio_output_plan,
    StreamingOutputSession &output_session,
    StreamingEncodeProgressEmitter &progress_emitter,
    std::int64_t &next_output_frame_index,
    std::int64_t &next_output_video_pts,
    std::int64_t &next_output_audio_pts
) {
    SegmentProcessResult result{
        .segment_summary = timeline::TimelineSegmentSummary{
            .kind = segment_plan.kind,
            .source_path = segment_plan.source_path,
            .start_microseconds = rescale_to_microseconds(next_output_video_pts, timeline_plan.output_video_time_base),
            .duration_microseconds = 0,
            .video_frame_count = 0,
            .audio_block_count = 0,
            .subtitles_enabled = segment_plan.subtitles_enabled,
            .inserted_silence = false
        }
    };

    const AudioOutputPlan *resolved_audio_plan = audio_output_plan.has_value()
        ? &*audio_output_plan
        : nullptr;
    const bool encode_audio = resolved_audio_plan != nullptr && resolved_audio_plan->encodes_audio();
    const bool copy_audio = resolved_audio_plan != nullptr && resolved_audio_plan->copies_audio();

    SegmentDecoderResources resources = open_segment_resources(segment_plan, encode_audio);
    PacketHandle demux_packet = allocate_packet();
    FrameHandle decoded_video_frame = allocate_frame();
    FrameHandle decoded_audio_frame = allocate_frame();

    SwsContextHandle video_scale_context{};
    int scale_width = 0;
    int scale_height = 0;
    AVPixelFormat scale_source_pixel_format = AV_PIX_FMT_NONE;
    std::int64_t next_fallback_source_pts =
        segment_plan.inspected_source_info.primary_video_stream->timestamps.start_pts.value_or(0);
    const auto fallback_video_duration_pts =
        infer_video_frame_duration_pts(*segment_plan.inspected_source_info.primary_video_stream).value_or(1);
    const std::int64_t segment_video_start_pts =
        segment_plan.inspected_source_info.primary_video_stream->timestamps.start_pts.value_or(0);

    SwrContextHandle audio_resample_context{};
    std::vector<std::vector<float>> pending_audio_channels{};
    std::int64_t normalized_audio_samples = 0;
    std::int64_t decoded_segment_audio_samples = 0;
    std::int64_t emitted_segment_audio_samples = 0;
    std::int64_t decoded_video_frame_index = 0;
    const std::int64_t segment_output_start_pts = next_output_video_pts;
    std::int64_t segment_output_end_pts = segment_output_start_pts;
    std::optional<std::int64_t> first_video_source_pts_in_output_time_base{};
    std::optional<std::int64_t> previous_video_source_pts{};
    Rational previous_video_source_time_base{};
    std::optional<PendingVideoFrameOutput> pending_video_frame{};

    if (encode_audio && resources.audio_decoder) {
        if (normalization_policy.audio_sample_format != NormalizedAudioSampleFormat::f32_planar) {
            throw std::runtime_error("Only f32_planar audio normalization is implemented in the streaming pipeline.");
        }

        if (normalization_policy.audio_block_samples <= 0) {
            throw std::runtime_error("The audio normalization policy must use a positive block size.");
        }

        AVChannelLayout output_channel_layout = make_default_audio_channel_layout(resolved_audio_plan->channel_count);
        SwrContext *raw_resample_context = nullptr;
        const auto resample_setup_result = swr_alloc_set_opts2(
            &raw_resample_context,
            &output_channel_layout,
            AV_SAMPLE_FMT_FLTP,
            resolved_audio_plan->sample_rate,
            &resources.audio_decoder->ch_layout,
            resources.audio_decoder->sample_fmt,
            resources.audio_decoder->sample_rate,
            0,
            nullptr
        );
        av_channel_layout_uninit(&output_channel_layout);
        if (resample_setup_result < 0 || raw_resample_context == nullptr) {
            throw std::runtime_error(
                "Failed to configure the streaming audio normalization context. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(resample_setup_result)
            );
        }

        audio_resample_context.reset(raw_resample_context);
        const auto resample_init_result = swr_init(audio_resample_context.get());
        if (resample_init_result < 0) {
            throw std::runtime_error(
                "Failed to initialize the streaming audio normalization context. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(resample_init_result)
            );
        }
    }

    // Audio packets can briefly outrun video cadence in normal interleaving, so keep them pending
    // until the decoded audio horizon has room to advance on the shared output timeline.
    std::deque<PacketHandle> pending_audio_packet_queue{};
    BoundedQueue<DecodedAudioSamples> decoded_audio_queue(queue_limits.decoded_audio_block_queue_depth);
    std::int64_t queued_decoded_audio_samples = 0;
    std::int64_t known_video_horizon_microseconds = 0;
    std::optional<std::int64_t> first_known_video_source_pts{};
    std::optional<std::int64_t> last_written_video_end_pts{};
    std::optional<std::int64_t> last_written_audio_pts{};

    if (encode_audio && queue_limits.startup_audio_preroll_microseconds < 0) {
        throw std::runtime_error("The streaming audio startup preroll must not be negative.");
    }

    const auto &main_video_stream =
        *timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info.primary_video_stream;

    const auto expected_segment_audio_samples_for_emitted_video = [&]() -> std::int64_t {
        if (!encode_audio || resolved_audio_plan == nullptr) {
            return 0;
        }

        return rescale_value(
            last_written_video_end_pts.has_value()
                ? (*last_written_video_end_pts - segment_output_start_pts)
                : 0,
            timeline_plan.output_video_time_base,
            resolved_audio_plan->time_base
        );
    };

    const auto update_known_video_timeline = [&](const AVPacket &video_packet) {
        if (!encode_audio || resolved_audio_plan == nullptr) {
            return;
        }

        const auto packet_timestamp = choose_packet_timestamp_seed(video_packet);
        if (!packet_timestamp.has_value()) {
            return;
        }

        if (!first_known_video_source_pts.has_value()) {
            first_known_video_source_pts = *packet_timestamp;
        }

        const auto packet_duration_pts = video_packet.duration > 0 ? video_packet.duration : fallback_video_duration_pts;
        const auto packet_end_pts = *packet_timestamp + packet_duration_pts;
        if (packet_end_pts <= segment_video_start_pts) {
            return;
        }

        known_video_horizon_microseconds = std::max(
            known_video_horizon_microseconds,
            rescale_value(
                packet_end_pts - segment_video_start_pts,
                segment_plan.inspected_source_info.primary_video_stream->timestamps.time_base,
                Rational{1, AV_TIME_BASE}
            )
        );
    };

    const auto pending_resampled_audio_samples = [&]() -> std::int64_t {
        if (pending_audio_channels.empty()) {
            return 0;
        }

        return static_cast<std::int64_t>(pending_audio_channels.front().size());
    };

    const auto audio_samples_to_microseconds = [&](const std::int64_t samples) -> std::int64_t {
        if (!encode_audio || resolved_audio_plan == nullptr || samples <= 0) {
            return 0;
        }

        return rescale_value(
            samples,
            resolved_audio_plan->time_base,
            Rational{1, AV_TIME_BASE}
        );
    };

    const auto written_video_horizon_microseconds = [&]() -> std::int64_t {
        if (!last_written_video_end_pts.has_value()) {
            return 0;
        }

        return rescale_value(
            *last_written_video_end_pts - segment_output_start_pts,
            timeline_plan.output_video_time_base,
            Rational{1, AV_TIME_BASE}
        );
    };

    const auto shared_video_horizon_microseconds = [&]() -> std::int64_t {
        return std::max(written_video_horizon_microseconds(), known_video_horizon_microseconds);
    };

    const auto written_audio_horizon_microseconds = [&]() -> std::int64_t {
        if (!last_written_audio_pts.has_value()) {
            return 0;
        }

        return audio_samples_to_microseconds(emitted_segment_audio_samples);
    };

    const auto buffered_decoded_audio_microseconds = [&]() -> std::int64_t {
        return audio_samples_to_microseconds(queued_decoded_audio_samples + pending_resampled_audio_samples());
    };

    const auto allowed_audio_horizon_microseconds = [&]() -> std::int64_t {
        if (!last_written_video_end_pts.has_value()) {
            if (first_known_video_source_pts.has_value()) {
                return std::max(
                    queue_limits.startup_audio_preroll_microseconds,
                    shared_video_horizon_microseconds()
                );
            }

            return queue_limits.startup_audio_preroll_microseconds;
        }

        return shared_video_horizon_microseconds();
    };

    const auto audio_decode_should_wait = [&]() -> bool {
        if (!encode_audio) {
            return false;
        }

        if (decoded_audio_queue.full()) {
            return true;
        }

        return (written_audio_horizon_microseconds() + buffered_decoded_audio_microseconds()) >=
            allowed_audio_horizon_microseconds();
    };

    const auto emit_output_audio_block = [&](
        const DecodedAudioSamples &source_block,
        const int samples_to_emit,
        const bool silent
    ) {
        if (!encode_audio || resolved_audio_plan == nullptr || samples_to_emit <= 0) {
            return;
        }

        const auto output_channels = silent
            ? std::vector<std::vector<float>>{}
            : copy_audio_block_prefix(source_block.channel_samples, samples_to_emit);
        auto output_block = make_output_audio_block(
            *resolved_audio_plan,
            normalization_policy,
            result.segment_summary.audio_block_count,
            next_output_audio_pts,
            samples_to_emit,
            silent,
            output_channels
        );
        output_session.push_audio_block(output_block);
        last_written_audio_pts = next_output_audio_pts;
        next_output_audio_pts += samples_to_emit;
        emitted_segment_audio_samples += samples_to_emit;
        ++result.segment_summary.audio_block_count;
        ++result.decoded_audio_block_count;
        if (silent) {
            result.segment_summary.inserted_silence = true;
        }
    };

    const auto emit_trailing_silence = [&](std::int64_t missing_samples) {
        if (missing_samples <= 0) {
            return;
        }

        if (normalization_policy.audio_block_samples <= 0) {
            throw std::runtime_error("The streaming audio output path requires a positive normalized audio block size.");
        }

        while (missing_samples > 0) {
            const int block_size = static_cast<int>(std::min<std::int64_t>(
                missing_samples,
                normalization_policy.audio_block_samples
            ));
            emit_output_audio_block(DecodedAudioSamples{}, block_size, true);
            missing_samples -= block_size;
        }
    };

    const auto drain_audio_queue = [&](const bool final_drain) {
        while (!decoded_audio_queue.empty()) {
            const auto &ready_block = decoded_audio_queue.front();
            if (ready_block.sample_format != normalization_policy.audio_sample_format ||
                resolved_audio_plan == nullptr ||
                ready_block.sample_rate != resolved_audio_plan->sample_rate ||
                ready_block.channel_count != resolved_audio_plan->channel_count) {
                throw std::runtime_error(
                    "The decoded " + std::string(timeline::to_string(segment_plan.kind)) +
                    " segment audio block shape does not match the main segment."
                );
            }

            if (!final_drain) {
                const auto next_audio_horizon_microseconds =
                    written_audio_horizon_microseconds() + audio_samples_to_microseconds(ready_block.samples_per_channel);
                if (next_audio_horizon_microseconds > allowed_audio_horizon_microseconds()) {
                    break;
                }
            }

            auto emitted_block = decoded_audio_queue.pop();
            queued_decoded_audio_samples -= emitted_block.samples_per_channel;
            decoded_segment_audio_samples += emitted_block.samples_per_channel;

            if (!final_drain) {
                emit_output_audio_block(emitted_block, emitted_block.samples_per_channel, false);
                continue;
            }

            const auto available_segment_samples =
                expected_segment_audio_samples_for_emitted_video() - emitted_segment_audio_samples;
            if (available_segment_samples <= 0) {
                continue;
            }

            if (emitted_block.samples_per_channel > available_segment_samples) {
                emit_output_audio_block(
                    emitted_block,
                    static_cast<int>(available_segment_samples),
                    false
                );
                break;
            }

            emit_output_audio_block(emitted_block, emitted_block.samples_per_channel, false);
        }
    };

    const auto emit_video_frame = [&](
        DecodedVideoFrame video_frame,
        const ResolvedVideoFrameTiming &timing,
        const std::int64_t output_duration_pts
    ) {
        std::int64_t subtitle_timestamp_microseconds = 0;
        if (segment_plan.subtitles_enabled && subtitle_settings.has_value()) {
            subtitle_timestamp_microseconds =
                subtitle_settings->timing_mode == timeline::SubtitleTimingMode::full_output_timeline
                    ? rescale_to_microseconds(timing.output_pts, timeline_plan.output_video_time_base)
                    : video_frame.timestamp.start_microseconds;
        }

        if (maybe_composite_subtitles(
                video_frame,
                subtitle_session,
                subtitle_settings,
                segment_plan.subtitles_enabled,
                subtitle_timestamp_microseconds)) {
            ++result.subtitled_video_frame_count;
        }

        const auto timing_origin = video_frame.timestamp.origin;
        video_frame.stream_index = main_video_stream.stream_index;
        video_frame.frame_index = next_output_frame_index;
        video_frame.timestamp.source_time_base = timeline_plan.output_video_time_base;
        video_frame.timestamp.source_pts = timing.output_pts;
        video_frame.timestamp.source_duration = output_duration_pts;
        video_frame.timestamp.origin = timing_origin;
        video_frame.timestamp.start_microseconds =
            rescale_to_microseconds(timing.output_pts, timeline_plan.output_video_time_base);
        video_frame.timestamp.duration_microseconds =
            rescale_to_microseconds(output_duration_pts, timeline_plan.output_video_time_base);
        video_frame.sample_aspect_ratio = video_output_plan.sample_aspect_ratio;

        output_session.push_frame(video_frame);
        const auto frame_end_pts = timing.output_pts + output_duration_pts;
        last_written_video_end_pts = frame_end_pts;
        segment_output_end_pts = std::max(segment_output_end_pts, frame_end_pts);
        ++result.segment_summary.video_frame_count;
        ++result.decoded_video_frame_count;
        progress_emitter.record_frame_written(
            static_cast<std::uint64_t>(next_output_frame_index + 1),
            rescale_to_microseconds(frame_end_pts, timeline_plan.output_video_time_base)
        );
        ++next_output_frame_index;

        if (encode_audio) {
            drain_audio_queue(false);
        }
    };

    const auto process_video_frame = [&](DecodedVideoFrame video_frame) {
        const auto resolved_timing = resolve_video_frame_timing_for_segment(
            segment_plan.kind,
            video_frame,
            video_output_plan,
            timeline_plan.output_video_time_base,
            segment_output_start_pts,
            first_video_source_pts_in_output_time_base,
            previous_video_source_pts,
            previous_video_source_time_base
        );

        if (pending_video_frame.has_value()) {
            const auto pending_duration_pts = resolved_timing.output_pts - pending_video_frame->timing.output_pts;
            if (pending_duration_pts <= 0) {
                throw std::runtime_error(
                    "The " + std::string(timeline::to_string(segment_plan.kind)) +
                    " segment produced a non-positive video frame duration on the output timeline."
                );
            }

            emit_video_frame(
                std::move(pending_video_frame->frame),
                pending_video_frame->timing,
                pending_duration_pts
            );
        }

        pending_video_frame = PendingVideoFrameOutput{
            .frame = std::move(video_frame),
            .timing = resolved_timing
        };
    };

    const auto enqueue_audio_block = [&](DecodedAudioSamples audio_block, const bool allow_final_drain) -> bool {
        if (decoded_audio_queue.full()) {
            drain_audio_queue(allow_final_drain);
        }

        if (decoded_audio_queue.full()) {
            return false;
        }

        queued_decoded_audio_samples += audio_block.samples_per_channel;
        decoded_audio_queue.push(std::move(audio_block));
        return true;
    };

    const auto emit_ready_audio_blocks = [&]() {
        if (!encode_audio || resolved_audio_plan == nullptr || pending_audio_channels.empty()) {
            return;
        }

        while (static_cast<int>(pending_audio_channels.front().size()) >= normalization_policy.audio_block_samples) {
            if (decoded_audio_queue.full()) {
                drain_audio_queue(false);
                if (decoded_audio_queue.full()) {
                    return;
                }
            }

            std::vector<std::vector<float>> block_channels(
                pending_audio_channels.size(),
                std::vector<float>(static_cast<std::size_t>(normalization_policy.audio_block_samples))
            );

            for (std::size_t channel_index = 0; channel_index < pending_audio_channels.size(); ++channel_index) {
                auto &pending = pending_audio_channels[channel_index];
                auto &block = block_channels[channel_index];
                std::copy_n(pending.begin(), normalization_policy.audio_block_samples, block.begin());
                pending.erase(pending.begin(), pending.begin() + normalization_policy.audio_block_samples);
            }

            if (!enqueue_audio_block(
                    build_audio_block(
                        *resolved_audio_plan,
                        result.segment_summary.audio_block_count + static_cast<std::int64_t>(decoded_audio_queue.size()),
                        normalized_audio_samples,
                        normalization_policy.audio_block_samples,
                        block_channels
                    ),
                    false)) {
                throw std::runtime_error(
                    "The streaming audio pipeline could not stage a decoded audio block into the bounded queue."
                );
            }

            normalized_audio_samples += normalization_policy.audio_block_samples;
        }
    };

    const auto receive_video_frames = [&]() {
        while (true) {
            const auto receive_result = avcodec_receive_frame(resources.video_decoder.get(), decoded_video_frame.get());
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive a decoded streaming video frame. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            DecodedVideoFrame normalized_frame = normalize_video_frame(
                *decoded_video_frame,
                *resources.video_stream,
                segment_plan.inspected_source_info.primary_video_stream->stream_index,
                decoded_video_frame_index,
                normalization_policy,
                video_scale_context,
                scale_width,
                scale_height,
                scale_source_pixel_format,
                next_fallback_source_pts,
                fallback_video_duration_pts
            );
            process_video_frame(std::move(normalized_frame));
            ++decoded_video_frame_index;
            av_frame_unref(decoded_video_frame.get());
        }
    };

    const auto receive_audio_frames = [&]() {
        if (!encode_audio || !resources.audio_decoder) {
            return;
        }

        while (true) {
            const auto receive_result = avcodec_receive_frame(resources.audio_decoder.get(), decoded_audio_frame.get());
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive decoded streaming audio samples. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            append_channel_samples(
                pending_audio_channels,
                resample_audio_frame(
                    *audio_resample_context,
                    decoded_audio_frame.get(),
                    resolved_audio_plan->channel_count
                )
            );
            emit_ready_audio_blocks();

            av_frame_unref(decoded_audio_frame.get());

            if (audio_decode_should_wait()) {
                return;
            }
        }
    };

    const auto flush_pending_audio_packets = [&]() {
        if (!encode_audio || !resources.audio_decoder) {
            return;
        }

        while (true) {
            receive_audio_frames();
            drain_audio_queue(false);
            emit_ready_audio_blocks();
            if (audio_decode_should_wait()) {
                return;
            }

            if (pending_audio_packet_queue.empty()) {
                return;
            }

            PacketHandle packet = std::move(pending_audio_packet_queue.front());
            pending_audio_packet_queue.pop_front();
            send_packet_or_throw(*resources.audio_decoder, packet.get());
        }
    };

    while (av_read_frame(resources.format_context.get(), demux_packet.get()) >= 0) {
        if (demux_packet->stream_index == segment_plan.inspected_source_info.primary_video_stream->stream_index) {
            update_known_video_timeline(*demux_packet);
            send_packet_or_throw(*resources.video_decoder, demux_packet.get());
            receive_video_frames();
        } else if (resources.audio_stream &&
                   segment_plan.inspected_source_info.primary_audio_stream.has_value() &&
                   demux_packet->stream_index == segment_plan.inspected_source_info.primary_audio_stream->stream_index) {
            if (copy_audio) {
                output_session.copy_audio_packet(*demux_packet);
            } else if (encode_audio) {
                auto queued_packet = allocate_packet();
                av_packet_move_ref(queued_packet.get(), demux_packet.get());
                pending_audio_packet_queue.push_back(std::move(queued_packet));
                flush_pending_audio_packets();
            }
        }

        av_packet_unref(demux_packet.get());

        if (encode_audio) {
            drain_audio_queue(false);
            flush_pending_audio_packets();
        }
    }

    send_packet_or_throw(*resources.video_decoder, nullptr);
    receive_video_frames();

    if (pending_video_frame.has_value()) {
        const auto final_duration_pts =
            pending_video_frame->timing.output_duration_pts.value_or(video_output_plan.frame_duration_pts);
        if (final_duration_pts <= 0) {
            throw std::runtime_error(
                "The " + std::string(timeline::to_string(segment_plan.kind)) +
                " segment final video frame does not expose a usable duration."
            );
        }

        emit_video_frame(
            std::move(pending_video_frame->frame),
            pending_video_frame->timing,
            final_duration_pts
        );
        pending_video_frame.reset();
    }

    if (encode_audio && resources.audio_decoder) {
        flush_pending_audio_packets();
        send_packet_or_throw(*resources.audio_decoder, nullptr);
        receive_audio_frames();

        while (true) {
            const auto flushed_channels = resample_audio_frame(
                *audio_resample_context,
                nullptr,
                resolved_audio_plan->channel_count
            );
            if (flushed_channels.empty() || flushed_channels.front().empty()) {
                break;
            }

            append_channel_samples(pending_audio_channels, flushed_channels);
            emit_ready_audio_blocks();
        }

        if (!pending_audio_channels.empty() && !pending_audio_channels.front().empty()) {
            const int tail_sample_count = static_cast<int>(pending_audio_channels.front().size());
            if (!enqueue_audio_block(
                    build_audio_block(
                        *resolved_audio_plan,
                        result.segment_summary.audio_block_count + static_cast<std::int64_t>(decoded_audio_queue.size()),
                        normalized_audio_samples,
                        tail_sample_count,
                        pending_audio_channels
                    ),
                    true)) {
                throw std::runtime_error(
                    "The streaming audio pipeline could not enqueue the final decoded audio tail block."
                );
            }

            normalized_audio_samples += tail_sample_count;
        }
        drain_audio_queue(true);
    }

    result.segment_summary.duration_microseconds = rescale_to_microseconds(
        segment_output_end_pts - segment_output_start_pts,
        timeline_plan.output_video_time_base
    );

    if (encode_audio && resolved_audio_plan != nullptr) {
        const auto expected_segment_samples = rescale_value(
            segment_output_end_pts - segment_output_start_pts,
            timeline_plan.output_video_time_base,
            resolved_audio_plan->time_base
        );

        if (!segment_plan.inspected_source_info.primary_audio_stream.has_value()) {
            emit_trailing_silence(expected_segment_samples - emitted_segment_audio_samples);
        } else {
            if (decoded_segment_audio_samples == 0 && expected_segment_samples > 0) {
                throw std::runtime_error(
                    "The decoded " + std::string(timeline::to_string(segment_plan.kind)) +
                    " segment exposed an audio stream but did not produce any audio samples."
                );
            }

            if (emitted_segment_audio_samples < expected_segment_samples) {
                emit_trailing_silence(expected_segment_samples - emitted_segment_audio_samples);
            }
        }
    }

    next_output_video_pts = segment_output_end_pts;
    return result;
}

subtitles::SubtitleRenderSessionResult create_subtitle_session(
    subtitles::SubtitleRenderer &subtitle_renderer,
    const timeline::TimelinePlan &timeline_plan,
    const job::EncodeJobSubtitleSettings &subtitle_settings
) {
    const auto &main_segment_info = timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info;
    const auto &video_stream = *main_segment_info.primary_video_stream;
    return subtitle_renderer.create_session(subtitles::SubtitleRenderSessionCreateRequest{
        .subtitle_path = subtitle_settings.subtitle_path,
        .format_hint = subtitle_settings.format_hint,
        .canvas_width = video_stream.width,
        .canvas_height = video_stream.height,
        .sample_aspect_ratio = rational_is_positive(video_stream.sample_aspect_ratio)
            ? video_stream.sample_aspect_ratio
            : Rational{1, 1}
    });
}

StreamingTranscodeResult transcode_impl(const StreamingTranscodeRequest &request) {
    if (request.timeline_plan == nullptr) {
        return make_error(
            "The streaming transcode request did not include a timeline plan.",
            "Assemble the timeline before starting the streaming encoder."
        );
    }

    const auto &timeline_plan = *request.timeline_plan;
    if (timeline_plan.segments.empty() || timeline_plan.main_segment_index >= timeline_plan.segments.size()) {
        return make_error(
            "The streaming transcode request did not include a valid main timeline segment.",
            "Rebuild the intro/main/outro timeline before starting the encode."
        );
    }

    if (request.subtitle_settings != nullptr &&
        request.subtitle_settings->has_value() &&
        request.subtitle_renderer == nullptr) {
        return make_error(
            "The streaming transcode request enabled subtitles without a subtitle renderer.",
            "Create the subtitle renderer before starting subtitle burn-in."
        );
    }

    try {
        const VideoOutputPlan video_output_plan = build_video_output_plan(timeline_plan);
        const auto *main_source_audio_stream =
            timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info.primary_audio_stream.has_value()
                ? &*timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info.primary_audio_stream
                : nullptr;
        const auto resolved_audio_output = resolve_audio_output_plan(AudioOutputResolveRequest{
            .output_path = request.media_encode_request.output_path,
            .settings = request.media_encode_request.audio_settings,
            .segment_count = timeline_plan.segments.size(),
            .main_source_audio_stream = main_source_audio_stream
        });
        if (resolved_audio_output.requested_copy_blocker.has_value()) {
            return make_error(
                "The selected audio mode is not compatible with the requested output.",
                *resolved_audio_output.requested_copy_blocker + " Use Auto or AAC instead."
            );
        }

        const auto audio_output_plan = build_audio_output_plan(resolved_audio_output);
        std::optional<AudioCopyTemplate> audio_copy_template{};
        if (audio_output_plan.has_value() && audio_output_plan->copies_audio()) {
            audio_copy_template = build_audio_copy_template(
                timeline_plan.segments[timeline_plan.main_segment_index]
            );
            if (!audio_copy_template.has_value()) {
                return make_error(
                    "The selected audio copy mode did not have a source stream to copy.",
                    "Choose Auto, AAC, or a source with audio."
                );
            }
        }

        StreamingOutputSession output_session(
            request.media_encode_request,
            video_output_plan,
            resolved_audio_output,
            audio_output_plan,
            std::move(audio_copy_template)
        );

        std::unique_ptr<subtitles::SubtitleRenderSession> subtitle_session{};
        if (request.subtitle_settings != nullptr && request.subtitle_settings->has_value()) {
            auto session_result = create_subtitle_session(
                *request.subtitle_renderer,
                timeline_plan,
                request.subtitle_settings->value()
            );
            if (!session_result.succeeded()) {
                return make_error(
                    session_result.error->message,
                    session_result.error->actionable_hint
                );
            }

            subtitle_session = std::move(session_result.session);
        }

        timeline::TimelineCompositionSummary timeline_summary{
            .segments = {},
            .output_video_time_base = timeline_plan.output_video_time_base,
            .output_frame_rate = timeline_plan.output_frame_rate,
            .output_audio_time_base = audio_output_plan.has_value()
                ? std::optional<Rational>(audio_output_plan->time_base)
                : std::nullopt
        };
        timeline_summary.segments.reserve(timeline_plan.segments.size());

        std::int64_t next_output_frame_index = 0;
        std::int64_t next_output_video_pts = 0;
        std::int64_t next_output_audio_pts = 0;
        std::int64_t decoded_video_frame_count = 0;
        std::int64_t decoded_audio_block_count = 0;
        std::int64_t subtitled_video_frame_count = 0;
        StreamingEncodeProgressEmitter progress_emitter(
            estimate_video_progress_totals(timeline_plan),
            request.progress_callback
        );

        for (const auto &segment_plan : timeline_plan.segments) {
            const auto segment_result = process_segment(
                timeline_plan,
                segment_plan,
                request.subtitle_settings != nullptr
                    ? *request.subtitle_settings
                    : std::optional<job::EncodeJobSubtitleSettings>{},
                subtitle_session.get(),
                video_output_plan,
                request.normalization_policy,
                request.queue_limits,
                audio_output_plan,
                output_session,
                progress_emitter,
                next_output_frame_index,
                next_output_video_pts,
                next_output_audio_pts
            );

            timeline_summary.segments.push_back(segment_result.segment_summary);
            decoded_video_frame_count += segment_result.decoded_video_frame_count;
            decoded_audio_block_count += segment_result.decoded_audio_block_count;
            subtitled_video_frame_count += segment_result.subtitled_video_frame_count;
        }

        timeline_summary.output_video_frame_count = decoded_video_frame_count;
        timeline_summary.output_audio_block_count = decoded_audio_block_count;
        timeline_summary.output_duration_microseconds =
            rescale_to_microseconds(next_output_video_pts, timeline_plan.output_video_time_base);
        auto encoded_media_summary = output_session.finish();
        progress_emitter.finish(
            static_cast<std::uint64_t>(encoded_media_summary.encoded_video_frame_count),
            timeline_summary.output_duration_microseconds
        );

        return StreamingTranscodeResult{
            .summary = StreamingTranscodeSummary{
                .timeline_summary = std::move(timeline_summary),
                .decoded_video_frame_count = decoded_video_frame_count,
                .decoded_audio_block_count = decoded_audio_block_count,
                .subtitled_video_frame_count = subtitled_video_frame_count,
                .encoded_media_summary = std::move(encoded_media_summary)
            }
        };
    } catch (const std::exception &exception) {
        return make_error(
            "Streaming transcode aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

}  // namespace

StreamingTranscodeResult StreamingTranscoder::transcode(const StreamingTranscodeRequest &request) noexcept {
    return transcode_impl(request);
}

}  // namespace utsure::core::media::streaming
