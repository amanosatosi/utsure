#include "streaming_transcode_pipeline.hpp"

#include "ffmpeg_media_support.hpp"
#include "utsure/core/media/media_inspector.hpp"

extern "C" {
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
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

constexpr std::uint64_t kEstimatedPacketSlotBytes = 512ULL * 1024ULL;

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
    int sample_rate{0};
    int channel_count{0};
    Rational time_base{};
    std::string channel_layout_name{"unknown"};
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

struct EncoderSelection final {
    const char *encoder_name{""};
};

struct EncodedPacketQueueEntry final {
    PacketHandle packet{};
    AVStream *stream{nullptr};
    Rational encoder_time_base{};
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
    const std::uint64_t rgba_surface_count =
        static_cast<std::uint64_t>(queue_limits.decoded_video_frame_queue_depth) +
        static_cast<std::uint64_t>(queue_limits.composited_video_frame_queue_depth) +
        2U;

    std::uint64_t rgba_surfaces_bytes = 0;
    if (!checked_mul_u64(*rgba_frame_bytes, rgba_surface_count, rgba_surfaces_bytes) ||
        !checked_add_u64(estimated_peak_bytes, rgba_surfaces_bytes, estimated_peak_bytes)) {
        return std::nullopt;
    }

    if (subtitles_present &&
        !checked_add_u64(estimated_peak_bytes, *rgba_frame_bytes, estimated_peak_bytes)) {
        return std::nullopt;
    }

    std::uint64_t encoder_frame_bytes = 0;
    if (!checked_mul_u64(*yuv420_frame_bytes, 2U, encoder_frame_bytes) ||
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

    std::uint64_t packet_slots = 0;
    if (!checked_add_u64(
            static_cast<std::uint64_t>(queue_limits.video_packet_queue_depth),
            static_cast<std::uint64_t>(queue_limits.audio_packet_queue_depth),
            packet_slots) ||
        !checked_add_u64(
            packet_slots,
            static_cast<std::uint64_t>(queue_limits.encoded_packet_queue_depth),
            packet_slots)) {
        return std::nullopt;
    }

    std::uint64_t packet_queue_reserve_bytes = 0;
    if (!checked_mul_u64(packet_slots, kEstimatedPacketSlotBytes, packet_queue_reserve_bytes) ||
        !checked_add_u64(estimated_peak_bytes, packet_queue_reserve_bytes, estimated_peak_bytes)) {
        return std::nullopt;
    }

    return PipelineMemoryBudget{
        .queue_limits = queue_limits,
        .normalized_rgba_frame_bytes = *rgba_frame_bytes,
        .encoder_yuv420_frame_bytes = *yuv420_frame_bytes,
        .normalized_audio_block_bytes = *audio_block_bytes,
        .packet_queue_reserve_bytes = packet_queue_reserve_bytes,
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

PacketHandle clone_packet(const AVPacket &packet) {
    PacketHandle cloned_packet(av_packet_clone(&packet));
    if (!cloned_packet) {
        throw std::runtime_error("Failed to clone an FFmpeg packet into a bounded pipeline queue.");
    }

    return cloned_packet;
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

SegmentDecoderResources open_segment_resources(const timeline::TimelineSegmentPlan &segment_plan) {
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
        audio_decoder = open_decoder_context(*audio_stream);
    }

    return SegmentDecoderResources{
        .format_context = std::move(format_context),
        .video_decoder = std::move(video_decoder),
        .video_stream = video_stream,
        .audio_decoder = std::move(audio_decoder),
        .audio_stream = audio_stream
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
            .source_time_base = ffmpeg_support::to_rational(stream.time_base),
            .source_pts = timestamp_seed.source_pts,
            .source_duration = std::nullopt,
            .origin = timestamp_seed.origin,
            .start_microseconds = rescale_to_microseconds(
                timestamp_seed.source_pts,
                ffmpeg_support::to_rational(stream.time_base)
            ),
            .duration_microseconds = std::nullopt
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
    const AudioStreamInfo &audio_stream_info,
    const std::int64_t block_index,
    const std::int64_t first_output_source_pts,
    const std::int64_t samples_written_before_block,
    const int block_sample_count,
    const std::vector<std::vector<float>> &block_channels
) {
    const AVRational sample_time_base{
        .num = 1,
        .den = audio_stream_info.sample_rate
    };
    const auto stream_time_base = to_av_rational(audio_stream_info.timestamps.time_base);
    const auto block_source_pts = first_output_source_pts +
        av_rescale_q(samples_written_before_block, sample_time_base, stream_time_base);
    const auto block_source_duration = av_rescale_q(block_sample_count, sample_time_base, stream_time_base);

    return DecodedAudioSamples{
        .stream_index = audio_stream_info.stream_index,
        .block_index = block_index,
        .timestamp = MediaTimestamp{
            .source_time_base = audio_stream_info.timestamps.time_base,
            .source_pts = block_source_pts,
            .source_duration = block_source_duration,
            .origin = TimestampOrigin::stream_cursor,
            .start_microseconds = rescale_to_microseconds(block_source_pts, audio_stream_info.timestamps.time_base),
            .duration_microseconds = rescale_to_microseconds(
                block_source_duration,
                audio_stream_info.timestamps.time_base
            )
        },
        .sample_rate = audio_stream_info.sample_rate,
        .channel_count = audio_stream_info.channel_count,
        .channel_layout_name = audio_stream_info.channel_layout_name,
        .sample_format = NormalizedAudioSampleFormat::f32_planar,
        .samples_per_channel = block_sample_count,
        .channel_samples = block_channels
    };
}

void composite_bitmap_into_frame(
    DecodedVideoFrame &video_frame,
    const subtitles::SubtitleBitmap &bitmap
) {
    if (bitmap.pixel_format != subtitles::SubtitleBitmapPixelFormat::rgba8_premultiplied) {
        throw std::runtime_error("Only premultiplied rgba8 subtitle bitmaps are supported for streaming burn-in.");
    }

    auto &plane = video_frame.planes.front();
    if (plane.line_stride_bytes <= 0 || bitmap.line_stride_bytes < (bitmap.width * 4)) {
        throw std::runtime_error("Streaming burn-in received an invalid plane or bitmap stride.");
    }

    const int clipped_left = std::max(0, bitmap.origin_x);
    const int clipped_top = std::max(0, bitmap.origin_y);
    const int clipped_right = std::min(video_frame.width, bitmap.origin_x + bitmap.width);
    const int clipped_bottom = std::min(video_frame.height, bitmap.origin_y + bitmap.height);
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
        return;
    }

    for (int destination_y = clipped_top; destination_y < clipped_bottom; ++destination_y) {
        const int source_y = destination_y - bitmap.origin_y;
        auto *destination_row = plane.bytes.data() +
            static_cast<std::size_t>(destination_y) * static_cast<std::size_t>(plane.line_stride_bytes);
        const auto *source_row = bitmap.bytes.data() +
            static_cast<std::size_t>(source_y) * static_cast<std::size_t>(bitmap.line_stride_bytes);

        for (int destination_x = clipped_left; destination_x < clipped_right; ++destination_x) {
            const int source_x = destination_x - bitmap.origin_x;
            const auto source_offset = static_cast<std::size_t>(source_x) * 4U;
            const auto destination_offset = static_cast<std::size_t>(destination_x) * 4U;

            const std::uint8_t source_alpha = source_row[source_offset + 3U];
            if (source_alpha == 0U) {
                continue;
            }

            const std::uint8_t inverse_alpha = static_cast<std::uint8_t>(255U - source_alpha);

            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const std::uint8_t source_value = source_row[source_offset + channel];
                const std::uint8_t destination_value = destination_row[destination_offset + channel];
                const std::uint16_t blended_value = static_cast<std::uint16_t>(source_value) +
                    static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(destination_value) * inverse_alpha + 127U) / 255U
                    );
                destination_row[destination_offset + channel] =
                    static_cast<std::uint8_t>(std::min<std::uint16_t>(255U, blended_value));
            }

            destination_row[destination_offset + 3U] = 255U;
        }
    }
}

bool is_rgba_frame_layout_supported(const DecodedVideoFrame &video_frame) {
    return video_frame.pixel_format == NormalizedVideoPixelFormat::rgba8 &&
        video_frame.planes.size() == 1 &&
        video_frame.width > 0 &&
        video_frame.height > 0;
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

    if (!is_rgba_frame_layout_supported(video_frame)) {
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
        composite_bitmap_into_frame(video_frame, bitmap);
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

AVSampleFormat choose_audio_encoder_sample_format(const AVCodec &encoder) {
    if (encoder.sample_fmts == nullptr) {
        return AV_SAMPLE_FMT_FLTP;
    }

    for (const AVSampleFormat *sample_format = encoder.sample_fmts;
         *sample_format != AV_SAMPLE_FMT_NONE;
         ++sample_format) {
        if (*sample_format == AV_SAMPLE_FMT_FLTP) {
            return *sample_format;
        }
    }

    throw std::runtime_error(
        "The default audio encoder does not support planar float audio input for the streaming pipeline."
    );
}

bool audio_encoder_supports_sample_rate(const AVCodec &encoder, const int sample_rate) {
    if (encoder.supported_samplerates == nullptr) {
        return true;
    }

    for (const int *supported_sample_rate = encoder.supported_samplerates;
         *supported_sample_rate != 0;
         ++supported_sample_rate) {
        if (*supported_sample_rate == sample_rate) {
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
    const AVCodec &encoder,
    const AVChannelLayout &requested_channel_layout
) {
    if (encoder.ch_layouts == nullptr) {
        return true;
    }

    for (const AVChannelLayout *supported_layout = encoder.ch_layouts;
         supported_layout->nb_channels != 0;
         ++supported_layout) {
        if (av_channel_layout_compare(supported_layout, &requested_channel_layout) == 0 ||
            supported_layout->nb_channels == requested_channel_layout.nb_channels) {
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

    if (!audio_encoder_supports_sample_rate(*encoder, plan.sample_rate)) {
        throw std::runtime_error(
            "The default AAC audio encoder does not support the required sample rate " +
            std::to_string(plan.sample_rate) + "."
        );
    }

    const AVSampleFormat sample_format = choose_audio_encoder_sample_format(*encoder);
    AVChannelLayout requested_channel_layout = make_default_audio_channel_layout(plan.channel_count);
    if (!audio_encoder_supports_channel_layout(*encoder, requested_channel_layout)) {
        av_channel_layout_uninit(&requested_channel_layout);
        throw std::runtime_error(
            "The default AAC audio encoder does not support the required channel count " +
            std::to_string(plan.channel_count) + "."
        );
    }

    CodecContextHandle codec_context(avcodec_alloc_context3(encoder));
    if (!codec_context) {
        av_channel_layout_uninit(&requested_channel_layout);
        throw std::runtime_error("Failed to allocate an FFmpeg audio encoder context.");
    }

    codec_context->codec_id = encoder->id;
    codec_context->codec_type = AVMEDIA_TYPE_AUDIO;
    codec_context->sample_fmt = sample_format;
    codec_context->sample_rate = plan.sample_rate;
    codec_context->time_base = to_av_rational(plan.time_base);
    codec_context->bit_rate = 128000;
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
        const std::optional<AudioOutputPlan> &audio_plan,
        const PipelineQueueLimits &queue_limits
    )
        : request_(request),
          video_plan_(video_plan),
          audio_plan_(audio_plan),
          encoded_packet_queue_(queue_limits.encoded_packet_queue_depth) {
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
            audio_codec_context_ = create_audio_encoder_context(*output_context_, *audio_stream_, *audio_plan_);
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
        return audio_codec_context_ != nullptr;
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
        drain_video_encoder_into_queue();
        drain_mux_queue();
    }

    void push_audio_block(const DecodedAudioSamples &audio_block) {
        if (!audio_codec_context_ || !audio_plan_.has_value() || audio_stream_ == nullptr) {
            throw std::runtime_error("The streaming output session was asked to encode audio without an audio stream.");
        }

        auto encoded_frame = build_encoded_audio_frame(audio_block);
        const auto send_result = avcodec_send_frame(audio_codec_context_.get(), encoded_frame.get());
        if (send_result < 0) {
            throw std::runtime_error(
                "Failed to send an audio block to the audio encoder. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(send_result)
            );
        }

        ++encoded_audio_block_count_;
        drain_audio_encoder_into_queue();
        drain_mux_queue();
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

            drain_video_encoder_into_queue();
            if (audio_codec_context_) {
                const auto flush_audio_result = avcodec_send_frame(audio_codec_context_.get(), nullptr);
                if (flush_audio_result < 0) {
                    throw std::runtime_error(
                        "Failed to flush the audio encoder. FFmpeg reported: " +
                        ffmpeg_support::ffmpeg_error_to_string(flush_audio_result)
                    );
                }

                drain_audio_encoder_into_queue();
            }
            drain_mux_queue();

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
            .output_info = *inspection_result.media_source_info,
            .encoded_video_frame_count = encoded_video_frame_count_
        };
    }

private:
    FrameHandle build_encoded_video_frame(const DecodedVideoFrame &decoded_frame) {
        if (!is_rgba_frame_layout_supported(decoded_frame)) {
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

        int frame_sample_count = audio_block.samples_per_channel;
        if (audio_codec_context_->frame_size > 0 && frame_sample_count != audio_codec_context_->frame_size) {
            if (frame_sample_count > audio_codec_context_->frame_size) {
                throw std::runtime_error(
                    "The streaming audio block size exceeds the audio encoder frame size and is not supported."
                );
            }

            if (!audio_encoder_supports_short_frame_samples(*audio_codec_context_)) {
                frame_sample_count = audio_codec_context_->frame_size;
            }
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
            std::copy_n(channel_samples.begin(), audio_block.samples_per_channel, destination);
            std::fill(destination + audio_block.samples_per_channel, destination + frame_sample_count, 0.0F);
        }

        encoded_frame->pts = audio_block.timestamp.source_pts.value_or(0);
        return encoded_frame;
    }

    void drain_video_encoder_into_queue() {
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

            PacketHandle owned_packet = allocate_packet();
            av_packet_move_ref(owned_packet.get(), reusable_video_receive_packet_.get());
            if (encoded_packet_queue_.full()) {
                drain_mux_queue();
            }

            encoded_packet_queue_.push(EncodedPacketQueueEntry{
                .packet = std::move(owned_packet),
                .stream = video_stream_,
                .encoder_time_base = video_plan_.time_base
            });
        }
    }

    void drain_audio_encoder_into_queue() {
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

            PacketHandle owned_packet = allocate_packet();
            av_packet_move_ref(owned_packet.get(), reusable_audio_receive_packet_.get());
            if (encoded_packet_queue_.full()) {
                drain_mux_queue();
            }

            encoded_packet_queue_.push(EncodedPacketQueueEntry{
                .packet = std::move(owned_packet),
                .stream = audio_stream_,
                .encoder_time_base = audio_plan_->time_base
            });
        }
    }

    void drain_mux_queue() {
        while (!encoded_packet_queue_.empty()) {
            EncodedPacketQueueEntry queue_entry = encoded_packet_queue_.pop();
            av_packet_rescale_ts(
                queue_entry.packet.get(),
                to_av_rational(queue_entry.encoder_time_base),
                queue_entry.stream->time_base
            );
            queue_entry.packet->stream_index = queue_entry.stream->index;

            const auto write_result = av_interleaved_write_frame(output_context_.get(), queue_entry.packet.get());
            if (write_result < 0) {
                throw std::runtime_error(
                    "Failed to mux an encoded streaming packet. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(write_result)
                );
            }
        }
    }

    MediaEncodeRequest request_{};
    VideoOutputPlan video_plan_{};
    std::optional<AudioOutputPlan> audio_plan_{};
    OutputFormatContextHandle output_context_{};
    CodecContextHandle video_codec_context_{};
    CodecContextHandle audio_codec_context_{};
    AVStream *video_stream_{nullptr};
    AVStream *audio_stream_{nullptr};
    PacketHandle reusable_video_receive_packet_{};
    PacketHandle reusable_audio_receive_packet_{};
    SwsContextHandle scale_context_{};
    BoundedQueue<EncodedPacketQueueEntry> encoded_packet_queue_;
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

std::optional<AudioOutputPlan> build_audio_output_plan(const timeline::TimelinePlan &timeline_plan) {
    if (!timeline_plan.output_audio_stream.has_value()) {
        return std::nullopt;
    }

    const auto &output_audio_stream = *timeline_plan.output_audio_stream;
    if (output_audio_stream.sample_rate <= 0 || output_audio_stream.channel_count <= 0) {
        throw std::runtime_error("The streaming pipeline requires a usable output audio sample rate and channel count.");
    }

    Rational time_base = output_audio_stream.timestamps.time_base;
    if (!rational_is_positive(time_base)) {
        time_base = Rational{
            .numerator = 1,
            .denominator = output_audio_stream.sample_rate
        };
    }

    if (!rational_is_positive(time_base)) {
        throw std::runtime_error("The streaming pipeline requires a usable output audio time base.");
    }

    return AudioOutputPlan{
        .sample_rate = output_audio_stream.sample_rate,
        .channel_count = output_audio_stream.channel_count,
        .time_base = time_base,
        .channel_layout_name = output_audio_stream.channel_layout_name
    };
}

DecodedAudioSamples make_output_audio_block(
    const AudioStreamInfo &output_audio_stream,
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
            static_cast<std::size_t>(output_audio_stream.channel_count),
            std::vector<float>(static_cast<std::size_t>(samples_per_channel), 0.0F)
        );
    } else {
        output_channels = channel_samples;
    }

    return DecodedAudioSamples{
        .stream_index = output_audio_stream.stream_index,
        .block_index = block_index,
        .timestamp = MediaTimestamp{
            .source_time_base = output_audio_stream.timestamps.time_base,
            .source_pts = output_pts,
            .source_duration = samples_per_channel,
            .origin = TimestampOrigin::stream_cursor,
            .start_microseconds = rescale_to_microseconds(output_pts, output_audio_stream.timestamps.time_base),
            .duration_microseconds = rescale_to_microseconds(
                samples_per_channel,
                output_audio_stream.timestamps.time_base
            )
        },
        .sample_rate = output_audio_stream.sample_rate,
        .channel_count = output_audio_stream.channel_count,
        .channel_layout_name = output_audio_stream.channel_layout_name,
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

void validate_video_frame_for_segment(
    const timeline::TimelineSegmentKind kind,
    const DecodedVideoFrame &frame,
    const VideoOutputPlan &video_output_plan,
    const Rational &output_video_time_base,
    std::optional<std::int64_t> &previous_pts_in_output_time_base
) {
    if (!is_rgba_frame_layout_supported(frame)) {
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

    const auto frame_duration = frame.timestamp.source_duration.value_or(0);
    if (frame_duration <= 0 || !rational_is_positive(frame.timestamp.source_time_base)) {
        throw std::runtime_error(
            "The " + std::string(timeline::to_string(kind)) +
            " segment contains a decoded frame with missing timing data."
        );
    }

    const auto converted_duration = rescale_value(
        frame_duration,
        frame.timestamp.source_time_base,
        output_video_time_base
    );
    if (converted_duration != video_output_plan.frame_duration_pts) {
        throw std::runtime_error(
            "The " + std::string(timeline::to_string(kind)) +
            " segment frame cadence does not match the main segment cadence."
        );
    }

    const auto current_pts = rescale_value(
        frame.timestamp.source_pts.value_or(0),
        frame.timestamp.source_time_base,
        output_video_time_base
    );
    if (previous_pts_in_output_time_base.has_value() &&
        (current_pts - *previous_pts_in_output_time_base) != video_output_plan.frame_duration_pts) {
        throw std::runtime_error(
            "The " + std::string(timeline::to_string(kind)) +
            " segment timestamps do not advance at the main segment cadence."
        );
    }

    previous_pts_in_output_time_base = current_pts;
}

SegmentProcessResult process_segment(
    const timeline::TimelinePlan &timeline_plan,
    const timeline::TimelineSegmentPlan &segment_plan,
    const std::optional<job::EncodeJobSubtitleSettings> &subtitle_settings,
    subtitles::SubtitleRenderSession *subtitle_session,
    const VideoOutputPlan &video_output_plan,
    const DecodeNormalizationPolicy &normalization_policy,
    const PipelineQueueLimits &queue_limits,
    StreamingOutputSession &output_session,
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

    SegmentDecoderResources resources = open_segment_resources(segment_plan);
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

    SwrContextHandle audio_resample_context{};
    std::vector<std::vector<float>> pending_audio_channels{};
    std::int64_t first_output_source_pts =
        segment_plan.inspected_source_info.primary_audio_stream.has_value()
            ? segment_plan.inspected_source_info.primary_audio_stream->timestamps.start_pts.value_or(0)
            : 0;
    bool first_output_source_pts_initialized = false;
    std::int64_t emitted_audio_samples = 0;
    std::int64_t decoded_segment_audio_samples = 0;
    std::int64_t emitted_segment_audio_samples = 0;
    std::optional<std::int64_t> previous_video_pts_in_output_time_base{};

    if (resources.audio_decoder) {
        if (normalization_policy.audio_sample_format != NormalizedAudioSampleFormat::f32_planar) {
            throw std::runtime_error("Only f32_planar audio normalization is implemented in the streaming pipeline.");
        }

        if (normalization_policy.audio_block_samples <= 0) {
            throw std::runtime_error("The audio normalization policy must use a positive block size.");
        }

        const auto &audio_stream_info = *segment_plan.inspected_source_info.primary_audio_stream;
        SwrContext *raw_resample_context = nullptr;
        const auto resample_setup_result = swr_alloc_set_opts2(
            &raw_resample_context,
            &resources.audio_decoder->ch_layout,
            AV_SAMPLE_FMT_FLTP,
            audio_stream_info.sample_rate,
            &resources.audio_decoder->ch_layout,
            resources.audio_decoder->sample_fmt,
            audio_stream_info.sample_rate,
            0,
            nullptr
        );
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

    BoundedQueue<PacketHandle> video_packet_queue(queue_limits.video_packet_queue_depth);
    BoundedQueue<PacketHandle> audio_packet_queue(queue_limits.audio_packet_queue_depth);
    BoundedQueue<DecodedVideoFrame> decoded_video_queue(queue_limits.decoded_video_frame_queue_depth);
    BoundedQueue<DecodedVideoFrame> composited_video_queue(queue_limits.composited_video_frame_queue_depth);
    BoundedQueue<DecodedAudioSamples> decoded_audio_queue(queue_limits.decoded_audio_block_queue_depth);

    const auto &main_video_stream =
        *timeline_plan.segments[timeline_plan.main_segment_index].inspected_source_info.primary_video_stream;

    const auto expected_segment_audio_samples_for_emitted_video = [&]() -> std::int64_t {
        if (!timeline_plan.output_audio_stream.has_value()) {
            return 0;
        }

        return rescale_value(
            result.segment_summary.video_frame_count * video_output_plan.frame_duration_pts,
            timeline_plan.output_video_time_base,
            timeline_plan.output_audio_stream->timestamps.time_base
        );
    };

    const auto emit_output_audio_block = [&](
        const DecodedAudioSamples &source_block,
        const int samples_to_emit,
        const bool silent
    ) {
        if (!timeline_plan.output_audio_stream.has_value() || samples_to_emit <= 0) {
            return;
        }

        const auto &output_audio_stream = *timeline_plan.output_audio_stream;
        const auto output_channels = silent
            ? std::vector<std::vector<float>>{}
            : copy_audio_block_prefix(source_block.channel_samples, samples_to_emit);
        auto output_block = make_output_audio_block(
            output_audio_stream,
            normalization_policy,
            result.segment_summary.audio_block_count,
            next_output_audio_pts,
            samples_to_emit,
            silent,
            output_channels
        );
        output_session.push_audio_block(output_block);
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

    const auto drain_composited_video_queue = [&]() {
        while (!composited_video_queue.empty()) {
            DecodedVideoFrame output_frame = composited_video_queue.pop();
            output_session.push_frame(output_frame);
        }
    };

    const auto drain_audio_queue = [&](const bool final_drain) {
        while (!decoded_audio_queue.empty()) {
            const auto &ready_block = decoded_audio_queue.front();
            const auto &output_audio_stream = *timeline_plan.output_audio_stream;
            if (ready_block.sample_format != normalization_policy.audio_sample_format ||
                ready_block.sample_rate != output_audio_stream.sample_rate ||
                ready_block.channel_count != output_audio_stream.channel_count) {
                throw std::runtime_error(
                    "The decoded " + std::string(timeline::to_string(segment_plan.kind)) +
                    " segment audio block shape does not match the main segment."
                );
            }

            const auto available_segment_samples =
                expected_segment_audio_samples_for_emitted_video() - emitted_segment_audio_samples;
            if (available_segment_samples <= 0) {
                if (!final_drain) {
                    break;
                }

                auto dropped_block = decoded_audio_queue.pop();
                decoded_segment_audio_samples += dropped_block.samples_per_channel;
                continue;
            }

            if (!final_drain &&
                ready_block.samples_per_channel > available_segment_samples) {
                break;
            }

            auto emitted_block = decoded_audio_queue.pop();
            decoded_segment_audio_samples += emitted_block.samples_per_channel;
            const int samples_to_emit = static_cast<int>(std::min<std::int64_t>(
                emitted_block.samples_per_channel,
                available_segment_samples
            ));
            emit_output_audio_block(emitted_block, samples_to_emit, false);
        }
    };

    const auto process_decoded_video_queue = [&]() {
        while (!decoded_video_queue.empty()) {
            DecodedVideoFrame video_frame = decoded_video_queue.pop();
            validate_video_frame_for_segment(
                segment_plan.kind,
                video_frame,
                video_output_plan,
                timeline_plan.output_video_time_base,
                previous_video_pts_in_output_time_base
            );

            std::int64_t subtitle_timestamp_microseconds = 0;
            if (segment_plan.subtitles_enabled && subtitle_settings.has_value()) {
                subtitle_timestamp_microseconds =
                    subtitle_settings->timing_mode == timeline::SubtitleTimingMode::full_output_timeline
                        ? rescale_to_microseconds(next_output_video_pts, timeline_plan.output_video_time_base)
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

            video_frame.stream_index = main_video_stream.stream_index;
            video_frame.frame_index = next_output_frame_index;
            video_frame.timestamp.source_time_base = timeline_plan.output_video_time_base;
            video_frame.timestamp.source_pts = next_output_video_pts;
            video_frame.timestamp.source_duration = video_output_plan.frame_duration_pts;
            video_frame.timestamp.origin = TimestampOrigin::stream_cursor;
            video_frame.timestamp.start_microseconds =
                rescale_to_microseconds(next_output_video_pts, timeline_plan.output_video_time_base);
            video_frame.timestamp.duration_microseconds = video_output_plan.frame_duration_microseconds;
            video_frame.sample_aspect_ratio = video_output_plan.sample_aspect_ratio;

            if (composited_video_queue.full()) {
                drain_composited_video_queue();
            }

            composited_video_queue.push(std::move(video_frame));
            ++result.segment_summary.video_frame_count;
            ++result.decoded_video_frame_count;
            ++next_output_frame_index;
            next_output_video_pts += video_output_plan.frame_duration_pts;
        }

        drain_composited_video_queue();
    };

    const auto stage_audio_block = [&](DecodedAudioSamples audio_block) {
        if (decoded_audio_queue.full()) {
            drain_audio_queue(false);
        }

        decoded_audio_queue.push(std::move(audio_block));
    };

    const auto emit_ready_audio_blocks = [&]() {
        if (pending_audio_channels.empty()) {
            return;
        }

        while (static_cast<int>(pending_audio_channels.front().size()) >= normalization_policy.audio_block_samples) {
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

            stage_audio_block(build_audio_block(
                *segment_plan.inspected_source_info.primary_audio_stream,
                result.segment_summary.audio_block_count + static_cast<std::int64_t>(decoded_audio_queue.size()),
                first_output_source_pts,
                emitted_audio_samples,
                normalization_policy.audio_block_samples,
                block_channels
            ));

            emitted_audio_samples += normalization_policy.audio_block_samples;
        }
    };

    const auto stage_video_frame = [&](DecodedVideoFrame video_frame) {
        if (decoded_video_queue.full()) {
            process_decoded_video_queue();
        }

        decoded_video_queue.push(std::move(video_frame));
        process_decoded_video_queue();
    };

    const auto receive_video_frames = [&]() {
        std::int64_t decoded_frame_index = result.decoded_video_frame_count;
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
                decoded_frame_index,
                normalization_policy,
                video_scale_context,
                scale_width,
                scale_height,
                scale_source_pixel_format,
                next_fallback_source_pts,
                fallback_video_duration_pts
            );
            normalized_frame.timestamp.source_duration = fallback_video_duration_pts;
            normalized_frame.timestamp.duration_microseconds =
                rescale_to_microseconds(fallback_video_duration_pts, normalized_frame.timestamp.source_time_base);
            stage_video_frame(std::move(normalized_frame));
            ++decoded_frame_index;
            av_frame_unref(decoded_video_frame.get());
        }
    };

    const auto receive_audio_frames = [&]() {
        if (!resources.audio_decoder) {
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

            if (!first_output_source_pts_initialized) {
                const auto timestamp_seed = choose_timestamp_seed(*decoded_audio_frame, first_output_source_pts);
                first_output_source_pts = timestamp_seed.source_pts;
                first_output_source_pts_initialized = true;
            }

            append_channel_samples(
                pending_audio_channels,
                resample_audio_frame(
                    *audio_resample_context,
                    decoded_audio_frame.get(),
                    segment_plan.inspected_source_info.primary_audio_stream->channel_count
                )
            );
            emit_ready_audio_blocks();

            av_frame_unref(decoded_audio_frame.get());
        }
    };

    while (av_read_frame(resources.format_context.get(), demux_packet.get()) >= 0) {
        if (demux_packet->stream_index == segment_plan.inspected_source_info.primary_video_stream->stream_index) {
            if (video_packet_queue.full()) {
                send_packet_or_throw(*resources.video_decoder, video_packet_queue.pop().get());
                receive_video_frames();
            }

            video_packet_queue.push(clone_packet(*demux_packet));
        } else if (resources.audio_decoder &&
                   demux_packet->stream_index == segment_plan.inspected_source_info.primary_audio_stream->stream_index) {
            if (audio_packet_queue.full()) {
                send_packet_or_throw(*resources.audio_decoder, audio_packet_queue.pop().get());
                receive_audio_frames();
            }

            audio_packet_queue.push(clone_packet(*demux_packet));
        }

        av_packet_unref(demux_packet.get());

        while (!video_packet_queue.empty()) {
            PacketHandle packet = video_packet_queue.pop();
            send_packet_or_throw(*resources.video_decoder, packet.get());
            receive_video_frames();
        }

        while (!audio_packet_queue.empty()) {
            PacketHandle packet = audio_packet_queue.pop();
            send_packet_or_throw(*resources.audio_decoder, packet.get());
            receive_audio_frames();
        }

        if (timeline_plan.output_audio_stream.has_value()) {
            drain_audio_queue(false);
        }
    }

    send_packet_or_throw(*resources.video_decoder, nullptr);
    receive_video_frames();

    if (resources.audio_decoder) {
        send_packet_or_throw(*resources.audio_decoder, nullptr);
        receive_audio_frames();

        while (true) {
            const auto flushed_channels = resample_audio_frame(
                *audio_resample_context,
                nullptr,
                segment_plan.inspected_source_info.primary_audio_stream->channel_count
            );
            if (flushed_channels.empty() || flushed_channels.front().empty()) {
                break;
            }

            append_channel_samples(pending_audio_channels, flushed_channels);
            emit_ready_audio_blocks();
        }

        if (!pending_audio_channels.empty() && !pending_audio_channels.front().empty()) {
            const int tail_sample_count = static_cast<int>(pending_audio_channels.front().size());
            stage_audio_block(build_audio_block(
                *segment_plan.inspected_source_info.primary_audio_stream,
                result.segment_summary.audio_block_count + static_cast<std::int64_t>(decoded_audio_queue.size()),
                first_output_source_pts,
                emitted_audio_samples,
                tail_sample_count,
                pending_audio_channels
            ));
        }
    }

    process_decoded_video_queue();
    if (timeline_plan.output_audio_stream.has_value()) {
        drain_audio_queue(true);
    }

    result.segment_summary.duration_microseconds = rescale_to_microseconds(
        result.segment_summary.video_frame_count * video_output_plan.frame_duration_pts,
        timeline_plan.output_video_time_base
    );

    if (timeline_plan.output_audio_stream.has_value()) {
        const auto expected_segment_samples = rescale_value(
            result.segment_summary.video_frame_count * video_output_plan.frame_duration_pts,
            timeline_plan.output_video_time_base,
            timeline_plan.output_audio_stream->timestamps.time_base
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
        const auto audio_output_plan = build_audio_output_plan(timeline_plan);
        StreamingOutputSession output_session(
            request.media_encode_request,
            video_output_plan,
            audio_output_plan,
            request.queue_limits
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
            .output_audio_time_base = timeline_plan.output_audio_stream.has_value()
                ? std::optional<Rational>(timeline_plan.output_audio_stream->timestamps.time_base)
                : std::nullopt
        };
        timeline_summary.segments.reserve(timeline_plan.segments.size());

        std::int64_t next_output_frame_index = 0;
        std::int64_t next_output_video_pts = 0;
        std::int64_t next_output_audio_pts = 0;
        std::int64_t decoded_video_frame_count = 0;
        std::int64_t decoded_audio_block_count = 0;
        std::int64_t subtitled_video_frame_count = 0;

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
                output_session,
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

        return StreamingTranscodeResult{
            .summary = StreamingTranscodeSummary{
                .timeline_summary = std::move(timeline_summary),
                .decoded_video_frame_count = decoded_video_frame_count,
                .decoded_audio_block_count = decoded_audio_block_count,
                .subtitled_video_frame_count = subtitled_video_frame_count,
                .encoded_media_summary = output_session.finish()
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
