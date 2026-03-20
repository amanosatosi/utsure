#include "utsure/core/job/encode_job_report.hpp"

#include "../media/streaming_transcode_pipeline.hpp"
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

std::string format_optional_path_leaf(const std::optional<std::filesystem::path> &path) {
    if (!path.has_value()) {
        return {};
    }

    return format_path_leaf(*path);
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

    report << "job.input.intro.present=" << (encode_job_summary.job.input.intro_source_path.has_value() ? "yes" : "no") << '\n';
    if (encode_job_summary.job.input.intro_source_path.has_value()) {
        report << "job.input.intro=" << format_optional_path_leaf(encode_job_summary.job.input.intro_source_path) << '\n';
    }
    report << "job.input.main_source=" << format_path_leaf(encode_job_summary.job.input.main_source_path) << '\n';
    report << "job.input.outro.present=" << (encode_job_summary.job.input.outro_source_path.has_value() ? "yes" : "no") << '\n';
    if (encode_job_summary.job.input.outro_source_path.has_value()) {
        report << "job.input.outro=" << format_optional_path_leaf(encode_job_summary.job.input.outro_source_path) << '\n';
    }
    report << "job.subtitles.present=" << (encode_job_summary.job.subtitles.has_value() ? "yes" : "no") << '\n';
    if (encode_job_summary.job.subtitles.has_value()) {
        report << "job.subtitles.path=" << format_path_leaf(encode_job_summary.job.subtitles->subtitle_path) << '\n';
        report << "job.subtitles.format_hint=" << encode_job_summary.job.subtitles->format_hint << '\n';
        report << "job.subtitles.timing_mode=" << timeline::to_string(encode_job_summary.job.subtitles->timing_mode) << '\n';
    }
    report << "job.output.path=" << format_path_leaf(encode_job_summary.job.output.output_path) << '\n';
    report << "job.output.video.codec=" << media::to_string(encode_job_summary.job.output.video.codec) << '\n';
    report << "job.output.video.preset=" << encode_job_summary.job.output.video.preset << '\n';
    report << "job.output.video.crf=" << encode_job_summary.job.output.video.crf << '\n';
    report << "job.output.audio.mode=" << media::to_string(encode_job_summary.job.output.audio.mode) << '\n';
    report << "job.output.audio.codec=" << media::to_string(encode_job_summary.job.output.audio.codec) << '\n';
    report << "job.output.audio.bitrate_kbps=" << encode_job_summary.job.output.audio.bitrate_kbps << '\n';
    report << "job.output.audio.sample_rate="
           << (encode_job_summary.job.output.audio.sample_rate_hz.has_value()
                   ? std::to_string(*encode_job_summary.job.output.audio.sample_rate_hz)
                   : std::string("auto"))
           << '\n';
    report << "job.output.audio.channels="
           << (encode_job_summary.job.output.audio.channel_count.has_value()
                   ? std::to_string(*encode_job_summary.job.output.audio.channel_count)
                   : std::string("auto"))
           << '\n';
    report << "job.execution.priority=" << to_string(encode_job_summary.job.execution.process_priority) << '\n';
    report << "decode.policy.video_pixel_format="
           << media::to_string(encode_job_summary.decode_normalization_policy.video_pixel_format) << '\n';
    report << "decode.policy.audio_sample_format="
           << media::to_string(encode_job_summary.decode_normalization_policy.audio_sample_format) << '\n';
    report << "decode.policy.audio_block_samples="
           << encode_job_summary.decode_normalization_policy.audio_block_samples << '\n';
    const auto runtime_behavior = media::streaming::resolve_streaming_runtime_behavior();
    report << "streaming.encoder_threads="
           << media::streaming::format_encoder_threading_summary(
                  runtime_behavior,
                  encode_job_summary.job.output.video.codec
              )
           << '\n';
    report << "streaming.video_queue_frames=" << runtime_behavior.video_frame_queue_depth << '\n';
    report << "streaming.audio_queue_blocks=" << runtime_behavior.decoded_audio_block_queue_depth << '\n';
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
    report << "timeline.segment_count=" << encode_job_summary.timeline_summary.segments.size() << '\n';
    report << "timeline.output.video_time_base="
           << format_rational(encode_job_summary.timeline_summary.output_video_time_base) << '\n';
    report << "timeline.output.frame_rate="
           << format_rational(encode_job_summary.timeline_summary.output_frame_rate) << '\n';
    report << "timeline.output.audio.present="
           << (encode_job_summary.timeline_summary.output_audio_time_base.has_value() ? "yes" : "no") << '\n';
    if (encode_job_summary.timeline_summary.output_audio_time_base.has_value()) {
        report << "timeline.output.audio_time_base="
               << format_rational(*encode_job_summary.timeline_summary.output_audio_time_base) << '\n';
    }
    report << "subtitles.burned_video_frames=" << encode_job_summary.subtitled_video_frame_count << '\n';
    report << "output.container=" << encode_job_summary.encoded_media_summary.output_info.container_format_name << '\n';
    report << "output.audio.requested_mode="
           << media::to_string(encode_job_summary.encoded_media_summary.resolved_audio_output.requested_mode) << '\n';
    report << "output.audio.resolved_mode="
           << media::to_string(encode_job_summary.encoded_media_summary.resolved_audio_output.resolved_mode) << '\n';
    report << "output.audio.decision="
           << encode_job_summary.encoded_media_summary.resolved_audio_output.decision_summary << '\n';
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
    if (encode_job_summary.encoded_media_summary.output_info.primary_audio_stream.has_value()) {
        const auto &output_audio = *encode_job_summary.encoded_media_summary.output_info.primary_audio_stream;
        report << '\n';
        report << "output.audio.codec=" << output_audio.codec_name << '\n';
        report << "output.audio.sample_rate=" << output_audio.sample_rate << '\n';
        report << "output.audio.channels=" << output_audio.channel_count << '\n';
        report << "output.audio.channel_layout=" << output_audio.channel_layout_name;
    }

    for (std::size_t index = 0; index < encode_job_summary.timeline_summary.segments.size(); ++index) {
        const auto &segment = encode_job_summary.timeline_summary.segments[index];
        report << '\n';
        report << "timeline.segment[" << index << "].kind=" << timeline::to_string(segment.kind) << '\n';
        report << "timeline.segment[" << index << "].path=" << format_path_leaf(segment.source_path) << '\n';
        report << "timeline.segment[" << index << "].start_us=" << segment.start_microseconds << '\n';
        report << "timeline.segment[" << index << "].duration_us=" << segment.duration_microseconds << '\n';
        report << "timeline.segment[" << index << "].video_frames=" << segment.video_frame_count << '\n';
        report << "timeline.segment[" << index << "].audio_blocks=" << segment.audio_block_count << '\n';
        report << "timeline.segment[" << index << "].subtitles="
               << (segment.subtitles_enabled ? "yes" : "no") << '\n';
        report << "timeline.segment[" << index << "].inserted_silence="
               << (segment.inserted_silence ? "yes" : "no");
    }

    return report.str();
}

}  // namespace utsure::core::job
