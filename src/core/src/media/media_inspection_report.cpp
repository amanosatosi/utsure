#include "utsure/core/media/media_inspection_report.hpp"

#include <sstream>
#include <string>

namespace utsure::core::media {

namespace {

std::string format_rational(const Rational &value) {
    if (!value.is_valid()) {
        return "unknown";
    }

    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

std::string format_optional_integer(const std::optional<std::int64_t> &value) {
    if (!value.has_value()) {
        return "unknown";
    }

    return std::to_string(*value);
}

}  // namespace

std::string format_media_inspection_report(const MediaSourceInfo &media_source_info) {
    std::ostringstream report;

    report << "container.format=" << media_source_info.container_format_name << '\n';
    report << "container.duration_us=" << format_optional_integer(media_source_info.container_duration_microseconds) << '\n';

    if (media_source_info.primary_video_stream.has_value()) {
        const auto &video = *media_source_info.primary_video_stream;
        report << "video.present=yes\n";
        report << "video.stream_index=" << video.stream_index << '\n';
        report << "video.codec=" << video.codec_name << '\n';
        report << "video.resolution=" << video.width << 'x' << video.height << '\n';
        report << "video.pixel_format=" << video.pixel_format_name << '\n';
        report << "video.average_frame_rate=" << format_rational(video.average_frame_rate) << '\n';
        report << "video.time_base=" << format_rational(video.timestamps.time_base) << '\n';
        report << "video.start_pts=" << format_optional_integer(video.timestamps.start_pts) << '\n';
        report << "video.duration_pts=" << format_optional_integer(video.timestamps.duration_pts) << '\n';
        report << "video.frame_count=" << format_optional_integer(video.frame_count) << '\n';
    } else {
        report << "video.present=no\n";
    }

    if (media_source_info.primary_audio_stream.has_value()) {
        const auto &audio = *media_source_info.primary_audio_stream;
        report << "audio.present=yes\n";
        report << "audio.stream_index=" << audio.stream_index << '\n';
        report << "audio.codec=" << audio.codec_name << '\n';
        report << "audio.sample_format=" << audio.sample_format_name << '\n';
        report << "audio.sample_rate=" << audio.sample_rate << '\n';
        report << "audio.channels=" << audio.channel_count << '\n';
        report << "audio.channel_layout=" << audio.channel_layout_name << '\n';
        report << "audio.time_base=" << format_rational(audio.timestamps.time_base) << '\n';
        report << "audio.start_pts=" << format_optional_integer(audio.timestamps.start_pts) << '\n';
        report << "audio.duration_pts=" << format_optional_integer(audio.timestamps.duration_pts) << '\n';
        report << "audio.frame_count=" << format_optional_integer(audio.frame_count);
    } else {
        report << "audio.present=no";
    }

    return report.str();
}

}  // namespace utsure::core::media
