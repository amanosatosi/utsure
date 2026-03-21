#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/job/encode_job_report.hpp"
#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/media/media_inspector.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using utsure::core::job::EncodeJob;
using utsure::core::job::EncodeJobLogLevel;
using utsure::core::job::EncodeJobLogMessage;
using utsure::core::job::EncodeJobObserver;
using utsure::core::job::EncodeJobProcessPriority;
using utsure::core::job::EncodeJobProgress;
using utsure::core::job::EncodeJobResult;
using utsure::core::job::EncodeJobRunner;
using utsure::core::job::EncodeJobRunOptions;
using utsure::core::job::EncodeJobStage;
using utsure::core::job::EncodeJobSummary;
using utsure::core::job::format_encode_job_report;
using utsure::core::media::AudioOutputMode;
using utsure::core::media::CpuUsageMode;
using utsure::core::media::DecodedMediaSource;
using utsure::core::media::MediaDecodeResult;
using utsure::core::media::MediaDecoder;
using utsure::core::media::MediaInspectionResult;
using utsure::core::media::MediaInspector;
using utsure::core::media::OutputVideoCodec;
using utsure::core::media::Rational;
using utsure::core::media::ResolvedAudioOutputMode;
using utsure::core::timeline::TimelineSegmentKind;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

bool contains_text(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

struct CollectingObserver final : EncodeJobObserver {
    std::vector<EncodeJobProgress> progress_updates{};
    std::vector<EncodeJobLogMessage> log_messages{};

    void on_progress(const EncodeJobProgress &progress) override {
        progress_updates.push_back(progress);
    }

    void on_log(const EncodeJobLogMessage &message) override {
        log_messages.push_back(message);
    }
};

std::string format_rational(const Rational &value) {
    if (!value.is_valid()) {
        return "unknown";
    }

    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

bool rational_is_positive(const Rational &value) {
    return value.is_valid() && value.numerator > 0 && value.denominator > 0;
}

bool rational_has_finer_precision(const Rational &left, const Rational &right) {
    if (!rational_is_positive(left)) {
        return false;
    }

    if (!rational_is_positive(right)) {
        return true;
    }

    return (left.numerator * right.denominator) < (right.numerator * left.denominator);
}

Rational choose_expected_output_video_time_base(const Rational &stream_time_base, const Rational &average_frame_rate) {
    const Rational nominal_frame_time_base = rational_is_positive(average_frame_rate)
        ? Rational{
            .numerator = average_frame_rate.denominator,
            .denominator = average_frame_rate.numerator
        }
        : Rational{};

    if (rational_has_finer_precision(stream_time_base, nominal_frame_time_base)) {
        return stream_time_base;
    }

    if (rational_is_positive(nominal_frame_time_base)) {
        return nominal_frame_time_base;
    }

    return stream_time_base;
}

bool frames_are_identical(
    const DecodedMediaSource &decoded_output,
    const std::size_t left_index,
    const std::size_t right_index
) {
    return decoded_output.video_frames[left_index].planes.front().bytes ==
        decoded_output.video_frames[right_index].planes.front().bytes;
}

std::int64_t decoded_video_end_microseconds(const DecodedMediaSource &decoded_output) {
    if (decoded_output.video_frames.empty()) {
        return 0;
    }

    const auto &last_frame = decoded_output.video_frames.back();
    return last_frame.timestamp.start_microseconds + last_frame.timestamp.duration_microseconds.value_or(0);
}

std::int64_t decoded_audio_end_microseconds(const DecodedMediaSource &decoded_output) {
    if (decoded_output.audio_blocks.empty()) {
        return 0;
    }

    const auto &last_block = decoded_output.audio_blocks.back();
    return last_block.timestamp.start_microseconds + last_block.timestamp.duration_microseconds.value_or(0);
}

bool has_irregular_video_timestamp_deltas(const DecodedMediaSource &decoded_output) {
    if (decoded_output.video_frames.size() < 3U) {
        return false;
    }

    const auto first_delta =
        decoded_output.video_frames[1].timestamp.start_microseconds -
        decoded_output.video_frames[0].timestamp.start_microseconds;
    if (first_delta <= 0) {
        return false;
    }

    for (std::size_t index = 2; index < decoded_output.video_frames.size(); ++index) {
        const auto current_delta =
            decoded_output.video_frames[index].timestamp.start_microseconds -
            decoded_output.video_frames[index - 1].timestamp.start_microseconds;
        if (current_delta <= 0) {
            return false;
        }

        if (current_delta != first_delta) {
            return true;
        }
    }

    return false;
}

int assert_output_decode(
    const DecodedMediaSource &decoded_output,
    const std::size_t expected_frame_count,
    const bool expect_audio
) {
    if (decoded_output.video_frames.size() != expected_frame_count) {
        return fail("Unexpected job-output video frame count.");
    }

    if (expect_audio && decoded_output.audio_blocks.empty()) {
        return fail("The encode-job output unexpectedly dropped audio.");
    }

    if (!expect_audio && !decoded_output.audio_blocks.empty()) {
        return fail("The encode-job output unexpectedly contains audio.");
    }

    for (std::size_t index = 1; index < decoded_output.video_frames.size(); ++index) {
        if (decoded_output.video_frames[index].timestamp.start_microseconds <=
            decoded_output.video_frames[index - 1].timestamp.start_microseconds) {
            return fail("Job-output timestamps are not strictly increasing.");
        }
    }

    return 0;
}

int assert_main_only_summary(
    const EncodeJobSummary &summary,
    const std::string_view expected_codec_name
) {
    if (summary.timeline_summary.segments.size() != 1 ||
        summary.timeline_summary.segments[0].kind != TimelineSegmentKind::main) {
        return fail("Unexpected main-only timeline segment summary.");
    }

    if (summary.decoded_video_frame_count != 48 ||
        summary.decoded_audio_block_count != 94 ||
        summary.timeline_summary.output_video_frame_count != 48 ||
        summary.timeline_summary.output_audio_block_count != 94) {
        return fail("Unexpected main-only decoded counts.");
    }

    if (format_rational(summary.timeline_summary.output_frame_rate) != "24/1" ||
        format_rational(summary.timeline_summary.output_video_time_base) != "1/24") {
        return fail("Unexpected main-only output cadence.");
    }

    if (!summary.encoded_media_summary.output_info.primary_audio_stream.has_value()) {
        return fail("The main-only encode-job output is missing audio.");
    }

    const auto &output_audio = *summary.encoded_media_summary.output_info.primary_audio_stream;
    if (summary.encoded_media_summary.output_info.primary_video_stream->codec_name != expected_codec_name ||
        output_audio.codec_name != "aac" ||
        output_audio.sample_rate != 48000 ||
        output_audio.channel_count != 1 ||
        summary.encoded_media_summary.resolved_audio_output.requested_mode != AudioOutputMode::auto_select ||
        summary.encoded_media_summary.resolved_audio_output.resolved_mode != ResolvedAudioOutputMode::encode_aac) {
        return fail("Unexpected main-only encoded output streams.");
    }

    if (summary.subtitled_video_frame_count != 0) {
        return fail("Main-only job unexpectedly reported subtitle burn-in.");
    }

    return 0;
}

int assert_timeline_summary(const EncodeJobSummary &summary) {
    if (summary.timeline_summary.segments.size() != 3 ||
        summary.timeline_summary.segments[0].kind != TimelineSegmentKind::intro ||
        summary.timeline_summary.segments[1].kind != TimelineSegmentKind::main ||
        summary.timeline_summary.segments[2].kind != TimelineSegmentKind::outro) {
        return fail("Unexpected intro/main/outro segment order.");
    }

    if (summary.decoded_video_frame_count != 96 ||
        summary.decoded_audio_block_count != 188 ||
        summary.timeline_summary.output_video_frame_count != 96 ||
        summary.timeline_summary.output_audio_block_count != 188) {
        return fail("Unexpected intro/main/outro decoded counts.");
    }

    if (summary.timeline_summary.segments[0].start_microseconds != 0 ||
        summary.timeline_summary.segments[1].start_microseconds != 1000000 ||
        summary.timeline_summary.segments[2].start_microseconds != 3000000) {
        return fail("Unexpected intro/main/outro segment starts.");
    }

    if (summary.timeline_summary.segments[0].duration_microseconds != 1000000 ||
        summary.timeline_summary.segments[1].duration_microseconds != 2000000 ||
        summary.timeline_summary.segments[2].duration_microseconds != 1000000) {
        return fail("Unexpected intro/main/outro segment durations.");
    }

    if (!summary.timeline_summary.segments[2].inserted_silence ||
        summary.timeline_summary.segments[2].audio_block_count != 47) {
        return fail("Unexpected outro audio stitching summary.");
    }

    if (format_rational(summary.timeline_summary.output_frame_rate) != "24/1" ||
        format_rational(summary.timeline_summary.output_video_time_base) != "1/24" ||
        !summary.timeline_summary.output_audio_time_base.has_value() ||
        format_rational(*summary.timeline_summary.output_audio_time_base) != "1/48000") {
        return fail("Unexpected intro/main/outro output cadence.");
    }

    if (!summary.encoded_media_summary.output_info.primary_audio_stream.has_value()) {
        return fail("The intro/main/outro encode-job output is missing audio.");
    }

    const auto &output_audio = *summary.encoded_media_summary.output_info.primary_audio_stream;
    if (summary.subtitled_video_frame_count != 0 ||
        output_audio.codec_name != "aac" ||
        output_audio.sample_rate != 48000 ||
        output_audio.channel_count != 1 ||
        summary.encoded_media_summary.resolved_audio_output.resolved_mode != ResolvedAudioOutputMode::encode_aac) {
        return fail("Unexpected intro/main/outro encode-job output state.");
    }

    return 0;
}

int assert_observer_flow(
    const CollectingObserver &observer,
    const int expected_decode_steps,
    const bool expect_subtitle_burn
) {
    if (observer.progress_updates.empty()) {
        return fail("The encode-job observer did not receive any progress updates.");
    }

    const int expected_total_steps = 1 + expected_decode_steps + (expect_subtitle_burn ? 1 : 0) + 1 + 1;
    if (observer.progress_updates.front().stage != EncodeJobStage::assembling_timeline) {
        return fail("The first observed encode-job stage was not timeline assembly.");
    }

    if (observer.progress_updates.back().stage != EncodeJobStage::completed) {
        return fail("The final observed encode-job stage was not completion.");
    }

    int decode_stage_count = 0;
    int subtitle_stage_count = 0;
    int previous_step = 0;

    for (const auto &progress : observer.progress_updates) {
        if (progress.total_steps != expected_total_steps) {
            return fail("The encode-job observer reported an unexpected total-step count.");
        }

        if (progress.current_step < previous_step || progress.current_step > (previous_step + 1)) {
            return fail("The encode-job observer reported an unexpected step progression.");
        }

        if (progress.stage == EncodeJobStage::decoding_segment) {
            ++decode_stage_count;
        }

        if (progress.stage == EncodeJobStage::burning_in_subtitles) {
            ++subtitle_stage_count;
        }

        previous_step = progress.current_step;
    }

    if (decode_stage_count != expected_decode_steps) {
        return fail("The encode-job observer reported an unexpected number of decode stages.");
    }

    if (subtitle_stage_count != (expect_subtitle_burn ? 1 : 0)) {
        return fail("The encode-job observer reported an unexpected subtitle-burn stage count.");
    }

    if (observer.progress_updates.back().current_step != expected_total_steps) {
        return fail("The encode-job observer did not report completion at the final step.");
    }

    if (observer.log_messages.empty()) {
        return fail("The encode-job observer did not receive any log messages.");
    }

    for (const auto &log_message : observer.log_messages) {
        if (log_message.level == EncodeJobLogLevel::error) {
            return fail("The encode-job observer reported an unexpected error log for a passing job.");
        }
    }

    return 0;
}

bool observer_logs_contain_text(const CollectingObserver &observer, std::string_view needle) {
    for (const auto &message : observer.log_messages) {
        if (contains_text(message.message, needle)) {
            return true;
        }
    }

    return false;
}

int assert_runtime_visibility(
    const CollectingObserver &observer,
    const EncodeJobSummary &summary
) {
    if (summary.job.execution.process_priority != EncodeJobProcessPriority::below_normal) {
        return fail("Unexpected default encode-job process priority.");
    }

    if (summary.job.execution.threading.cpu_usage_mode != CpuUsageMode::auto_select) {
        return fail("Unexpected default encode-job CPU mode.");
    }

    const auto report = format_encode_job_report(summary);
    if (!contains_text(report, "job.execution.priority=below_normal") ||
        !contains_text(report, "job.execution.cpu_mode=auto") ||
        !contains_text(report, "streaming.encoder_threads=auto (") ||
        !contains_text(report, "streaming.video_workers=") ||
        !contains_text(report, "streaming.video_queue_frames=70") ||
        !contains_text(report, "streaming.audio_queue_blocks=8")) {
        return fail("The encode-job report did not include the expected runtime settings.");
    }

    if (!observer_logs_contain_text(observer, "Encoding runtime request: CPU mode auto, encoder threads auto (") ||
        !observer_logs_contain_text(observer, "video queue 70 frames") ||
        !observer_logs_contain_text(observer, "priority Below Normal") ||
        !observer_logs_contain_text(observer, "Video encoder:") ||
        !observer_logs_contain_text(observer, "Video decoder (") ||
        !observer_logs_contain_text(observer, "Stage timing:")) {
        return fail("The encode-job observer logs did not include the expected runtime settings.");
    }

    return 0;
}

int run_threading_mode_selection_assertion() {
    const auto conservative_count = utsure::core::media::resolve_requested_ffmpeg_thread_count(
        utsure::core::media::TranscodeThreadingSettings{
            .cpu_usage_mode = CpuUsageMode::conservative
        },
        8U
    );
    const auto aggressive_count = utsure::core::media::resolve_requested_ffmpeg_thread_count(
        utsure::core::media::TranscodeThreadingSettings{
            .cpu_usage_mode = CpuUsageMode::aggressive
        },
        8U
    );
    const auto auto_count = utsure::core::media::resolve_requested_ffmpeg_thread_count(
        utsure::core::media::TranscodeThreadingSettings{
            .cpu_usage_mode = CpuUsageMode::auto_select
        },
        8U
    );

    if (conservative_count != 4) {
        return fail("Conservative CPU mode did not resolve to half of the synthetic logical core count.");
    }

    if (aggressive_count != 7) {
        return fail("Aggressive CPU mode did not resolve to one less than the synthetic logical core count.");
    }

    if (aggressive_count <= conservative_count) {
        return fail("Aggressive CPU mode did not select more FFmpeg threads than Conservative mode.");
    }

    if (auto_count != 0) {
        return fail("Auto CPU mode should leave FFmpeg thread count selection to the backend.");
    }

    std::cout << "threading.auto=0\n";
    std::cout << "threading.conservative=" << conservative_count << '\n';
    std::cout << "threading.aggressive=" << aggressive_count << '\n';
    return 0;
}

std::vector<const EncodeJobProgress *> collect_fine_encode_updates(const CollectingObserver &observer) {
    std::vector<const EncodeJobProgress *> updates{};
    updates.reserve(observer.progress_updates.size());

    for (const auto &progress : observer.progress_updates) {
        if (progress.stage == EncodeJobStage::encoding_output && progress.stage_fraction.has_value()) {
            updates.push_back(&progress);
        }
    }

    return updates;
}

int assert_fine_encode_progress(
    const CollectingObserver &observer,
    const EncodeJobSummary &summary
) {
    const auto fine_updates = collect_fine_encode_updates(observer);
    if (fine_updates.empty()) {
        return fail("The encode job did not report any fine-grained streaming encode progress.");
    }

    std::optional<std::uint64_t> expected_total_frames{};
    std::optional<std::int64_t> expected_total_duration_us{};
    std::uint64_t previous_encoded_frames = 0;
    std::int64_t previous_encoded_duration_us = 0;
    double previous_stage_fraction = 0.0;
    bool saw_positive_encoded_fps = false;

    for (const auto *progress : fine_updates) {
        if (!progress->encoded_video_frames.has_value() ||
            !progress->total_video_frames.has_value() ||
            !progress->encoded_video_duration_us.has_value() ||
            !progress->total_video_duration_us.has_value()) {
            return fail("A fine-grained encode progress update was missing frame or duration totals.");
        }

        if (*progress->stage_fraction < 0.0 || *progress->stage_fraction > 1.0) {
            return fail("A fine-grained encode progress update reported an invalid stage fraction.");
        }

        if (*progress->stage_fraction + 1e-9 < previous_stage_fraction) {
            return fail("Fine-grained encode progress stage fractions must be monotonic.");
        }

        if (*progress->encoded_video_frames < previous_encoded_frames) {
            return fail("Fine-grained encode progress encoded-frame counts must be monotonic.");
        }

        if (*progress->encoded_video_duration_us < previous_encoded_duration_us) {
            return fail("Fine-grained encode progress encoded durations must be monotonic.");
        }

        if (*progress->total_video_frames == 0U || *progress->total_video_duration_us <= 0) {
            return fail("Fine-grained encode progress reported unusable total estimates.");
        }

        if (!expected_total_frames.has_value()) {
            expected_total_frames = *progress->total_video_frames;
        } else if (*progress->total_video_frames != *expected_total_frames) {
            return fail("Fine-grained encode progress total-frame estimates changed mid-encode.");
        }

        if (!expected_total_duration_us.has_value()) {
            expected_total_duration_us = *progress->total_video_duration_us;
        } else if (*progress->total_video_duration_us != *expected_total_duration_us) {
            return fail("Fine-grained encode progress total-duration estimates changed mid-encode.");
        }

        if (progress->encoded_fps.has_value()) {
            if (*progress->encoded_fps <= 0.0) {
                return fail("Fine-grained encode progress reported a non-positive EFPS value.");
            }

            saw_positive_encoded_fps = true;
        }

        previous_stage_fraction = *progress->stage_fraction;
        previous_encoded_frames = *progress->encoded_video_frames;
        previous_encoded_duration_us = *progress->encoded_video_duration_us;
    }

    const auto &final_progress = *fine_updates.back();
    if (std::fabs(*final_progress.stage_fraction - 1.0) > 1e-9) {
        return fail("The final fine-grained encode progress update did not report 100% completion.");
    }

    if (*final_progress.encoded_video_frames !=
        static_cast<std::uint64_t>(summary.encoded_media_summary.encoded_video_frame_count)) {
        return fail("The final fine-grained encode progress update reported the wrong encoded frame count.");
    }

    if (*final_progress.encoded_video_duration_us != summary.timeline_summary.output_duration_microseconds) {
        return fail("The final fine-grained encode progress update reported the wrong encoded duration.");
    }

    if (std::llabs(
            static_cast<long long>(*final_progress.total_video_frames) -
            static_cast<long long>(summary.timeline_summary.output_video_frame_count)) > 1) {
        return fail("The final fine-grained encode progress total-frame estimate drifted too far.");
    }

    if (std::llabs(*final_progress.total_video_duration_us - summary.timeline_summary.output_duration_microseconds) >
        100000) {
        return fail("The final fine-grained encode progress total-duration estimate drifted too far.");
    }

    if (!saw_positive_encoded_fps) {
        return fail("The encode job never reported a positive EFPS value during fine-grained progress.");
    }

    return 0;
}

int assert_streaming_memory_observer_flow(const CollectingObserver &observer) {
    if (observer.progress_updates.empty()) {
        return fail("The streaming-memory encode job did not report any progress.");
    }

    if (observer.progress_updates.front().stage != EncodeJobStage::assembling_timeline) {
        return fail("The streaming-memory encode job did not start at timeline assembly.");
    }

    if (observer.progress_updates.back().stage != EncodeJobStage::completed) {
        return fail("The streaming-memory encode job did not report completion.");
    }

    if (observer.log_messages.empty()) {
        return fail("The streaming-memory encode job did not report any log messages.");
    }

    return 0;
}

std::string build_validation_report(
    const EncodeJobSummary &encode_job_summary,
    const DecodedMediaSource &decoded_output
) {
    std::string report = format_encode_job_report(encode_job_summary);
    report += "\nverified.output.video_frames=" + std::to_string(decoded_output.video_frames.size());
    report += "\nverified.output.audio_blocks=" + std::to_string(decoded_output.audio_blocks.size());
    report += "\nverified.output.frame0.start_us=" +
              std::to_string(decoded_output.video_frames[0].timestamp.start_microseconds);
    report += "\nverified.output.frame1.start_us=" +
              std::to_string(decoded_output.video_frames[1].timestamp.start_microseconds);
    report += "\nverified.output.frame2.start_us=" +
              std::to_string(decoded_output.video_frames[2].timestamp.start_microseconds);
    return report;
}

int run_main_only_job_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &output_path,
    const OutputVideoCodec codec,
    const std::string_view expected_codec_name
) {
    CollectingObserver observer{};
    const EncodeJob job{
        .input = {
            .main_source_path = sample_path
        },
        .output = {
            .output_path = output_path,
            .video = {
                .codec = codec,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult job_result = EncodeJobRunner::run(job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!job_result.succeeded()) {
        const std::string error_message =
            "Encode job failed unexpectedly: " +
            job_result.error->message +
            " Hint: " +
            job_result.error->actionable_hint;
        return fail(error_message);
    }

    const MediaDecodeResult output_decode_result = MediaDecoder::decode(output_path);
    if (!output_decode_result.succeeded()) {
        const std::string error_message =
            "Muxed output decode failed unexpectedly: " +
            output_decode_result.error->message +
            " Hint: " +
            output_decode_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto structure_result = assert_output_decode(*output_decode_result.decoded_media_source, 48U, true);
    if (structure_result != 0) {
        return structure_result;
    }

    if (output_decode_result.decoded_media_source->video_frames[0].timestamp.start_microseconds != 0 ||
        output_decode_result.decoded_media_source->video_frames[1].timestamp.start_microseconds != 41667 ||
        output_decode_result.decoded_media_source->video_frames[2].timestamp.start_microseconds != 83333) {
        return fail("Unexpected main-only output timestamps.");
    }

    const auto summary_result =
        assert_main_only_summary(*job_result.encode_job_summary, expected_codec_name);
    if (summary_result != 0) {
        return summary_result;
    }

    const auto observer_result = assert_observer_flow(observer, 1, false);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto fine_progress_result = assert_fine_encode_progress(observer, *job_result.encode_job_summary);
    if (fine_progress_result != 0) {
        return fine_progress_result;
    }

    const auto runtime_visibility_result = assert_runtime_visibility(observer, *job_result.encode_job_summary);
    if (runtime_visibility_result != 0) {
        return runtime_visibility_result;
    }

    std::cout << build_validation_report(
        *job_result.encode_job_summary,
        *output_decode_result.decoded_media_source
    ) << '\n';
    return 0;
}

int run_timeline_h264_assertion(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path,
    const std::filesystem::path &output_path
) {
    CollectingObserver observer{};
    const EncodeJob job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .output = {
            .output_path = output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult job_result = EncodeJobRunner::run(job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!job_result.succeeded()) {
        const std::string error_message =
            "Timeline encode job failed unexpectedly: " +
            job_result.error->message +
            " Hint: " +
            job_result.error->actionable_hint;
        return fail(error_message);
    }

    const MediaDecodeResult output_decode_result = MediaDecoder::decode(output_path);
    if (!output_decode_result.succeeded()) {
        const std::string error_message =
            "Timeline output decode failed unexpectedly: " +
            output_decode_result.error->message +
            " Hint: " +
            output_decode_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto structure_result = assert_output_decode(*output_decode_result.decoded_media_source, 96U, true);
    if (structure_result != 0) {
        return structure_result;
    }

    if (output_decode_result.decoded_media_source->video_frames[24].timestamp.start_microseconds != 1000000 ||
        output_decode_result.decoded_media_source->video_frames[72].timestamp.start_microseconds != 3000000) {
        return fail("Unexpected intro/main/outro output boundary timestamps.");
    }

    if (frames_are_identical(*output_decode_result.decoded_media_source, 0U, 24U) ||
        frames_are_identical(*output_decode_result.decoded_media_source, 24U, 72U)) {
        return fail("The encoded intro/main/outro output does not preserve distinct segment visuals.");
    }

    const auto summary_result = assert_timeline_summary(*job_result.encode_job_summary);
    if (summary_result != 0) {
        return summary_result;
    }

    if (job_result.encode_job_summary->encoded_media_summary.output_info.primary_video_stream->codec_name != "h264") {
        return fail("Unexpected intro/main/outro encoded video codec.");
    }

    const auto observer_result = assert_observer_flow(observer, 3, false);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto fine_progress_result = assert_fine_encode_progress(observer, *job_result.encode_job_summary);
    if (fine_progress_result != 0) {
        return fine_progress_result;
    }

    const auto runtime_visibility_result = assert_runtime_visibility(observer, *job_result.encode_job_summary);
    if (runtime_visibility_result != 0) {
        return runtime_visibility_result;
    }

    std::cout << build_validation_report(
        *job_result.encode_job_summary,
        *output_decode_result.decoded_media_source
    ) << '\n';
    return 0;
}

int run_streaming_memory_budget_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    CollectingObserver observer{};
    const EncodeJob job{
        .input = {
            .main_source_path = main_path
        },
        .output = {
            .output_path = output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult job_result = EncodeJobRunner::run(job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!job_result.succeeded()) {
        const std::string error_message =
            "The streaming-memory encode job failed unexpectedly: " +
            job_result.error->message +
            " Hint: " +
            job_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto &summary = *job_result.encode_job_summary;
    if (summary.decoded_video_frame_count != 192 ||
        summary.timeline_summary.output_video_frame_count != 192 ||
        summary.decoded_audio_block_count != 0 ||
        summary.subtitled_video_frame_count != 0) {
        return fail("Unexpected counts for the streaming-memory encode job.");
    }

    if (!summary.encoded_media_summary.output_info.primary_video_stream.has_value() ||
        summary.encoded_media_summary.output_info.primary_video_stream->width != 1920 ||
        summary.encoded_media_summary.output_info.primary_video_stream->height != 1080 ||
        summary.encoded_media_summary.output_info.primary_audio_stream.has_value()) {
        return fail("Unexpected output streams for the streaming-memory encode job.");
    }

    const MediaInspectionResult inspection_result = MediaInspector::inspect(output_path);
    if (!inspection_result.succeeded()) {
        return fail("The streaming-memory output could not be inspected.");
    }

    if (assert_streaming_memory_observer_flow(observer) != 0) {
        return 1;
    }

    if (assert_fine_encode_progress(observer, *job_result.encode_job_summary) != 0) {
        return 1;
    }

    if (assert_runtime_visibility(observer, *job_result.encode_job_summary) != 0) {
        return 1;
    }

    std::cout << "streaming_memory_budget=passed\n";
    std::cout << "output.video.resolution=1920x1080\n";
    std::cout << "output.encoded_video_frames=" << summary.encoded_media_summary.encoded_video_frame_count << '\n';
    return 0;
}

int run_disable_audio_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    CollectingObserver observer{};
    EncodeJob job{
        .input = {
            .main_source_path = main_path
        },
        .output = {
            .output_path = output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };
    job.output.audio.mode = AudioOutputMode::disable;

    const EncodeJobResult job_result = EncodeJobRunner::run(job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!job_result.succeeded()) {
        const std::string error_message =
            "The disable-audio encode job failed unexpectedly: " +
            job_result.error->message +
            " Hint: " +
            job_result.error->actionable_hint;
        return fail(error_message);
    }

    const MediaDecodeResult output_decode_result = MediaDecoder::decode(output_path);
    if (!output_decode_result.succeeded()) {
        const std::string error_message =
            "The disable-audio output decode failed unexpectedly: " +
            output_decode_result.error->message +
            " Hint: " +
            output_decode_result.error->actionable_hint;
        return fail(error_message);
    }

    if (assert_output_decode(*output_decode_result.decoded_media_source, 48U, false) != 0) {
        return 1;
    }

    const auto &summary = *job_result.encode_job_summary;
    if (summary.decoded_audio_block_count != 0 ||
        summary.timeline_summary.output_audio_block_count != 0 ||
        summary.timeline_summary.output_audio_time_base.has_value() ||
        summary.encoded_media_summary.output_info.primary_audio_stream.has_value() ||
        summary.encoded_media_summary.resolved_audio_output.resolved_mode != ResolvedAudioOutputMode::disabled) {
        return fail("Unexpected output state for the disable-audio encode job.");
    }

    if (assert_fine_encode_progress(observer, summary) != 0) {
        return 1;
    }

    if (assert_runtime_visibility(observer, summary) != 0) {
        return 1;
    }

    std::cout << build_validation_report(
        *job_result.encode_job_summary,
        *output_decode_result.decoded_media_source
    ) << '\n';
    return 0;
}

int run_copy_audio_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    CollectingObserver observer{};
    EncodeJob job{
        .input = {
            .main_source_path = main_path
        },
        .output = {
            .output_path = output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };
    job.output.audio.mode = AudioOutputMode::copy_source;

    const EncodeJobResult job_result = EncodeJobRunner::run(job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!job_result.succeeded()) {
        const std::string error_message =
            "The copy-audio encode job failed unexpectedly: " +
            job_result.error->message +
            " Hint: " +
            job_result.error->actionable_hint;
        return fail(error_message);
    }

    const MediaDecodeResult output_decode_result = MediaDecoder::decode(output_path);
    if (!output_decode_result.succeeded()) {
        const std::string error_message =
            "The copy-audio output decode failed unexpectedly: " +
            output_decode_result.error->message +
            " Hint: " +
            output_decode_result.error->actionable_hint;
        return fail(error_message);
    }

    if (assert_output_decode(*output_decode_result.decoded_media_source, 48U, true) != 0) {
        return 1;
    }

    const auto &summary = *job_result.encode_job_summary;
    if (!summary.encoded_media_summary.output_info.primary_audio_stream.has_value() ||
        summary.decoded_audio_block_count != 0 ||
        summary.timeline_summary.output_audio_block_count != 0 ||
        summary.encoded_media_summary.resolved_audio_output.resolved_mode != ResolvedAudioOutputMode::copy_source ||
        !summary.timeline_summary.output_audio_time_base.has_value() ||
        format_rational(*summary.timeline_summary.output_audio_time_base) != "1/48000") {
        return fail("Unexpected output state for the copy-audio encode job.");
    }

    const auto &output_audio = *summary.encoded_media_summary.output_info.primary_audio_stream;
    if (output_audio.codec_name != "aac" ||
        output_audio.sample_rate != 48000 ||
        output_audio.channel_count != 1) {
        return fail("Unexpected copied output audio stream.");
    }

    if (assert_fine_encode_progress(observer, summary) != 0) {
        return 1;
    }

    if (assert_runtime_visibility(observer, summary) != 0) {
        return 1;
    }

    std::cout << build_validation_report(
        *job_result.encode_job_summary,
        *output_decode_result.decoded_media_source
    ) << '\n';
    return 0;
}

int run_coarse_timebase_ntsc_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    const MediaInspectionResult input_inspection_result = MediaInspector::inspect(main_path);
    if (!input_inspection_result.succeeded() ||
        !input_inspection_result.media_source_info->primary_video_stream.has_value()) {
        return fail("The coarse-timebase NTSC sample could not be inspected.");
    }

    const auto &input_video_stream = *input_inspection_result.media_source_info->primary_video_stream;
    if (format_rational(input_video_stream.average_frame_rate) != "24000/1001" ||
        format_rational(input_video_stream.timestamps.time_base) != "1/1000") {
        return fail("The coarse-timebase NTSC sample does not expose the expected input cadence metadata.");
    }

    CollectingObserver observer{};
    EncodeJob job{
        .input = {
            .main_source_path = main_path
        },
        .output = {
            .output_path = output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };
    job.output.audio.mode = AudioOutputMode::encode_aac;

    const EncodeJobResult job_result = EncodeJobRunner::run(job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!job_result.succeeded()) {
        const std::string error_message =
            "The coarse-timebase NTSC encode job failed unexpectedly: " +
            job_result.error->message +
            " Hint: " +
            job_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto &summary = *job_result.encode_job_summary;
    const auto expected_output_video_time_base = choose_expected_output_video_time_base(
        input_video_stream.timestamps.time_base,
        input_video_stream.average_frame_rate
    );
    if (format_rational(summary.timeline_summary.output_frame_rate) != "24000/1001" ||
        format_rational(summary.timeline_summary.output_video_time_base) !=
            format_rational(expected_output_video_time_base) ||
        !summary.timeline_summary.output_audio_time_base.has_value() ||
        format_rational(*summary.timeline_summary.output_audio_time_base) != "1/48000" ||
        summary.encoded_media_summary.resolved_audio_output.resolved_mode != ResolvedAudioOutputMode::encode_aac) {
        return fail("Unexpected coarse-timebase NTSC output cadence.");
    }

    const MediaDecodeResult output_decode_result = MediaDecoder::decode(output_path);
    if (!output_decode_result.succeeded()) {
        const std::string error_message =
            "The coarse-timebase NTSC output decode failed unexpectedly: " +
            output_decode_result.error->message +
            " Hint: " +
            output_decode_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto structure_result = assert_output_decode(*output_decode_result.decoded_media_source, 48U, true);
    if (structure_result != 0) {
        return structure_result;
    }

    const auto av_duration_delta = std::llabs(
        decoded_video_end_microseconds(*output_decode_result.decoded_media_source) -
        decoded_audio_end_microseconds(*output_decode_result.decoded_media_source)
    );
    if (av_duration_delta > 100000) {
        return fail("The coarse-timebase NTSC output audio/video durations drifted too far apart.");
    }

    const auto observer_result = assert_observer_flow(observer, 1, false);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto fine_progress_result = assert_fine_encode_progress(observer, summary);
    if (fine_progress_result != 0) {
        return fine_progress_result;
    }

    const auto runtime_visibility_result = assert_runtime_visibility(observer, summary);
    if (runtime_visibility_result != 0) {
        return runtime_visibility_result;
    }

    std::cout << build_validation_report(
        *job_result.encode_job_summary,
        *output_decode_result.decoded_media_source
    ) << '\n';
    return 0;
}

int run_irregular_timestamp_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    const MediaInspectionResult input_inspection_result = MediaInspector::inspect(main_path);
    if (!input_inspection_result.succeeded() ||
        !input_inspection_result.media_source_info->primary_video_stream.has_value()) {
        return fail("The irregular-timestamp sample could not be inspected.");
    }

    const MediaDecodeResult input_decode_result = MediaDecoder::decode(main_path);
    if (!input_decode_result.succeeded()) {
        const std::string error_message =
            "The irregular-timestamp sample could not be decoded: " +
            input_decode_result.error->message +
            " Hint: " +
            input_decode_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto &decoded_input = *input_decode_result.decoded_media_source;
    if (decoded_input.video_frames.empty() || decoded_input.audio_blocks.empty()) {
        return fail("The irregular-timestamp sample did not expose both video and audio.");
    }

    if (!has_irregular_video_timestamp_deltas(decoded_input)) {
        return fail("The irregular-timestamp sample unexpectedly decoded at a fixed frame cadence.");
    }

    CollectingObserver observer{};
    EncodeJob job{
        .input = {
            .main_source_path = main_path
        },
        .output = {
            .output_path = output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };
    job.output.audio.mode = AudioOutputMode::encode_aac;

    const EncodeJobResult job_result = EncodeJobRunner::run(job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!job_result.succeeded()) {
        const std::string error_message =
            "The irregular-timestamp encode job failed unexpectedly: " +
            job_result.error->message +
            " Hint: " +
            job_result.error->actionable_hint;
        return fail(error_message);
    }

    const MediaDecodeResult output_decode_result = MediaDecoder::decode(output_path);
    if (!output_decode_result.succeeded()) {
        const std::string error_message =
            "The irregular-timestamp output decode failed unexpectedly: " +
            output_decode_result.error->message +
            " Hint: " +
            output_decode_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto &decoded_output = *output_decode_result.decoded_media_source;
    if (assert_output_decode(decoded_output, decoded_input.video_frames.size(), true) != 0) {
        return 1;
    }

    if (!has_irregular_video_timestamp_deltas(decoded_output)) {
        return fail("The streaming encode output did not preserve monotonic irregular video timestamps.");
    }

    const auto av_duration_delta = std::llabs(
        decoded_video_end_microseconds(decoded_output) -
        decoded_audio_end_microseconds(decoded_output)
    );
    if (av_duration_delta > 100000) {
        return fail("The irregular-timestamp output audio/video durations drifted too far apart.");
    }

    const auto &input_video_stream = *input_inspection_result.media_source_info->primary_video_stream;
    const auto &summary = *job_result.encode_job_summary;
    const auto expected_output_video_time_base = choose_expected_output_video_time_base(
        input_video_stream.timestamps.time_base,
        input_video_stream.average_frame_rate
    );
    if (format_rational(summary.timeline_summary.output_frame_rate) !=
            format_rational(input_video_stream.average_frame_rate) ||
        format_rational(summary.timeline_summary.output_video_time_base) !=
            format_rational(expected_output_video_time_base) ||
        !summary.timeline_summary.output_audio_time_base.has_value() ||
        format_rational(*summary.timeline_summary.output_audio_time_base) != "1/48000" ||
        summary.timeline_summary.output_video_frame_count !=
            static_cast<std::int64_t>(decoded_input.video_frames.size()) ||
        summary.encoded_media_summary.resolved_audio_output.resolved_mode !=
            ResolvedAudioOutputMode::encode_aac) {
        return fail("Unexpected summary state for the irregular-timestamp encode job.");
    }

    const auto output_duration_delta = std::llabs(
        summary.timeline_summary.output_duration_microseconds -
        decoded_video_end_microseconds(decoded_output)
    );
    if (output_duration_delta > 100000) {
        return fail("The irregular-timestamp summary duration drifted too far from decoded output video timing.");
    }

    const auto observer_result = assert_observer_flow(observer, 1, false);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto fine_progress_result = assert_fine_encode_progress(observer, summary);
    if (fine_progress_result != 0) {
        return fine_progress_result;
    }

    const auto runtime_visibility_result = assert_runtime_visibility(observer, summary);
    if (runtime_visibility_result != 0) {
        return runtime_visibility_result;
    }

    std::cout << build_validation_report(
        *job_result.encode_job_summary,
        decoded_output
    ) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        return fail(
            "Usage: utsure_core_encode_job_tests "
            "[--threading-modes] | "
            "[--h264|--h265] <input> <output> | "
            "[--timeline-h264] <intro> <main> <outro> <output> | "
            "[--streaming-memory-budget] <input> <output> | "
            "[--disable-audio] <input> <output> | "
            "[--copy-audio] <input> <output> | "
            "[--coarse-timebase-ntsc] <input> <output> | "
            "[--irregular-timestamps] <input> <output>"
        );
    }

    const std::string_view mode(argv[1]);

    if (mode == "--threading-modes" && argc == 2) {
        return run_threading_mode_selection_assertion();
    }

    if (mode == "--h264" && argc == 4) {
        return run_main_only_job_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            OutputVideoCodec::h264,
            "h264"
        );
    }

    if (mode == "--h265" && argc == 4) {
        return run_main_only_job_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            OutputVideoCodec::h265,
            "hevc"
        );
    }

    if (mode == "--timeline-h264" && argc == 6) {
        return run_timeline_h264_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5])
        );
    }

    if (mode == "--streaming-memory-budget" && argc == 4) {
        return run_streaming_memory_budget_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    if (mode == "--disable-audio" && argc == 4) {
        return run_disable_audio_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    if (mode == "--copy-audio" && argc == 4) {
        return run_copy_audio_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    if (mode == "--coarse-timebase-ntsc" && argc == 4) {
        return run_coarse_timebase_ntsc_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    if (mode == "--irregular-timestamps" && argc == 4) {
        return run_irregular_timestamp_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    return fail("Unknown mode or wrong argument count for utsure_core_encode_job_tests.");
}
