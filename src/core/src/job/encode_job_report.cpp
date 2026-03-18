#include "utsure/core/job/encode_job_report.hpp"

#include "utsure/core/media/decoded_media.hpp"

#include <sstream>
#include <string>

namespace utsure::core::job {

namespace {

std::string format_path_leaf(const std::filesystem::path &path) {
    if (path.empty()) {
        return {};
    }

    const auto leaf = path.filename();
    if (!leaf.empty()) {
        return leaf.string();
    }

    return path.lexically_normal().string();
}

std::string format_rational(const media::Rational &value) {
    if (!value.is_valid()) {
        return "unknown";
    }

    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

}  // namespace

std::string format_encode_job_report(const EncodeJobSummary &encode_job_summary) {
    std::ostringstream report;

    report << "job.input.main_source=" << format_path_leaf(encode_job_summary.job.input.main_source_path) << '\n';
    report << "job.subtitles.present=" << (encode_job_summary.job.subtitles.has_value() ? "yes" : "no") << '\n';
    if (encode_job_summary.job.subtitles.has_value()) {
        report << "job.subtitles.path=" << format_path_leaf(encode_job_summary.job.subtitles->subtitle_path) << '\n';
        report << "job.subtitles.format_hint=" << encode_job_summary.job.subtitles->format_hint << '\n';
    }
    report << "job.output.path=" << format_path_leaf(encode_job_summary.job.output.output_path) << '\n';
    report << "job.output.video.codec=" << media::to_string(encode_job_summary.job.output.video.codec) << '\n';
    report << "job.output.video.preset=" << encode_job_summary.job.output.video.preset << '\n';
    report << "job.output.video.crf=" << encode_job_summary.job.output.video.crf << '\n';
    report << "decode.policy.video_pixel_format="
           << media::to_string(encode_job_summary.decode_normalization_policy.video_pixel_format) << '\n';
    report << "decode.policy.audio_sample_format="
           << media::to_string(encode_job_summary.decode_normalization_policy.audio_sample_format) << '\n';
    report << "decode.policy.audio_block_samples="
           << encode_job_summary.decode_normalization_policy.audio_block_samples << '\n';
    report << "input.container=" << encode_job_summary.inspected_input_info.container_format_name << '\n';
    report << "input.video.present="
           << (encode_job_summary.inspected_input_info.primary_video_stream.has_value() ? "yes" : "no") << '\n';

    if (encode_job_summary.inspected_input_info.primary_video_stream.has_value()) {
        const auto &input_video = *encode_job_summary.inspected_input_info.primary_video_stream;
        report << "input.video.codec=" << input_video.codec_name << '\n';
        report << "input.video.average_frame_rate=" << format_rational(input_video.average_frame_rate) << '\n';
        report << "input.video.time_base=" << format_rational(input_video.timestamps.time_base) << '\n';
    }

    report << "input.audio.present="
           << (encode_job_summary.inspected_input_info.primary_audio_stream.has_value() ? "yes" : "no") << '\n';

    if (encode_job_summary.inspected_input_info.primary_audio_stream.has_value()) {
        const auto &input_audio = *encode_job_summary.inspected_input_info.primary_audio_stream;
        report << "input.audio.codec=" << input_audio.codec_name << '\n';
        report << "input.audio.sample_rate=" << input_audio.sample_rate << '\n';
    }

    report << "decoded.video_frames=" << encode_job_summary.decoded_video_frame_count << '\n';
    report << "decoded.audio_blocks=" << encode_job_summary.decoded_audio_block_count << '\n';
    report << "subtitles.burned_video_frames=" << encode_job_summary.subtitled_video_frame_count << '\n';
    report << "output.container=" << encode_job_summary.encoded_media_summary.output_info.container_format_name << '\n';
    report << "output.encoded_video_frames=" << encode_job_summary.encoded_media_summary.encoded_video_frame_count
           << '\n';
    report << "output.video.present="
           << (encode_job_summary.encoded_media_summary.output_info.primary_video_stream.has_value() ? "yes" : "no")
           << '\n';

    if (encode_job_summary.encoded_media_summary.output_info.primary_video_stream.has_value()) {
        const auto &output_video = *encode_job_summary.encoded_media_summary.output_info.primary_video_stream;
        report << "output.video.codec=" << output_video.codec_name << '\n';
        report << "output.video.resolution=" << output_video.width << 'x' << output_video.height << '\n';
        report << "output.video.pixel_format=" << output_video.pixel_format_name << '\n';
        report << "output.video.average_frame_rate=" << format_rational(output_video.average_frame_rate) << '\n';
    }

    report << "output.audio.present="
           << (encode_job_summary.encoded_media_summary.output_info.primary_audio_stream.has_value() ? "yes" : "no");

    return report.str();
}

}  // namespace utsure::core::job
