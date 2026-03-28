#include "utsure/core/media/media_decode_report.hpp"
#include "utsure/core/media/media_decoder.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using utsure::core::media::DecodedMediaSource;
using utsure::core::media::DecodeNormalizationPolicy;
using utsure::core::media::MediaDecodeResult;
using utsure::core::media::MediaDecoder;
using utsure::core::media::NormalizedAudioSampleFormat;
using utsure::core::media::NormalizedVideoPixelFormat;
using utsure::core::media::TimestampOrigin;
using utsure::core::media::format_media_decode_report;

constexpr std::string_view kExpectedSampleReport =
    "input.name=inspection-sample.avi\n"
    "policy.video_pixel_format=rgba8\n"
    "policy.audio_sample_format=f32_planar\n"
    "policy.audio_block_samples=1024\n"
    "video.present=yes\n"
    "video.time_base=1/24\n"
    "video.average_frame_rate=24/1\n"
    "video.frame_count=48\n"
    "video.frame[0].source_pts=0\n"
    "video.frame[0].source_duration=1\n"
    "video.frame[0].origin=decoded_pts\n"
    "video.frame[0].start_us=0\n"
    "video.frame[0].duration_us=41667\n"
    "video.frame[0].pixel_format=rgba8\n"
    "video.frame[0].resolution=320x180\n"
    "video.frame[0].planes=1\n"
    "video.frame[0].plane0.stride=1280\n"
    "video.frame[0].plane0.bytes=230400\n"
    "video.frame[1].source_pts=1\n"
    "video.frame[1].source_duration=1\n"
    "video.frame[1].origin=decoded_pts\n"
    "video.frame[1].start_us=41667\n"
    "video.frame[1].duration_us=41667\n"
    "video.frame[1].pixel_format=rgba8\n"
    "video.frame[1].resolution=320x180\n"
    "video.frame[1].planes=1\n"
    "video.frame[1].plane0.stride=1280\n"
    "video.frame[1].plane0.bytes=230400\n"
    "video.frame[2].source_pts=2\n"
    "video.frame[2].source_duration=1\n"
    "video.frame[2].origin=decoded_pts\n"
    "video.frame[2].start_us=83333\n"
    "video.frame[2].duration_us=41667\n"
    "video.frame[2].pixel_format=rgba8\n"
    "video.frame[2].resolution=320x180\n"
    "video.frame[2].planes=1\n"
    "video.frame[2].plane0.stride=1280\n"
    "video.frame[2].plane0.bytes=230400\n"
    "audio.present=yes\n"
    "audio.time_base=1/48000\n"
    "audio.sample_rate=48000\n"
    "audio.channels=1\n"
    "audio.block_count=94\n"
    "audio.total_samples_per_channel=96000\n"
    "audio.block[0].source_pts=0\n"
    "audio.block[0].source_duration=1024\n"
    "audio.block[0].origin=stream_cursor\n"
    "audio.block[0].start_us=0\n"
    "audio.block[0].duration_us=21333\n"
    "audio.block[0].sample_format=f32_planar\n"
    "audio.block[0].sample_rate=48000\n"
    "audio.block[0].channels=1\n"
    "audio.block[0].samples_per_channel=1024\n"
    "audio.block[1].source_pts=1024\n"
    "audio.block[1].source_duration=1024\n"
    "audio.block[1].origin=stream_cursor\n"
    "audio.block[1].start_us=21333\n"
    "audio.block[1].duration_us=21333\n"
    "audio.block[1].sample_format=f32_planar\n"
    "audio.block[1].sample_rate=48000\n"
    "audio.block[1].channels=1\n"
    "audio.block[1].samples_per_channel=1024\n"
    "audio.block[2].source_pts=2048\n"
    "audio.block[2].source_duration=1024\n"
    "audio.block[2].origin=stream_cursor\n"
    "audio.block[2].start_us=42667\n"
    "audio.block[2].duration_us=21333\n"
    "audio.block[2].sample_format=f32_planar\n"
    "audio.block[2].sample_rate=48000\n"
    "audio.block[2].channels=1\n"
    "audio.block[2].samples_per_channel=1024";

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int assert_decoded_structure(const DecodedMediaSource &decoded_media_source) {
    if (decoded_media_source.normalization_policy.video_pixel_format != NormalizedVideoPixelFormat::rgba8) {
        return fail("Unexpected normalized video pixel format.");
    }

    if (decoded_media_source.normalization_policy.audio_sample_format != NormalizedAudioSampleFormat::f32_planar) {
        return fail("Unexpected normalized audio sample format.");
    }

    if (decoded_media_source.normalization_policy.audio_block_samples != 1024) {
        return fail("Unexpected normalized audio block size.");
    }

    if (decoded_media_source.video_frames.size() != 48) {
        return fail("Unexpected decoded video frame count.");
    }

    if (decoded_media_source.audio_blocks.size() != 94) {
        return fail("Unexpected decoded audio block count.");
    }

    const auto &first_video_frame = decoded_media_source.video_frames.front();
    if (first_video_frame.timestamp.origin != TimestampOrigin::decoded_pts) {
        return fail("The first video frame should use decoded_pts timing.");
    }

    if (first_video_frame.planes.size() != 1 || first_video_frame.planes.front().bytes.size() != 230400) {
        return fail("Unexpected normalized video plane layout.");
    }

    const auto &first_audio_block = decoded_media_source.audio_blocks.front();
    if (first_audio_block.timestamp.origin != TimestampOrigin::stream_cursor) {
        return fail("The first audio block should use stream_cursor timing.");
    }

    if (first_audio_block.channel_samples.size() != 1 ||
        first_audio_block.channel_samples.front().size() != 1024) {
        return fail("Unexpected normalized audio channel layout.");
    }

    const auto &last_audio_block = decoded_media_source.audio_blocks.back();
    if (last_audio_block.samples_per_channel != 768) {
        return fail("Unexpected tail audio block size.");
    }

    return 0;
}

int run_sample_assertion(const std::filesystem::path &sample_path) {
    const MediaDecodeResult result = MediaDecoder::decode(sample_path);
    if (!result.succeeded()) {
        const std::string error_message =
            "Sample decode failed unexpectedly: " +
            result.error->message +
            " Hint: " +
            result.error->actionable_hint;
        return fail(error_message);
    }

    const auto &decoded_media_source = *result.decoded_media_source;
    const std::string actual_report = format_media_decode_report(decoded_media_source);
    std::cout << actual_report << '\n';

    if (actual_report != kExpectedSampleReport) {
        std::cerr << "Expected decode report:\n" << kExpectedSampleReport << "\n";
        std::cerr << "Actual decode report:\n" << actual_report << "\n";
        return 1;
    }

    return assert_decoded_structure(decoded_media_source);
}

int run_missing_input_assertion(const std::filesystem::path &missing_path) {
    const MediaDecodeResult result = MediaDecoder::decode(missing_path);
    if (result.succeeded() || !result.error.has_value()) {
        return fail("Missing-input decode unexpectedly succeeded.");
    }

    const std::string expected_message =
        "Cannot decode media input '" + missing_path.lexically_normal().string() + "': the file does not exist.";

    if (result.error->message != expected_message) {
        std::cerr << "Expected error message:\n" << expected_message << "\n";
        std::cerr << "Actual error message:\n" << result.error->message << "\n";
        return 1;
    }

    if (result.error->actionable_hint !=
        "Check that the path is correct and that the file has been created before decode.") {
        std::cerr << "Unexpected actionable hint:\n" << result.error->actionable_hint << "\n";
        return 1;
    }

    std::cout << result.error->message << '\n';
    std::cout << result.error->actionable_hint << '\n';
    return 0;
}

int run_preview_session_sequential_assertion(const std::filesystem::path &sample_path) {
    DecodeNormalizationPolicy normalization_policy{};
    normalization_policy.video_max_width = 320;
    normalization_policy.video_max_height = 180;

    auto session_result = MediaDecoder::create_video_preview_session(sample_path, normalization_policy);
    if (!session_result.succeeded()) {
        const std::string error_message =
            "Preview session creation failed unexpectedly: " +
            session_result.error->message +
            " Hint: " +
            session_result.error->actionable_hint;
        return fail(error_message);
    }

    constexpr std::size_t kPreviewWindowFrameCount = 96;
    auto first_window_result = session_result.session->seek_and_decode_window_at_time(0, kPreviewWindowFrameCount);
    if (!first_window_result.succeeded()) {
        const std::string error_message =
            "The initial preview seek window failed unexpectedly: " +
            first_window_result.error->message +
            " Hint: " +
            first_window_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto &first_window = *first_window_result.video_frames;
    if (first_window.size() != kPreviewWindowFrameCount) {
        return fail("The initial preview seek window did not fill the expected 96-frame cache window.");
    }

    auto second_window_result = session_result.session->decode_next_window(kPreviewWindowFrameCount);
    if (!second_window_result.succeeded()) {
        const std::string error_message =
            "The sequential preview window failed unexpectedly after the first 96-frame window: " +
            second_window_result.error->message +
            " Hint: " +
            second_window_result.error->actionable_hint;
        return fail(error_message);
    }

    const auto &second_window = *second_window_result.video_frames;
    if (second_window.empty()) {
        return fail("The second preview window unexpectedly returned no frames.");
    }

    if (second_window.front().timestamp.start_microseconds <=
        first_window.back().timestamp.start_microseconds) {
        return fail("The sequential preview window did not advance past the first 96-frame boundary.");
    }

    auto seek_after_sequential_result = session_result.session->seek_and_decode_window_at_time(1000000, 16);
    if (!seek_after_sequential_result.succeeded()) {
        const std::string error_message =
            "Preview seek failed unexpectedly after sequential window playback: " +
            seek_after_sequential_result.error->message +
            " Hint: " +
            seek_after_sequential_result.error->actionable_hint;
        return fail(error_message);
    }

    if (seek_after_sequential_result.video_frames->empty()) {
        return fail("Preview seek after sequential playback returned no frames.");
    }

    std::cout << "first_window.frames=" << first_window.size() << '\n';
    std::cout << "second_window.frames=" << second_window.size() << '\n';
    std::cout << "second_window.first_start_us=" << second_window.front().timestamp.start_microseconds << '\n';
    std::cout << "seek_after_sequential.frames=" << seek_after_sequential_result.video_frames->size() << '\n';
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 3) {
        return fail("Usage: utsure_core_media_decode_tests [--sample|--missing|--preview-session-sequential] <path>");
    }

    const std::string_view mode(argv[1]);
    const std::filesystem::path path(argv[2]);

    if (mode == "--sample") {
        return run_sample_assertion(path);
    }

    if (mode == "--missing") {
        return run_missing_input_assertion(path);
    }

    if (mode == "--preview-session-sequential") {
        return run_preview_session_sequential_assertion(path);
    }

    return fail("Unknown mode. Use --sample, --missing, or --preview-session-sequential.");
}
