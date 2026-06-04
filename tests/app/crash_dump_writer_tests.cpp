#include "crash_dump_writer.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>

namespace {

int fail(const char *message) {
    std::cerr << message << '\n';
    return 1;
}

std::string read_text_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

bool contains_text(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

int assert_crash_path_helpers() {
    const auto stem = utsure::app::crash::make_crash_file_stem(1700000000, 1234UL);
    if (!contains_text(stem, "utsure-crash-") || !contains_text(stem, "-pid-1234")) {
        return fail("Crash file stem did not include the expected prefix and pid.");
    }

    const auto paths = utsure::app::crash::make_crash_artifact_paths(
        std::filesystem::path{"crash-root"},
        1700000000,
        1234UL
    );
    if (paths.dump_path.extension() != ".dmp" ||
        paths.sidecar_path.extension() != ".json" ||
        paths.log_path.extension() != ".txt") {
        return fail("Crash artifact paths did not use expected extensions.");
    }

    std::cout << "crash_dump_writer.path_helpers=ok\n";
    return 0;
}

int assert_crash_context_snapshot_and_json() {
    utsure::app::crash::reset_crash_context_for_tests();
    utsure::app::crash::update_crash_context(utsure::app::crash::CrashContextUpdate{
        .runner_slot_index = 2,
        .active_job_count = 3,
        .input_path = "C:/input/hevc.mp4",
        .output_path = "C:/output/out.mp4",
        .source_codec = "hevc",
        .source_pixel_format = "yuv420p",
        .decoded_frame_format = "yuv420p",
        .video_output_codec = "h264",
        .resolution = "1920x1080",
        .current_stage = "encode_stage",
        .segment_name = "main",
        .frame_index = 42,
        .pts = 1001,
        .decoder_thread_count = 1,
        .decoder_thread_type = "frame",
        .encoder_thread_count = 2,
        .encoder_thread_type = "slice",
        .subtitle_enabled = true,
        .subtitle_setup_mode = "serialized",
        .frame_transfer_path = "sws_scale",
        .current_rss_bytes = 123456,
        .peak_rss_bytes = 234567,
        .cancellation_requested = false,
        .build_version = "utsure test",
        .git_commit = "abc123",
        .last_log_message = "First decoded video frame diagnostics"
    });
    utsure::app::crash::update_crash_context_from_runtime_log(
        "Streaming frame checkpoint: segment=main, frame=43, pts=2002, source_codec=hevc, "
        "video_queue_depth=1, decoder_threads=1, encoder_threads=2, current_rss=345678, peak_rss=456789."
    );

    const auto snapshot = utsure::app::crash::crash_context_snapshot();
    if (snapshot.runner_slot_index != 2 ||
        snapshot.active_job_count != 3 ||
        snapshot.source_codec != "hevc" ||
        snapshot.frame_index != 43 ||
        snapshot.pts != 2002 ||
        snapshot.current_rss_bytes != 345678 ||
        snapshot.peak_rss_bytes != 456789) {
        return fail("Crash context snapshot did not preserve expected update fields.");
    }

    const auto json = utsure::app::crash::crash_context_to_json(snapshot);
    if (!contains_text(json, "\"source_codec\": \"hevc\"") ||
        !contains_text(json, "\"decoded_frame_format\": \"yuv420p\"") ||
        !contains_text(json, "\"frame_transfer_path\": \"sws_scale\"") ||
        !contains_text(json, "\"runner_slot_index\": 2")) {
        return fail("Crash context JSON did not include expected fields.");
    }

    std::cout << "crash_dump_writer.context_json=ok\n";
    return 0;
}

int assert_sidecar_write() {
    const auto sidecar_dir = std::filesystem::temp_directory_path() / "utsure-crash-writer-tests";
    const auto paths = utsure::app::crash::make_crash_artifact_paths(sidecar_dir, 1700000000, 777UL);
    utsure::app::crash::CrashContextSnapshot snapshot{};
    snapshot.source_codec = "hevc";
    snapshot.current_stage = "decode_stage";
    snapshot.build_version = "utsure test";
    snapshot.git_commit = "abc123";

    std::string error_message;
    if (!utsure::app::crash::write_crash_sidecar_for_test(paths, snapshot, &error_message)) {
        std::cerr << "sidecar_error=" << error_message << '\n';
        return fail("Crash sidecar writer failed in controlled test mode.");
    }

    const auto json = read_text_file(paths.sidecar_path);
    if (!contains_text(json, "\"source_codec\": \"hevc\"") ||
        !contains_text(json, "\"current_stage\": \"decode_stage\"")) {
        return fail("Crash sidecar file did not contain expected JSON fields.");
    }

    std::error_code remove_error{};
    std::filesystem::remove(paths.sidecar_path, remove_error);
    std::filesystem::remove(paths.log_path, remove_error);
    std::filesystem::remove(paths.dump_path, remove_error);
    std::filesystem::remove(sidecar_dir, remove_error);

    std::cout << "crash_dump_writer.sidecar=ok\n";
    return 0;
}

}  // namespace

int main() {
    if (assert_crash_path_helpers() != 0 ||
        assert_crash_context_snapshot_and_json() != 0 ||
        assert_sidecar_write() != 0) {
        return 1;
    }

    return 0;
}
