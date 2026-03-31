#include "utsure/core/media/media_encoder.hpp"

#include "ffmpeg_media_support.hpp"
#include "transcode_threading.hpp"
#include "utsure/core/media/media_inspector.hpp"

extern "C" {
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace utsure::core::media {

namespace {

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

        if (format_context->pb != nullptr && format_context->oformat != nullptr &&
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

struct EncoderSelection final {
    const char *encoder_name{""};
    const char *expected_codec_name{""};
};

struct VideoEncodePlan final {
    int width{0};
    int height{0};
    Rational time_base{};
    Rational average_frame_rate{};
    Rational sample_aspect_ratio{1, 1};
    std::int64_t first_output_pts{0};
    std::int64_t fallback_duration_pts{1};
};

MediaEncodeResult make_error(
    std::string output_path,
    std::string message,
    std::string actionable_hint
) {
    return MediaEncodeResult{
        .encoded_media_summary = std::nullopt,
        .error = MediaEncodeError{
            .output_path = std::move(output_path),
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

Rational derive_average_frame_rate(const Rational &time_base) {
    if (!time_base.is_valid() || time_base.numerator <= 0 || time_base.denominator <= 0) {
        return Rational{};
    }

    return Rational{
        .numerator = time_base.denominator,
        .denominator = time_base.numerator
    };
}

EncoderSelection select_encoder(const OutputVideoCodec codec) {
    switch (codec) {
    case OutputVideoCodec::h264:
        return EncoderSelection{
            .encoder_name = "libx264",
            .expected_codec_name = "h264"
        };
    case OutputVideoCodec::h265:
        return EncoderSelection{
            .encoder_name = "libx265",
            .expected_codec_name = "hevc"
        };
    default:
        throw std::runtime_error("Unsupported output video codec selection.");
    }
}

VideoEncodePlan build_video_encode_plan(const DecodedMediaSource &decoded_media_source) {
    if (!decoded_media_source.source_info.primary_video_stream.has_value()) {
        throw std::runtime_error("The decoded source does not expose a primary video stream.");
    }

    if (decoded_media_source.video_frames.empty()) {
        throw std::runtime_error("The decoded source does not contain any video frames to encode.");
    }

    const auto &source_video_stream = *decoded_media_source.source_info.primary_video_stream;
    const auto &first_frame = decoded_media_source.video_frames.front();

    if (first_frame.pixel_format != NormalizedVideoPixelFormat::rgba8 || first_frame.planes.size() != 1) {
        throw std::runtime_error("The encoder currently only supports rgba8 decoded frames with a single plane.");
    }

    if (first_frame.width <= 0 || first_frame.height <= 0) {
        throw std::runtime_error("The decoded frame stream does not expose a valid output resolution.");
    }

    if ((first_frame.width % 2) != 0 || (first_frame.height % 2) != 0) {
        throw std::runtime_error("The v1 software backends currently require even frame dimensions for yuv420p output.");
    }

    Rational time_base = source_video_stream.timestamps.time_base;
    if (!time_base.is_valid()) {
        time_base = first_frame.timestamp.source_time_base;
    }

    Rational average_frame_rate = source_video_stream.average_frame_rate;
    if (!average_frame_rate.is_valid()) {
        average_frame_rate = derive_average_frame_rate(time_base);
    }

    if (!time_base.is_valid()) {
        throw std::runtime_error("The decoded frame stream does not expose a usable source time base.");
    }

    const std::int64_t first_output_pts = first_frame.timestamp.source_pts.value_or(0);
    const std::int64_t fallback_duration_pts = first_frame.timestamp.source_duration.value_or(1);

    for (std::size_t index = 0; index < decoded_media_source.video_frames.size(); ++index) {
        const auto &frame = decoded_media_source.video_frames[index];
        if (frame.pixel_format != NormalizedVideoPixelFormat::rgba8 || frame.planes.size() != 1) {
            throw std::runtime_error("The decoded frame stream contains an unsupported video frame format.");
        }

        if (frame.width != first_frame.width || frame.height != first_frame.height) {
            throw std::runtime_error("The v1 software backends currently require a constant frame resolution.");
        }

        if (index > 0) {
            const auto &previous_frame = decoded_media_source.video_frames[index - 1];
            if (previous_frame.timestamp.source_pts.has_value() && frame.timestamp.source_pts.has_value() &&
                *frame.timestamp.source_pts <= *previous_frame.timestamp.source_pts) {
                throw std::runtime_error("The decoded frame timestamps must be strictly increasing for encode.");
            }
        }
    }

    return VideoEncodePlan{
        .width = first_frame.width,
        .height = first_frame.height,
        .time_base = time_base,
        .average_frame_rate = average_frame_rate,
        .sample_aspect_ratio = first_frame.sample_aspect_ratio,
        .first_output_pts = first_output_pts,
        .fallback_duration_pts = std::max<std::int64_t>(1, fallback_duration_pts)
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
    const VideoEncodePlan &plan
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
    codec_context->framerate = plan.average_frame_rate.is_valid()
        ? to_av_rational(plan.average_frame_rate)
        : av_inv_q(codec_context->time_base);
    codec_context->sample_aspect_ratio = to_av_rational(plan.sample_aspect_ratio);
    const auto logical_core_count = detail::effective_logical_core_count(detail::detect_logical_core_count());
    const auto threading = detail::choose_encoder_threading(request.threading, *encoder, logical_core_count);
    codec_context->thread_count = threading.thread_count;
    if (threading.thread_type != 0) {
        codec_context->thread_type = threading.thread_type;
    }

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

FrameHandle build_encoded_video_frame(
    const DecodedVideoFrame &decoded_frame,
    const VideoEncodePlan &plan,
    AVCodecContext &codec_context,
    SwsContextHandle &scale_context
) {
    if (!scale_context) {
        SwsContext *raw_scale_context = sws_getContext(
            plan.width,
            plan.height,
            AV_PIX_FMT_RGBA,
            plan.width,
            plan.height,
            codec_context.pix_fmt,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr
        );
        if (raw_scale_context == nullptr) {
            throw std::runtime_error("Failed to create the video encode scaling context.");
        }

        scale_context.reset(raw_scale_context);
    }

    auto encoded_frame = allocate_frame();
    encoded_frame->format = codec_context.pix_fmt;
    encoded_frame->width = codec_context.width;
    encoded_frame->height = codec_context.height;

    const auto buffer_result = av_frame_get_buffer(encoded_frame.get(), 1);
    if (buffer_result < 0) {
        throw std::runtime_error(
            "Failed to allocate the encoded video frame buffer. FFmpeg reported: " +
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
        scale_context.get(),
        source_data,
        source_linesize,
        0,
        plan.height,
        encoded_frame->data,
        encoded_frame->linesize
    );
    if (scale_result <= 0) {
        throw std::runtime_error("FFmpeg failed to convert the decoded frame into the encoder pixel format.");
    }

    const auto source_pts = decoded_frame.timestamp.source_pts.value_or(plan.first_output_pts);
    encoded_frame->pts = source_pts - plan.first_output_pts;
    encoded_frame->duration = decoded_frame.timestamp.source_duration.value_or(plan.fallback_duration_pts);

    return encoded_frame;
}

void write_available_packets(
    AVCodecContext &codec_context,
    AVFormatContext &output_context,
    AVStream &video_stream,
    AVPacket &packet
) {
    while (true) {
        const auto receive_result = avcodec_receive_packet(&codec_context, &packet);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
            return;
        }

        if (receive_result < 0) {
            throw std::runtime_error(
                "Failed to receive an encoded packet. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(receive_result)
            );
        }

        av_packet_rescale_ts(&packet, codec_context.time_base, video_stream.time_base);
        packet.stream_index = video_stream.index;

        const auto write_result = av_interleaved_write_frame(&output_context, &packet);
        av_packet_unref(&packet);
        if (write_result < 0) {
            throw std::runtime_error(
                "Failed to write an encoded packet to the output container. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(write_result)
            );
        }
    }
}

EncodedMediaSummary encode_video_output(
    const DecodedMediaSource &decoded_media_source,
    const MediaEncodeRequest &request
) {
    const auto output_path = request.output_path.lexically_normal();
    const auto output_path_string = output_path.string();
    if (output_path_string.empty()) {
        throw std::runtime_error("The output path must not be empty.");
    }

    const auto plan = build_video_encode_plan(decoded_media_source);
    auto output_context = create_output_context(output_path_string);

    AVStream *raw_video_stream = avformat_new_stream(output_context.get(), nullptr);
    if (raw_video_stream == nullptr) {
        throw std::runtime_error("Failed to create the output video stream.");
    }

    auto codec_context = create_video_encoder_context(*output_context, *raw_video_stream, request, plan);
    open_output_file(*output_context, output_path);

    const auto header_result = avformat_write_header(output_context.get(), nullptr);
    if (header_result < 0) {
        throw std::runtime_error(
            "Failed to write the output container header. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(header_result)
        );
    }

    PacketHandle packet = allocate_packet();
    SwsContextHandle scale_context{};

    for (const auto &decoded_frame : decoded_media_source.video_frames) {
        auto encoded_frame = build_encoded_video_frame(decoded_frame, plan, *codec_context, scale_context);

        const auto send_result = avcodec_send_frame(codec_context.get(), encoded_frame.get());
        if (send_result < 0) {
            throw std::runtime_error(
                "Failed to send a frame to the video encoder. FFmpeg reported: " +
                ffmpeg_support::ffmpeg_error_to_string(send_result)
            );
        }

        write_available_packets(*codec_context, *output_context, *raw_video_stream, *packet);
    }

    const auto flush_result = avcodec_send_frame(codec_context.get(), nullptr);
    if (flush_result < 0) {
        throw std::runtime_error(
            "Failed to flush the video encoder. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(flush_result)
        );
    }

    write_available_packets(*codec_context, *output_context, *raw_video_stream, *packet);

    const auto trailer_result = av_write_trailer(output_context.get());
    if (trailer_result < 0) {
        throw std::runtime_error(
            "Failed to finalize the output container. FFmpeg reported: " +
            ffmpeg_support::ffmpeg_error_to_string(trailer_result)
        );
    }

    if (output_context->pb != nullptr && (output_context->oformat->flags & AVFMT_NOFILE) == 0) {
        avio_closep(&output_context->pb);
    }

    const auto inspection_result = MediaInspector::inspect(output_path);
    if (!inspection_result.succeeded()) {
        throw std::runtime_error(
            "The encoded output could not be inspected after write. " + inspection_result.error->message +
            " Hint: " + inspection_result.error->actionable_hint
        );
    }

    return EncodedMediaSummary{
        .output_path = output_path,
        .video_settings = request.video_settings,
        .output_info = *inspection_result.media_source_info,
        .encoded_video_frame_count = static_cast<std::int64_t>(decoded_media_source.video_frames.size())
    };
}

}  // namespace

bool MediaEncodeResult::succeeded() const noexcept {
    return encoded_media_summary.has_value() && !error.has_value();
}

const char *to_string(const OutputVideoCodec codec) noexcept {
    switch (codec) {
    case OutputVideoCodec::h264:
        return "h264";
    case OutputVideoCodec::h265:
        return "h265";
    default:
        return "unknown";
    }
}

const char *to_string(const CpuUsageMode mode) noexcept {
    switch (mode) {
    case CpuUsageMode::auto_select:
        return "auto";
    case CpuUsageMode::conservative:
        return "conservative";
    case CpuUsageMode::aggressive:
        return "aggressive";
    default:
        return "unknown";
    }
}

int resolve_requested_ffmpeg_thread_count(
    const TranscodeThreadingSettings &settings,
    const std::uint32_t logical_core_count
) noexcept {
    return detail::resolve_requested_ffmpeg_thread_count_impl(
        settings.cpu_usage_mode,
        detail::effective_logical_core_count(settings.logical_core_count_override.value_or(logical_core_count))
    );
}

MediaEncodeResult MediaEncoder::encode(
    const DecodedMediaSource &decoded_media_source,
    const MediaEncodeRequest &request
) noexcept {
    try {
        return MediaEncodeResult{
            .encoded_media_summary = encode_video_output(decoded_media_source, request),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_error(
            request.output_path.string(),
            "Media encode aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

}  // namespace utsure::core::media
