#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/job/encode_job_report.hpp"
#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using utsure::core::job::EncodeJob;
using utsure::core::job::EncodeJobResult;
using utsure::core::job::EncodeJobRunner;
using utsure::core::job::EncodeJobSummary;
using utsure::core::job::format_encode_job_report;
using utsure::core::media::DecodedMediaSource;
using utsure::core::media::MediaDecodeResult;
using utsure::core::media::MediaDecoder;
using utsure::core::media::OutputVideoCodec;
using utsure::core::media::Rational;
using utsure::core::subtitles::SubtitleRenderRequest;
using utsure::core::subtitles::SubtitleRenderResult;
using utsure::core::subtitles::SubtitleRenderSessionCreateRequest;
using utsure::core::subtitles::create_default_subtitle_renderer;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

std::string format_rational(const Rational &value) {
    if (!value.is_valid()) {
        return "unknown";
    }

    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

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

bool frames_are_identical(
    const DecodedMediaSource &left,
    const DecodedMediaSource &right,
    const std::size_t frame_index
) {
    return left.video_frames[frame_index].planes.front().bytes == right.video_frames[frame_index].planes.front().bytes;
}

bool any_frame_changed_in_range(
    const DecodedMediaSource &plain_output,
    const DecodedMediaSource &burned_output,
    const std::size_t start_index,
    const std::size_t frame_count
) {
    for (std::size_t index = 0; index < frame_count; ++index) {
        if (!frames_are_identical(plain_output, burned_output, start_index + index)) {
            return true;
        }
    }

    return false;
}

bool frame_changed(
    const DecodedMediaSource &plain_output,
    const DecodedMediaSource &burned_output,
    const std::size_t frame_index
) {
    return !frames_are_identical(plain_output, burned_output, frame_index);
}

int assert_decoded_output(const DecodedMediaSource &decoded_output, const std::size_t expected_frame_count) {
    if (decoded_output.video_frames.size() != expected_frame_count) {
        return fail("Unexpected burned-output video frame count.");
    }

    if (!decoded_output.audio_blocks.empty()) {
        return fail("The subtitle burn-in path should still emit video-only outputs.");
    }

    for (std::size_t index = 1; index < decoded_output.video_frames.size(); ++index) {
        if (decoded_output.video_frames[index].timestamp.start_microseconds <=
            decoded_output.video_frames[index - 1].timestamp.start_microseconds) {
            return fail("Unexpected burned-output timestamp sequence.");
        }
    }

    return 0;
}

std::string build_validation_report(
    const EncodeJobSummary &encode_job_summary,
    const DecodedMediaSource &plain_output,
    const DecodedMediaSource &burned_output
) {
    std::string report = format_encode_job_report(encode_job_summary);
    report += "\nverified.output.video_frames=" + std::to_string(burned_output.video_frames.size());
    report += "\nverified.output.frame0.start_us=" +
              std::to_string(burned_output.video_frames[0].timestamp.start_microseconds);
    report += "\nverified.output.frame1.start_us=" +
              std::to_string(burned_output.video_frames[1].timestamp.start_microseconds);
    report += "\nverified.output.frame0.changed=" +
              std::string(frames_are_identical(plain_output, burned_output, 0U) ? "no" : "yes");
    return report;
}

int run_render_assertion(const std::filesystem::path &subtitle_path) {
    auto subtitle_renderer = create_default_subtitle_renderer();
    if (!subtitle_renderer) {
        return fail("The default subtitle renderer could not be created.");
    }

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = subtitle_path,
        .format_hint = "ass",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    };

    auto session_result = subtitle_renderer->create_session(session_request);
    if (!session_result.succeeded()) {
        const std::string error_message =
            "libassmod session creation failed unexpectedly: " +
            session_result.error->message +
            " Hint: " +
            session_result.error->actionable_hint;
        return fail(error_message);
    }

    const SubtitleRenderResult visible_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 41667
    });
    if (!visible_result.succeeded()) {
        const std::string error_message =
            "Visible subtitle render failed unexpectedly: " +
            visible_result.error->message +
            " Hint: " +
            visible_result.error->actionable_hint;
        return fail(error_message);
    }

    const SubtitleRenderResult hidden_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 500000
    });
    if (!hidden_result.succeeded()) {
        const std::string error_message =
            "Hidden subtitle render failed unexpectedly: " +
            hidden_result.error->message +
            " Hint: " +
            hidden_result.error->actionable_hint;
        return fail(error_message);
    }

    if (visible_result.rendered_frame->bitmaps.empty()) {
        return fail("Expected visible subtitle content at 41667 us.");
    }

    if (!hidden_result.rendered_frame->bitmaps.empty()) {
        return fail("Expected no subtitle content at 500000 us.");
    }

    std::cout << "session.subtitle_path=" << format_path_leaf(subtitle_path) << '\n';
    std::cout << "session.format_hint=ass\n";
    std::cout << "session.canvas=320x180\n";
    std::cout << "session.sample_aspect_ratio=" << format_rational(session_request.sample_aspect_ratio) << '\n';
    std::cout << "visible.timestamp_us=41667\n";
    std::cout << "visible.has_content=yes\n";
    std::cout << "hidden.timestamp_us=500000\n";
    std::cout << "hidden.has_content=no\n";
    return 0;
}

int run_burn_in_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path,
    const OutputVideoCodec codec
) {
    const EncodeJob plain_job{
        .input = {
            .main_source_path = sample_path
        },
        .output = {
            .output_path = plain_output_path,
            .video = {
                .codec = codec,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJob burned_job{
        .input = {
            .main_source_path = sample_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = codec,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    if (!plain_job_result.succeeded()) {
        return fail("Plain encode job failed unexpectedly before subtitle comparison.");
    }

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job);
    if (!burned_job_result.succeeded()) {
        return fail("Subtitle burn-in job failed unexpectedly.");
    }

    if (burned_job_result.encode_job_summary->subtitled_video_frame_count != 11) {
        return fail("Unexpected count of subtitled video frames in the burn-in summary.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("Subtitle burn-in output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, 48U) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, 48U) != 0) {
        return 1;
    }

    if (frames_are_identical(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("Subtitle burn-in did not alter the first output frame.");
    }

    std::cout << build_validation_report(
        *burned_job_result.encode_job_summary,
        *plain_output_decode.decoded_media_source,
        *burned_output_decode.decoded_media_source
    ) << '\n';
    return 0;
}

int run_timeline_burn_in_assertion(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path
) {
    const EncodeJob plain_job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .output = {
            .output_path = plain_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJob burned_job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job);
    if (!plain_job_result.succeeded() || !burned_job_result.succeeded()) {
        return fail("Timeline subtitle burn-in jobs failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    if (summary.timeline_summary.segments.size() != 3 ||
        summary.timeline_summary.segments[0].subtitles_enabled ||
        !summary.timeline_summary.segments[1].subtitles_enabled ||
        summary.timeline_summary.segments[2].subtitles_enabled) {
        return fail("Timeline subtitle scope did not stay on the main segment.");
    }

    if (summary.subtitled_video_frame_count != 11) {
        return fail("Unexpected count of subtitled video frames for the timeline burn-in path.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("Timeline subtitle output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, 96U) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, 96U) != 0) {
        return 1;
    }

    // Encoder prediction can let changed main-segment frames influence later compressed frames.
    // Keep this assertion anchored to the first intro frame, which should remain outside subtitle scope.
    if (frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("Timeline subtitle burn-in altered the first intro frame unexpectedly.");
    }

    if (!any_frame_changed_in_range(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 24U, 48U)) {
        return fail("Timeline subtitle burn-in did not alter any main-segment frames.");
    }

    if (!frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 24U)) {
        return fail("Timeline subtitle burn-in did not change the first main-segment frame.");
    }

    std::cout << "timeline.intro.frame0.changed=no\n";
    std::cout << "timeline.main.changed=yes\n";
    std::cout << "timeline.outro.scope=not_asserted_after_encode\n";
    std::cout << "timeline.subtitled_frames=" << summary.subtitled_video_frame_count << '\n';
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 3) {
        return fail(
            "Usage: utsure_core_subtitle_burn_in_tests "
            "[--render <subtitle>|--h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--h265 <input> <subtitle> <plain-output> <burned-output>|"
            "--timeline-h264 <intro> <main> <outro> <subtitle> <plain-output> <burned-output>]"
        );
    }

    const std::string_view mode(argv[1]);

    if (mode == "--render" && argc == 3) {
        return run_render_assertion(std::filesystem::path(argv[2]));
    }

    if (mode == "--h264" && argc == 6) {
        return run_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            OutputVideoCodec::h264
        );
    }

    if (mode == "--h265" && argc == 6) {
        return run_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            OutputVideoCodec::h265
        );
    }

    if (mode == "--timeline-h264" && argc == 8) {
        return run_timeline_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6]),
            std::filesystem::path(argv[7])
        );
    }

    return fail("Unknown mode or wrong argument count for utsure_core_subtitle_burn_in_tests.");
}
