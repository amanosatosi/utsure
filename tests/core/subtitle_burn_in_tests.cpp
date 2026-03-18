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

constexpr std::string_view kExpectedRenderReport =
    "session.subtitle_path=subtitle-burn-sample.ass\n"
    "session.format_hint=ass\n"
    "session.canvas=320x180\n"
    "session.sample_aspect_ratio=1/1\n"
    "visible.timestamp_us=41667\n"
    "visible.has_content=yes\n"
    "hidden.timestamp_us=500000\n"
    "hidden.has_content=no";

constexpr std::string_view kExpectedH264Report =
    "job.input.main_source=inspection-sample.avi\n"
    "job.subtitles.present=yes\n"
    "job.subtitles.path=subtitle-burn-sample.ass\n"
    "job.subtitles.format_hint=ass\n"
    "job.output.path=subtitle-burn-h264.mp4\n"
    "job.output.video.codec=h264\n"
    "job.output.video.preset=medium\n"
    "job.output.video.crf=23\n"
    "decode.policy.video_pixel_format=rgba8\n"
    "decode.policy.audio_sample_format=f32_planar\n"
    "decode.policy.audio_block_samples=1024\n"
    "input.container=avi\n"
    "input.video.present=yes\n"
    "input.video.codec=rawvideo\n"
    "input.video.average_frame_rate=24/1\n"
    "input.video.time_base=1/24\n"
    "input.audio.present=yes\n"
    "input.audio.codec=pcm_s16le\n"
    "input.audio.sample_rate=48000\n"
    "decoded.video_frames=48\n"
    "decoded.audio_blocks=94\n"
    "subtitles.burned_video_frames=11\n"
    "output.container=mov,mp4,m4a,3gp,3g2,mj2\n"
    "output.encoded_video_frames=48\n"
    "output.video.present=yes\n"
    "output.video.codec=h264\n"
    "output.video.resolution=320x180\n"
    "output.video.pixel_format=yuv420p\n"
    "output.video.average_frame_rate=24/1\n"
    "output.audio.present=no\n"
    "verified.output.video_frames=48\n"
    "verified.output.frame0.start_us=0\n"
    "verified.output.frame1.start_us=41667\n"
    "verified.output.frame0.changed=yes";

constexpr std::string_view kExpectedH265Report =
    "job.input.main_source=inspection-sample.avi\n"
    "job.subtitles.present=yes\n"
    "job.subtitles.path=subtitle-burn-sample.ass\n"
    "job.subtitles.format_hint=ass\n"
    "job.output.path=subtitle-burn-h265.mp4\n"
    "job.output.video.codec=h265\n"
    "job.output.video.preset=medium\n"
    "job.output.video.crf=23\n"
    "decode.policy.video_pixel_format=rgba8\n"
    "decode.policy.audio_sample_format=f32_planar\n"
    "decode.policy.audio_block_samples=1024\n"
    "input.container=avi\n"
    "input.video.present=yes\n"
    "input.video.codec=rawvideo\n"
    "input.video.average_frame_rate=24/1\n"
    "input.video.time_base=1/24\n"
    "input.audio.present=yes\n"
    "input.audio.codec=pcm_s16le\n"
    "input.audio.sample_rate=48000\n"
    "decoded.video_frames=48\n"
    "decoded.audio_blocks=94\n"
    "subtitles.burned_video_frames=11\n"
    "output.container=mov,mp4,m4a,3gp,3g2,mj2\n"
    "output.encoded_video_frames=48\n"
    "output.video.present=yes\n"
    "output.video.codec=hevc\n"
    "output.video.resolution=320x180\n"
    "output.video.pixel_format=yuv420p\n"
    "output.video.average_frame_rate=24/1\n"
    "output.audio.present=no\n"
    "verified.output.video_frames=48\n"
    "verified.output.frame0.start_us=0\n"
    "verified.output.frame1.start_us=41667\n"
    "verified.output.frame0.changed=yes";

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

    std::string actual_report;
    actual_report += "session.subtitle_path=" + format_path_leaf(subtitle_path);
    actual_report += "\nsession.format_hint=ass";
    actual_report += "\nsession.canvas=320x180";
    actual_report += "\nsession.sample_aspect_ratio=" + format_rational(session_request.sample_aspect_ratio);
    actual_report += "\nvisible.timestamp_us=41667";
    actual_report += "\nvisible.has_content=yes";
    actual_report += "\nhidden.timestamp_us=500000";
    actual_report += "\nhidden.has_content=no";
    std::cout << actual_report << '\n';

    if (actual_report != kExpectedRenderReport) {
        std::cerr << "Expected render report:\n" << kExpectedRenderReport << "\n";
        std::cerr << "Actual render report:\n" << actual_report << "\n";
        return 1;
    }

    return 0;
}

int assert_decoded_output(const DecodedMediaSource &decoded_output) {
    if (decoded_output.video_frames.size() != 48) {
        return fail("Unexpected burned-output video frame count.");
    }

    if (!decoded_output.audio_blocks.empty()) {
        return fail("The subtitle burn-in path should still emit video-only outputs.");
    }

    if (decoded_output.video_frames[0].timestamp.start_microseconds != 0 ||
        decoded_output.video_frames[1].timestamp.start_microseconds != 41667) {
        return fail("Unexpected burned-output timestamp sequence.");
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

int run_burn_in_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path,
    const OutputVideoCodec codec,
    const std::string_view expected_report
) {
    const EncodeJob plain_job{
        .input = {
            .main_source_path = sample_path
        },
        .subtitles = std::nullopt,
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
        const std::string error_message =
            "Plain encode job failed unexpectedly before subtitle comparison: " +
            plain_job_result.error->message +
            " Hint: " +
            plain_job_result.error->actionable_hint;
        return fail(error_message);
    }

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job);
    if (!burned_job_result.succeeded()) {
        const std::string error_message =
            "Subtitle burn-in job failed unexpectedly: " +
            burned_job_result.error->message +
            " Hint: " +
            burned_job_result.error->actionable_hint;
        return fail(error_message);
    }

    if (burned_job_result.encode_job_summary->subtitled_video_frame_count != 11) {
        return fail("Unexpected count of subtitled video frames in the burn-in summary.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    if (!plain_output_decode.succeeded()) {
        const std::string error_message =
            "Plain output decode failed unexpectedly: " +
            plain_output_decode.error->message +
            " Hint: " +
            plain_output_decode.error->actionable_hint;
        return fail(error_message);
    }

    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!burned_output_decode.succeeded()) {
        const std::string error_message =
            "Burned output decode failed unexpectedly: " +
            burned_output_decode.error->message +
            " Hint: " +
            burned_output_decode.error->actionable_hint;
        return fail(error_message);
    }

    const auto plain_structure_result = assert_decoded_output(*plain_output_decode.decoded_media_source);
    if (plain_structure_result != 0) {
        return plain_structure_result;
    }

    const auto burned_structure_result = assert_decoded_output(*burned_output_decode.decoded_media_source);
    if (burned_structure_result != 0) {
        return burned_structure_result;
    }

    if (frames_are_identical(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("Subtitle burn-in did not alter the first output frame.");
    }

    const std::string actual_report = build_validation_report(
        *burned_job_result.encode_job_summary,
        *plain_output_decode.decoded_media_source,
        *burned_output_decode.decoded_media_source
    );
    std::cout << actual_report << '\n';

    if (actual_report != expected_report) {
        std::cerr << "Expected burn-in report:\n" << expected_report << "\n";
        std::cerr << "Actual burn-in report:\n" << actual_report << "\n";
        return 1;
    }

    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 3) {
        return fail(
            "Usage: utsure_core_subtitle_burn_in_tests "
            "[--render <subtitle>|--h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--h265 <input> <subtitle> <plain-output> <burned-output>]"
        );
    }

    const std::string_view mode(argv[1]);

    if (mode == "--render") {
        if (argc != 3) {
            return fail("Usage: utsure_core_subtitle_burn_in_tests --render <subtitle>");
        }

        return run_render_assertion(std::filesystem::path(argv[2]));
    }

    if (mode == "--h264") {
        if (argc != 6) {
            return fail(
                "Usage: utsure_core_subtitle_burn_in_tests --h264 "
                "<input> <subtitle> <plain-output> <burned-output>"
            );
        }

        return run_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            OutputVideoCodec::h264,
            kExpectedH264Report
        );
    }

    if (mode == "--h265") {
        if (argc != 6) {
            return fail(
                "Usage: utsure_core_subtitle_burn_in_tests --h265 "
                "<input> <subtitle> <plain-output> <burned-output>"
            );
        }

        return run_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            OutputVideoCodec::h265,
            kExpectedH265Report
        );
    }

    return fail("Unknown mode. Use --render, --h264, or --h265.");
}
