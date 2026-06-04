#include "crash_dump_writer.hpp"

#include "utsure/core/build_info.hpp"
#include "utsure/core/job/encode_job.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
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

std::int64_t current_unix_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
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

std::string sanitize_message_value(std::string value) {
    constexpr std::size_t kMaxStoredMessageBytes = 4096U;
    if (value.size() > kMaxStoredMessageBytes) {
        value.resize(kMaxStoredMessageBytes);
        value += "...";
    }
    return value;
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
    std::size_t value_end = message.find_first_of(",.;\n", value_start);
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
        update.current_rss_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
        update.peak_rss_bytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
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

BOOL write_minidump(const std::filesystem::path &dump_path, void *exception_pointers) noexcept {
    HANDLE file = CreateFileW(
        dump_path.wstring().c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception_info{};
    MINIDUMP_EXCEPTION_INFORMATION *exception_info_ptr = nullptr;
    if (exception_pointers != nullptr) {
        exception_info.ThreadId = GetCurrentThreadId();
        exception_info.ExceptionPointers = static_cast<EXCEPTION_POINTERS *>(exception_pointers);
        exception_info.ClientPointers = FALSE;
        exception_info_ptr = &exception_info;
    }

    const BOOL result = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        selected_dump_type(),
        exception_info_ptr,
        nullptr,
        nullptr
    );
    CloseHandle(file);
    return result;
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *exception_pointers) {
    write_crash_dump_for_current_process(exception_pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

void terminate_handler() {
    write_crash_dump_for_current_process(nullptr);
    std::_Exit(3);
}

void signal_handler(int /*signal_number*/) {
    write_crash_dump_for_current_process(nullptr);
    std::_Exit(3);
}
#endif

}  // namespace

std::filesystem::path raw_default_crash_dump_directory();

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
         << "-pid-" << process_id;
    return stem.str();
}

CrashArtifactPaths make_crash_artifact_paths(
    const std::filesystem::path &directory,
    const std::int64_t unix_seconds,
    const unsigned long process_id
) {
    const auto stem = make_crash_file_stem(unix_seconds, process_id);
    return CrashArtifactPaths{
        .dump_path = directory / (stem + ".dmp"),
        .sidecar_path = directory / (stem + ".json"),
        .log_path = directory / (stem + ".log.txt")
    };
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
    append_json_string(json, "last_log_message", snapshot.last_log_message, false);
    json << indent << "}";
}

std::string crash_context_collection_to_json(const CrashContextCollectionSnapshot &snapshot) {
    std::ostringstream json;
    json << "{\n";
    append_json_string(json, "schema", "utsure.crash_context.collection.v1", true);
    append_json_string(json, "build_version", snapshot.last_updated_context.build_version, true);
    append_json_string(json, "git_commit", snapshot.last_updated_context.git_commit, true);
    append_json_uint(json, "crashing_thread_id", snapshot.crashing_thread_id, true);
    append_json_int(json, "last_updated_runner_slot", snapshot.last_updated_runner_slot, true);
    append_json_int(json, "active_job_count", snapshot.active_job_count, true);
    append_json_int(json, "queue_job_index", snapshot.last_updated_context.queue_job_index, true);
    append_json_int(json, "runner_slot_index", snapshot.last_updated_context.runner_slot_index, true);
    append_json_string(json, "input_path", snapshot.last_updated_context.input_path, true);
    append_json_string(json, "output_path", snapshot.last_updated_context.output_path, true);
    append_json_string(json, "source_codec", snapshot.last_updated_context.source_codec, true);
    append_json_string(json, "decoded_frame_format", snapshot.last_updated_context.decoded_frame_format, true);
    append_json_string(json, "current_stage", snapshot.last_updated_context.current_stage, true);
    append_json_string(json, "frame_transfer_path", snapshot.last_updated_context.frame_transfer_path, true);
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
    if (!enriched_update.active_job_count.has_value()) {
        snapshot.active_job_count = active_encode_job_count_storage().load();
    }

    if (enriched_update.runner_slot_index.has_value()) {
        auto &runner_context = runner_context_for_slot(*enriched_update.runner_slot_index);
        populate_default_build_fields(runner_context);
        apply_optional(runner_context, enriched_update);
        runner_context.runner_slot_index = *enriched_update.runner_slot_index;
        if (!enriched_update.active_job_count.has_value()) {
            runner_context.active_job_count = active_encode_job_count_storage().load();
        }
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
    std::error_code error{};
    std::filesystem::create_directories(paths.sidecar_path.parent_path(), error);
    if (error) {
        if (error_message != nullptr) {
            *error_message = error.message();
        }
        return false;
    }

    std::ofstream sidecar(paths.sidecar_path, std::ios::binary | std::ios::trunc);
    if (!sidecar) {
        if (error_message != nullptr) {
            *error_message = "Failed to open crash sidecar for writing.";
        }
        return false;
    }
    sidecar << crash_context_collection_to_json(snapshot);
    return static_cast<bool>(sidecar);
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
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    std::set_terminate(terminate_handler);
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGILL, signal_handler);
    std::signal(SIGFPE, signal_handler);
#endif
}

CrashDumpWriteResult write_crash_dump_for_current_process(void *exception_pointers) noexcept {
    CrashDumpWriteResult result{};
    bool expected = false;
    if (!dump_write_in_progress().compare_exchange_strong(expected, true)) {
        result.error_message = "Crash dump write already in progress.";
        return result;
    }
    try {
        const unsigned long crashing_thread = current_thread_id();
        std::filesystem::path dump_directory{};
        {
            const std::lock_guard lock(crash_directory_mutex());
            dump_directory = cached_crash_dump_directory();
        }
        if (dump_directory.empty()) {
            dump_directory = raw_default_crash_dump_directory();
        }
        const auto paths = make_crash_artifact_paths(
            dump_directory,
            current_unix_seconds(),
            current_process_id()
        );
        result.paths = paths;

#if defined(_WIN32)
        result.dump_written = write_minidump(paths.dump_path, exception_pointers) != FALSE;
        result.dump_type = selected_dump_type_name();
        if (!result.dump_written && result.error_message.empty()) {
            result.error_message = "MiniDumpWriteDump failed.";
        }
#else
        result.dump_written = false;
        result.dump_type = "unsupported";
        if (result.error_message.empty()) {
            result.error_message = "Crash dumps are only supported on Windows.";
        }
#endif
        const auto snapshot = crash_context_collection_snapshot(crashing_thread);
        std::string sidecar_error{};
        result.sidecar_written = write_crash_sidecar_for_test(paths, snapshot, &sidecar_error);
        if (!result.sidecar_written && result.error_message.empty()) {
            result.error_message = sidecar_error;
        }
        std::fprintf(
            stderr,
            "utsure crash dump: dump=%s sidecar=%s type=%s written=%d sidecar_written=%d\n",
            utf8_path_string(paths.dump_path).c_str(),
            utf8_path_string(paths.sidecar_path).c_str(),
            result.dump_type.c_str(),
            result.dump_written ? 1 : 0,
            result.sidecar_written ? 1 : 0
        );
    } catch (...) {
        result.error_message = "Crash dump writer raised an unexpected exception.";
    }
    dump_write_in_progress().store(false);
    return result;
}

}  // namespace utsure::app::crash
