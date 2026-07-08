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
    std::optional<std::string> subtitle_renderer_ptr{};
    std::optional<std::string> subtitle_track_ptr{};
    std::optional<std::string> subtitle_library_ptr{};
    std::optional<std::string> subtitle_render_thread_id{};
    std::optional<std::string> subtitle_renderer_created_thread_id{};
    std::optional<std::string> subtitle_renderer_destroyed_thread_id{};
    std::optional<int> active_subtitle_render_count{};
    std::optional<std::int64_t> last_subtitle_render_start_pts{};
    std::optional<std::int64_t> last_subtitle_render_end_pts{};
    std::optional<int> last_subtitle_event_count{};
    std::optional<int> registered_image_asset_count{};
    std::optional<std::string> last_registered_image_asset_name{};
    std::optional<std::string> last_registered_image_asset_path{};
    std::optional<bool> subtitle_cleanup_started{};
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
    std::string subtitle_renderer_ptr{};
    std::string subtitle_track_ptr{};
    std::string subtitle_library_ptr{};
    std::string subtitle_render_thread_id{};
    std::string subtitle_renderer_created_thread_id{};
    std::string subtitle_renderer_destroyed_thread_id{};
    int active_subtitle_render_count{0};
    std::int64_t last_subtitle_render_start_pts{0};
    std::int64_t last_subtitle_render_end_pts{0};
    int last_subtitle_event_count{0};
    int registered_image_asset_count{0};
    std::string last_registered_image_asset_name{};
    std::string last_registered_image_asset_path{};
    bool subtitle_cleanup_started{false};
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
    std::filesystem::path handler_entered_path{};
    std::filesystem::path dump_failed_path{};
};

struct CrashDumpSidecarMetadata final {
    bool handler_entered{false};
    bool dump_write_success{false};
    unsigned long dump_write_error_code{0};
    std::string dump_write_error_message{};
    std::string dump_path_attempted{};
    unsigned long seh_exception_code{0};
    std::string exception_address{};
    bool cxx_exception_active{false};
    std::string cxx_exception_type{};
    std::string cxx_exception_message{};
};

struct CrashDumpWriteResult final {
    bool handler_entered{false};
    bool handler_marker_written{false};
    bool dump_written{false};
    bool sidecar_written{false};
    bool failure_marker_written{false};
    CrashArtifactPaths paths{};
    std::string dump_type{};
    unsigned long dump_error_code{0};
    std::string error_message{};
};

struct CrashDumpDirectoryResolutionOptions final {
    std::optional<std::filesystem::path> override_directory{};
    std::optional<std::filesystem::path> executable_path{};
    std::optional<std::filesystem::path> local_app_data_directory{};
    bool simulate_portable_directory_failure{false};
};

struct CrashDumpSetupStatus final {
    bool enabled{false};
    std::filesystem::path resolved_directory{};
    bool directory_exists{false};
    bool directory_writable{false};
    bool unhandled_exception_filter_installed{false};
    bool vectored_exception_handler_installed{false};
    bool terminate_handler_installed{false};
    bool signal_handlers_installed{false};
    unsigned long process_id{0};
    std::string build_version{};
    std::string git_commit{};
    std::string previous_unhandled_exception_filter{};
};

[[nodiscard]] std::filesystem::path default_crash_dump_directory();
[[nodiscard]] std::filesystem::path crash_dump_directory_for_crash_path();
void hold_crash_dump_directory_lock_for_test(bool hold);
[[nodiscard]] std::filesystem::path resolve_crash_dump_directory_for_test(
    const CrashDumpDirectoryResolutionOptions &options
);
[[nodiscard]] std::string make_crash_file_stem(std::int64_t unix_seconds, unsigned long process_id);
[[nodiscard]] std::string make_crash_file_stem(
    std::int64_t unix_milliseconds,
    unsigned long process_id,
    unsigned long thread_id,
    unsigned int sequence_number
);
[[nodiscard]] CrashArtifactPaths make_crash_artifact_paths(
    const std::filesystem::path &directory,
    std::int64_t unix_seconds,
    unsigned long process_id
);
[[nodiscard]] CrashArtifactPaths make_crash_artifact_paths(
    const std::filesystem::path &directory,
    std::int64_t unix_milliseconds,
    unsigned long process_id,
    unsigned long thread_id,
    unsigned int sequence_number
);
[[nodiscard]] CrashArtifactPaths choose_available_crash_artifact_paths(
    const std::filesystem::path &directory,
    std::int64_t unix_milliseconds,
    unsigned long process_id,
    unsigned long thread_id
);
[[nodiscard]] std::string crash_context_to_json(const CrashContextSnapshot &snapshot);
[[nodiscard]] std::string crash_context_collection_to_json(const CrashContextCollectionSnapshot &snapshot);
[[nodiscard]] std::string crash_context_collection_to_json(
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata
);
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
[[nodiscard]] bool write_crash_sidecar_for_test(
    const CrashArtifactPaths &paths,
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata,
    std::string *error_message = nullptr
);
[[nodiscard]] bool write_handler_entered_marker_for_test(
    const CrashArtifactPaths &paths,
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata,
    std::string *error_message = nullptr
);
[[nodiscard]] bool write_dump_failed_marker_for_test(
    const CrashArtifactPaths &paths,
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata,
    std::string *error_message = nullptr
);

void configure_crash_log_flushing() noexcept;
void install_crash_handlers() noexcept;
void initialize_crash_dump_directory() noexcept;
[[nodiscard]] CrashDumpSetupStatus crash_dump_setup_status() noexcept;
[[nodiscard]] std::string format_crash_dump_setup_log(const CrashDumpSetupStatus &status);
[[nodiscard]] CrashDumpWriteResult write_crash_dump_for_current_process(
    void *exception_pointers = nullptr,
    const CrashDumpSidecarMetadata *precaptured_metadata = nullptr
) noexcept;
[[nodiscard]] CrashDumpWriteResult write_diagnostic_dump_now() noexcept;

}  // namespace utsure::app::crash
