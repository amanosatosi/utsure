#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/job/encode_job_report.hpp"
#include "utsure/core/media/media_decoder.hpp"

#include <cstddef>
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
using utsure::core::job::EncodeJobProgress;
using utsure::core::job::EncodeJobResult;
using utsure::core::job::EncodeJobRunner;
using utsure::core::job::EncodeJobRunOptions;
using utsure::core::job::EncodeJobStage;
using utsure::core::job::EncodeJobSummary;
using utsure::core::job::format_encode_job_report;
using utsure::core::media::DecodedMediaSource;
using utsure::core::media::MediaDecodeResult;
using utsure::core::media::MediaDecoder;
using utsure::core::media::OutputVideoCodec;
using utsure::core::media::Rational;
using utsure::core::timeline::TimelineSegmentKind;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
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

bool frames_are_identical(
    const DecodedMediaSource &decoded_output,
    const std::size_t left_index,
    const std::size_t right_index
) {
    return decoded_output.video_frames[left_index].planes.front().bytes ==
        decoded_output.video_frames[right_index].planes.front().bytes;
}

int assert_output_decode(
    const DecodedMediaSource &decoded_output,
    const std::size_t expected_frame_count
) {
    if (decoded_output.video_frames.size() != expected_frame_count) {
        return fail("Unexpected job-output video frame count.");
    }

    if (!decoded_output.audio_blocks.empty()) {
        return fail("The current encode-job output should still be video-only.");
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

    if (summary.encoded_media_summary.output_info.primary_video_stream->codec_name != expected_codec_name ||
        summary.encoded_media_summary.output_info.primary_audio_stream.has_value()) {
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

    if (summary.subtitled_video_frame_count != 0 ||
        summary.encoded_media_summary.output_info.primary_audio_stream.has_value()) {
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

    const auto structure_result = assert_output_decode(*output_decode_result.decoded_media_source, 48U);
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

    const auto structure_result = assert_output_decode(*output_decode_result.decoded_media_source, 96U);
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

    std::cout << build_validation_report(
        *job_result.encode_job_summary,
        *output_decode_result.decoded_media_source
    ) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 4) {
        return fail(
            "Usage: utsure_core_encode_job_tests "
            "[--h264|--h265] <input> <output> | "
            "[--timeline-h264] <intro> <main> <outro> <output>"
        );
    }

    const std::string_view mode(argv[1]);

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

    return fail("Unknown mode or wrong argument count for utsure_core_encode_job_tests.");
}
