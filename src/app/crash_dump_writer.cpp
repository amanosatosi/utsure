#include "crash_dump_writer.hpp"

#include "utsure/core/build_info.hpp"
#include "utsure/core/job/encode_job.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#endif

namespace utsure::app::crash {
namespace {

std::mutex &context_mutex() {
    static auto *mutex = new std::mutex();
    return *mutex;
}

CrashContextSnapshot &mutable_context() {
    static auto *context = new CrashContextSnapshot();
    return *context;
}

std::mutex &crash_directory_mutex() {
    static auto *mutex = new std::mutex();
    return *mutex;
}

std::filesystem::path &cached_crash_dump_directory() {
    static auto *path = new std::filesystem::path();
    return *path;
}

std::vector<CrashContextSnapshot> &mutable_runner_contexts() {
    static auto *contexts = new std::vector<CrashContextSnapshot>();
    return *contexts;
}

int &last_updated_runner_slot() {
    static auto *slot = new int{-1};
    return *slot;
}

std::atomic_int &active_encode_job_count_storage() {
    static auto *count = new std::atomic_int{0};
    return *count;
}

thread_local int current_thread_runner_slot = -1;

std::atomic_bool &dump_write_in_progress() {
    static auto *flag = new std::atomic_bool{false};
    return *flag;
}

std::atomic_bool &exception_dump_already_started() {
    static auto *flag = new std::atomic_bool{false};
    return *flag;
}

std::atomic_uint &crash_artifact_sequence_storage() {
    static auto *sequence = new std::atomic_uint{0};
    return *sequence;
}

std::atomic_bool &unhandled_exception_filter_installed_storage() {
    static auto *installed = new std::atomic_bool{false};
    return *installed;
}

std::atomic_bool &vectored_exception_handler_installed_storage() {
    static auto *installed = new std::atomic_bool{false};
    return *installed;
}

std::atomic_bool &terminate_handler_installed_storage() {
    static auto *installed = new std::atomic_bool{false};
    return *installed;
}

std::atomic_bool &signal_handlers_installed_storage() {
    static auto *installed = new std::atomic_bool{false};
    return *installed;
}

std::string &previous_unhandled_exception_filter_text() {
    static auto *text = new std::string("unknown");
    return *text;
}

std::string utf8_path_string(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto text = path.lexically_normal().u8string();
    return std::string(reinterpret_cast<const char *>(text.c_str()), text.size());
#else
    return path.lexically_normal().string();
#endif
}

std::string current_build_version() {
    return std::string(core::BuildInfo::project_name()) + " " + std::string(core::BuildInfo::project_version());
}

std::string git_commit_from_environment() {
    const char *commit = std::getenv("UTSURE_GIT_COMMIT");
    if (commit != nullptr && std::string_view(commit).size() > 0U) {
        return commit;
    }
    commit = std::getenv("GITHUB_SHA");
    if (commit != nullptr && std::string_view(commit).size() > 0U) {
        return commit;
    }
    return "unknown";
}

void populate_default_build_fields(CrashContextSnapshot &snapshot) {
    if (snapshot.build_version.empty()) {
        snapshot.build_version = current_build_version();
    }
    if (snapshot.git_commit.empty() || snapshot.git_commit == "unknown") {
        snapshot.git_commit = git_commit_from_environment();
    }
}

bool ensure_directory_writable(const std::filesystem::path &directory) {
    std::error_code error{};
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    const auto probe_path = directory / ".utsure-crash-dump-write-test.tmp";
    {
        std::ofstream probe(probe_path, std::ios::binary | std::ios::trunc);
        if (!probe) {
            return false;
        }
        probe << "ok";
    }
    std::filesystem::remove(probe_path, error);
    return true;
}

CrashContextSnapshot &runner_context_for_slot(const int runner_slot_index) {
    auto &contexts = mutable_runner_contexts();
    if (runner_slot_index < 0) {
        return mutable_context();
    }
    const auto target_size = static_cast<std::size_t>(runner_slot_index + 1);
    if (contexts.size() < target_size) {
        contexts.resize(target_size);
        for (auto &context : contexts) {
            populate_default_build_fields(context);
        }
    }
    return contexts[static_cast<std::size_t>(runner_slot_index)];
}

std::int64_t current_unix_milliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

unsigned long current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return 0UL;
#endif
}

unsigned long current_thread_id() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentThreadId());
#else
    return 0UL;
#endif
}

std::string two_digit(const int value) {
    std::ostringstream stream;
    if (value < 10) {
        stream << '0';
    }
    stream << value;
    return stream.str();
}

std::string three_digit(const int value) {
    std::ostringstream stream;
    stream << std::setw(3) << std::setfill('0') << std::clamp(value, 0, 999);
    return stream.str();
}

std::string four_digit(const unsigned int value) {
    std::ostringstream stream;
    stream << std::setw(4) << std::setfill('0') << value;
    return stream.str();
}

bool path_exists(const std::filesystem::path &path) {
    std::error_code error{};
    return std::filesystem::exists(path, error);
}

bool any_crash_artifact_path_exists(const CrashArtifactPaths &paths) {
    return path_exists(paths.dump_path) ||
        path_exists(paths.sidecar_path) ||
        path_exists(paths.log_path) ||
        path_exists(paths.handler_entered_path) ||
        path_exists(paths.dump_failed_path);
}

bool write_text_file_no_overwrite(
    const std::filesystem::path &path,
    const std::string &text,
    std::string *error_message
) {
    std::error_code error{};
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        if (error_message != nullptr) {
            *error_message = error.message();
        }
        return false;
    }
    if (std::filesystem::exists(path, error)) {
        if (error_message != nullptr) {
            *error_message = "Crash artifact already exists: " + utf8_path_string(path);
        }
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error_message != nullptr) {
            *error_message = "Failed to open crash artifact for writing: " + utf8_path_string(path);
        }
        return false;
    }
    output << text;
    if (!output) {
        if (error_message != nullptr) {
            *error_message = "Failed to finish writing crash artifact: " + utf8_path_string(path);
        }
        return false;
    }
    return true;
}

std::string sanitize_message_value(std::string value) {
    constexpr std::size_t kMaxStoredMessageBytes = 4096U;
    if (value.size() > kMaxStoredMessageBytes) {
        value.resize(kMaxStoredMessageBytes);
        value += "...";
    }
    return value;
}

void capture_current_cxx_exception_metadata(CrashDumpSidecarMetadata &metadata) noexcept {
    const auto active_exception = std::current_exception();
    if (active_exception == nullptr) {
        return;
    }

    metadata.cxx_exception_active = true;
    try {
        std::rethrow_exception(active_exception);
    } catch (const std::exception &exception) {
        metadata.cxx_exception_type = sanitize_message_value(typeid(exception).name());
        metadata.cxx_exception_message = sanitize_message_value(exception.what());
    } catch (...) {
        metadata.cxx_exception_type = "unknown_non_standard_exception";
        metadata.cxx_exception_message = "A non-standard C++ exception was active during termination.";
    }
}

std::string json_escape(const std::string &value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\':
            escaped << "\\\\";
            break;
        case '"':
            escaped << "\\\"";
            break;
        case '\b':
            escaped << "\\b";
            break;
        case '\f':
            escaped << "\\f";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (character < 0x20U) {
                escaped << "\\u";
                constexpr char kHex[] = "0123456789abcdef";
                escaped << "00" << kHex[(character >> 4U) & 0x0FU] << kHex[character & 0x0FU];
            } else {
                escaped << static_cast<char>(character);
            }
            break;
        }
    }
    return escaped.str();
}

void append_json_string(std::ostringstream &json, const char *key, const std::string &value, const bool trailing_comma) {
    json << "  \"" << key << "\": \"" << json_escape(value) << '"';
    if (trailing_comma) {
        json << ',';
    }
    json << '\n';
}

void append_json_int(std::ostringstream &json, const char *key, const std::int64_t value, const bool trailing_comma) {
    json << "  \"" << key << "\": " << value;
    if (trailing_comma) {
        json << ',';
    }
    json << '\n';
}

void append_json_uint(std::ostringstream &json, const char *key, const std::uint64_t value, const bool trailing_comma) {
    json << "  \"" << key << "\": " << value;
    if (trailing_comma) {
        json << ',';
    }
    json << '\n';
}

void append_json_bool(std::ostringstream &json, const char *key, const bool value, const bool trailing_comma) {
    json << "  \"" << key << "\": " << (value ? "true" : "false");
    if (trailing_comma) {
        json << ',';
    }
    json << '\n';
}

std::optional<std::string> extract_value_after(std::string_view message, std::string_view key) {
    const auto key_position = message.find(key);
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }

    const std::size_t value_start = key_position + key.size();
    std::size_t value_end = message.find_first_of(",.; \t\r\n", value_start);
    if (value_end == std::string_view::npos) {
        value_end = message.size();
    }

    while (value_end > value_start && message[value_end - 1U] == ' ') {
        --value_end;
    }
    if (value_end <= value_start) {
        return std::nullopt;
    }

    return std::string(message.substr(value_start, value_end - value_start));
}

std::optional<std::string> extract_tail_value_after(std::string_view message, std::string_view key) {
    const auto key_position = message.find(key);
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }

    const std::size_t value_start = key_position + key.size();
    std::size_t value_end = message.size();
    while (value_end > value_start && std::isspace(static_cast<unsigned char>(message[value_end - 1U])) != 0) {
        --value_end;
    }
    if (value_end <= value_start) {
        return std::nullopt;
    }
    return std::string(message.substr(value_start, value_end - value_start));
}

std::optional<std::string> extract_quoted_value_after(
    std::string_view message,
    std::string_view key,
    const char quote
) {
    const auto key_position = message.find(key);
    if (key_position == std::string_view::npos) {
        return std::nullopt;
    }

    const std::size_t value_start = key_position + key.size();
    const auto value_end = message.find(quote, value_start);
    if (value_end == std::string_view::npos || value_end <= value_start) {
        return std::nullopt;
    }

    return std::string(message.substr(value_start, value_end - value_start));
}

std::optional<int> parse_int_value(std::string_view text) {
    int value = 0;
    bool has_digit = false;
    bool negative = false;
    for (const char character : text) {
        if (character == '-' && !has_digit) {
            negative = true;
            continue;
        }
        if (character < '0' || character > '9') {
            break;
        }
        has_digit = true;
        value = (value * 10) + (character - '0');
    }
    if (!has_digit) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

std::optional<std::int64_t> parse_int64_value(std::string_view text) {
    std::int64_t value = 0;
    bool has_digit = false;
    bool negative = false;
    for (const char character : text) {
        if (character == '-' && !has_digit) {
            negative = true;
            continue;
        }
        if (character < '0' || character > '9') {
            break;
        }
        has_digit = true;
        value = (value * 10) + (character - '0');
    }
    if (!has_digit) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

std::optional<bool> parse_bool_value(const std::string_view text) {
    if (text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "yes") {
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE" || text == "off" || text == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_bytes_after(std::string_view message, std::string_view key) {
    const auto raw_value = extract_value_after(message, key);
    if (!raw_value.has_value()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    bool has_digit = false;
    for (const char character : *raw_value) {
        if (character < '0' || character > '9') {
            break;
        }
        has_digit = true;
        value = (value * 10ULL) + static_cast<std::uint64_t>(character - '0');
    }
    return has_digit ? std::optional<std::uint64_t>(value) : std::nullopt;
}

void apply_optional(CrashContextSnapshot &snapshot, const CrashContextUpdate &update) {
    if (update.queue_job_index.has_value()) {
        snapshot.queue_job_index = *update.queue_job_index;
    }
    if (update.runner_slot_index.has_value()) {
        snapshot.runner_slot_index = *update.runner_slot_index;
    }
    if (update.active_job_count.has_value()) {
        snapshot.active_job_count = *update.active_job_count;
    }
    if (update.input_path.has_value()) {
        snapshot.input_path = *update.input_path;
    }
    if (update.output_path.has_value()) {
        snapshot.output_path = *update.output_path;
    }
    if (update.subtitle_path.has_value()) {
        snapshot.subtitle_path = *update.subtitle_path;
    }
    if (update.source_codec.has_value()) {
        snapshot.source_codec = *update.source_codec;
    }
    if (update.source_pixel_format.has_value()) {
        snapshot.source_pixel_format = *update.source_pixel_format;
    }
    if (update.decoded_frame_format.has_value()) {
        snapshot.decoded_frame_format = *update.decoded_frame_format;
    }
    if (update.video_output_codec.has_value()) {
        snapshot.video_output_codec = *update.video_output_codec;
    }
    if (update.resolution.has_value()) {
        snapshot.resolution = *update.resolution;
    }
    if (update.current_stage.has_value()) {
        snapshot.current_stage = *update.current_stage;
    }
    if (update.segment_name.has_value()) {
        snapshot.segment_name = *update.segment_name;
    }
    if (update.frame_index.has_value()) {
        snapshot.frame_index = *update.frame_index;
    }
    if (update.pts.has_value()) {
        snapshot.pts = *update.pts;
    }
    if (update.decoder_thread_count.has_value()) {
        snapshot.decoder_thread_count = *update.decoder_thread_count;
    }
    if (update.decoder_thread_type.has_value()) {
        snapshot.decoder_thread_type = *update.decoder_thread_type;
    }
    if (update.encoder_thread_count.has_value()) {
        snapshot.encoder_thread_count = *update.encoder_thread_count;
    }
    if (update.encoder_thread_type.has_value()) {
        snapshot.encoder_thread_type = *update.encoder_thread_type;
    }
    if (update.subtitle_enabled.has_value()) {
        snapshot.subtitle_enabled = *update.subtitle_enabled;
    }
    if (update.subtitle_setup_mode.has_value()) {
        snapshot.subtitle_setup_mode = *update.subtitle_setup_mode;
    }
    if (update.frame_transfer_path.has_value()) {
        snapshot.frame_transfer_path = *update.frame_transfer_path;
    }
    if (update.current_rss_bytes.has_value()) {
        snapshot.current_rss_bytes = *update.current_rss_bytes;
    }
    if (update.peak_rss_bytes.has_value()) {
        snapshot.peak_rss_bytes = *update.peak_rss_bytes;
    }
    if (update.cancellation_requested.has_value()) {
        snapshot.cancellation_requested = *update.cancellation_requested;
    }
    if (update.subtitle_renderer_ptr.has_value()) {
        snapshot.subtitle_renderer_ptr = *update.subtitle_renderer_ptr;
    }
    if (update.subtitle_track_ptr.has_value()) {
        snapshot.subtitle_track_ptr = *update.subtitle_track_ptr;
    }
    if (update.subtitle_library_ptr.has_value()) {
        snapshot.subtitle_library_ptr = *update.subtitle_library_ptr;
    }
    if (update.subtitle_render_thread_id.has_value()) {
        snapshot.subtitle_render_thread_id = *update.subtitle_render_thread_id;
    }
    if (update.subtitle_renderer_created_thread_id.has_value()) {
        snapshot.subtitle_renderer_created_thread_id = *update.subtitle_renderer_created_thread_id;
    }
    if (update.subtitle_renderer_destroyed_thread_id.has_value()) {
        snapshot.subtitle_renderer_destroyed_thread_id = *update.subtitle_renderer_destroyed_thread_id;
    }
    if (update.active_subtitle_render_count.has_value()) {
        snapshot.active_subtitle_render_count = *update.active_subtitle_render_count;
    }
    if (update.last_subtitle_render_start_pts.has_value()) {
        snapshot.last_subtitle_render_start_pts = *update.last_subtitle_render_start_pts;
    }
    if (update.last_subtitle_render_end_pts.has_value()) {
        snapshot.last_subtitle_render_end_pts = *update.last_subtitle_render_end_pts;
    }
    if (update.last_subtitle_event_count.has_value()) {
        snapshot.last_subtitle_event_count = *update.last_subtitle_event_count;
    }
    if (update.registered_image_asset_count.has_value()) {
        snapshot.registered_image_asset_count = *update.registered_image_asset_count;
    }
    if (update.last_registered_image_asset_name.has_value()) {
        snapshot.last_registered_image_asset_name = *update.last_registered_image_asset_name;
    }
    if (update.last_registered_image_asset_path.has_value()) {
        snapshot.last_registered_image_asset_path = *update.last_registered_image_asset_path;
    }
    if (update.subtitle_cleanup_started.has_value()) {
        snapshot.subtitle_cleanup_started = *update.subtitle_cleanup_started;
    }
    if (update.mangetsu_colorcoding_accepted_lines.has_value()) {
        snapshot.mangetsu_colorcoding_accepted_lines = *update.mangetsu_colorcoding_accepted_lines;
    }
    if (update.mangetsu_colorcoding_feed_completed.has_value()) {
        snapshot.mangetsu_colorcoding_feed_completed = *update.mangetsu_colorcoding_feed_completed;
    }
    if (update.build_version.has_value()) {
        snapshot.build_version = *update.build_version;
    }
    if (update.git_commit.has_value()) {
        snapshot.git_commit = *update.git_commit;
    }
    if (update.last_log_message.has_value()) {
        snapshot.last_log_message = sanitize_message_value(*update.last_log_message);
    }
}

#if defined(_WIN32)
std::filesystem::path local_app_data_directory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return std::filesystem::path(buffer.data());
    }
    const DWORD temp_length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (temp_length > 0 && temp_length < buffer.size()) {
        return std::filesystem::path(buffer.data());
    }
    return std::filesystem::current_path();
}

std::filesystem::path executable_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return std::filesystem::path(buffer.data());
    }
    return {};
}

std::filesystem::path crash_dump_directory_override() {
    std::array<wchar_t, MAX_PATH * 4> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        L"UTSURE_CRASH_DUMP_DIR",
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length > 0 && length < buffer.size()) {
        return std::filesystem::path(buffer.data());
    }
    return {};
}

void update_memory_snapshot(CrashContextUpdate &update) noexcept {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        if (!update.current_rss_bytes.has_value()) {
            update.current_rss_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
        }
        if (!update.peak_rss_bytes.has_value()) {
            update.peak_rss_bytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
        }
    }
}

MINIDUMP_TYPE selected_dump_type() noexcept {
    DWORD flags = MiniDumpNormal |
        MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules |
        MiniDumpWithProcessThreadData;
    const char *full_dump = std::getenv("UTSURE_FULL_CRASH_DUMP");
    if (full_dump != nullptr &&
        std::string_view(full_dump) != "0" &&
        std::string_view(full_dump) != "false" &&
        std::string_view(full_dump) != "FALSE") {
        flags |= MiniDumpWithFullMemory |
            MiniDumpWithHandleData |
            MiniDumpWithIndirectlyReferencedMemory;
    }
    return static_cast<MINIDUMP_TYPE>(flags);
}

std::string selected_dump_type_name() {
    const char *full_dump = std::getenv("UTSURE_FULL_CRASH_DUMP");
    if (full_dump != nullptr &&
        std::string_view(full_dump) != "0" &&
        std::string_view(full_dump) != "false" &&
        std::string_view(full_dump) != "FALSE") {
        return "full";
    }
    return "normal";
}

bool is_hard_fault_exception_code(const DWORD code) noexcept {
    constexpr DWORD kStatusHeapCorruption = 0xC0000374UL;
    constexpr DWORD kStatusStackBufferOverrun = 0xC0000409UL;

    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_INVALID_DISPOSITION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case kStatusHeapCorruption:
    case kStatusStackBufferOverrun:
        return true;
    default:
        return false;
    }
}

bool should_write_vectored_exception_dump(EXCEPTION_POINTERS *exception_pointers) noexcept {
    if (exception_pointers == nullptr || exception_pointers->ExceptionRecord == nullptr) {
        return false;
    }

    return is_hard_fault_exception_code(exception_pointers->ExceptionRecord->ExceptionCode);
}

unsigned long exception_code_from_pointers(void *exception_pointers) noexcept {
    if (exception_pointers == nullptr) {
        return 0UL;
    }
    const auto *pointers = static_cast<EXCEPTION_POINTERS *>(exception_pointers);
    if (pointers->ExceptionRecord == nullptr) {
        return 0UL;
    }
    return static_cast<unsigned long>(pointers->ExceptionRecord->ExceptionCode);
}

std::string exception_address_from_pointers(void *exception_pointers) {
    if (exception_pointers == nullptr) {
        return {};
    }
    const auto *pointers = static_cast<EXCEPTION_POINTERS *>(exception_pointers);
    if (pointers->ExceptionRecord == nullptr) {
        return {};
    }
    std::ostringstream text;
    text << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
    return text.str();
}

struct MiniDumpAttemptResult final {
    bool file_handle_created{false};
    bool dump_written{false};
    unsigned long error_code{0};
};

MiniDumpAttemptResult write_minidump(const std::filesystem::path &dump_path, void *exception_pointers) noexcept {
    MiniDumpAttemptResult attempt{};
    HANDLE file = CreateFileW(
        dump_path.wstring().c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        attempt.error_code = static_cast<unsigned long>(GetLastError());
        return attempt;
    }
    attempt.file_handle_created = true;

    MINIDUMP_EXCEPTION_INFORMATION exception_info{};
    MINIDUMP_EXCEPTION_INFORMATION *exception_info_ptr = nullptr;
    if (exception_pointers != nullptr) {
        exception_info.ThreadId = GetCurrentThreadId();
        exception_info.ExceptionPointers = static_cast<EXCEPTION_POINTERS *>(exception_pointers);
        exception_info.ClientPointers = FALSE;
        exception_info_ptr = &exception_info;
    }

    const BOOL write_result = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        selected_dump_type(),
        exception_info_ptr,
        nullptr,
        nullptr
    );
    if (write_result == FALSE) {
        attempt.error_code = static_cast<unsigned long>(GetLastError());
    }
    CloseHandle(file);
    attempt.dump_written = write_result != FALSE;
    return attempt;
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *exception_pointers) {
    (void)write_crash_dump_for_current_process(exception_pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG CALLBACK vectored_exception_handler(EXCEPTION_POINTERS *exception_pointers) {
    if (should_write_vectored_exception_dump(exception_pointers)) {
        (void)write_crash_dump_for_current_process(exception_pointers);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminate_handler() {
    (void)write_crash_dump_for_current_process(nullptr);
    std::_Exit(3);
}

void signal_handler(int /*signal_number*/) {
    (void)write_crash_dump_for_current_process(nullptr);
    std::_Exit(3);
}
#endif

}  // namespace

std::filesystem::path raw_default_crash_dump_directory();

std::filesystem::path crash_dump_directory_for_crash_path() {
    std::unique_lock lock(crash_directory_mutex(), std::try_to_lock);
    if (!lock.owns_lock() || cached_crash_dump_directory().empty()) {
        return raw_default_crash_dump_directory();
    }
    return cached_crash_dump_directory();
}

void hold_crash_dump_directory_lock_for_test(const bool hold) {
    static auto *held_lock = new std::optional<std::unique_lock<std::mutex>>();
    if (hold) {
        if (!held_lock->has_value()) {
            held_lock->emplace(crash_directory_mutex());
        }
        return;
    }
    held_lock->reset();
}

std::filesystem::path default_crash_dump_directory() {
    {
        const std::lock_guard lock(crash_directory_mutex());
        if (!cached_crash_dump_directory().empty()) {
            return cached_crash_dump_directory();
        }
    }

    initialize_crash_dump_directory();
    const std::lock_guard lock(crash_directory_mutex());
    if (!cached_crash_dump_directory().empty()) {
        return cached_crash_dump_directory();
    }
    return raw_default_crash_dump_directory();
}

std::filesystem::path resolve_crash_dump_directory_for_test(
    const CrashDumpDirectoryResolutionOptions &options
) {
    if (options.override_directory.has_value() && !options.override_directory->empty()) {
        ensure_directory_writable(*options.override_directory);
        return *options.override_directory;
    }

    if (options.executable_path.has_value() && !options.executable_path->empty()) {
        const auto portable_directory = options.executable_path->parent_path() / "crash-dumps";
        if (!options.simulate_portable_directory_failure && ensure_directory_writable(portable_directory)) {
            return portable_directory;
        }
    }

    std::filesystem::path local_directory{};
    if (options.local_app_data_directory.has_value() && !options.local_app_data_directory->empty()) {
        local_directory = *options.local_app_data_directory / "Utsure" / "crash-dumps";
    } else {
        local_directory = raw_default_crash_dump_directory();
    }
    ensure_directory_writable(local_directory);
    return local_directory;
}

void initialize_crash_dump_directory() noexcept {
    try {
        CrashDumpDirectoryResolutionOptions options{};
#if defined(_WIN32)
        const auto override_directory = crash_dump_directory_override();
        if (!override_directory.empty()) {
            options.override_directory = override_directory;
        }
        options.executable_path = executable_path();
        options.local_app_data_directory = local_app_data_directory();
#else
        const char *override_directory = std::getenv("UTSURE_CRASH_DUMP_DIR");
        if (override_directory != nullptr && std::string_view(override_directory).size() > 0U) {
            options.override_directory = std::filesystem::path(override_directory);
        }
        options.executable_path = std::filesystem::current_path() / "utsure";
        options.local_app_data_directory = std::filesystem::temp_directory_path();
#endif
        auto resolved_directory = resolve_crash_dump_directory_for_test(options);
        const std::lock_guard lock(crash_directory_mutex());
        cached_crash_dump_directory() = std::move(resolved_directory);
    } catch (...) {
        try {
            auto fallback = raw_default_crash_dump_directory();
            ensure_directory_writable(fallback);
            const std::lock_guard lock(crash_directory_mutex());
            cached_crash_dump_directory() = std::move(fallback);
        } catch (...) {
        }
    }
}

std::filesystem::path raw_default_crash_dump_directory() {
#if defined(_WIN32)
    return local_app_data_directory() / "Utsure" / "crash-dumps";
#else
    return std::filesystem::temp_directory_path() / "Utsure" / "crash-dumps";
#endif
}

std::string make_crash_file_stem(const std::int64_t unix_seconds, const unsigned long process_id) {
    return make_crash_file_stem(unix_seconds * 1000, process_id, current_thread_id(), 0U);
}

std::string make_crash_file_stem(
    const std::int64_t unix_milliseconds,
    const unsigned long process_id,
    const unsigned long thread_id,
    const unsigned int sequence_number
) {
    const std::int64_t unix_seconds = unix_milliseconds / 1000;
    const int millisecond = static_cast<int>(unix_milliseconds % 1000);
    std::time_t raw_time = static_cast<std::time_t>(unix_seconds);
    std::tm time_parts{};
#if defined(_WIN32)
    localtime_s(&time_parts, &raw_time);
#else
    localtime_r(&raw_time, &time_parts);
#endif
    std::ostringstream stem;
    stem << "utsure-crash-"
         << (time_parts.tm_year + 1900)
         << two_digit(time_parts.tm_mon + 1)
         << two_digit(time_parts.tm_mday)
         << '-'
         << two_digit(time_parts.tm_hour)
         << two_digit(time_parts.tm_min)
         << two_digit(time_parts.tm_sec)
         << '-' << three_digit(millisecond)
         << "-pid-" << process_id
         << "-tid-" << thread_id
         << "-seq-" << four_digit(sequence_number);
    return stem.str();
}

CrashArtifactPaths make_crash_artifact_paths(
    const std::filesystem::path &directory,
    const std::int64_t unix_seconds,
    const unsigned long process_id
) {
    return make_crash_artifact_paths(directory, unix_seconds * 1000, process_id, current_thread_id(), 0U);
}

CrashArtifactPaths make_crash_artifact_paths(
    const std::filesystem::path &directory,
    const std::int64_t unix_milliseconds,
    const unsigned long process_id,
    const unsigned long thread_id,
    const unsigned int sequence_number
) {
    const auto stem = make_crash_file_stem(unix_milliseconds, process_id, thread_id, sequence_number);
    return CrashArtifactPaths{
        .dump_path = directory / (stem + ".dmp"),
        .sidecar_path = directory / (stem + ".json"),
        .log_path = directory / (stem + ".log.txt"),
        .handler_entered_path = directory / (stem + ".handler-entered.txt"),
        .dump_failed_path = directory / (stem + ".dump-failed.txt")
    };
}

CrashArtifactPaths choose_available_crash_artifact_paths(
    const std::filesystem::path &directory,
    const std::int64_t unix_milliseconds,
    const unsigned long process_id,
    const unsigned long thread_id
) {
    const unsigned int base_sequence = crash_artifact_sequence_storage().fetch_add(1U);
    for (unsigned int attempt = 0; attempt < 1000U; ++attempt) {
        const auto paths = make_crash_artifact_paths(
            directory,
            unix_milliseconds,
            process_id,
            thread_id,
            base_sequence + attempt
        );
        if (!any_crash_artifact_path_exists(paths)) {
            return paths;
        }
    }
    return make_crash_artifact_paths(
        directory,
        unix_milliseconds,
        process_id,
        thread_id,
        base_sequence + 1000U
    );
}

std::string crash_context_to_json(const CrashContextSnapshot &snapshot) {
    std::ostringstream json;
    json << "{\n";
    append_json_string(json, "schema", "utsure.crash_context.v1", true);
    append_json_string(json, "build_version", snapshot.build_version, true);
    append_json_string(json, "git_commit", snapshot.git_commit, true);
    append_json_int(json, "queue_job_index", snapshot.queue_job_index, true);
    append_json_int(json, "runner_slot_index", snapshot.runner_slot_index, true);
    append_json_int(json, "active_job_count", snapshot.active_job_count, true);
    append_json_string(json, "input_path", snapshot.input_path, true);
    append_json_string(json, "output_path", snapshot.output_path, true);
    append_json_string(json, "subtitle_path", snapshot.subtitle_path, true);
    append_json_string(json, "source_codec", snapshot.source_codec, true);
    append_json_string(json, "source_pixel_format", snapshot.source_pixel_format, true);
    append_json_string(json, "decoded_frame_format", snapshot.decoded_frame_format, true);
    append_json_string(json, "video_output_codec", snapshot.video_output_codec, true);
    append_json_string(json, "resolution", snapshot.resolution, true);
    append_json_string(json, "current_stage", snapshot.current_stage, true);
    append_json_string(json, "segment_name", snapshot.segment_name, true);
    append_json_int(json, "frame_index", snapshot.frame_index, true);
    append_json_int(json, "pts", snapshot.pts, true);
    append_json_int(json, "decoder_thread_count", snapshot.decoder_thread_count, true);
    append_json_string(json, "decoder_thread_type", snapshot.decoder_thread_type, true);
    append_json_int(json, "encoder_thread_count", snapshot.encoder_thread_count, true);
    append_json_string(json, "encoder_thread_type", snapshot.encoder_thread_type, true);
    append_json_bool(json, "subtitle_enabled", snapshot.subtitle_enabled, true);
    append_json_string(json, "subtitle_setup_mode", snapshot.subtitle_setup_mode, true);
    append_json_string(json, "frame_transfer_path", snapshot.frame_transfer_path, true);
    append_json_uint(json, "current_rss_bytes", snapshot.current_rss_bytes, true);
    append_json_uint(json, "peak_rss_bytes", snapshot.peak_rss_bytes, true);
    append_json_bool(json, "cancellation_requested", snapshot.cancellation_requested, true);
    append_json_string(json, "subtitle_renderer_ptr", snapshot.subtitle_renderer_ptr, true);
    append_json_string(json, "subtitle_track_ptr", snapshot.subtitle_track_ptr, true);
    append_json_string(json, "subtitle_library_ptr", snapshot.subtitle_library_ptr, true);
    append_json_string(json, "subtitle_render_thread_id", snapshot.subtitle_render_thread_id, true);
    append_json_string(json, "subtitle_renderer_created_thread_id", snapshot.subtitle_renderer_created_thread_id, true);
    append_json_string(json, "subtitle_renderer_destroyed_thread_id", snapshot.subtitle_renderer_destroyed_thread_id, true);
    append_json_int(json, "active_subtitle_render_count", snapshot.active_subtitle_render_count, true);
    append_json_int(json, "last_subtitle_render_start_pts", snapshot.last_subtitle_render_start_pts, true);
    append_json_int(json, "last_subtitle_render_end_pts", snapshot.last_subtitle_render_end_pts, true);
    append_json_int(json, "last_subtitle_event_count", snapshot.last_subtitle_event_count, true);
    append_json_int(json, "registered_image_asset_count", snapshot.registered_image_asset_count, true);
    append_json_string(json, "last_registered_image_asset_name", snapshot.last_registered_image_asset_name, true);
    append_json_string(json, "last_registered_image_asset_path", snapshot.last_registered_image_asset_path, true);
    append_json_bool(json, "subtitle_cleanup_started", snapshot.subtitle_cleanup_started, true);
    append_json_int(json, "mangetsu_colorcoding_accepted_lines", snapshot.mangetsu_colorcoding_accepted_lines, true);
    append_json_bool(json, "mangetsu_colorcoding_feed_completed", snapshot.mangetsu_colorcoding_feed_completed, true);
    append_json_string(json, "last_log_message", snapshot.last_log_message, false);
    json << "}\n";
    return json.str();
}

void append_context_json_object(std::ostringstream &json, const CrashContextSnapshot &snapshot, const std::string &indent) {
    json << indent << "{\n";
    append_json_string(json, "schema", "utsure.crash_context.runner.v1", true);
    append_json_int(json, "queue_job_index", snapshot.queue_job_index, true);
    append_json_int(json, "runner_slot_index", snapshot.runner_slot_index, true);
    append_json_string(json, "input_path", snapshot.input_path, true);
    append_json_string(json, "output_path", snapshot.output_path, true);
    append_json_string(json, "subtitle_path", snapshot.subtitle_path, true);
    append_json_string(json, "source_codec", snapshot.source_codec, true);
    append_json_string(json, "source_pixel_format", snapshot.source_pixel_format, true);
    append_json_string(json, "decoded_frame_format", snapshot.decoded_frame_format, true);
    append_json_string(json, "video_output_codec", snapshot.video_output_codec, true);
    append_json_string(json, "resolution", snapshot.resolution, true);
    append_json_string(json, "current_stage", snapshot.current_stage, true);
    append_json_string(json, "segment_name", snapshot.segment_name, true);
    append_json_int(json, "frame_index", snapshot.frame_index, true);
    append_json_int(json, "pts", snapshot.pts, true);
    append_json_int(json, "decoder_thread_count", snapshot.decoder_thread_count, true);
    append_json_string(json, "decoder_thread_type", snapshot.decoder_thread_type, true);
    append_json_int(json, "encoder_thread_count", snapshot.encoder_thread_count, true);
    append_json_string(json, "encoder_thread_type", snapshot.encoder_thread_type, true);
    append_json_bool(json, "subtitle_enabled", snapshot.subtitle_enabled, true);
    append_json_string(json, "subtitle_setup_mode", snapshot.subtitle_setup_mode, true);
    append_json_string(json, "frame_transfer_path", snapshot.frame_transfer_path, true);
    append_json_uint(json, "current_rss_bytes", snapshot.current_rss_bytes, true);
    append_json_uint(json, "peak_rss_bytes", snapshot.peak_rss_bytes, true);
    append_json_bool(json, "cancellation_requested", snapshot.cancellation_requested, true);
    append_json_string(json, "subtitle_renderer_ptr", snapshot.subtitle_renderer_ptr, true);
    append_json_string(json, "subtitle_track_ptr", snapshot.subtitle_track_ptr, true);
    append_json_string(json, "subtitle_library_ptr", snapshot.subtitle_library_ptr, true);
    append_json_string(json, "subtitle_render_thread_id", snapshot.subtitle_render_thread_id, true);
    append_json_string(json, "subtitle_renderer_created_thread_id", snapshot.subtitle_renderer_created_thread_id, true);
    append_json_string(json, "subtitle_renderer_destroyed_thread_id", snapshot.subtitle_renderer_destroyed_thread_id, true);
    append_json_int(json, "active_subtitle_render_count", snapshot.active_subtitle_render_count, true);
    append_json_int(json, "last_subtitle_render_start_pts", snapshot.last_subtitle_render_start_pts, true);
    append_json_int(json, "last_subtitle_render_end_pts", snapshot.last_subtitle_render_end_pts, true);
    append_json_int(json, "last_subtitle_event_count", snapshot.last_subtitle_event_count, true);
    append_json_int(json, "registered_image_asset_count", snapshot.registered_image_asset_count, true);
    append_json_string(json, "last_registered_image_asset_name", snapshot.last_registered_image_asset_name, true);
    append_json_string(json, "last_registered_image_asset_path", snapshot.last_registered_image_asset_path, true);
    append_json_bool(json, "subtitle_cleanup_started", snapshot.subtitle_cleanup_started, true);
    append_json_int(json, "mangetsu_colorcoding_accepted_lines", snapshot.mangetsu_colorcoding_accepted_lines, true);
    append_json_bool(json, "mangetsu_colorcoding_feed_completed", snapshot.mangetsu_colorcoding_feed_completed, true);
    append_json_string(json, "last_log_message", snapshot.last_log_message, false);
    json << indent << "}";
}

std::string crash_context_collection_to_json(const CrashContextCollectionSnapshot &snapshot) {
    return crash_context_collection_to_json(snapshot, CrashDumpSidecarMetadata{});
}

std::string crash_context_collection_to_json(
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata
) {
    std::ostringstream json;
    json << "{\n";
    append_json_string(json, "schema", "utsure.crash_context.collection.v1", true);
    append_json_string(json, "build_version", snapshot.last_updated_context.build_version, true);
    append_json_string(json, "git_commit", snapshot.last_updated_context.git_commit, true);
    append_json_bool(json, "handler_entered", metadata.handler_entered, true);
    append_json_bool(json, "dump_write_success", metadata.dump_write_success, true);
    append_json_uint(json, "dump_write_error_code", metadata.dump_write_error_code, true);
    append_json_string(json, "dump_write_error_message", metadata.dump_write_error_message, true);
    append_json_string(json, "dump_path_attempted", metadata.dump_path_attempted, true);
    append_json_uint(json, "exception_code", metadata.seh_exception_code, true);
    append_json_string(json, "exception_address", metadata.exception_address, true);
    append_json_bool(json, "cxx_exception_active", metadata.cxx_exception_active, true);
    append_json_string(json, "cxx_exception_type", metadata.cxx_exception_type, true);
    append_json_string(json, "cxx_exception_message", metadata.cxx_exception_message, true);
    append_json_uint(json, "crashing_thread_id", snapshot.crashing_thread_id, true);
    append_json_int(json, "last_updated_runner_slot", snapshot.last_updated_runner_slot, true);
    append_json_int(json, "active_job_count", snapshot.active_job_count, true);
    append_json_int(json, "queue_job_index", snapshot.last_updated_context.queue_job_index, true);
    append_json_int(json, "runner_slot_index", snapshot.last_updated_context.runner_slot_index, true);
    append_json_string(json, "input_path", snapshot.last_updated_context.input_path, true);
    append_json_string(json, "output_path", snapshot.last_updated_context.output_path, true);
    append_json_string(json, "subtitle_path", snapshot.last_updated_context.subtitle_path, true);
    append_json_string(json, "source_codec", snapshot.last_updated_context.source_codec, true);
    append_json_string(json, "decoded_frame_format", snapshot.last_updated_context.decoded_frame_format, true);
    append_json_string(json, "current_stage", snapshot.last_updated_context.current_stage, true);
    append_json_string(json, "frame_transfer_path", snapshot.last_updated_context.frame_transfer_path, true);
    append_json_string(json, "subtitle_renderer_ptr", snapshot.last_updated_context.subtitle_renderer_ptr, true);
    append_json_string(json, "subtitle_track_ptr", snapshot.last_updated_context.subtitle_track_ptr, true);
    append_json_string(json, "subtitle_library_ptr", snapshot.last_updated_context.subtitle_library_ptr, true);
    append_json_string(json, "subtitle_render_thread_id", snapshot.last_updated_context.subtitle_render_thread_id, true);
    append_json_int(json, "active_subtitle_render_count", snapshot.last_updated_context.active_subtitle_render_count, true);
    append_json_int(json, "last_subtitle_render_start_pts", snapshot.last_updated_context.last_subtitle_render_start_pts, true);
    append_json_int(json, "last_subtitle_render_end_pts", snapshot.last_updated_context.last_subtitle_render_end_pts, true);
    append_json_int(json, "last_subtitle_event_count", snapshot.last_updated_context.last_subtitle_event_count, true);
    append_json_int(json, "registered_image_asset_count", snapshot.last_updated_context.registered_image_asset_count, true);
    append_json_bool(json, "subtitle_cleanup_started", snapshot.last_updated_context.subtitle_cleanup_started, true);
    append_json_int(
        json,
        "mangetsu_colorcoding_accepted_lines",
        snapshot.last_updated_context.mangetsu_colorcoding_accepted_lines,
        true
    );
    append_json_bool(
        json,
        "mangetsu_colorcoding_feed_completed",
        snapshot.last_updated_context.mangetsu_colorcoding_feed_completed,
        true
    );
    append_json_string(json, "last_log_message", snapshot.last_updated_context.last_log_message, true);
    json << "  \"runner_contexts\": [\n";
    for (std::size_t index = 0; index < snapshot.runner_contexts.size(); ++index) {
        append_context_json_object(json, snapshot.runner_contexts[index], "    ");
        if (index + 1U < snapshot.runner_contexts.size()) {
            json << ',';
        }
        json << '\n';
    }
    json << "  ]\n";
    json << "}\n";
    return json.str();
}

std::vector<CrashArtifactPaths> find_recent_crash_artifacts(
    const std::filesystem::path &directory,
    const std::size_t max_count
) {
    std::vector<CrashArtifactPaths> artifacts{};
    std::error_code error{};
    if (!std::filesystem::exists(directory, error)) {
        return artifacts;
    }

    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file(error)) {
            continue;
        }
        const auto path = entry.path();
        if (path.extension() != ".dmp") {
            continue;
        }
        const auto stem = path.stem().string();
        artifacts.push_back(CrashArtifactPaths{
            .dump_path = path,
            .sidecar_path = directory / (stem + ".json"),
            .log_path = directory / (stem + ".log.txt")
        });
    }

    std::sort(
        artifacts.begin(),
        artifacts.end(),
        [](const CrashArtifactPaths &left, const CrashArtifactPaths &right) {
            std::error_code left_error{};
            std::error_code right_error{};
            return std::filesystem::last_write_time(left.dump_path, left_error) >
                std::filesystem::last_write_time(right.dump_path, right_error);
        }
    );
    if (artifacts.size() > max_count) {
        artifacts.resize(max_count);
    }
    return artifacts;
}

void reset_crash_context_for_tests() {
    const std::lock_guard lock(context_mutex());
    mutable_context() = CrashContextSnapshot{};
    mutable_runner_contexts().clear();
    last_updated_runner_slot() = -1;
    active_encode_job_count_storage().store(0);
    exception_dump_already_started().store(false);
    current_thread_runner_slot = -1;
    populate_default_build_fields(mutable_context());
    {
        const std::lock_guard directory_lock(crash_directory_mutex());
        cached_crash_dump_directory().clear();
    }
}

void update_crash_context(const CrashContextUpdate &update) {
    CrashContextUpdate enriched_update = update;
    if (!enriched_update.runner_slot_index.has_value() && current_thread_runner_slot >= 0) {
        enriched_update.runner_slot_index = current_thread_runner_slot;
    }
#if defined(_WIN32)
    update_memory_snapshot(enriched_update);
#endif
    const std::lock_guard lock(context_mutex());
    auto &snapshot = mutable_context();
    populate_default_build_fields(snapshot);
    apply_optional(snapshot, enriched_update);

    if (enriched_update.runner_slot_index.has_value()) {
        auto &runner_context = runner_context_for_slot(*enriched_update.runner_slot_index);
        populate_default_build_fields(runner_context);
        apply_optional(runner_context, enriched_update);
        runner_context.runner_slot_index = *enriched_update.runner_slot_index;
        last_updated_runner_slot() = *enriched_update.runner_slot_index;
    } else if (snapshot.runner_slot_index >= 0) {
        last_updated_runner_slot() = snapshot.runner_slot_index;
    }
}

void set_current_thread_runner_slot(const int runner_slot_index) noexcept {
    current_thread_runner_slot = runner_slot_index;
}

void clear_current_thread_runner_slot() noexcept {
    current_thread_runner_slot = -1;
}

int begin_active_encode_job(const int runner_slot_index) noexcept {
    set_current_thread_runner_slot(runner_slot_index);
    const int active_count = active_encode_job_count_storage().fetch_add(1) + 1;
    try {
        update_crash_context(CrashContextUpdate{
            .runner_slot_index = runner_slot_index,
            .active_job_count = active_count,
            .current_stage = "worker_active"
        });
    } catch (...) {
    }
    return active_count;
}

int end_active_encode_job(const int runner_slot_index) noexcept {
    int active_count = active_encode_job_count_storage().load();
    while (active_count > 0 &&
           !active_encode_job_count_storage().compare_exchange_weak(active_count, active_count - 1)) {
    }
    const int remaining_count = std::max(active_count - 1, 0);
    try {
        update_crash_context(CrashContextUpdate{
            .runner_slot_index = runner_slot_index,
            .active_job_count = remaining_count,
            .current_stage = "worker_idle",
            .cancellation_requested = false
        });
    } catch (...) {
    }
    if (current_thread_runner_slot == runner_slot_index) {
        clear_current_thread_runner_slot();
    }
    return remaining_count;
}

int current_active_encode_job_count() noexcept {
    return active_encode_job_count_storage().load();
}

void update_crash_context_from_progress(const core::job::EncodeJobProgress &progress) {
    CrashContextUpdate update{};
    update.current_stage = core::job::to_string(progress.stage);
    if (progress.encoded_video_frames.has_value()) {
        update.frame_index = static_cast<std::int64_t>(*progress.encoded_video_frames);
    }
    update_crash_context(update);
}

void update_crash_context_from_runtime_log(const std::string_view message) {
    CrashContextUpdate update{};
    update.last_log_message = std::string(message);

    if (message.find("Native direct encode used") != std::string_view::npos) {
        update.frame_transfer_path = "native_direct_encode";
    } else if (message.find("Native direct encode bypassed") != std::string_view::npos ||
               message.find("sws_scale") != std::string_view::npos) {
        update.frame_transfer_path = "sws_scale";
    }
    if (message.find("Subtitle stage start") != std::string_view::npos) {
        update.current_stage = "subtitle_stage";
    } else if (message.find("feeding-mangetsu-colorcoding") != std::string_view::npos) {
        update.current_stage = "feeding-mangetsu-colorcoding";
    } else if (message.find("Decode stage start") != std::string_view::npos) {
        update.current_stage = "decode_stage";
    } else if (message.find("Encode stage start") != std::string_view::npos) {
        update.current_stage = "encode_stage";
    } else if (message.find("Mux stage start") != std::string_view::npos) {
        update.current_stage = "mux_stage";
    }

    if (auto value = extract_value_after(message, "source_codec=")) {
        update.source_codec = *value;
    }
    if (auto value = extract_value_after(message, "source codec ")) {
        update.source_codec = *value;
    }
    if (auto value = extract_value_after(message, "stream_pixel_format=")) {
        update.source_pixel_format = *value;
    }
    if (auto value = extract_value_after(message, "frame_format=")) {
        update.decoded_frame_format = *value;
    }
    if (auto value = extract_value_after(message, "resolution=")) {
        update.resolution = *value;
    }
    if (auto value = extract_value_after(message, "segment=")) {
        update.segment_name = *value;
    }
    if (auto value = extract_value_after(message, "name=")) {
        update.segment_name = *value;
    }
    if (auto value = extract_quoted_value_after(message, "subtitle_path='", '\'')) {
        update.subtitle_path = *value;
    } else if (auto value = extract_value_after(message, "subtitle_path=")) {
        update.subtitle_path = *value;
    }
    if (auto value = extract_value_after(message, "decoder_active_thread_type=")) {
        update.decoder_thread_type = *value;
    }
    if (auto value = extract_value_after(message, "encoder_active_thread_type=")) {
        update.encoder_thread_type = *value;
    }
    if (auto value = extract_value_after(message, "subtitle composition mode ")) {
        update.subtitle_setup_mode = *value;
    }
    if (auto value = extract_value_after(message, "decoder_threads=")) {
        update.decoder_thread_count = parse_int_value(*value);
    }
    if (auto value = extract_value_after(message, "encoder_threads=")) {
        update.encoder_thread_count = parse_int_value(*value);
    }
    if (auto value = extract_value_after(message, "frame=")) {
        update.frame_index = parse_int64_value(*value);
    }
    if (auto value = extract_value_after(message, "pts=")) {
        update.pts = parse_int64_value(*value);
    }
    if (message.find("subtitle render start") != std::string_view::npos) {
        update.current_stage = "subtitle_rendering";
        if (auto value = extract_value_after(message, "pts_us=")) {
            update.last_subtitle_render_start_pts = parse_int64_value(*value);
            update.pts = parse_int64_value(*value);
        }
    }
    if (message.find("subtitle render end") != std::string_view::npos) {
        if (auto value = extract_value_after(message, "pts_us=")) {
            update.last_subtitle_render_end_pts = parse_int64_value(*value);
            update.pts = parse_int64_value(*value);
        }
    }
    if (auto value = extract_value_after(message, "renderer=")) {
        update.subtitle_renderer_ptr = *value;
    }
    if (auto value = extract_value_after(message, "track=")) {
        update.subtitle_track_ptr = *value;
    }
    if (auto value = extract_value_after(message, "library=")) {
        update.subtitle_library_ptr = *value;
    }
    if (message.find("subtitle render start") != std::string_view::npos ||
        message.find("subtitle render end") != std::string_view::npos) {
        if (auto value = extract_value_after(message, "thread_id=")) {
            update.subtitle_render_thread_id = *value;
        }
    }
    if (auto value = extract_value_after(message, "subtitle_renderer_created_thread_id=")) {
        update.subtitle_renderer_created_thread_id = *value;
    }
    if (auto value = extract_value_after(message, "subtitle_renderer_destroyed_thread_id=")) {
        update.subtitle_renderer_destroyed_thread_id = *value;
    }
    if (auto value = extract_value_after(message, "active_subtitle_render_count=")) {
        update.active_subtitle_render_count = parse_int_value(*value);
    }
    if (auto value = extract_value_after(message, "last_subtitle_event_count=")) {
        update.last_subtitle_event_count = parse_int_value(*value);
    }
    if (auto value = extract_value_after(message, "registered_image_asset_count=")) {
        update.registered_image_asset_count = parse_int_value(*value);
    }
    if (auto value = extract_value_after(message, "last_registered_image_asset_name=")) {
        update.last_registered_image_asset_name = *value;
    }
    if (auto value = extract_tail_value_after(message, "last_registered_image_asset_path=")) {
        update.last_registered_image_asset_path = *value;
    }
    if (auto value = extract_value_after(message, "subtitle_cleanup_started=")) {
        update.subtitle_cleanup_started = parse_bool_value(*value);
    }
    if (auto value = extract_value_after(message, "mangetsu_colorcoding_accepted_lines=")) {
        update.mangetsu_colorcoding_accepted_lines = parse_int_value(*value);
    }
    if (auto value = extract_value_after(message, "mangetsu_colorcoding_feed_completed=")) {
        update.mangetsu_colorcoding_feed_completed = parse_bool_value(*value);
    }
    if (auto rss = parse_bytes_after(message, "current_rss=")) {
        update.current_rss_bytes = *rss;
    }
    if (auto rss = parse_bytes_after(message, "peak_rss=")) {
        update.peak_rss_bytes = *rss;
    }

    update_crash_context(update);
}

void mark_crash_context_cancellation_requested(const bool requested) {
    update_crash_context(CrashContextUpdate{.cancellation_requested = requested});
}

CrashContextSnapshot crash_context_snapshot() {
    std::unique_lock lock(context_mutex(), std::try_to_lock);
    if (!lock.owns_lock()) {
        CrashContextSnapshot fallback{};
        populate_default_build_fields(fallback);
        fallback.current_stage = "snapshot_unavailable_context_lock_busy";
        return fallback;
    }

    auto snapshot = mutable_context();
    populate_default_build_fields(snapshot);
    return snapshot;
}

CrashContextCollectionSnapshot crash_context_collection_snapshot(const unsigned long crashing_thread_id) {
    std::unique_lock lock(context_mutex(), std::try_to_lock);
    CrashContextCollectionSnapshot collection{};
    collection.crashing_thread_id = crashing_thread_id;
    collection.active_job_count = active_encode_job_count_storage().load();
    if (!lock.owns_lock()) {
        populate_default_build_fields(collection.last_updated_context);
        collection.last_updated_context.current_stage = "snapshot_unavailable_context_lock_busy";
        return collection;
    }

    collection.last_updated_runner_slot = last_updated_runner_slot();
    collection.last_updated_context = mutable_context();
    populate_default_build_fields(collection.last_updated_context);
    collection.last_updated_context.active_job_count = collection.active_job_count;
    collection.runner_contexts = mutable_runner_contexts();
    for (auto &runner_context : collection.runner_contexts) {
        populate_default_build_fields(runner_context);
        runner_context.active_job_count = collection.active_job_count;
    }
    return collection;
}

bool write_crash_sidecar_for_test(
    const CrashArtifactPaths &paths,
    const CrashContextCollectionSnapshot &snapshot,
    std::string *error_message
) {
    return write_crash_sidecar_for_test(paths, snapshot, CrashDumpSidecarMetadata{}, error_message);
}

bool write_crash_sidecar_for_test(
    const CrashArtifactPaths &paths,
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata,
    std::string *error_message
) {
    return write_text_file_no_overwrite(
        paths.sidecar_path,
        crash_context_collection_to_json(snapshot, metadata),
        error_message
    );
}

std::string crash_marker_text(
    const char *marker_name,
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata
) {
    std::ostringstream text;
    text << "marker=" << marker_name << '\n'
         << "timestamp_unix_ms=" << current_unix_milliseconds() << '\n'
         << "pid=" << current_process_id() << '\n'
         << "tid=" << snapshot.crashing_thread_id << '\n'
         << "handler_entered=" << (metadata.handler_entered ? 1 : 0) << '\n'
         << "dump_write_success=" << (metadata.dump_write_success ? 1 : 0) << '\n'
         << "dump_write_error_code=" << metadata.dump_write_error_code << '\n'
         << "dump_write_error_message=" << metadata.dump_write_error_message << '\n'
         << "dump_path_attempted=" << metadata.dump_path_attempted << '\n'
         << "exception_code=" << metadata.seh_exception_code << '\n'
         << "exception_address=" << metadata.exception_address << '\n'
         << "cxx_exception_active=" << (metadata.cxx_exception_active ? 1 : 0) << '\n'
         << "cxx_exception_type=" << metadata.cxx_exception_type << '\n'
         << "cxx_exception_message=" << metadata.cxx_exception_message << '\n'
         << "current_stage=" << snapshot.last_updated_context.current_stage << '\n'
         << "last_log_message=" << snapshot.last_updated_context.last_log_message << '\n';
    return text.str();
}

bool write_handler_entered_marker_for_test(
    const CrashArtifactPaths &paths,
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata,
    std::string *error_message
) {
    return write_text_file_no_overwrite(
        paths.handler_entered_path,
        crash_marker_text("handler_entered", snapshot, metadata),
        error_message
    );
}

bool write_dump_failed_marker_for_test(
    const CrashArtifactPaths &paths,
    const CrashContextCollectionSnapshot &snapshot,
    const CrashDumpSidecarMetadata &metadata,
    std::string *error_message
) {
    return write_text_file_no_overwrite(
        paths.dump_failed_path,
        crash_marker_text("dump_failed", snapshot, metadata),
        error_message
    );
}

void configure_crash_log_flushing() noexcept {
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
}

void install_crash_handlers() noexcept {
    initialize_crash_dump_directory();
    try {
        update_crash_context(CrashContextUpdate{
            .build_version = current_build_version(),
            .git_commit = git_commit_from_environment()
        });
    } catch (...) {
    }
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto previous_filter = SetUnhandledExceptionFilter(unhandled_exception_filter);
    {
        std::ostringstream text;
        text << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(previous_filter);
        previous_unhandled_exception_filter_text() = text.str();
    }
    unhandled_exception_filter_installed_storage().store(true);
    if (AddVectoredExceptionHandler(1, vectored_exception_handler) != nullptr) {
        vectored_exception_handler_installed_storage().store(true);
    }
    std::set_terminate(terminate_handler);
    terminate_handler_installed_storage().store(true);
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGILL, signal_handler);
    std::signal(SIGFPE, signal_handler);
    signal_handlers_installed_storage().store(true);
#endif
}

CrashDumpSetupStatus crash_dump_setup_status() noexcept {
    CrashDumpSetupStatus status{};
    status.build_version = current_build_version();
    status.git_commit = git_commit_from_environment();
    status.process_id = current_process_id();
    status.resolved_directory = default_crash_dump_directory();
    std::error_code error{};
    status.directory_exists = std::filesystem::exists(status.resolved_directory, error);
    status.directory_writable = ensure_directory_writable(status.resolved_directory);
#if defined(_WIN32)
    status.enabled = true;
    status.unhandled_exception_filter_installed = unhandled_exception_filter_installed_storage().load();
    status.vectored_exception_handler_installed = vectored_exception_handler_installed_storage().load();
    status.terminate_handler_installed = terminate_handler_installed_storage().load();
    status.signal_handlers_installed = signal_handlers_installed_storage().load();
    status.previous_unhandled_exception_filter = previous_unhandled_exception_filter_text();
#else
    status.enabled = false;
    status.previous_unhandled_exception_filter = "unsupported";
#endif
    return status;
}

std::string format_crash_dump_setup_log(const CrashDumpSetupStatus &status) {
    std::ostringstream text;
    text << "Crash dump writer: enabled=" << (status.enabled ? 1 : 0)
         << " directory=" << utf8_path_string(status.resolved_directory)
         << " exists=" << (status.directory_exists ? 1 : 0)
         << " writable=" << (status.directory_writable ? 1 : 0)
         << " seh_filter_installed=" << (status.unhandled_exception_filter_installed ? 1 : 0)
         << " vectored_handler_installed=" << (status.vectored_exception_handler_installed ? 1 : 0)
         << " terminate_handler_installed=" << (status.terminate_handler_installed ? 1 : 0)
         << " signal_handlers_installed=" << (status.signal_handlers_installed ? 1 : 0)
         << " pid=" << status.process_id
         << " build_version=\"" << status.build_version << '"'
         << " git_commit=" << status.git_commit
         << " previous_filter=" << status.previous_unhandled_exception_filter;
    return text.str();
}

CrashDumpWriteResult write_crash_dump_for_current_process(void *exception_pointers) noexcept {
    CrashDumpWriteResult result{};
    result.handler_entered = true;
    if (exception_pointers != nullptr) {
        bool already_started = false;
        if (!exception_dump_already_started().compare_exchange_strong(already_started, true)) {
            result.error_message = "Crash dump for this exception is already in progress or already written.";
            return result;
        }
    }
    bool expected = false;
    if (!dump_write_in_progress().compare_exchange_strong(expected, true)) {
        result.error_message = "Crash dump write already in progress.";
        return result;
    }
    try {
        const unsigned long crashing_thread = current_thread_id();
        const auto dump_directory = crash_dump_directory_for_crash_path();
        const auto paths = choose_available_crash_artifact_paths(
            dump_directory,
            current_unix_milliseconds(),
            current_process_id(),
            crashing_thread
        );
        result.paths = paths;
        auto snapshot = crash_context_collection_snapshot(crashing_thread);
        CrashDumpSidecarMetadata metadata{
            .handler_entered = true,
            .dump_write_success = false,
            .dump_write_error_code = 0,
            .dump_write_error_message = {},
            .dump_path_attempted = utf8_path_string(paths.dump_path)
        };
#if defined(_WIN32)
        metadata.seh_exception_code = exception_code_from_pointers(exception_pointers);
        metadata.exception_address = exception_address_from_pointers(exception_pointers);
#endif
        capture_current_cxx_exception_metadata(metadata);
        std::string marker_error{};
        result.handler_marker_written = write_handler_entered_marker_for_test(
            paths,
            snapshot,
            metadata,
            &marker_error
        );

#if defined(_WIN32)
        const auto dump_attempt = write_minidump(paths.dump_path, exception_pointers);
        result.dump_written = dump_attempt.dump_written;
        result.dump_error_code = dump_attempt.error_code;
        result.dump_type = selected_dump_type_name();
        if (!result.dump_written && result.error_message.empty()) {
            result.error_message = dump_attempt.file_handle_created
                ? "MiniDumpWriteDump failed."
                : "Failed to create crash dump file.";
        }
#else
        result.dump_written = false;
        result.dump_type = "unsupported";
        result.dump_error_code = 0;
        if (result.error_message.empty()) {
            result.error_message = "Crash dumps are only supported on Windows.";
        }
#endif
        metadata.dump_write_success = result.dump_written;
        metadata.dump_write_error_code = result.dump_error_code;
        metadata.dump_write_error_message = result.error_message;
        if (!result.dump_written) {
            std::string failure_marker_error{};
            result.failure_marker_written = write_dump_failed_marker_for_test(
                paths,
                snapshot,
                metadata,
                &failure_marker_error
            );
            if (!result.failure_marker_written && result.error_message.empty()) {
                result.error_message = failure_marker_error;
            }
        }
        std::string sidecar_error{};
        result.sidecar_written = write_crash_sidecar_for_test(paths, snapshot, metadata, &sidecar_error);
        if (!result.sidecar_written && result.error_message.empty()) {
            result.error_message = sidecar_error;
        }
        std::fprintf(
            stderr,
            "utsure crash dump: dump=%s sidecar=%s type=%s handler_marker=%d written=%d sidecar_written=%d failure_marker=%d error=%lu\n",
            utf8_path_string(paths.dump_path).c_str(),
            utf8_path_string(paths.sidecar_path).c_str(),
            result.dump_type.c_str(),
            result.handler_marker_written ? 1 : 0,
            result.dump_written ? 1 : 0,
            result.sidecar_written ? 1 : 0,
            result.failure_marker_written ? 1 : 0,
            result.dump_error_code
        );
    } catch (...) {
        result.error_message = "Crash dump writer raised an unexpected exception.";
    }
    dump_write_in_progress().store(false);
    return result;
}

CrashDumpWriteResult write_diagnostic_dump_now() noexcept {
    try {
        update_crash_context(CrashContextUpdate{.current_stage = "manual_diagnostic_dump"});
    } catch (...) {
    }
    return write_crash_dump_for_current_process(nullptr);
}

}  // namespace utsure::app::crash
