#include "utsure/core/media/media_decode_report.hpp"

#include <algorithm>
#include <optional>
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

void append_video_entry(std::ostringstream &report, const DecodedVideoFrame &frame) {
    report << "video.frame[" << frame.frame_index << "].source_pts="
           << format_optional_integer(frame.timestamp.source_pts) << '\n';
    report << "video.frame[" << frame.frame_index << "].source_duration="
           << format_optional_integer(frame.timestamp.source_duration) << '\n';
    report << "video.frame[" << frame.frame_index << "].origin=" << to_string(frame.timestamp.origin) << '\n';
    report << "video.frame[" << frame.frame_index << "].start_us=" << frame.timestamp.start_microseconds << '\n';
    report << "video.frame[" << frame.frame_index << "].duration_us="
           << format_optional_integer(frame.timestamp.duration_microseconds) << '\n';
    report << "video.frame[" << frame.frame_index << "].pixel_format=" << to_string(frame.pixel_format) << '\n';
    report << "video.frame[" << frame.frame_index << "].resolution=" << frame.width << 'x' << frame.height << '\n';
    report << "video.frame[" << frame.frame_index << "].planes=" << frame.planes.size() << '\n';

    if (!frame.planes.empty()) {
        const auto &plane = frame.planes.front();
        report << "video.frame[" << frame.frame_index << "].plane0.stride=" << plane.line_stride_bytes << '\n';
        report << "video.frame[" << frame.frame_index << "].plane0.bytes=" << plane.bytes.size() << '\n';
    }
}

void append_audio_entry(std::ostringstream &report, const DecodedAudioSamples &samples) {
    report << "audio.block[" << samples.block_index << "].source_pts="
           << format_optional_integer(samples.timestamp.source_pts) << '\n';
    report << "audio.block[" << samples.block_index << "].source_duration="
           << format_optional_integer(samples.timestamp.source_duration) << '\n';
    report << "audio.block[" << samples.block_index << "].origin=" << to_string(samples.timestamp.origin) << '\n';
    report << "audio.block[" << samples.block_index << "].start_us=" << samples.timestamp.start_microseconds << '\n';
    report << "audio.block[" << samples.block_index << "].duration_us="
           << format_optional_integer(samples.timestamp.duration_microseconds) << '\n';
    report << "audio.block[" << samples.block_index << "].sample_format=" << to_string(samples.sample_format) << '\n';
    report << "audio.block[" << samples.block_index << "].sample_rate=" << samples.sample_rate << '\n';
    report << "audio.block[" << samples.block_index << "].channels=" << samples.channel_count << '\n';
    report << "audio.block[" << samples.block_index << "].samples_per_channel=" << samples.samples_per_channel << '\n';
}

}  // namespace

std::string format_media_decode_report(
    const DecodedMediaSource &decoded_media_source,
    const MediaDecodeReportOptions &options
) {
    std::ostringstream report;

    report << "input.name=" << decoded_media_source.source_info.input_name << '\n';
    report << "policy.video_pixel_format="
           << to_string(decoded_media_source.normalization_policy.video_pixel_format) << '\n';
    report << "policy.audio_sample_format="
           << to_string(decoded_media_source.normalization_policy.audio_sample_format) << '\n';
    report << "policy.audio_block_samples=" << decoded_media_source.normalization_policy.audio_block_samples << '\n';

    if (decoded_media_source.source_info.primary_video_stream.has_value()) {
        report << "video.present=yes\n";
        report << "video.time_base="
               << format_rational(decoded_media_source.source_info.primary_video_stream->timestamps.time_base) << '\n';
        report << "video.average_frame_rate="
               << format_rational(decoded_media_source.source_info.primary_video_stream->average_frame_rate) << '\n';
        report << "video.frame_count=" << decoded_media_source.video_frames.size() << '\n';

        const auto representative_video_frames =
            std::min(options.representative_video_frames, decoded_media_source.video_frames.size());
        for (std::size_t index = 0; index < representative_video_frames; ++index) {
            append_video_entry(report, decoded_media_source.video_frames[index]);
        }
    } else {
        report << "video.present=no\n";
    }

    if (decoded_media_source.source_info.primary_audio_stream.has_value()) {
        report << "audio.present=yes\n";
        report << "audio.time_base="
               << format_rational(decoded_media_source.source_info.primary_audio_stream->timestamps.time_base) << '\n';
        report << "audio.sample_rate=" << decoded_media_source.source_info.primary_audio_stream->sample_rate << '\n';
        report << "audio.channels=" << decoded_media_source.source_info.primary_audio_stream->channel_count << '\n';
        report << "audio.block_count=" << decoded_media_source.audio_blocks.size() << '\n';

        std::int64_t total_audio_samples = 0;
        for (const auto &audio_block : decoded_media_source.audio_blocks) {
            total_audio_samples += audio_block.samples_per_channel;
        }

        report << "audio.total_samples_per_channel=" << total_audio_samples << '\n';

        const auto representative_audio_blocks =
            std::min(options.representative_audio_blocks, decoded_media_source.audio_blocks.size());
        for (std::size_t index = 0; index < representative_audio_blocks; ++index) {
            append_audio_entry(report, decoded_media_source.audio_blocks[index]);
        }
    } else {
        report << "audio.present=no\n";
    }

    return report.str();
}

}  // namespace utsure::core::media
