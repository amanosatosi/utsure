#include "utsure/core/job/encode_job_preflight.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using utsure::core::job::EncodeJob;
using utsure::core::job::EncodeJobPreflight;
using utsure::core::job::EncodeJobPreflightIssueCode;
using utsure::core::job::EncodeJobPreflightIssueSeverity;
using utsure::core::job::EncodeJobPreflightResult;
using utsure::core::job::EncodeJobSubtitleSettings;
using utsure::core::job::format_encode_job_preview;
using utsure::core::media::AudioOutputMode;
using utsure::core::media::OutputVideoCodec;
using utsure::core::timeline::SubtitleTimingMode;
using utsure::core::timeline::TimelineSegmentKind;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

void remove_file_if_present(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

bool issues_contain(
    const EncodeJobPreflightResult &result,
    const EncodeJobPreflightIssueCode code,
    const EncodeJobPreflightIssueSeverity severity
) {
    for (const auto &issue : result.issues) {
        if (issue.code == code && issue.severity == severity) {
            return true;
        }
    }

    return false;
}

EncodeJob make_base_job(
    const std::filesystem::path &main_source_path,
    const std::filesystem::path &output_path
) {
    return EncodeJob{
        .input = {
            .main_source_path = main_source_path
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
}

int run_valid_preview_assertion(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &output_path
) {
    remove_file_if_present(output_path);

    auto job = make_base_job(main_path, output_path);
    job.input.intro_source_path = intro_path;
    job.input.outro_source_path = outro_path;
    job.subtitles = EncodeJobSubtitleSettings{
        .subtitle_path = subtitle_path,
        .format_hint = "ass",
        .timing_mode = SubtitleTimingMode::main_segment_only
    };

    const auto result = EncodeJobPreflight::inspect(job);
    if (!result.can_start_encode()) {
        return fail("A valid preflight job was rejected unexpectedly.");
    }

    if (!result.preview_summary.has_value()) {
        return fail("A valid preflight job did not produce a preview summary.");
    }

    const auto &preview = *result.preview_summary;
    if (preview.segment_count != 3 ||
        preview.segment_kinds.size() != 3 ||
        preview.segment_kinds[0] != TimelineSegmentKind::intro ||
        preview.segment_kinds[1] != TimelineSegmentKind::main ||
        preview.segment_kinds[2] != TimelineSegmentKind::outro) {
        return fail("Unexpected preview segment summary.");
    }

    if (!preview.main_source_info.primary_video_stream.has_value() ||
        preview.main_source_info.primary_video_stream->width != 320 ||
        preview.main_source_info.primary_video_stream->height != 180) {
        return fail("Unexpected preview video resolution.");
    }

    if (!preview.output_audio_present ||
        !preview.subtitles_enabled ||
        preview.output_exists ||
        preview.subtitle_timing_mode != SubtitleTimingMode::main_segment_only) {
        return fail("Unexpected preview output state.");
    }

    if (preview.source_audio_summary != "pcm_s16le 1ch 48000 Hz" ||
        preview.output_audio_summary != "AAC 192k 1ch 48000 Hz (Auto)") {
        return fail("Unexpected preview audio summary.");
    }

    const auto preview_text = format_encode_job_preview(preview);
    std::cout << preview_text << '\n';
    return 0;
}

int run_invalid_fps_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &bad_intro_path,
    const std::filesystem::path &output_path
) {
    remove_file_if_present(output_path);

    auto job = make_base_job(main_path, output_path);
    job.input.intro_source_path = bad_intro_path;

    const auto result = EncodeJobPreflight::inspect(job);
    if (result.can_start_encode()) {
        return fail("A mismatched-fps preflight job passed unexpectedly.");
    }

    if (!issues_contain(
            result,
            EncodeJobPreflightIssueCode::timeline_validation_failed,
            EncodeJobPreflightIssueSeverity::error)) {
        return fail("The mismatched-fps preflight job did not report the expected timeline error.");
    }

    if (result.preview_summary.has_value()) {
        return fail("A failed timeline preflight unexpectedly produced a preview summary.");
    }

    std::cout << "timeline_validation=failed\n";
    return 0;
}

int run_missing_subtitle_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &missing_subtitle_path,
    const std::filesystem::path &output_path
) {
    remove_file_if_present(output_path);

    auto job = make_base_job(main_path, output_path);
    job.subtitles = EncodeJobSubtitleSettings{
        .subtitle_path = missing_subtitle_path,
        .format_hint = "ass"
    };

    const auto result = EncodeJobPreflight::inspect(job);
    if (result.can_start_encode()) {
        return fail("A missing-subtitle preflight job passed unexpectedly.");
    }

    if (!issues_contain(
            result,
            EncodeJobPreflightIssueCode::invalid_subtitle_source,
            EncodeJobPreflightIssueSeverity::error)) {
        return fail("The missing-subtitle preflight job did not report the expected subtitle error.");
    }

    std::cout << "subtitle_validation=failed\n";
    return 0;
}

int run_overwrite_warning_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    {
        std::ofstream output_file(output_path);
        output_file << "placeholder";
    }

    const auto result = EncodeJobPreflight::inspect(make_base_job(main_path, output_path));
    remove_file_if_present(output_path);

    if (!result.can_start_encode()) {
        return fail("An overwrite-warning preflight job was blocked unexpectedly.");
    }

    if (!result.requires_output_overwrite_confirmation()) {
        return fail("The overwrite-warning preflight job did not request overwrite confirmation.");
    }

    if (!issues_contain(
            result,
            EncodeJobPreflightIssueCode::output_will_be_overwritten,
            EncodeJobPreflightIssueSeverity::warning)) {
        return fail("The overwrite-warning preflight job did not emit the expected warning.");
    }

    std::cout << "overwrite_warning=yes\n";
    return 0;
}

int run_output_conflicts_main_assertion(const std::filesystem::path &main_path) {
    const auto result = EncodeJobPreflight::inspect(make_base_job(main_path, main_path));

    if (result.can_start_encode()) {
        return fail("A preflight job that writes over the main source passed unexpectedly.");
    }

    if (!issues_contain(
            result,
            EncodeJobPreflightIssueCode::invalid_output_path,
            EncodeJobPreflightIssueSeverity::error)) {
        return fail("The output-conflicts-main preflight job did not report the expected output-path error.");
    }

    if (!issues_contain(
            result,
            EncodeJobPreflightIssueCode::output_will_be_overwritten,
            EncodeJobPreflightIssueSeverity::warning)) {
        return fail("The output-conflicts-main preflight job did not report the expected overwrite warning.");
    }

    if (result.preview_summary.has_value()) {
        return fail("An output-conflicts-main preflight job unexpectedly produced a preview summary.");
    }

    std::cout << "output_conflict=main_source\n";
    return 0;
}

int run_streaming_memory_budget_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    remove_file_if_present(output_path);

    const auto result = EncodeJobPreflight::inspect(make_base_job(main_path, output_path));
    if (!result.can_start_encode()) {
        return fail("A streaming-memory preflight job was blocked unexpectedly.");
    }

    if (issues_contain(
            result,
            EncodeJobPreflightIssueCode::working_set_limit_exceeded,
            EncodeJobPreflightIssueSeverity::error)) {
        return fail("The streaming-memory preflight job still reported the old working-set error.");
    }

    if (!result.preview_summary.has_value()) {
        return fail("A streaming-memory preflight job should still report a preview summary.");
    }

    const auto &preview = *result.preview_summary;
    if (!preview.main_source_info.primary_video_stream.has_value() ||
        preview.main_source_info.primary_video_stream->width != 1920 ||
        preview.main_source_info.primary_video_stream->height != 1080) {
        return fail("Unexpected preview resolution for the streaming-memory job.");
    }

    std::cout << "streaming_memory_budget=allowed\n";
    return 0;
}

int run_copy_blocked_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    remove_file_if_present(output_path);

    auto job = make_base_job(main_path, output_path);
    job.output.audio.mode = AudioOutputMode::copy_source;

    const auto result = EncodeJobPreflight::inspect(job);
    if (result.can_start_encode()) {
        return fail("A copy-blocked preflight job passed unexpectedly.");
    }

    if (!issues_contain(
            result,
            EncodeJobPreflightIssueCode::invalid_audio_settings,
            EncodeJobPreflightIssueSeverity::error)) {
        return fail("The copy-blocked preflight job did not report the expected audio-settings error.");
    }

    if (!result.preview_summary.has_value()) {
        return fail("A copy-blocked preflight job should still produce a preview summary.");
    }

    if (result.preview_summary->output_audio_summary != "Copy blocked") {
        return fail("The copy-blocked preflight job did not report the expected output audio summary.");
    }

    std::cout << "audio_copy=blocked\n";
    return 0;
}

int run_auto_copy_preview_assertion(
    const std::filesystem::path &main_path,
    const std::filesystem::path &output_path
) {
    remove_file_if_present(output_path);

    const auto result = EncodeJobPreflight::inspect(make_base_job(main_path, output_path));
    if (!result.can_start_encode()) {
        return fail("An auto-copy-safe preflight job was blocked unexpectedly.");
    }

    if (!result.preview_summary.has_value()) {
        return fail("An auto-copy-safe preflight job did not produce a preview summary.");
    }

    const auto &preview = *result.preview_summary;
    if (!preview.output_audio_present ||
        preview.source_audio_summary != "aac 1ch 48000 Hz" ||
        preview.output_audio_summary != "Copy source aac 1ch 48000 Hz (Auto)") {
        return fail("Unexpected auto-copy-safe preview audio summary.");
    }

    std::cout << "audio_copy=auto_safe\n";
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        return fail(
            "Usage: utsure_core_encode_job_preflight_tests "
            "[--valid-preview <intro> <main> <outro> <subtitle> <output>|"
            "--invalid-fps <main> <bad-intro> <output>|"
            "--missing-subtitle <main> <missing-subtitle> <output>|"
            "--overwrite-warning <main> <output>|"
            "--output-conflicts-main <main>|"
            "--streaming-memory-budget <main> <output>|"
            "--copy-blocked <main> <output>|"
            "--auto-copy-preview <main> <output>]"
        );
    }

    const std::string_view mode(argv[1]);

    if (mode == "--valid-preview" && argc == 7) {
        return run_valid_preview_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6])
        );
    }

    if (mode == "--invalid-fps" && argc == 5) {
        return run_invalid_fps_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4])
        );
    }

    if (mode == "--missing-subtitle" && argc == 5) {
        return run_missing_subtitle_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4])
        );
    }

    if (mode == "--overwrite-warning" && argc == 4) {
        return run_overwrite_warning_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    if (mode == "--output-conflicts-main" && argc == 3) {
        return run_output_conflicts_main_assertion(std::filesystem::path(argv[2]));
    }

    if (mode == "--streaming-memory-budget" && argc == 4) {
        return run_streaming_memory_budget_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    if (mode == "--copy-blocked" && argc == 4) {
        return run_copy_blocked_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    if (mode == "--auto-copy-preview" && argc == 4) {
        return run_auto_copy_preview_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3])
        );
    }

    return fail("Unknown mode or wrong argument count for utsure_core_encode_job_preflight_tests.");
}
