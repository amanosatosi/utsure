#include "utsure/core/media/media_decoder.hpp"

#include "ffmpeg_media_support.hpp"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace utsure::core::media {

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

struct OpenStreamResources final {
    FormatContextHandle format_context{};
    CodecContextHandle codec_context{};
    AVStream *stream{nullptr};
};

struct VideoPreviewSession::Impl final {
    std::filesystem::path input_path{};
    std::string input_path_string{};
    DecodeNormalizationPolicy normalization_policy{};
    VideoStreamInfo video_stream_info{};
    OpenStreamResources resources{};
    PacketHandle packet{};
    FrameHandle decoded_frame{};
    SwsContextHandle scale_context{};
    int scale_width{0};
    int scale_height{0};
    AVPixelFormat scale_source_pixel_format{AV_PIX_FMT_NONE};
    std::int64_t fallback_duration_pts{1};
    std::int64_t next_fallback_source_pts{0};
    std::int64_t frame_index{0};
    bool drain_sent{false};
    bool exhausted{false};
};

struct TimestampSeed final {
    std::int64_t source_pts{0};
    TimestampOrigin origin{TimestampOrigin::stream_cursor};
};

MediaDecodeResult make_error(
    std::string input_path,
    std::string message,
    std::string actionable_hint
) {
    return MediaDecodeResult{
        .decoded_media_source = std::nullopt,
        .error = MediaDecodeError{
            .input_path = std::move(input_path),
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

VideoFrameDecodeResult make_video_frame_error(
    std::string input_path,
    std::string message,
    std::string actionable_hint
) {
    return VideoFrameDecodeResult{
        .video_frame = std::nullopt,
        .error = MediaDecodeError{
            .input_path = std::move(input_path),
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

VideoFrameWindowDecodeResult make_video_frame_window_error(
    std::string input_path,
    std::string message,
    std::string actionable_hint
) {
    return VideoFrameWindowDecodeResult{
        .video_frames = std::nullopt,
        .error = MediaDecodeError{
            .input_path = std::move(input_path),
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

VideoPreviewSessionCreateResult make_video_preview_session_error(
    std::string input_path,
    std::string message,
    std::string actionable_hint
) {
    return VideoPreviewSessionCreateResult{
        .session = nullptr,
        .error = MediaDecodeError{
            .input_path = std::move(input_path),
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

std::int64_t rescale_to_microseconds(const std::int64_t value, const Rational &time_base) {
    return av_rescale_q(value, to_av_rational(time_base), AV_TIME_BASE_Q);
}

bool rational_is_positive(const Rational &value) {
    return value.is_valid() && value.numerator > 0 && value.denominator > 0;
}

std::optional<std::int64_t> infer_video_frame_duration_pts(const VideoStreamInfo &video_stream_info) {
    if (!video_stream_info.timestamps.time_base.is_valid() ||
        !video_stream_info.average_frame_rate.is_valid() ||
        video_stream_info.average_frame_rate.numerator <= 0 ||
        video_stream_info.average_frame_rate.denominator <= 0) {
        return std::nullopt;
    }

    const AVRational frame_rate = to_av_rational(video_stream_info.average_frame_rate);
    const auto frame_duration_pts = av_rescale_q(1, av_inv_q(frame_rate), to_av_rational(video_stream_info.timestamps.time_base));
    if (frame_duration_pts <= 0) {
        return std::nullopt;
    }

    return frame_duration_pts;
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

OpenStreamResources open_stream_resources(
    const std::string &input_path_string,
    const int stream_index,
    const AVMediaType expected_media_type
) {
    auto format_context = open_format_context(input_path_string);

    if (stream_index < 0 || stream_index >= static_cast<int>(format_context->nb_streams)) {
        throw std::runtime_error(
            "The selected stream index " + std::to_string(stream_index) +
            " is not present in '" + input_path_string + "'."
        );
    }

    auto *stream = format_context->streams[stream_index];
    if (stream == nullptr || stream->codecpar == nullptr) {
        throw std::runtime_error(
            "The selected stream index " + std::to_string(stream_index) +
            " in '" + input_path_string + "' does not expose codec parameters."
        );
    }

    if (stream->codecpar->codec_type != expected_media_type) {
        throw std::runtime_error(
            "The selected stream index " + std::to_string(stream_index) +
            " in '" + input_path_string + "' is not the expected media type."
        );
    }

    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        throw std::runtime_error(
            "No decoder is available for stream index " + std::to_string(stream_index) +
            " in '" + input_path_string + "'."
        );
    }

    CodecContextHandle codec_context(avcodec_alloc_context3(decoder));
    if (!codec_context) {
        throw std::runtime_error("Failed to allocate an FFmpeg codec context.");
    }

    const auto parameters_result = avcodec_parameters_to_context(codec_context.get(), stream->codecpar);
    if (parameters_result < 0) {
        throw std::runtime_error(
            "Failed to copy codec parameters for stream index " + std::to_string(stream_index) +
            ". FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(parameters_result)
        );
    }

    codec_context->pkt_timebase = stream->time_base;

    const auto open_result = avcodec_open2(codec_context.get(), decoder, nullptr);
    if (open_result < 0) {
        throw std::runtime_error(
            "Failed to open the decoder for stream index " + std::to_string(stream_index) +
            ". FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(open_result)
        );
    }

    return OpenStreamResources{
        .format_context = std::move(format_context),
        .codec_context = std::move(codec_context),
        .stream = stream
    };
}

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

void send_packet_or_throw(AVCodecContext &codec_context, AVPacket *packet) {
    const auto send_result = avcodec_send_packet(&codec_context, packet);
    if (send_result < 0) {
        throw std::runtime_error(
            "Failed to send a packet to the decoder. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(send_result)
        );
    }
}

std::pair<int, int> choose_normalized_video_dimensions(
    const AVFrame &source_frame,
    const DecodeNormalizationPolicy &normalization_policy
) {
    if (source_frame.width <= 0 || source_frame.height <= 0) {
        throw std::runtime_error("The video decoder returned a frame without valid dimensions.");
    }

    const int max_width = normalization_policy.video_max_width;
    const int max_height = normalization_policy.video_max_height;
    if (max_width <= 0 && max_height <= 0) {
        return {source_frame.width, source_frame.height};
    }

    const double width_scale = max_width > 0
        ? static_cast<double>(max_width) / static_cast<double>(source_frame.width)
        : 1.0;
    const double height_scale = max_height > 0
        ? static_cast<double>(max_height) / static_cast<double>(source_frame.height)
        : 1.0;
    const double scale = std::min(width_scale, height_scale);
    if (scale >= 1.0) {
        return {source_frame.width, source_frame.height};
    }

    return {
        std::max(1, static_cast<int>(std::lround(static_cast<double>(source_frame.width) * scale))),
        std::max(1, static_cast<int>(std::lround(static_cast<double>(source_frame.height) * scale)))
    };
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

    const auto [target_width, target_height] = choose_normalized_video_dimensions(source_frame, normalization_policy);
    if (!scale_context ||
        scale_width != target_width ||
        scale_height != target_height ||
        scale_source_pixel_format != source_pixel_format) {
        SwsContext *raw_scale_context = sws_getContext(
            source_frame.width,
            source_frame.height,
            source_pixel_format,
            target_width,
            target_height,
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr
        );

        if (raw_scale_context == nullptr) {
            throw std::runtime_error("Failed to create an FFmpeg scaling context for video normalization.");
        }

        scale_context.reset(raw_scale_context);
        scale_width = target_width;
        scale_height = target_height;
        scale_source_pixel_format = source_pixel_format;
    }

    auto normalized_frame = allocate_frame();
    normalized_frame->format = AV_PIX_FMT_RGBA;
    normalized_frame->width = target_width;
    normalized_frame->height = target_height;

    const auto buffer_result = av_frame_get_buffer(normalized_frame.get(), 1);
    if (buffer_result < 0) {
        throw std::runtime_error(
            "Failed to allocate the normalized video frame buffer. FFmpeg reported: " +
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
        throw std::runtime_error("FFmpeg did not produce normalized video output for a decoded frame.");
    }

    VideoPlane plane{
        .line_stride_bytes = normalized_frame->linesize[0],
        .visible_width = target_width,
        .visible_height = target_height,
        .bytes = std::vector<std::uint8_t>(
            static_cast<std::size_t>(normalized_frame->linesize[0]) * static_cast<std::size_t>(target_height)
        )
    };

    for (int row = 0; row < target_height; ++row) {
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
        .width = target_width,
        .height = target_height,
        .sample_aspect_ratio = choose_sample_aspect_ratio(source_frame, stream),
        .pixel_format = normalization_policy.video_pixel_format,
        .planes = {std::move(plane)}
    };
}

void assign_video_durations(
    std::vector<DecodedVideoFrame> &video_frames,
    const Rational &time_base,
    const std::int64_t fallback_duration_pts
) {
    if (video_frames.empty()) {
        return;
    }

    for (std::size_t index = 0; index < video_frames.size(); ++index) {
        auto &frame = video_frames[index];

        std::int64_t duration_pts = fallback_duration_pts;
        if (index + 1 < video_frames.size() &&
            frame.timestamp.source_pts.has_value() &&
            video_frames[index + 1].timestamp.source_pts.has_value()) {
            const auto candidate_duration = *video_frames[index + 1].timestamp.source_pts - *frame.timestamp.source_pts;
            if (candidate_duration > 0) {
                duration_pts = candidate_duration;
            }
        }

        frame.timestamp.source_duration = duration_pts;
        frame.timestamp.duration_microseconds = rescale_to_microseconds(duration_pts, time_base);
    }
}

std::vector<DecodedVideoFrame> decode_video_frames(
    const std::string &input_path_string,
    const VideoStreamInfo &video_stream_info,
    const DecodeNormalizationPolicy &normalization_policy
) {
    OpenStreamResources resources = open_stream_resources(
        input_path_string,
        video_stream_info.stream_index,
        AVMEDIA_TYPE_VIDEO
    );

    if (normalization_policy.video_pixel_format != NormalizedVideoPixelFormat::rgba8) {
        throw std::runtime_error("Only rgba8 video normalization is implemented in this milestone.");
    }

    const auto fallback_duration_pts = infer_video_frame_duration_pts(video_stream_info).value_or(1);
    std::int64_t next_fallback_source_pts = video_stream_info.timestamps.start_pts.value_or(0);

    PacketHandle packet = allocate_packet();
    FrameHandle decoded_frame = allocate_frame();
    SwsContextHandle scale_context{};
    int scale_width = 0;
    int scale_height = 0;
    AVPixelFormat scale_source_pixel_format = AV_PIX_FMT_NONE;

    std::vector<DecodedVideoFrame> video_frames{};
    std::int64_t frame_index = 0;

    auto receive_frames = [&]() {
        while (true) {
            const auto receive_result = avcodec_receive_frame(resources.codec_context.get(), decoded_frame.get());
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive a decoded video frame. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            video_frames.push_back(normalize_video_frame(
                *decoded_frame,
                *resources.stream,
                video_stream_info.stream_index,
                frame_index,
                normalization_policy,
                scale_context,
                scale_width,
                scale_height,
                scale_source_pixel_format,
                next_fallback_source_pts,
                fallback_duration_pts
            ));

            ++frame_index;
            av_frame_unref(decoded_frame.get());
        }
    };

    while (av_read_frame(resources.format_context.get(), packet.get()) >= 0) {
        if (packet->stream_index == video_stream_info.stream_index) {
            send_packet_or_throw(*resources.codec_context, packet.get());
            receive_frames();
        }

        av_packet_unref(packet.get());
    }

    send_packet_or_throw(*resources.codec_context, nullptr);
    receive_frames();

    assign_video_durations(video_frames, video_stream_info.timestamps.time_base, fallback_duration_pts);
    return video_frames;
}

std::vector<DecodedVideoFrame> decode_video_frame_window_at_time_internal(
    const std::string &input_path_string,
    const VideoStreamInfo &video_stream_info,
    const DecodeNormalizationPolicy &normalization_policy,
    const std::int64_t requested_time_us,
    const std::size_t maximum_frame_count
) {
    OpenStreamResources resources = open_stream_resources(
        input_path_string,
        video_stream_info.stream_index,
        AVMEDIA_TYPE_VIDEO
    );

    if (normalization_policy.video_pixel_format != NormalizedVideoPixelFormat::rgba8) {
        throw std::runtime_error("Only rgba8 video normalization is implemented in this milestone.");
    }
    if (maximum_frame_count == 0) {
        throw std::runtime_error("Preview frame window decode requires at least one frame.");
    }

    const auto fallback_duration_pts = infer_video_frame_duration_pts(video_stream_info).value_or(1);
    const auto stream_time_base = to_av_rational(video_stream_info.timestamps.time_base);
    const auto normalized_requested_time_us = std::max<std::int64_t>(requested_time_us, 0);
    std::int64_t next_fallback_source_pts = video_stream_info.timestamps.start_pts.value_or(0);

    if (normalized_requested_time_us > 0 && video_stream_info.timestamps.time_base.is_valid()) {
        const auto requested_pts = av_rescale_q(normalized_requested_time_us, AV_TIME_BASE_Q, stream_time_base);
        const auto seek_result = av_seek_frame(
            resources.format_context.get(),
            video_stream_info.stream_index,
            requested_pts,
            AVSEEK_FLAG_BACKWARD
        );
        if (seek_result >= 0) {
            avcodec_flush_buffers(resources.codec_context.get());
            next_fallback_source_pts = requested_pts;
        }
    }

    PacketHandle packet = allocate_packet();
    FrameHandle decoded_frame = allocate_frame();
    SwsContextHandle scale_context{};
    int scale_width = 0;
    int scale_height = 0;
    AVPixelFormat scale_source_pixel_format = AV_PIX_FMT_NONE;

    std::optional<DecodedVideoFrame> frame_before_or_at{};
    std::vector<DecodedVideoFrame> preview_frames{};
    std::int64_t frame_index = 0;
    bool collecting_frames = false;

    auto receive_frames = [&]() {
        while (true) {
            const auto receive_result = avcodec_receive_frame(resources.codec_context.get(), decoded_frame.get());
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive a decoded preview video frame. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            auto normalized_frame = normalize_video_frame(
                *decoded_frame,
                *resources.stream,
                video_stream_info.stream_index,
                frame_index,
                normalization_policy,
                scale_context,
                scale_width,
                scale_height,
                scale_source_pixel_format,
                next_fallback_source_pts,
                fallback_duration_pts
            );
            ++frame_index;
            av_frame_unref(decoded_frame.get());

            if (collecting_frames) {
                preview_frames.push_back(std::move(normalized_frame));
                if (preview_frames.size() >= maximum_frame_count) {
                    return;
                }
                continue;
            }

            if (normalized_frame.timestamp.start_microseconds <= normalized_requested_time_us) {
                frame_before_or_at = std::move(normalized_frame);
                continue;
            }

            if (frame_before_or_at.has_value()) {
                const auto previous_distance =
                    std::llabs(normalized_requested_time_us - frame_before_or_at->timestamp.start_microseconds);
                const auto next_distance =
                    std::llabs(normalized_frame.timestamp.start_microseconds - normalized_requested_time_us);
                if (previous_distance <= next_distance) {
                    preview_frames.push_back(std::move(*frame_before_or_at));
                    if (preview_frames.size() >= maximum_frame_count) {
                        return;
                    }
                }
            }

            preview_frames.push_back(std::move(normalized_frame));
            collecting_frames = true;
            if (preview_frames.size() >= maximum_frame_count) {
                return;
            }
        }
    };

    while (av_read_frame(resources.format_context.get(), packet.get()) >= 0) {
        if (packet->stream_index == video_stream_info.stream_index) {
            send_packet_or_throw(*resources.codec_context, packet.get());
            receive_frames();
            av_packet_unref(packet.get());
            if (preview_frames.size() >= maximum_frame_count) {
                break;
            }
            continue;
        }

        av_packet_unref(packet.get());
    }

    send_packet_or_throw(*resources.codec_context, nullptr);
    receive_frames();

    if (preview_frames.empty() && frame_before_or_at.has_value()) {
        preview_frames.push_back(std::move(*frame_before_or_at));
    }

    if (preview_frames.empty()) {
        throw std::runtime_error("The selected source did not decode into any previewable video frames.");
    }

    assign_video_durations(preview_frames, video_stream_info.timestamps.time_base, fallback_duration_pts);
    return preview_frames;
}

std::vector<DecodedVideoFrame> decode_video_frame_window_from_current_position(
    VideoPreviewSession::Impl &session,
    const std::size_t maximum_frame_count
) {
    if (maximum_frame_count == 0) {
        throw std::runtime_error("Preview frame window decode requires at least one frame.");
    }
    if (session.exhausted) {
        return {};
    }

    std::vector<DecodedVideoFrame> preview_frames{};
    auto receive_frames = [&]() {
        while (preview_frames.size() < maximum_frame_count) {
            const auto receive_result = avcodec_receive_frame(
                session.resources.codec_context.get(),
                session.decoded_frame.get()
            );
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive a decoded preview video frame. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            preview_frames.push_back(normalize_video_frame(
                *session.decoded_frame,
                *session.resources.stream,
                session.video_stream_info.stream_index,
                session.frame_index,
                session.normalization_policy,
                session.scale_context,
                session.scale_width,
                session.scale_height,
                session.scale_source_pixel_format,
                session.next_fallback_source_pts,
                session.fallback_duration_pts
            ));
            ++session.frame_index;
            av_frame_unref(session.decoded_frame.get());
        }
    };

    receive_frames();
    while (preview_frames.size() < maximum_frame_count) {
        const auto read_result = av_read_frame(session.resources.format_context.get(), session.packet.get());
        if (read_result < 0) {
            break;
        }

        if (session.packet->stream_index == session.video_stream_info.stream_index) {
            send_packet_or_throw(*session.resources.codec_context, session.packet.get());
            receive_frames();
        }

        av_packet_unref(session.packet.get());
    }

    if (!session.drain_sent) {
        send_packet_or_throw(*session.resources.codec_context, nullptr);
        session.drain_sent = true;
        receive_frames();
    }

    if (preview_frames.empty()) {
        session.exhausted = true;
        return {};
    }

    assign_video_durations(
        preview_frames,
        session.video_stream_info.timestamps.time_base,
        session.fallback_duration_pts
    );
    return preview_frames;
}

std::vector<DecodedVideoFrame> seek_and_decode_video_frame_window(
    VideoPreviewSession::Impl &session,
    const std::int64_t requested_time_us,
    const std::size_t maximum_frame_count
) {
    if (maximum_frame_count == 0) {
        throw std::runtime_error("Preview frame window decode requires at least one frame.");
    }

    const auto stream_time_base = to_av_rational(session.video_stream_info.timestamps.time_base);
    const auto normalized_requested_time_us = std::max<std::int64_t>(requested_time_us, 0);
    if (session.video_stream_info.timestamps.time_base.is_valid()) {
        const auto requested_pts = normalized_requested_time_us > 0
            ? av_rescale_q(normalized_requested_time_us, AV_TIME_BASE_Q, stream_time_base)
            : session.video_stream_info.timestamps.start_pts.value_or(0);
        const auto seek_result = av_seek_frame(
            session.resources.format_context.get(),
            session.video_stream_info.stream_index,
            requested_pts,
            AVSEEK_FLAG_BACKWARD
        );
        if (seek_result < 0) {
            throw std::runtime_error(
                "Failed to seek preview media input '" + session.input_path_string + "'. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(seek_result)
            );
        }

        session.next_fallback_source_pts = requested_pts;
        avcodec_flush_buffers(session.resources.codec_context.get());
    } else {
        session.next_fallback_source_pts = session.video_stream_info.timestamps.start_pts.value_or(0);
    }

    session.frame_index = 0;
    session.drain_sent = false;
    session.exhausted = false;
    av_frame_unref(session.decoded_frame.get());
    av_packet_unref(session.packet.get());

    std::optional<DecodedVideoFrame> frame_before_or_at{};
    std::vector<DecodedVideoFrame> preview_frames{};
    bool collecting_frames = false;

    auto receive_frames = [&]() {
        while (true) {
            const auto receive_result = avcodec_receive_frame(
                session.resources.codec_context.get(),
                session.decoded_frame.get()
            );
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive a decoded preview video frame. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            auto normalized_frame = normalize_video_frame(
                *session.decoded_frame,
                *session.resources.stream,
                session.video_stream_info.stream_index,
                session.frame_index,
                session.normalization_policy,
                session.scale_context,
                session.scale_width,
                session.scale_height,
                session.scale_source_pixel_format,
                session.next_fallback_source_pts,
                session.fallback_duration_pts
            );
            ++session.frame_index;
            av_frame_unref(session.decoded_frame.get());

            if (collecting_frames) {
                preview_frames.push_back(std::move(normalized_frame));
                if (preview_frames.size() >= maximum_frame_count) {
                    return;
                }
                continue;
            }

            if (normalized_frame.timestamp.start_microseconds <= normalized_requested_time_us) {
                frame_before_or_at = std::move(normalized_frame);
                continue;
            }

            if (frame_before_or_at.has_value()) {
                const auto previous_distance =
                    std::llabs(normalized_requested_time_us - frame_before_or_at->timestamp.start_microseconds);
                const auto next_distance =
                    std::llabs(normalized_frame.timestamp.start_microseconds - normalized_requested_time_us);
                if (previous_distance <= next_distance) {
                    preview_frames.push_back(std::move(*frame_before_or_at));
                    if (preview_frames.size() >= maximum_frame_count) {
                        return;
                    }
                }
            }

            preview_frames.push_back(std::move(normalized_frame));
            collecting_frames = true;
            if (preview_frames.size() >= maximum_frame_count) {
                return;
            }
        }
    };

    while (av_read_frame(session.resources.format_context.get(), session.packet.get()) >= 0) {
        if (session.packet->stream_index == session.video_stream_info.stream_index) {
            send_packet_or_throw(*session.resources.codec_context, session.packet.get());
            receive_frames();
            av_packet_unref(session.packet.get());
            if (preview_frames.size() >= maximum_frame_count) {
                break;
            }
            continue;
        }

        av_packet_unref(session.packet.get());
    }

    if (!session.drain_sent) {
        send_packet_or_throw(*session.resources.codec_context, nullptr);
        session.drain_sent = true;
        receive_frames();
    }

    if (preview_frames.empty() && frame_before_or_at.has_value()) {
        preview_frames.push_back(std::move(*frame_before_or_at));
    }

    if (preview_frames.empty()) {
        session.exhausted = true;
        throw std::runtime_error("The selected source did not decode into any previewable video frames.");
    }

    assign_video_durations(
        preview_frames,
        session.video_stream_info.timestamps.time_base,
        session.fallback_duration_pts
    );
    return preview_frames;
}

std::vector<std::vector<float>> resample_audio_frame(
    SwrContext &resample_context,
    const AVFrame *decoded_frame,
    const int channel_count
) {
    const int input_samples = decoded_frame != nullptr ? decoded_frame->nb_samples : 0;
    const int output_capacity = swr_get_out_samples(&resample_context, input_samples);
    if (output_capacity < 0) {
        throw std::runtime_error("FFmpeg failed to estimate normalized audio output capacity.");
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
            "Failed to normalize decoded audio samples. FFmpeg reported: " +
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

void emit_ready_audio_blocks(
    std::vector<DecodedAudioSamples> &audio_blocks,
    std::vector<std::vector<float>> &pending_channels,
    const AudioStreamInfo &audio_stream_info,
    const DecodeNormalizationPolicy &normalization_policy,
    const std::int64_t first_output_source_pts,
    std::int64_t &samples_written
) {
    if (pending_channels.empty()) {
        return;
    }

    while (static_cast<int>(pending_channels.front().size()) >= normalization_policy.audio_block_samples) {
        std::vector<std::vector<float>> block_channels(
            pending_channels.size(),
            std::vector<float>(static_cast<std::size_t>(normalization_policy.audio_block_samples))
        );

        for (std::size_t channel_index = 0; channel_index < pending_channels.size(); ++channel_index) {
            auto &pending = pending_channels[channel_index];
            auto &block = block_channels[channel_index];
            std::copy_n(pending.begin(), normalization_policy.audio_block_samples, block.begin());
            pending.erase(pending.begin(), pending.begin() + normalization_policy.audio_block_samples);
        }

        audio_blocks.push_back(build_audio_block(
            audio_stream_info,
            static_cast<std::int64_t>(audio_blocks.size()),
            first_output_source_pts,
            samples_written,
            normalization_policy.audio_block_samples,
            block_channels
        ));

        samples_written += normalization_policy.audio_block_samples;
    }
}

std::vector<DecodedAudioSamples> decode_audio_blocks(
    const std::string &input_path_string,
    const AudioStreamInfo &audio_stream_info,
    const DecodeNormalizationPolicy &normalization_policy
) {
    OpenStreamResources resources = open_stream_resources(
        input_path_string,
        audio_stream_info.stream_index,
        AVMEDIA_TYPE_AUDIO
    );

    if (normalization_policy.audio_sample_format != NormalizedAudioSampleFormat::f32_planar) {
        throw std::runtime_error("Only f32_planar audio normalization is implemented in this milestone.");
    }

    if (normalization_policy.audio_block_samples <= 0) {
        throw std::runtime_error("The audio normalization policy must use a positive block size.");
    }

    if (audio_stream_info.sample_rate <= 0 || audio_stream_info.channel_count <= 0) {
        throw std::runtime_error("The selected audio stream does not expose a usable sample rate and channel count.");
    }

    SwrContext *raw_resample_context = nullptr;
    const auto resample_setup_result = swr_alloc_set_opts2(
        &raw_resample_context,
        &resources.codec_context->ch_layout,
        AV_SAMPLE_FMT_FLTP,
        audio_stream_info.sample_rate,
        &resources.codec_context->ch_layout,
        resources.codec_context->sample_fmt,
        audio_stream_info.sample_rate,
        0,
        nullptr
    );
    if (resample_setup_result < 0 || raw_resample_context == nullptr) {
        throw std::runtime_error(
            "Failed to configure the audio normalization context. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(resample_setup_result)
        );
    }

    SwrContextHandle resample_context(raw_resample_context);
    const auto resample_init_result = swr_init(resample_context.get());
    if (resample_init_result < 0) {
        throw std::runtime_error(
            "Failed to initialize the audio normalization context. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(resample_init_result)
        );
    }

    PacketHandle packet = allocate_packet();
    FrameHandle decoded_frame = allocate_frame();

    std::vector<DecodedAudioSamples> audio_blocks{};
    std::vector<std::vector<float>> pending_channels{};
    std::int64_t samples_written = 0;
    std::int64_t first_output_source_pts = audio_stream_info.timestamps.start_pts.value_or(0);
    bool first_output_source_pts_initialized = false;

    auto receive_frames = [&]() {
        while (true) {
            const auto receive_result = avcodec_receive_frame(resources.codec_context.get(), decoded_frame.get());
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }

            if (receive_result < 0) {
                throw std::runtime_error(
                    "Failed to receive decoded audio samples. FFmpeg reported: " +
                    ffmpeg_support::ffmpeg_error_to_string(receive_result)
                );
            }

            if (!first_output_source_pts_initialized) {
                const auto timestamp_seed = choose_timestamp_seed(*decoded_frame, first_output_source_pts);
                first_output_source_pts = timestamp_seed.source_pts;
                first_output_source_pts_initialized = true;
            }

            append_channel_samples(
                pending_channels,
                resample_audio_frame(*resample_context, decoded_frame.get(), audio_stream_info.channel_count)
            );

            emit_ready_audio_blocks(
                audio_blocks,
                pending_channels,
                audio_stream_info,
                normalization_policy,
                first_output_source_pts,
                samples_written
            );

            av_frame_unref(decoded_frame.get());
        }
    };

    while (av_read_frame(resources.format_context.get(), packet.get()) >= 0) {
        if (packet->stream_index == audio_stream_info.stream_index) {
            send_packet_or_throw(*resources.codec_context, packet.get());
            receive_frames();
        }

        av_packet_unref(packet.get());
    }

    send_packet_or_throw(*resources.codec_context, nullptr);
    receive_frames();

    while (true) {
        const auto flushed_channels = resample_audio_frame(*resample_context, nullptr, audio_stream_info.channel_count);
        if (flushed_channels.empty() || flushed_channels.front().empty()) {
            break;
        }

        append_channel_samples(pending_channels, flushed_channels);
        emit_ready_audio_blocks(
            audio_blocks,
            pending_channels,
            audio_stream_info,
            normalization_policy,
            first_output_source_pts,
            samples_written
        );
    }

    if (!pending_channels.empty() && !pending_channels.front().empty()) {
        const int tail_sample_count = static_cast<int>(pending_channels.front().size());
        audio_blocks.push_back(build_audio_block(
            audio_stream_info,
            static_cast<std::int64_t>(audio_blocks.size()),
            first_output_source_pts,
            samples_written,
            tail_sample_count,
            pending_channels
        ));
    }

    return audio_blocks;
}

}  // namespace

bool MediaDecodeResult::succeeded() const noexcept {
    return decoded_media_source.has_value() && !error.has_value();
}

bool VideoFrameDecodeResult::succeeded() const noexcept {
    return video_frame.has_value() && !error.has_value();
}

bool VideoFrameWindowDecodeResult::succeeded() const noexcept {
    return video_frames.has_value() && !error.has_value();
}

bool VideoPreviewSessionCreateResult::succeeded() const noexcept {
    return session != nullptr && !error.has_value();
}

MediaDecodeResult MediaDecoder::decode(
    const std::filesystem::path &input_path,
    const DecodeNormalizationPolicy &normalization_policy,
    const DecodeStreamSelection &stream_selection
) noexcept {
    try {
        const auto normalized_input_path = input_path.lexically_normal();
        const auto input_path_string = normalized_input_path.string();

        if (input_path_string.empty()) {
            return make_error(
                input_path_string,
                "Cannot decode media input because no file path was provided.",
                "Provide a path to a readable media file before starting decode."
            );
        }

        std::error_code filesystem_error;
        const bool input_exists = std::filesystem::exists(normalized_input_path, filesystem_error);
        if (filesystem_error) {
            return make_error(
                input_path_string,
                "Cannot decode media input '" + input_path_string + "': the file system could not be queried.",
                "The operating system reported: " + filesystem_error.message()
            );
        }

        if (!input_exists) {
            return make_error(
                input_path_string,
                "Cannot decode media input '" + input_path_string + "': the file does not exist.",
                "Check that the path is correct and that the file has been created before decode."
            );
        }

        auto format_context = open_format_context(input_path_string);

        const auto primary_video_stream_index = av_find_best_stream(
            format_context.get(),
            AVMEDIA_TYPE_VIDEO,
            -1,
            -1,
            nullptr,
            0
        );
        if (primary_video_stream_index < 0 && primary_video_stream_index != AVERROR_STREAM_NOT_FOUND) {
            return make_error(
                input_path_string,
                "Failed to select a primary video stream from '" + input_path_string + "'.",
                "FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(primary_video_stream_index)
            );
        }

        const auto primary_audio_stream_index = av_find_best_stream(
            format_context.get(),
            AVMEDIA_TYPE_AUDIO,
            -1,
            -1,
            nullptr,
            0
        );
        if (primary_audio_stream_index < 0 && primary_audio_stream_index != AVERROR_STREAM_NOT_FOUND) {
            return make_error(
                input_path_string,
                "Failed to select a primary audio stream from '" + input_path_string + "'.",
                "FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(primary_audio_stream_index)
            );
        }

        if (primary_video_stream_index == AVERROR_STREAM_NOT_FOUND &&
            primary_audio_stream_index == AVERROR_STREAM_NOT_FOUND) {
            return make_error(
                input_path_string,
                "No audio or video streams were found in '" + input_path_string + "'.",
                "Provide a media file that contains at least one decodable audio or video stream."
            );
        }

        DecodedMediaSource decoded_media_source{
            .source_info = ffmpeg_support::build_media_source_info(
                normalized_input_path,
                *format_context,
                primary_video_stream_index >= 0 ? primary_video_stream_index : -1,
                primary_audio_stream_index >= 0 ? primary_audio_stream_index : -1
            ),
            .normalization_policy = normalization_policy,
            .video_frames = {},
            .audio_blocks = {}
        };

        if (stream_selection.decode_video && decoded_media_source.source_info.primary_video_stream.has_value()) {
            decoded_media_source.video_frames = decode_video_frames(
                input_path_string,
                *decoded_media_source.source_info.primary_video_stream,
                normalization_policy
            );
        }

        if (stream_selection.decode_audio && decoded_media_source.source_info.primary_audio_stream.has_value()) {
            decoded_media_source.audio_blocks = decode_audio_blocks(
                input_path_string,
                *decoded_media_source.source_info.primary_audio_stream,
                normalization_policy
            );
        }

        return MediaDecodeResult{
            .decoded_media_source = std::move(decoded_media_source),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_error(
            input_path.string(),
            "Media decode aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

VideoFrameDecodeResult MediaDecoder::decode_video_frame_at_time(
    const std::filesystem::path &input_path,
    const std::int64_t requested_time_microseconds,
    const DecodeNormalizationPolicy &normalization_policy
) noexcept {
    try {
        const auto normalized_input_path = input_path.lexically_normal();
        const auto input_path_string = normalized_input_path.string();

        if (input_path_string.empty()) {
            return make_video_frame_error(
                input_path_string,
                "Cannot decode a preview frame because no file path was provided.",
                "Provide a path to a readable media file before requesting preview."
            );
        }

        std::error_code filesystem_error;
        const bool input_exists = std::filesystem::exists(normalized_input_path, filesystem_error);
        if (filesystem_error) {
            return make_video_frame_error(
                input_path_string,
                "Cannot decode preview media input '" + input_path_string + "': the file system could not be queried.",
                "The operating system reported: " + filesystem_error.message()
            );
        }

        if (!input_exists) {
            return make_video_frame_error(
                input_path_string,
                "Cannot decode preview media input '" + input_path_string + "': the file does not exist.",
                "Check that the path is correct and that the file has been created before requesting preview."
            );
        }

        auto format_context = open_format_context(input_path_string);
        const auto primary_video_stream_index = av_find_best_stream(
            format_context.get(),
            AVMEDIA_TYPE_VIDEO,
            -1,
            -1,
            nullptr,
            0
        );
        if (primary_video_stream_index < 0) {
            return make_video_frame_error(
                input_path_string,
                "No previewable video stream was found in '" + input_path_string + "'.",
                primary_video_stream_index == AVERROR_STREAM_NOT_FOUND
                    ? "Provide a media file that contains a decodable video stream before enabling Preview."
                    : "FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(primary_video_stream_index)
            );
        }

        const auto source_info = ffmpeg_support::build_media_source_info(
            normalized_input_path,
            *format_context,
            primary_video_stream_index,
            -1
        );
        if (!source_info.primary_video_stream.has_value()) {
            return make_video_frame_error(
                input_path_string,
                "The selected source does not expose a primary video stream for preview.",
                "Choose a source file with a decodable video stream before enabling Preview."
            );
        }

        const auto preview_frames = decode_video_frame_window_at_time_internal(
            input_path_string,
            *source_info.primary_video_stream,
            normalization_policy,
            requested_time_microseconds,
            1
        );

        return VideoFrameDecodeResult{
            .video_frame = preview_frames.front(),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_video_frame_error(
            input_path.string(),
            "Preview frame decode aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

VideoFrameWindowDecodeResult MediaDecoder::decode_video_frame_window_at_time(
    const std::filesystem::path &input_path,
    const std::int64_t requested_time_microseconds,
    const std::size_t maximum_frame_count,
    const DecodeNormalizationPolicy &normalization_policy
) noexcept {
    try {
        const auto normalized_input_path = input_path.lexically_normal();
        const auto input_path_string = normalized_input_path.string();

        if (input_path_string.empty()) {
            return make_video_frame_window_error(
                input_path_string,
                "Cannot decode a preview frame window because no file path was provided.",
                "Provide a path to a readable media file before requesting preview."
            );
        }

        std::error_code filesystem_error;
        const bool input_exists = std::filesystem::exists(normalized_input_path, filesystem_error);
        if (filesystem_error) {
            return make_video_frame_window_error(
                input_path_string,
                "Cannot decode preview media input '" + input_path_string + "': the file system could not be queried.",
                "The operating system reported: " + filesystem_error.message()
            );
        }

        if (!input_exists) {
            return make_video_frame_window_error(
                input_path_string,
                "Cannot decode preview media input '" + input_path_string + "': the file does not exist.",
                "Check that the path is correct and that the file has been created before requesting preview."
            );
        }

        auto format_context = open_format_context(input_path_string);
        const auto primary_video_stream_index = av_find_best_stream(
            format_context.get(),
            AVMEDIA_TYPE_VIDEO,
            -1,
            -1,
            nullptr,
            0
        );
        if (primary_video_stream_index < 0) {
            return make_video_frame_window_error(
                input_path_string,
                "No previewable video stream was found in '" + input_path_string + "'.",
                primary_video_stream_index == AVERROR_STREAM_NOT_FOUND
                    ? "Provide a media file that contains a decodable video stream before enabling Preview."
                    : "FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(primary_video_stream_index)
            );
        }

        const auto source_info = ffmpeg_support::build_media_source_info(
            normalized_input_path,
            *format_context,
            primary_video_stream_index,
            -1
        );
        if (!source_info.primary_video_stream.has_value()) {
            return make_video_frame_window_error(
                input_path_string,
                "The selected source does not expose a primary video stream for preview.",
                "Choose a source file with a decodable video stream before enabling Preview."
            );
        }

        return VideoFrameWindowDecodeResult{
            .video_frames = decode_video_frame_window_at_time_internal(
                input_path_string,
                *source_info.primary_video_stream,
                normalization_policy,
                requested_time_microseconds,
                maximum_frame_count
            ),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_video_frame_window_error(
            input_path.string(),
            "Preview frame window decode aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

VideoPreviewSessionCreateResult MediaDecoder::create_video_preview_session(
    const std::filesystem::path &input_path,
    const DecodeNormalizationPolicy &normalization_policy
) noexcept {
    try {
        const auto normalized_input_path = input_path.lexically_normal();
        const auto input_path_string = normalized_input_path.string();

        if (input_path_string.empty()) {
            return make_video_preview_session_error(
                input_path_string,
                "Cannot create a preview session because no file path was provided.",
                "Provide a path to a readable media file before requesting preview."
            );
        }

        std::error_code filesystem_error;
        const bool input_exists = std::filesystem::exists(normalized_input_path, filesystem_error);
        if (filesystem_error) {
            return make_video_preview_session_error(
                input_path_string,
                "Cannot create preview media input '" + input_path_string + "': the file system could not be queried.",
                "The operating system reported: " + filesystem_error.message()
            );
        }

        if (!input_exists) {
            return make_video_preview_session_error(
                input_path_string,
                "Cannot create preview media input '" + input_path_string + "': the file does not exist.",
                "Check that the path is correct and that the file has been created before requesting preview."
            );
        }

        auto format_context = open_format_context(input_path_string);
        const auto primary_video_stream_index = av_find_best_stream(
            format_context.get(),
            AVMEDIA_TYPE_VIDEO,
            -1,
            -1,
            nullptr,
            0
        );
        if (primary_video_stream_index < 0) {
            return make_video_preview_session_error(
                input_path_string,
                "No previewable video stream was found in '" + input_path_string + "'.",
                primary_video_stream_index == AVERROR_STREAM_NOT_FOUND
                    ? "Provide a media file that contains a decodable video stream before enabling Preview."
                    : "FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(primary_video_stream_index)
            );
        }

        const auto source_info = ffmpeg_support::build_media_source_info(
            normalized_input_path,
            *format_context,
            primary_video_stream_index,
            -1
        );
        if (!source_info.primary_video_stream.has_value()) {
            return make_video_preview_session_error(
                input_path_string,
                "The selected source does not expose a primary video stream for preview.",
                "Choose a source file with a decodable video stream before enabling Preview."
            );
        }

        auto impl = std::make_unique<VideoPreviewSession::Impl>();
        impl->input_path = normalized_input_path;
        impl->input_path_string = input_path_string;
        impl->normalization_policy = normalization_policy;
        impl->video_stream_info = *source_info.primary_video_stream;
        impl->resources = open_stream_resources(
            input_path_string,
            impl->video_stream_info.stream_index,
            AVMEDIA_TYPE_VIDEO
        );
        impl->packet = allocate_packet();
        impl->decoded_frame = allocate_frame();
        impl->fallback_duration_pts = infer_video_frame_duration_pts(impl->video_stream_info).value_or(1);
        impl->next_fallback_source_pts = impl->video_stream_info.timestamps.start_pts.value_or(0);

        return VideoPreviewSessionCreateResult{
            .session = std::unique_ptr<VideoPreviewSession>(new VideoPreviewSession(std::move(impl))),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_video_preview_session_error(
            input_path.string(),
            "Preview session creation aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

VideoPreviewSession::VideoPreviewSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

VideoPreviewSession::VideoPreviewSession(VideoPreviewSession &&) noexcept = default;

VideoPreviewSession &VideoPreviewSession::operator=(VideoPreviewSession &&) noexcept = default;

VideoPreviewSession::~VideoPreviewSession() = default;

VideoFrameWindowDecodeResult VideoPreviewSession::seek_and_decode_window_at_time(
    const std::int64_t requested_time_microseconds,
    const std::size_t maximum_frame_count
) noexcept {
    try {
        if (!impl_) {
            return make_video_frame_window_error(
                "",
                "The preview session is not initialized.",
                "Create a preview session before requesting preview frames."
            );
        }

        return VideoFrameWindowDecodeResult{
            .video_frames = seek_and_decode_video_frame_window(*impl_, requested_time_microseconds, maximum_frame_count),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_video_frame_window_error(
            impl_ != nullptr ? impl_->input_path_string : std::string{},
            "Preview frame window decode aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

VideoFrameWindowDecodeResult VideoPreviewSession::decode_next_window(const std::size_t maximum_frame_count) noexcept {
    try {
        if (!impl_) {
            return make_video_frame_window_error(
                "",
                "The preview session is not initialized.",
                "Create a preview session before requesting preview frames."
            );
        }

        const auto preview_frames = decode_video_frame_window_from_current_position(*impl_, maximum_frame_count);
        if (preview_frames.empty()) {
            return make_video_frame_window_error(
                impl_->input_path_string,
                "The preview session has no additional frames available.",
                "Seek to another position or choose a longer source clip."
            );
        }

        return VideoFrameWindowDecodeResult{
            .video_frames = preview_frames,
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_video_frame_window_error(
            impl_ != nullptr ? impl_->input_path_string : std::string{},
            "Preview frame window decode aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

}  // namespace utsure::core::media
