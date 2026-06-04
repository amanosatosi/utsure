#include "crash_dump_writer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

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

void set_env_var(const char *name, const std::string &value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env_var(const char *name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
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

int assert_crash_dump_directory_override() {
    const auto override_dir = (std::filesystem::temp_directory_path() / "utsure-crash-override-tests").lexically_normal();
    set_env_var("UTSURE_CRASH_DUMP_DIR", override_dir.string());
    const auto resolved = utsure::app::crash::default_crash_dump_directory().lexically_normal();
    unset_env_var("UTSURE_CRASH_DUMP_DIR");
    if (resolved != override_dir) {
        return fail("Crash dump directory override was not honored.");
    }

    std::cout << "crash_dump_writer.directory_override=ok\n";
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
    utsure::app::crash::set_current_thread_runner_slot(2);
    utsure::app::crash::update_crash_context_from_runtime_log(
        "Streaming frame checkpoint: segment=main, frame=43, pts=2002, source_codec=hevc, "
        "video_queue_depth=1, decoder_threads=1, encoder_threads=2, current_rss=345678, peak_rss=456789."
    );
    utsure::app::crash::clear_current_thread_runner_slot();

    const auto snapshot = utsure::app::crash::crash_context_snapshot();
    const auto collection = utsure::app::crash::crash_context_collection_snapshot(99UL);
    if (snapshot.runner_slot_index != 2 ||
        snapshot.active_job_count != 3 ||
        snapshot.source_codec != "hevc" ||
        snapshot.frame_index != 43 ||
        snapshot.pts != 2002 ||
        snapshot.current_rss_bytes != 345678 ||
        snapshot.peak_rss_bytes != 456789) {
        return fail("Crash context snapshot did not preserve expected update fields.");
    }
    if (collection.crashing_thread_id != 99UL ||
        collection.last_updated_runner_slot != 2 ||
        collection.active_job_count != 0 ||
        collection.runner_contexts.size() <= 2U ||
        collection.runner_contexts[2].source_codec != "hevc" ||
        collection.runner_contexts[2].frame_index != 43) {
        return fail("Crash context collection did not preserve per-runner slot fields.");
    }

    const auto json = utsure::app::crash::crash_context_collection_to_json(collection);
    if (!contains_text(json, "\"source_codec\": \"hevc\"") ||
        !contains_text(json, "\"decoded_frame_format\": \"yuv420p\"") ||
        !contains_text(json, "\"frame_transfer_path\": \"sws_scale\"") ||
        !contains_text(json, "\"runner_slot_index\": 2") ||
        !contains_text(json, "\"runner_contexts\"") ||
        !contains_text(json, "\"crashing_thread_id\": 99")) {
        return fail("Crash context JSON did not include expected fields.");
    }

    std::cout << "crash_dump_writer.context_json=ok\n";
    return 0;
}

int assert_active_count_lifetime_helpers() {
    utsure::app::crash::reset_crash_context_for_tests();
    if (utsure::app::crash::current_active_encode_job_count() != 0) {
        return fail("Crash active count did not reset to zero.");
    }
    const int active_after_first = utsure::app::crash::begin_active_encode_job(4);
    const int active_after_second = utsure::app::crash::begin_active_encode_job(7);
    if (active_after_first != 1 || active_after_second != 2 ||
        utsure::app::crash::current_active_encode_job_count() != 2) {
        return fail("Crash active count did not increment for active jobs.");
    }
    const int active_after_end = utsure::app::crash::end_active_encode_job(4);
    const int active_after_final_end = utsure::app::crash::end_active_encode_job(7);
    if (active_after_end != 1 || active_after_final_end != 0 ||
        utsure::app::crash::current_active_encode_job_count() != 0) {
        return fail("Crash active count did not decrement for active jobs.");
    }

    std::cout << "crash_dump_writer.active_count=ok\n";
    return 0;
}

int assert_sidecar_write() {
    const auto sidecar_dir = std::filesystem::temp_directory_path() / "utsure-crash-writer-tests";
    const auto paths = utsure::app::crash::make_crash_artifact_paths(sidecar_dir, 1700000000, 777UL);
    utsure::app::crash::CrashContextCollectionSnapshot snapshot{};
    snapshot.last_updated_context.source_codec = "hevc";
    snapshot.last_updated_context.current_stage = "decode_stage";
    snapshot.last_updated_context.build_version = "utsure test";
    snapshot.last_updated_context.git_commit = "abc123";
    snapshot.runner_contexts.push_back(snapshot.last_updated_context);

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

#if defined(_WIN32)
[[noreturn]] void crash_child_process() {
    utsure::app::crash::reset_crash_context_for_tests();
    utsure::app::crash::install_crash_handlers();
    utsure::app::crash::begin_active_encode_job(3);
    utsure::app::crash::update_crash_context(utsure::app::crash::CrashContextUpdate{
        .runner_slot_index = 3,
        .input_path = "child-input-hevc.mp4",
        .source_codec = "hevc",
        .current_stage = "controlled_child_crash",
        .build_version = "utsure child smoke",
        .git_commit = "child-test"
    });
    volatile int *crash_address = nullptr;
    *crash_address = 1;
    std::_Exit(99);
}

int assert_controlled_child_dump(const char *executable_path) {
    const auto dump_dir = std::filesystem::temp_directory_path() / "utsure-controlled-crash-dump-test";
    std::error_code error{};
    std::filesystem::remove_all(dump_dir, error);
    std::filesystem::create_directories(dump_dir, error);
    if (error) {
        return fail("Failed to create controlled crash dump temp directory.");
    }

    set_env_var("UTSURE_CRASH_DUMP_DIR", dump_dir.string());
    const std::string command = "\"" + std::filesystem::absolute(executable_path).string() + "\" --child-crash";
    const int child_exit = std::system(command.c_str());
    unset_env_var("UTSURE_CRASH_DUMP_DIR");
    if (child_exit == 0) {
        return fail("Controlled crash child did not fail as expected.");
    }

    bool found_dump = false;
    bool found_sidecar = false;
    std::string sidecar_text{};
    for (const auto &entry : std::filesystem::directory_iterator(dump_dir, error)) {
        if (entry.path().extension() == ".dmp") {
            found_dump = true;
        } else if (entry.path().extension() == ".json") {
            found_sidecar = true;
            sidecar_text = read_text_file(entry.path());
        }
    }
    if (!found_dump) {
        return fail("Controlled crash child did not write a .dmp file.");
    }
    if (!found_sidecar ||
        !contains_text(sidecar_text, "\"build_version\": \"utsure child smoke\"") ||
        !contains_text(sidecar_text, "\"source_codec\": \"hevc\"") ||
        !contains_text(sidecar_text, "\"runner_slot_index\": 3") ||
        !contains_text(sidecar_text, "\"runner_contexts\"")) {
        return fail("Controlled crash child did not write the expected sidecar context.");
    }

    std::filesystem::remove_all(dump_dir, error);
    std::cout << "crash_dump_writer.controlled_child_dump=ok\n";
    return 0;
}
#endif

}  // namespace

int main(int argc, char *argv[]) {
#if defined(_WIN32)
    if (argc > 1 && std::string(argv[1]) == "--child-crash") {
        crash_child_process();
    }
#endif
    if (assert_crash_path_helpers() != 0 ||
        assert_crash_dump_directory_override() != 0 ||
        assert_crash_context_snapshot_and_json() != 0 ||
        assert_active_count_lifetime_helpers() != 0 ||
        assert_sidecar_write() != 0) {
        return 1;
    }
#if defined(_WIN32)
    if (assert_controlled_child_dump(argv[0]) != 0) {
        return 1;
    }
#endif

    return 0;
}
