#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/job/encode_job_report.hpp"
#include "utsure/core/media/media_decoder.hpp"

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

constexpr std::string_view kExpectedH264Report =
    "job.input.main_source=inspection-sample.avi\n"
    "job.subtitles.present=no\n"
    "job.output.path=job-sample-h264.mp4\n"
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
    "subtitles.burned_video_frames=0\n"
    "output.container=mov,mp4,m4a,3gp,3g2,mj2\n"
    "output.encoded_video_frames=48\n"
    "output.video.present=yes\n"
    "output.video.codec=h264\n"
    "output.video.resolution=320x180\n"
    "output.video.pixel_format=yuv420p\n"
    "output.video.average_frame_rate=24/1\n"
    "output.audio.present=no\n"
    "verified.output.video_frames=48\n"
    "verified.output.audio_blocks=0\n"
    "verified.output.frame0.start_us=0\n"
    "verified.output.frame1.start_us=41667\n"
    "verified.output.frame2.start_us=83333";

constexpr std::string_view kExpectedH265Report =
    "job.input.main_source=inspection-sample.avi\n"
    "job.subtitles.present=no\n"
    "job.output.path=job-sample-h265.mp4\n"
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
    "subtitles.burned_video_frames=0\n"
    "output.container=mov,mp4,m4a,3gp,3g2,mj2\n"
    "output.encoded_video_frames=48\n"
    "output.video.present=yes\n"
    "output.video.codec=hevc\n"
    "output.video.resolution=320x180\n"
    "output.video.pixel_format=yuv420p\n"
    "output.video.average_frame_rate=24/1\n"
    "output.audio.present=no\n"
    "verified.output.video_frames=48\n"
    "verified.output.audio_blocks=0\n"
    "verified.output.frame0.start_us=0\n"
    "verified.output.frame1.start_us=41667\n"
    "verified.output.frame2.start_us=83333";

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int assert_decoded_output(const DecodedMediaSource &decoded_output) {
    if (decoded_output.video_frames.size() != 48) {
        return fail("Unexpected job-output video frame count.");
    }

    if (!decoded_output.audio_blocks.empty()) {
        return fail("The current job path should still emit video-only outputs.");
    }

    if (decoded_output.video_frames[0].timestamp.start_microseconds != 0 ||
        decoded_output.video_frames[1].timestamp.start_microseconds != 41667 ||
        decoded_output.video_frames[2].timestamp.start_microseconds != 83333) {
        return fail("Unexpected job-output timestamp sequence.");
    }

    for (std::size_t index = 1; index < decoded_output.video_frames.size(); ++index) {
        if (decoded_output.video_frames[index].timestamp.start_microseconds <=
            decoded_output.video_frames[index - 1].timestamp.start_microseconds) {
            return fail("Job-output timestamps are not strictly increasing.");
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

int run_job_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &output_path,
    const OutputVideoCodec codec,
    const std::string_view expected_report
) {
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

    const EncodeJobResult job_result = EncodeJobRunner::run(job);
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

    const auto structure_result = assert_decoded_output(*output_decode_result.decoded_media_source);
    if (structure_result != 0) {
        return structure_result;
    }

    const std::string actual_report = build_validation_report(
        *job_result.encode_job_summary,
        *output_decode_result.decoded_media_source
    );
    std::cout << actual_report << '\n';

    if (actual_report != expected_report) {
        std::cerr << "Expected job report:\n" << expected_report << "\n";
        std::cerr << "Actual job report:\n" << actual_report << "\n";
        return 1;
    }

    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 4) {
        return fail("Usage: utsure_core_encode_job_tests [--h264|--h265] <input> <output>");
    }

    const std::string_view mode(argv[1]);
    const std::filesystem::path input_path(argv[2]);
    const std::filesystem::path output_path(argv[3]);

    if (mode == "--h264") {
        return run_job_assertion(input_path, output_path, OutputVideoCodec::h264, kExpectedH264Report);
    }

    if (mode == "--h265") {
        return run_job_assertion(input_path, output_path, OutputVideoCodec::h265, kExpectedH265Report);
    }

    return fail("Unknown mode. Use --h264 or --h265.");
}
