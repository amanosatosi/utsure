#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/media/media_encoder.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using utsure::core::media::DecodedMediaSource;
using utsure::core::media::EncodedMediaSummary;
using utsure::core::media::MediaDecodeResult;
using utsure::core::media::MediaDecoder;
using utsure::core::media::MediaEncodeRequest;
using utsure::core::media::MediaEncodeResult;
using utsure::core::media::MediaEncoder;
using utsure::core::media::OutputVideoCodec;
using utsure::core::media::Rational;
using utsure::core::media::to_string;

constexpr std::string_view kExpectedH264Summary =
    "request.codec=h264\n"
    "request.preset=medium\n"
    "request.crf=23\n"
    "output.container=mov,mp4,m4a,3gp,3g2,mj2\n"
    "output.video.codec=h264\n"
    "output.video.resolution=320x180\n"
    "output.video.pixel_format=yuv420p\n"
    "output.video.average_frame_rate=24/1\n"
    "output.audio.present=no\n"
    "decoded_output.video_frames=48\n"
    "decoded_output.audio_blocks=0\n"
    "decoded_output.frame0.start_us=0\n"
    "decoded_output.frame1.start_us=41667\n"
    "decoded_output.frame2.start_us=83333";

constexpr std::string_view kExpectedH265Summary =
    "request.codec=h265\n"
    "request.preset=medium\n"
    "request.crf=23\n"
    "output.container=mov,mp4,m4a,3gp,3g2,mj2\n"
    "output.video.codec=hevc\n"
    "output.video.resolution=320x180\n"
    "output.video.pixel_format=yuv420p\n"
    "output.video.average_frame_rate=24/1\n"
    "output.audio.present=no\n"
    "decoded_output.video_frames=48\n"
    "decoded_output.audio_blocks=0\n"
    "decoded_output.frame0.start_us=0\n"
    "decoded_output.frame1.start_us=41667\n"
    "decoded_output.frame2.start_us=83333";

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

int assert_decoded_output(const DecodedMediaSource &decoded_output) {
    if (decoded_output.video_frames.size() != 48) {
        return fail("Unexpected encoded-output video frame count.");
    }

    if (!decoded_output.audio_blocks.empty()) {
        return fail("The v1 software encoder path should currently emit video-only outputs.");
    }

    if (decoded_output.video_frames[0].timestamp.start_microseconds != 0 ||
        decoded_output.video_frames[1].timestamp.start_microseconds != 41667 ||
        decoded_output.video_frames[2].timestamp.start_microseconds != 83333) {
        return fail("Unexpected encoded-output timestamp sequence.");
    }

    for (std::size_t index = 1; index < decoded_output.video_frames.size(); ++index) {
        if (decoded_output.video_frames[index].timestamp.start_microseconds <=
            decoded_output.video_frames[index - 1].timestamp.start_microseconds) {
            return fail("Encoded-output timestamps are not strictly increasing.");
        }
    }

    return 0;
}

std::string build_summary(
    const EncodedMediaSummary &encoded_media_summary,
    const DecodedMediaSource &decoded_output
) {
    std::ostringstream summary;
    summary << "request.codec=" << to_string(encoded_media_summary.video_settings.codec) << '\n';
    summary << "request.preset=" << encoded_media_summary.video_settings.preset << '\n';
    summary << "request.crf=" << encoded_media_summary.video_settings.crf << '\n';
    summary << "output.container=" << encoded_media_summary.output_info.container_format_name << '\n';

    if (!encoded_media_summary.output_info.primary_video_stream.has_value()) {
        summary << "output.video.codec=unknown\n";
        summary << "output.video.resolution=0x0\n";
        summary << "output.video.pixel_format=unknown\n";
        summary << "output.video.average_frame_rate=unknown\n";
    } else {
        const auto &video = *encoded_media_summary.output_info.primary_video_stream;
        summary << "output.video.codec=" << video.codec_name << '\n';
        summary << "output.video.resolution=" << video.width << 'x' << video.height << '\n';
        summary << "output.video.pixel_format=" << video.pixel_format_name << '\n';
        summary << "output.video.average_frame_rate=" << format_rational(video.average_frame_rate) << '\n';
    }

    summary << "output.audio.present="
            << (encoded_media_summary.output_info.primary_audio_stream.has_value() ? "yes" : "no") << '\n';
    summary << "decoded_output.video_frames=" << decoded_output.video_frames.size() << '\n';
    summary << "decoded_output.audio_blocks=" << decoded_output.audio_blocks.size() << '\n';
    summary << "decoded_output.frame0.start_us=" << decoded_output.video_frames[0].timestamp.start_microseconds << '\n';
    summary << "decoded_output.frame1.start_us=" << decoded_output.video_frames[1].timestamp.start_microseconds << '\n';
    summary << "decoded_output.frame2.start_us=" << decoded_output.video_frames[2].timestamp.start_microseconds;
    return summary.str();
}

int run_encode_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &output_path,
    const OutputVideoCodec codec,
    const std::string_view expected_summary
) {
    const MediaDecodeResult decode_result = MediaDecoder::decode(sample_path);
    if (!decode_result.succeeded()) {
        const std::string error_message =
            "Sample decode failed unexpectedly before encode: " +
            decode_result.error->message +
            " Hint: " +
            decode_result.error->actionable_hint;
        return fail(error_message);
    }

    const MediaEncodeRequest request{
        .output_path = output_path,
        .video_settings = {
            .codec = codec,
            .preset = "medium",
            .crf = 23
        }
    };

    const MediaEncodeResult encode_result = MediaEncoder::encode(*decode_result.decoded_media_source, request);
    if (!encode_result.succeeded()) {
        const std::string error_message =
            "Encode failed unexpectedly: " +
            encode_result.error->message +
            " Hint: " +
            encode_result.error->actionable_hint;
        return fail(error_message);
    }

    const MediaDecodeResult encoded_output_decode = MediaDecoder::decode(output_path);
    if (!encoded_output_decode.succeeded()) {
        const std::string error_message =
            "Encoded output decode failed unexpectedly: " +
            encoded_output_decode.error->message +
            " Hint: " +
            encoded_output_decode.error->actionable_hint;
        return fail(error_message);
    }

    const auto structure_result = assert_decoded_output(*encoded_output_decode.decoded_media_source);
    if (structure_result != 0) {
        return structure_result;
    }

    const std::string actual_summary = build_summary(
        *encode_result.encoded_media_summary,
        *encoded_output_decode.decoded_media_source
    );
    std::cout << actual_summary << '\n';

    if (actual_summary != expected_summary) {
        std::cerr << "Expected encode summary:\n" << expected_summary << "\n";
        std::cerr << "Actual encode summary:\n" << actual_summary << "\n";
        return 1;
    }

    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 4) {
        return fail("Usage: utsure_core_media_encode_tests [--h264|--h265] <input> <output>");
    }

    const std::string_view mode(argv[1]);
    const std::filesystem::path input_path(argv[2]);
    const std::filesystem::path output_path(argv[3]);

    if (mode == "--h264") {
        return run_encode_assertion(input_path, output_path, OutputVideoCodec::h264, kExpectedH264Summary);
    }

    if (mode == "--h265") {
        return run_encode_assertion(input_path, output_path, OutputVideoCodec::h265, kExpectedH265Summary);
    }

    return fail("Unknown mode. Use --h264 or --h265.");
}
