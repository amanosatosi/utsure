#include "utsure/core/media/media_inspector.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include <array>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace utsure::core::media {

namespace {

using FormatContextHandle = std::unique_ptr<AVFormatContext, decltype(&avformat_close_input)>;

std::string ffmpeg_error_to_string(int error_code) {
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
    if (parameters.ch_layout.nb_channels <= 0 || parameters.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        return "unknown";
    }

    std::array<char, 128> buffer{};
    const auto describe_result = av_channel_layout_describe(&parameters.ch_layout, buffer.data(), buffer.size());
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

MediaInspectionResult make_error(
    std::string input_path,
    std::string message,
    std::string actionable_hint
) {
    return MediaInspectionResult{
        .media_source_info = std::nullopt,
        .error = MediaInspectionError{
            .input_path = std::move(input_path),
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

}  // namespace

bool Rational::is_valid() const noexcept {
    return denominator != 0;
}

bool MediaInspectionResult::succeeded() const noexcept {
    return media_source_info.has_value() && !error.has_value();
}

MediaInspectionResult MediaInspector::inspect(const std::filesystem::path &input_path) noexcept {
    try {
        const auto normalized_input_path = input_path.lexically_normal();
        const auto input_path_string = normalized_input_path.string();

        if (input_path_string.empty()) {
            return make_error(
                input_path_string,
                "Cannot inspect media input because no file path was provided.",
                "Provide a path to a readable media file before starting inspection."
            );
        }

        std::error_code filesystem_error;
        const bool input_exists = std::filesystem::exists(normalized_input_path, filesystem_error);
        if (filesystem_error) {
            return make_error(
                input_path_string,
                "Cannot inspect media input '" + input_path_string + "': the file system could not be queried.",
                "The operating system reported: " + filesystem_error.message()
            );
        }

        if (!input_exists) {
            return make_error(
                input_path_string,
                "Cannot inspect media input '" + input_path_string + "': the file does not exist.",
                "Check that the path is correct and that the file has been created before inspection."
            );
        }

        AVFormatContext *raw_format_context = nullptr;
        const auto open_result = avformat_open_input(&raw_format_context, input_path_string.c_str(), nullptr, nullptr);
        if (open_result < 0) {
            return make_error(
                input_path_string,
                "Failed to open media input '" + input_path_string + "'.",
                "FFmpeg reported: " + ffmpeg_error_to_string(open_result)
            );
        }

        FormatContextHandle format_context(raw_format_context, &avformat_close_input);

        const auto stream_info_result = avformat_find_stream_info(format_context.get(), nullptr);
        if (stream_info_result < 0) {
            return make_error(
                input_path_string,
                "Failed to read stream information from '" + input_path_string + "'.",
                "FFmpeg reported: " + ffmpeg_error_to_string(stream_info_result)
            );
        }

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
                "FFmpeg reported: " + ffmpeg_error_to_string(primary_video_stream_index)
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
                "FFmpeg reported: " + ffmpeg_error_to_string(primary_audio_stream_index)
            );
        }

        if (primary_video_stream_index == AVERROR_STREAM_NOT_FOUND && primary_audio_stream_index == AVERROR_STREAM_NOT_FOUND) {
            return make_error(
                input_path_string,
                "No audio or video streams were found in '" + input_path_string + "'.",
                "Provide a media file that contains at least one decodable audio or video stream."
            );
        }

        MediaSourceInfo media_source_info{
            .input_name = normalized_input_path.filename().string(),
            .container_format_name = format_context->iformat != nullptr ? format_context->iformat->name : "unknown",
            .container_duration_microseconds = to_optional_pts(format_context->duration),
            .primary_video_stream = std::nullopt,
            .primary_audio_stream = std::nullopt
        };

        if (primary_video_stream_index >= 0) {
            media_source_info.primary_video_stream =
                build_video_stream_info(*format_context->streams[primary_video_stream_index]);
        }

        if (primary_audio_stream_index >= 0) {
            media_source_info.primary_audio_stream =
                build_audio_stream_info(*format_context->streams[primary_audio_stream_index]);
        }

        return MediaInspectionResult{
            .media_source_info = std::move(media_source_info),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_error(
            input_path.string(),
            "Media inspection aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

}  // namespace utsure::core::media
