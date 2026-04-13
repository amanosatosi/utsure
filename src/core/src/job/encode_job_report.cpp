#include "utsure/core/job/encode_job_report.hpp"

#include "../media/streaming_transcode_pipeline.hpp"
#include "../media/transcode_threading.hpp"
#include "utsure/core/media/decoded_media.hpp"

#include <iomanip>
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

std::string format_decimal(const double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

double microseconds_to_milliseconds(const std::uint64_t microseconds) noexcept {
    return static_cast<double>(microseconds) / 1000.0;
}

double microseconds_to_milliseconds(const std::int64_t microseconds) noexcept {
    return static_cast<double>(microseconds) / 1000.0;
}

double percentage_of_total(
    const std::uint64_t stage_microseconds,
    const std::int64_t total_elapsed_microseconds
) noexcept {
    if (stage_microseconds == 0U || total_elapsed_microseconds <= 0) {
        return 0.0;
    }

    return (static_cast<double>(stage_microseconds) * 100.0) /
        static_cast<double>(total_elapsed_microseconds);
}

}  // namespace

std::string format_encode_job_report(const EncodeJobSummary &encode_job_summary) {
    std::ostringstream report;
    const auto &runtime = encode_job_summary.streaming_runtime;
    const media::streaming::StreamingRuntimeBehavior runtime_behavior{
        .detected_logical_core_count = runtime.detected_logical_core_count,
        .effective_logical_core_count = runtime.effective_logical_core_count,
        .cpu_usage_mode = runtime.cpu_usage_mode,
        .selected_video_decoder_thread_count = runtime.selected_video_decoder_thread_count,
        .selected_video_decoder_thread_type = runtime.selected_video_decoder_thread_type,
        .selected_video_encoder_thread_count = runtime.selected_video_encoder_thread_count,
        .selected_video_encoder_thread_type = runtime.selected_video_encoder_thread_type,
        .video_processing_worker_count = runtime.video_processing_worker_count,
        .subtitle_processing_worker_count = runtime.subtitle_processing_worker_count,
        .video_frame_queue_depth = runtime.video_frame_queue_depth,
        .decoded_audio_block_queue_depth = runtime.decoded_audio_block_queue_depth,
        .subtitle_bitmap_mode = runtime.subtitle_bitmap_mode,
        .subtitle_composition_mode = runtime.subtitle_composition_mode,
        .subtitle_diagnostics_mode = runtime.subtitle_diagnostics_mode
    };

    report << "job.input.intro.present=" << (encode_job_summary.job.input.intro_source_path.has_value() ? "yes" : "no") << '\n';
    if (encode_job_summary.job.input.intro_source_path.has_value()) {
        report << "job.input.intro=" << format_optional_path_leaf(encode_job_summary.job.input.intro_source_path) << '\n';
    }
    report << "job.input.main_source=" << format_path_leaf(encode_job_summary.job.input.main_source_path) << '\n';
    report << "job.input.main_trim.in_us="
           << (encode_job_summary.job.input.main_source_trim_in_us.has_value()
                   ? std::to_string(*encode_job_summary.job.input.main_source_trim_in_us)
                   : std::string("none"))
           << '\n';
    report << "job.input.main_trim.out_us="
           << (encode_job_summary.job.input.main_source_trim_out_us.has_value()
                   ? std::to_string(*encode_job_summary.job.input.main_source_trim_out_us)
                   : std::string("none"))
           << '\n';
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
    report << "job.execution.cpu_mode="
           << media::to_string(encode_job_summary.job.execution.threading.cpu_usage_mode) << '\n';
    report << "decode.policy.video_pixel_format="
           << media::to_string(encode_job_summary.decode_normalization_policy.video_pixel_format) << '\n';
    report << "decode.policy.audio_sample_format="
           << media::to_string(encode_job_summary.decode_normalization_policy.audio_sample_format) << '\n';
    report << "decode.policy.audio_block_samples="
           << encode_job_summary.decode_normalization_policy.audio_block_samples << '\n';
    report << "streaming.logical_cores.detected=" << runtime.detected_logical_core_count << '\n';
    report << "streaming.logical_cores.effective=" << runtime.effective_logical_core_count << '\n';
    report << "streaming.cpu_mode=" << media::to_string(runtime.cpu_usage_mode) << '\n';
    report << "streaming.decoder_thread_count=" << runtime.selected_video_decoder_thread_count << '\n';
    report << "streaming.decoder_thread_type="
           << media::detail::format_ffmpeg_thread_type_detail(runtime.selected_video_decoder_thread_type) << '\n';
    report << "streaming.encoder_threads="
           << media::streaming::format_encoder_threading_summary(
                  runtime_behavior,
                  encode_job_summary.job.output.video.codec
              )
           << '\n';
    report << "streaming.encoder_thread_count=" << runtime.selected_video_encoder_thread_count << '\n';
    report << "streaming.encoder_thread_type="
           << media::detail::format_ffmpeg_thread_type_detail(runtime.selected_video_encoder_thread_type) << '\n';
    report << "streaming.video_workers=" << runtime.video_processing_worker_count << '\n';
    report << "streaming.subtitle_workers=" << runtime.subtitle_processing_worker_count << '\n';
    report << "streaming.video_queue_frames=" << runtime.video_frame_queue_depth << '\n';
    report << "streaming.audio_queue_blocks=" << runtime.decoded_audio_block_queue_depth << '\n';
    report << "streaming.subtitle.bitmap_mode=" << runtime.subtitle_bitmap_mode << '\n';
    report << "streaming.subtitle.composition_mode=" << runtime.subtitle_composition_mode << '\n';
    report << "streaming.subtitle.diagnostics_mode=" << runtime.subtitle_diagnostics_mode << '\n';
    report << "streaming.performance.total_elapsed_ms="
           << format_decimal(microseconds_to_milliseconds(runtime.total_elapsed_microseconds)) << '\n';
    report << "streaming.performance.average_output_fps=" << format_decimal(runtime.average_output_fps) << '\n';
    report << "streaming.performance.video_decode_ms="
           << format_decimal(microseconds_to_milliseconds(runtime.video_decode_microseconds)) << '\n';
    report << "streaming.performance.video_decode_pct="
           << format_decimal(percentage_of_total(runtime.video_decode_microseconds, runtime.total_elapsed_microseconds))
           << '\n';
    report << "streaming.performance.video_process_ms="
           << format_decimal(microseconds_to_milliseconds(runtime.video_process_microseconds)) << '\n';
    report << "streaming.performance.video_process_pct="
           << format_decimal(percentage_of_total(runtime.video_process_microseconds, runtime.total_elapsed_microseconds))
           << '\n';
    report << "streaming.performance.subtitle_compose_ms="
           << format_decimal(microseconds_to_milliseconds(runtime.subtitle_compose_microseconds)) << '\n';
    report << "streaming.performance.subtitle_compose_pct="
           << format_decimal(percentage_of_total(runtime.subtitle_compose_microseconds, runtime.total_elapsed_microseconds))
           << '\n';
    report << "streaming.performance.video_encode_ms="
           << format_decimal(microseconds_to_milliseconds(runtime.video_encode_microseconds)) << '\n';
    report << "streaming.performance.video_encode_pct="
           << format_decimal(percentage_of_total(runtime.video_encode_microseconds, runtime.total_elapsed_microseconds))
           << '\n';
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
