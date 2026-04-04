#include "ffmpeg_media_support.hpp"
#include "utsure/core/media/audio_stream_selection.hpp"

extern "C" {
#include <libavcodec/codec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include <array>
#include <algorithm>
#include <string_view>

namespace utsure::core::media::ffmpeg_support {

namespace {

Rational normalize_positive_rational(const AVRational value, const Rational fallback = Rational{1, 1}) {
    if (value.num > 0 && value.den > 0) {
        return to_rational(value);
    }

    return fallback;
}

std::optional<std::string> optional_metadata_value(const AVDictionary *metadata, const char *key) {
    if (metadata == nullptr || key == nullptr) {
        return std::nullopt;
    }

    const auto *entry = av_dict_get(metadata, key, nullptr, 0);
    if (entry == nullptr || entry->value == nullptr) {
        return std::nullopt;
    }

    std::string value(entry->value);
    if (value.empty()) {
        return std::nullopt;
    }

    return value;
}

std::optional<std::string> optional_language_metadata(const AVStream &stream) {
    const auto language = optional_metadata_value(stream.metadata, "language");
    if (!language.has_value()) {
        return std::nullopt;
    }

    if (*language == "und" || *language == "UND") {
        return std::nullopt;
    }

    return language;
}

std::optional<std::string> optional_title_metadata(const AVStream &stream) {
    if (const auto title = optional_metadata_value(stream.metadata, "title")) {
        return title;
    }
    return optional_metadata_value(stream.metadata, "handler_name");
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
        .language_tag = optional_language_metadata(stream),
        .title = optional_title_metadata(stream),
        .disposition_default = (stream.disposition & AV_DISPOSITION_DEFAULT) != 0,
        .disposition_forced = (stream.disposition & AV_DISPOSITION_FORCED) != 0,
        .decoder_available = avcodec_find_decoder(parameters.codec_id) != nullptr,
        .timestamps = build_timestamp_info(stream),
        .frame_count = to_optional_pts(stream.nb_frames)
    };
}

MediaSourceInfo build_media_source_info(
    const std::filesystem::path &input_path,
    const AVFormatContext &format_context,
    const int primary_video_stream_index
) {
    MediaSourceInfo media_source_info{
        .input_name = input_path.lexically_normal().filename().string(),
        .container_format_name = format_context.iformat != nullptr ? format_context.iformat->name : "unknown",
        .container_duration_microseconds = to_optional_pts(format_context.duration),
        .primary_video_stream = std::nullopt,
        .audio_streams = {},
        .selected_audio_stream_index = std::nullopt,
        .primary_audio_stream = std::nullopt
    };

    if (primary_video_stream_index >= 0) {
        media_source_info.primary_video_stream =
            build_video_stream_info(*format_context.streams[primary_video_stream_index]);
    }

    media_source_info.audio_streams.reserve(format_context.nb_streams);
    for (unsigned int stream_index = 0; stream_index < format_context.nb_streams; ++stream_index) {
        const auto *stream = format_context.streams[stream_index];
        if (stream == nullptr || stream->codecpar == nullptr || stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        media_source_info.audio_streams.push_back(build_audio_stream_info(*stream));
    }

    media_source_info.selected_audio_stream_index = select_preferred_audio_stream_index(media_source_info.audio_streams);
    if (media_source_info.selected_audio_stream_index.has_value()) {
        const auto selected_stream = std::find_if(
            media_source_info.audio_streams.begin(),
            media_source_info.audio_streams.end(),
            [&](const AudioStreamInfo &audio_stream) {
                return audio_stream.stream_index == *media_source_info.selected_audio_stream_index;
            }
        );
        if (selected_stream != media_source_info.audio_streams.end()) {
            media_source_info.primary_audio_stream = *selected_stream;
        }
    }

    return media_source_info;
}

}  // namespace utsure::core::media::ffmpeg_support
