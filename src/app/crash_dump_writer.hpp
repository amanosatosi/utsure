#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace utsure::app::crash {

struct CrashContextUpdate final {
    std::optional<int> queue_job_index{};
    std::optional<int> runner_slot_index{};
    std::optional<int> active_job_count{};
    std::optional<std::string> input_path{};
    std::optional<std::string> output_path{};
    std::optional<std::string> source_codec{};
    std::optional<std::string> source_pixel_format{};
    std::optional<std::string> decoded_frame_format{};
    std::optional<std::string> video_output_codec{};
    std::optional<std::string> resolution{};
    std::optional<std::string> current_stage{};
    std::optional<std::string> segment_name{};
    std::optional<std::int64_t> frame_index{};
    std::optional<std::int64_t> pts{};
    std::optional<int> decoder_thread_count{};
    std::optional<std::string> decoder_thread_type{};
    std::optional<int> encoder_thread_count{};
    std::optional<std::string> encoder_thread_type{};
    std::optional<bool> subtitle_enabled{};
    std::optional<std::string> subtitle_setup_mode{};
    std::optional<std::string> frame_transfer_path{};
    std::optional<std::uint64_t> current_rss_bytes{};
    std::optional<std::uint64_t> peak_rss_bytes{};
    std::optional<bool> cancellation_requested{};
    std::optional<std::string> build_version{};
    std::optional<std::string> git_commit{};
    std::optional<std::string> last_log_message{};
};

struct CrashContextSnapshot final {
    int queue_job_index{-1};
    int runner_slot_index{-1};
    int active_job_count{0};
    std::string input_path{};
    std::string output_path{};
    std::string source_codec{"unknown"};
    std::string source_pixel_format{"unknown"};
    std::string decoded_frame_format{"unknown"};
    std::string video_output_codec{"unknown"};
    std::string resolution{"unknown"};
    std::string current_stage{"starting"};
    std::string segment_name{};
    std::int64_t frame_index{-1};
    std::int64_t pts{0};
    int decoder_thread_count{0};
    std::string decoder_thread_type{"unknown"};
    int encoder_thread_count{0};
    std::string encoder_thread_type{"unknown"};
    bool subtitle_enabled{false};
    std::string subtitle_setup_mode{"unknown"};
    std::string frame_transfer_path{"unknown"};
    std::uint64_t current_rss_bytes{0};
    std::uint64_t peak_rss_bytes{0};
    bool cancellation_requested{false};
    std::string build_version{};
    std::string git_commit{"unknown"};
    std::string last_log_message{};
};

struct CrashContextCollectionSnapshot final {
    unsigned long crashing_thread_id{0};
    int last_updated_runner_slot{-1};
    int active_job_count{0};
    CrashContextSnapshot last_updated_context{};
    std::vector<CrashContextSnapshot> runner_contexts{};
};

struct CrashArtifactPaths final {
    std::filesystem::path dump_path{};
    std::filesystem::path sidecar_path{};
    std::filesystem::path log_path{};
};

struct CrashDumpWriteResult final {
    bool dump_written{false};
    bool sidecar_written{false};
    CrashArtifactPaths paths{};
    std::string dump_type{};
    std::string error_message{};
};

struct CrashDumpDirectoryResolutionOptions final {
    std::optional<std::filesystem::path> override_directory{};
    std::optional<std::filesystem::path> executable_path{};
    std::optional<std::filesystem::path> local_app_data_directory{};
    bool simulate_portable_directory_failure{false};
};

[[nodiscard]] std::filesystem::path default_crash_dump_directory();
[[nodiscard]] std::filesystem::path cached_crash_dump_directory_for_crash_path_for_test();
void hold_crash_dump_directory_lock_for_test(bool hold);
[[nodiscard]] std::filesystem::path resolve_crash_dump_directory_for_test(
    const CrashDumpDirectoryResolutionOptions &options
);
[[nodiscard]] std::string make_crash_file_stem(std::int64_t unix_seconds, unsigned long process_id);
[[nodiscard]] CrashArtifactPaths make_crash_artifact_paths(
    const std::filesystem::path &directory,
    std::int64_t unix_seconds,
    unsigned long process_id
);
[[nodiscard]] std::string crash_context_to_json(const CrashContextSnapshot &snapshot);
[[nodiscard]] std::string crash_context_collection_to_json(const CrashContextCollectionSnapshot &snapshot);
[[nodiscard]] std::vector<CrashArtifactPaths> find_recent_crash_artifacts(
    const std::filesystem::path &directory,
    std::size_t max_count = 5
);

void reset_crash_context_for_tests();
void update_crash_context(const CrashContextUpdate &update);
void set_current_thread_runner_slot(int runner_slot_index) noexcept;
void clear_current_thread_runner_slot() noexcept;
[[nodiscard]] int begin_active_encode_job(int runner_slot_index) noexcept;
[[nodiscard]] int end_active_encode_job(int runner_slot_index) noexcept;
[[nodiscard]] int current_active_encode_job_count() noexcept;
void update_crash_context_from_progress(const utsure::core::job::EncodeJobProgress &progress);
void update_crash_context_from_runtime_log(std::string_view message);
void mark_crash_context_cancellation_requested(bool requested);
[[nodiscard]] CrashContextSnapshot crash_context_snapshot();
[[nodiscard]] CrashContextCollectionSnapshot crash_context_collection_snapshot(unsigned long crashing_thread_id = 0);

[[nodiscard]] bool write_crash_sidecar_for_test(
    const CrashArtifactPaths &paths,
    const CrashContextCollectionSnapshot &snapshot,
    std::string *error_message = nullptr
);

void configure_crash_log_flushing() noexcept;
void install_crash_handlers() noexcept;
void initialize_crash_dump_directory() noexcept;
[[nodiscard]] CrashDumpWriteResult write_crash_dump_for_current_process(void *exception_pointers = nullptr) noexcept;

}  // namespace utsure::app::crash
