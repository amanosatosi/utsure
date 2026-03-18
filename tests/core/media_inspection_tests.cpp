#include "utsure/core/media/media_inspection_report.hpp"
#include "utsure/core/media/media_inspector.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using utsure::core::media::MediaInspectionResult;
using utsure::core::media::MediaInspector;
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

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 3) {
        return fail("Usage: utsure_core_media_inspection_tests [--sample|--missing] <path>");
    }

    const std::string_view mode(argv[1]);
    const std::filesystem::path path(argv[2]);

    if (mode == "--sample") {
        return run_sample_assertion(path);
    }

    if (mode == "--missing") {
        return run_missing_input_assertion(path);
    }

    return fail("Unknown mode. Use --sample or --missing.");
}
