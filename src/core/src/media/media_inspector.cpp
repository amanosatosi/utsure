#include "utsure/core/media/media_inspector.hpp"

#include "ffmpeg_media_support.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace utsure::core::media {

namespace {

using ffmpeg_support::FormatContextHandle;

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
                "FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(open_result)
            );
        }

        FormatContextHandle format_context(raw_format_context);

        const auto stream_info_result = avformat_find_stream_info(format_context.get(), nullptr);
        if (stream_info_result < 0) {
            return make_error(
                input_path_string,
                "Failed to read stream information from '" + input_path_string + "'.",
                "FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(stream_info_result)
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
                "FFmpeg reported: " + ffmpeg_support::ffmpeg_error_to_string(primary_video_stream_index)
            );
        }

        const auto media_source_info = ffmpeg_support::build_media_source_info(
            normalized_input_path,
            *format_context,
            primary_video_stream_index >= 0 ? primary_video_stream_index : -1
        );

        if (!media_source_info.primary_video_stream.has_value() &&
            !media_source_info.primary_audio_stream.has_value()) {
            return make_error(
                input_path_string,
                "No decodable audio or video streams were found in '" + input_path_string + "'.",
                "Provide a media file that contains at least one decodable audio or video stream."
            );
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
