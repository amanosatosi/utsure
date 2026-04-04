#include "utsure/core/media/audio_stream_selection.hpp"
#include "utsure/core/media/media_inspection_report.hpp"
#include "utsure/core/media/media_inspector.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using utsure::core::media::MediaInspectionResult;
using utsure::core::media::MediaInspector;
using utsure::core::media::MediaSourceInfo;
using utsure::core::media::audio_stream_has_explicit_japanese_metadata;
using utsure::core::media::format_media_inspection_report;

constexpr std::string_view kExpectedSampleReport =
    "container.format=avi\n"
    "container.duration_us=2000000\n"
    "video.present=yes\n"
    "video.stream_index=0\n"
    "video.codec=rawvideo\n"
    "video.resolution=320x180\n"
    "video.pixel_format=yuv420p\n"
    "video.average_frame_rate=24/1\n"
    "video.time_base=1/24\n"
    "video.start_pts=0\n"
    "video.duration_pts=48\n"
    "video.frame_count=48\n"
    "audio.present=yes\n"
    "audio.stream_index=1\n"
    "audio.codec=pcm_s16le\n"
    "audio.sample_format=s16\n"
    "audio.sample_rate=48000\n"
    "audio.channels=1\n"
    "audio.channel_layout=unknown\n"
    "audio.time_base=1/48000\n"
    "audio.start_pts=0\n"
    "audio.duration_pts=unknown\n"
    "audio.frame_count=96000";

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

const utsure::core::media::AudioStreamInfo *find_audio_stream_by_language(
    const MediaSourceInfo &source_info,
    std::string_view language_tag
) {
    const auto stream = std::find_if(
        source_info.audio_streams.begin(),
        source_info.audio_streams.end(),
        [&](const auto &audio_stream) {
            return audio_stream.language_tag.has_value() && *audio_stream.language_tag == language_tag;
        }
    );
    return stream != source_info.audio_streams.end() ? &*stream : nullptr;
}

const utsure::core::media::AudioStreamInfo *find_japanese_audio_stream(const MediaSourceInfo &source_info) {
    const auto stream = std::find_if(
        source_info.audio_streams.begin(),
        source_info.audio_streams.end(),
        [](const auto &audio_stream) {
            return audio_stream_has_explicit_japanese_metadata(audio_stream);
        }
    );
    return stream != source_info.audio_streams.end() ? &*stream : nullptr;
}

int assert_selected_audio_matches_primary(const MediaSourceInfo &source_info) {
    if (!source_info.primary_audio_stream.has_value()) {
        return fail("Expected a selected primary audio stream.");
    }

    if (!source_info.selected_audio_stream_index.has_value()) {
        return fail("Expected the selected audio stream index to be populated.");
    }

    if (*source_info.selected_audio_stream_index != source_info.primary_audio_stream->stream_index) {
        return fail("The selected audio stream index did not match the primary audio stream.");
    }

    return 0;
}

int run_sample_assertion(const std::filesystem::path &sample_path) {
    const MediaInspectionResult result = MediaInspector::inspect(sample_path);
    if (!result.succeeded()) {
        const std::string error_message =
            "Sample inspection failed unexpectedly: " +
            result.error->message +
            " Hint: " +
            result.error->actionable_hint;
        return fail(error_message);
    }

    const std::string actual_report = format_media_inspection_report(*result.media_source_info);
    std::cout << actual_report << '\n';

    if (actual_report != kExpectedSampleReport) {
        std::cerr << "Expected inspection report:\n" << kExpectedSampleReport << "\n";
        std::cerr << "Actual inspection report:\n" << actual_report << "\n";
        return 1;
    }

    return 0;
}

int run_missing_input_assertion(const std::filesystem::path &missing_path) {
    const MediaInspectionResult result = MediaInspector::inspect(missing_path);
    if (result.succeeded() || !result.error.has_value()) {
        return fail("Missing-input inspection unexpectedly succeeded.");
    }

    const std::string expected_message =
        "Cannot inspect media input '" + missing_path.lexically_normal().string() + "': the file does not exist.";

    if (result.error->message != expected_message) {
        std::cerr << "Expected error message:\n" << expected_message << "\n";
        std::cerr << "Actual error message:\n" << result.error->message << "\n";
        return 1;
    }

    if (result.error->actionable_hint !=
        "Check that the path is correct and that the file has been created before inspection.") {
        std::cerr << "Unexpected actionable hint:\n" << result.error->actionable_hint << "\n";
        return 1;
    }

    std::cout << result.error->message << '\n';
    std::cout << result.error->actionable_hint << '\n';
    return 0;
}

int run_multi_audio_japanese_assertion(const std::filesystem::path &sample_path) {
    const MediaInspectionResult result = MediaInspector::inspect(sample_path);
    if (!result.succeeded()) {
        return fail("The multi-audio Japanese sample failed inspection unexpectedly.");
    }

    const auto &source_info = *result.media_source_info;
    if (source_info.audio_streams.size() != 2) {
        return fail("Expected the Japanese-priority sample to expose exactly two audio streams.");
    }

    if (assert_selected_audio_matches_primary(source_info) != 0) {
        return 1;
    }

    const auto *english_stream = find_audio_stream_by_language(source_info, "eng");
    const auto *japanese_stream = find_japanese_audio_stream(source_info);
    if (english_stream == nullptr || japanese_stream == nullptr) {
        return fail("The Japanese-priority sample did not expose both English and Japanese metadata.");
    }

    if (!english_stream->disposition_default || japanese_stream->disposition_default) {
        return fail("The Japanese-priority sample did not preserve the expected default disposition setup.");
    }

    if (source_info.primary_audio_stream->stream_index != japanese_stream->stream_index ||
        source_info.primary_audio_stream->channel_count != 1 ||
        !audio_stream_has_explicit_japanese_metadata(*source_info.primary_audio_stream)) {
        return fail("Japanese audio was not selected ahead of the default non-Japanese track.");
    }

    std::cout << "selected.audio.language="
              << source_info.primary_audio_stream->language_tag.value_or("unknown") << '\n';
    std::cout << "selected.audio.stream_index=" << source_info.primary_audio_stream->stream_index << '\n';
    return 0;
}

int run_multi_audio_default_assertion(const std::filesystem::path &sample_path) {
    const MediaInspectionResult result = MediaInspector::inspect(sample_path);
    if (!result.succeeded()) {
        return fail("The multi-audio default-track sample failed inspection unexpectedly.");
    }

    const auto &source_info = *result.media_source_info;
    if (source_info.audio_streams.size() != 2) {
        return fail("Expected the default-track sample to expose exactly two audio streams.");
    }

    if (assert_selected_audio_matches_primary(source_info) != 0) {
        return 1;
    }

    const auto default_stream = std::find_if(
        source_info.audio_streams.begin(),
        source_info.audio_streams.end(),
        [](const auto &audio_stream) {
            return audio_stream.disposition_default;
        }
    );
    if (default_stream == source_info.audio_streams.end()) {
        return fail("The default-track sample did not expose a default audio stream.");
    }

    if (source_info.primary_audio_stream->stream_index != default_stream->stream_index ||
        source_info.primary_audio_stream->channel_count != 2) {
        return fail("The default audio stream was not selected when no Japanese track was present.");
    }

    std::cout << "selected.audio.language="
              << source_info.primary_audio_stream->language_tag.value_or("unknown") << '\n';
    std::cout << "selected.audio.stream_index=" << source_info.primary_audio_stream->stream_index << '\n';
    return 0;
}

int run_multi_audio_fallback_assertion(const std::filesystem::path &sample_path) {
    const MediaInspectionResult result = MediaInspector::inspect(sample_path);
    if (!result.succeeded()) {
        return fail("The multi-audio fallback sample failed inspection unexpectedly.");
    }

    const auto &source_info = *result.media_source_info;
    if (source_info.audio_streams.size() != 2) {
        return fail("Expected the fallback sample to expose exactly two audio streams.");
    }

    if (assert_selected_audio_matches_primary(source_info) != 0) {
        return 1;
    }

    const auto expected_stream = std::min_element(
        source_info.audio_streams.begin(),
        source_info.audio_streams.end(),
        [](const auto &left, const auto &right) {
            return left.stream_index < right.stream_index;
        }
    );
    if (expected_stream == source_info.audio_streams.end()) {
        return fail("The fallback sample unexpectedly exposed no audio streams.");
    }

    const bool any_metadata_present = std::any_of(
        source_info.audio_streams.begin(),
        source_info.audio_streams.end(),
        [](const auto &audio_stream) {
            return audio_stream.language_tag.has_value() ||
                audio_stream.title.has_value() ||
                audio_stream.disposition_default;
        }
    );
    if (any_metadata_present) {
        return fail("The fallback sample unexpectedly exposed language/title/default metadata.");
    }

    if (source_info.primary_audio_stream->stream_index != expected_stream->stream_index ||
        source_info.primary_audio_stream->channel_count != 1) {
        return fail("The fallback audio selection did not use the first decodable stream index.");
    }

    std::cout << "selected.audio.stream_index=" << source_info.primary_audio_stream->stream_index << '\n';
    std::cout << "selected.audio.channels=" << source_info.primary_audio_stream->channel_count << '\n';
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 3) {
        return fail(
            "Usage: utsure_core_media_inspection_tests "
            "[--sample|--missing|--multi-audio-japanese|--multi-audio-default|--multi-audio-fallback] <path>"
        );
    }

    const std::string_view mode(argv[1]);
    const std::filesystem::path path(argv[2]);

    if (mode == "--sample") {
        return run_sample_assertion(path);
    }

    if (mode == "--missing") {
        return run_missing_input_assertion(path);
    }

    if (mode == "--multi-audio-japanese") {
        return run_multi_audio_japanese_assertion(path);
    }

    if (mode == "--multi-audio-default") {
        return run_multi_audio_default_assertion(path);
    }

    if (mode == "--multi-audio-fallback") {
        return run_multi_audio_fallback_assertion(path);
    }

    return fail("Unknown mode for utsure_core_media_inspection_tests.");
}
