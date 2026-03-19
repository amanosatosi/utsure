#include "ffmpeg_media_support.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include <array>
#include <string_view>

namespace utsure::core::media::ffmpeg_support {

namespace {

Rational normalize_positive_rational(const AVRational value, const Rational fallback = Rational{1, 1}) {
    if (value.num > 0 && value.den > 0) {
        return to_rational(value);
    }

    return fallback;
}

}  // namespace

void FormatContextDeleter::operator()(AVFormatContext *format_context) const noexcept {
    if (format_context == nullptr) {
        return;
    }

    avformat_close_input(&format_context);
}

std::string ffmpeg_error_to_string(const int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error_code, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

Rational to_rational(const AVRational value) {
    return Rational{
        .numerator = static_cast<std::int64_t>(value.num),
        .denominator = static_cast<std::int64_t>(value.den)
    };
}

std::optional<std::int64_t> to_optional_pts(const std::int64_t value) {
    if (value == AV_NOPTS_VALUE) {
        return std::nullopt;
    }

    return value;
}

std::string codec_name_from_parameters(const AVCodecParameters &parameters) {
    const auto *codec_name = avcodec_get_name(parameters.codec_id);
    if (codec_name == nullptr || std::string_view(codec_name).empty()) {
        return "unknown";
    }

    return codec_name;
}

std::string pixel_format_name_from_parameters(const AVCodecParameters &parameters) {
    if (parameters.format < 0) {
        return "unknown";
    }

    const auto *pixel_format_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters.format));
    if (pixel_format_name == nullptr) {
        return "unknown";
    }

    return pixel_format_name;
}

std::string sample_format_name_from_parameters(const AVCodecParameters &parameters) {
    if (parameters.format < 0) {
        return "unknown";
    }

    const auto *sample_format_name = av_get_sample_fmt_name(static_cast<AVSampleFormat>(parameters.format));
    if (sample_format_name == nullptr) {
        return "unknown";
    }

    return sample_format_name;
}

std::string channel_layout_name_from_parameters(const AVCodecParameters &parameters) {
    return channel_layout_name_from_layout(parameters.ch_layout);
}

std::string channel_layout_name_from_layout(const AVChannelLayout &channel_layout) {
    if (channel_layout.nb_channels <= 0 || channel_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        return "unknown";
    }

    std::array<char, 128> buffer{};
    const auto describe_result = av_channel_layout_describe(&channel_layout, buffer.data(), buffer.size());
    if (describe_result < 0) {
        return "unknown";
    }

    return std::string(buffer.data());
}

TimestampInfo build_timestamp_info(const AVStream &stream) {
    return TimestampInfo{
        .time_base = to_rational(stream.time_base),
        .start_pts = to_optional_pts(stream.start_time),
        .duration_pts = to_optional_pts(stream.duration)
    };
}

VideoStreamInfo build_video_stream_info(const AVStream &stream) {
    const auto &parameters = *stream.codecpar;
    return VideoStreamInfo{
        .stream_index = stream.index,
        .codec_name = codec_name_from_parameters(parameters),
        .width = parameters.width,
        .height = parameters.height,
        .sample_aspect_ratio = normalize_positive_rational(stream.sample_aspect_ratio),
        .pixel_format_name = pixel_format_name_from_parameters(parameters),
        .average_frame_rate = to_rational(stream.avg_frame_rate),
        .timestamps = build_timestamp_info(stream),
        .frame_count = to_optional_pts(stream.nb_frames)
    };
}

AudioStreamInfo build_audio_stream_info(const AVStream &stream) {
    const auto &parameters = *stream.codecpar;
    return AudioStreamInfo{
        .stream_index = stream.index,
        .codec_name = codec_name_from_parameters(parameters),
        .sample_format_name = sample_format_name_from_parameters(parameters),
        .sample_rate = parameters.sample_rate,
        .channel_count = parameters.ch_layout.nb_channels,
        .channel_layout_name = channel_layout_name_from_parameters(parameters),
        .timestamps = build_timestamp_info(stream),
        .frame_count = to_optional_pts(stream.nb_frames)
    };
}

MediaSourceInfo build_media_source_info(
    const std::filesystem::path &input_path,
    const AVFormatContext &format_context,
    const int primary_video_stream_index,
    const int primary_audio_stream_index
) {
    MediaSourceInfo media_source_info{
        .input_name = input_path.lexically_normal().filename().string(),
        .container_format_name = format_context.iformat != nullptr ? format_context.iformat->name : "unknown",
        .container_duration_microseconds = to_optional_pts(format_context.duration),
        .primary_video_stream = std::nullopt,
        .primary_audio_stream = std::nullopt
    };

    if (primary_video_stream_index >= 0) {
        media_source_info.primary_video_stream =
            build_video_stream_info(*format_context.streams[primary_video_stream_index]);
    }

    if (primary_audio_stream_index >= 0) {
        media_source_info.primary_audio_stream =
            build_audio_stream_info(*format_context.streams[primary_audio_stream_index]);
    }

    return media_source_info;
}

}  // namespace utsure::core::media::ffmpeg_support
