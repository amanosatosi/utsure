#include "external_tool_runner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace utsure::core::process {

namespace {

std::filesystem::path normalize_existing_path(const std::filesystem::path &path) {
    std::error_code error;
    const auto absolute_path = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : absolute_path.lexically_normal();
}

bool executable_exists(const std::filesystem::path &path) {
    if (path.empty()) {
        return false;
    }

    std::error_code error;
    return std::filesystem::exists(path, error) &&
        !error &&
        std::filesystem::is_regular_file(path, error) &&
        !error;
}

bool path_is_explicit(const std::string &candidate) {
    return candidate.find('/') != std::string::npos || candidate.find('\\') != std::string::npos;
}

std::vector<std::string> split_string(const std::string &value, const char delimiter) {
    std::vector<std::string> parts{};
    std::size_t cursor = 0;
    while (cursor <= value.size()) {
        const std::size_t next_delimiter = value.find(delimiter, cursor);
        const std::size_t part_length = next_delimiter == std::string::npos
            ? value.size() - cursor
            : next_delimiter - cursor;
        parts.push_back(value.substr(cursor, part_length));
        if (next_delimiter == std::string::npos) {
            break;
        }
        cursor = next_delimiter + 1;
    }

    return parts;
}

#ifdef _WIN32
class WindowsHandle final {
public:
    WindowsHandle() = default;
    explicit WindowsHandle(HANDLE value) : value_(value) {}

    ~WindowsHandle() {
        reset();
    }

    WindowsHandle(const WindowsHandle &) = delete;
    WindowsHandle &operator=(const WindowsHandle &) = delete;

    WindowsHandle(WindowsHandle &&other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }

    WindowsHandle &operator=(WindowsHandle &&other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE released = value_;
        value_ = nullptr;
        return released;
    }

    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_{nullptr};
};

std::wstring utf8_to_wstring(const std::string &value) {
    if (value.empty()) {
        return {};
    }

    const int required_size = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0
    );
    if (required_size <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring wide_value(static_cast<std::size_t>(required_size), L'\0');
    const int converted_size = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        wide_value.data(),
        required_size
    );
    if (converted_size <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    return wide_value;
}

std::wstring quote_windows_command_line_argument(const std::wstring &argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    bool needs_quotes = false;
    for (const wchar_t character : argument) {
        if (character == L' ' || character == L'\t' || character == L'\n' || character == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return argument;
    }

    std::wstring quoted{L"\""};
    std::size_t backslash_count = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslash_count;
            continue;
        }

        if (character == L'"') {
            quoted.append(backslash_count * 2U + 1U, L'\\');
            quoted.push_back(character);
            backslash_count = 0;
            continue;
        }

        quoted.append(backslash_count, L'\\');
        backslash_count = 0;
        quoted.push_back(character);
    }
    quoted.append(backslash_count * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring build_windows_command_line(
    const std::filesystem::path &executable,
    const std::vector<std::string> &arguments
) {
    std::wstring command_line = quote_windows_command_line_argument(executable.wstring());
    for (const auto &argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_windows_command_line_argument(utf8_to_wstring(argument));
    }
    return command_line;
}

std::string read_available_pipe_output(HANDLE pipe) {
    std::string output{};
    std::array<char, 4096> buffer{};

    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == 0 || available == 0) {
            break;
        }

        const DWORD bytes_to_read = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD bytes_read = 0;
        if (ReadFile(pipe, buffer.data(), bytes_to_read, &bytes_read, nullptr) == 0 || bytes_read == 0) {
            break;
        }
        output.append(buffer.data(), buffer.data() + bytes_read);
    }

    return output;
}

std::vector<std::string> windows_executable_suffixes(const std::string &candidate_name) {
    const std::filesystem::path candidate_path(candidate_name);
    if (candidate_path.has_extension()) {
        return {candidate_name};
    }

    std::vector<std::string> suffixes{};
    const char *path_ext_env = std::getenv("PATHEXT");
    const std::string path_ext = (path_ext_env != nullptr && path_ext_env[0] != '\0')
        ? path_ext_env
        : ".COM;.EXE;.BAT;.CMD";
    for (const auto &raw_suffix : split_string(path_ext, ';')) {
        if (raw_suffix.empty()) {
            continue;
        }

        std::string suffix = raw_suffix;
        for (auto &character : suffix) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        suffixes.push_back(candidate_name + suffix);
    }

    suffixes.push_back(candidate_name);
    return suffixes;
}
#endif

std::optional<std::filesystem::path> resolve_explicit_executable_path(const std::string &candidate_name) {
    const std::filesystem::path candidate_path(candidate_name);
    if (!executable_exists(candidate_path)) {
        return std::nullopt;
    }

    return normalize_existing_path(candidate_path);
}

}  // namespace

bool ExternalToolRunResult::succeeded() const noexcept {
    return launched && exit_code == 0 && failure_message.empty();
}

std::optional<std::filesystem::path> find_executable_on_path(const std::vector<std::string> &candidate_names) {
    const char *path_env = std::getenv("PATH");
    const std::string search_path = path_env != nullptr ? path_env : "";
#ifdef _WIN32
    constexpr char kPathDelimiter = ';';
#else
    constexpr char kPathDelimiter = ':';
#endif

    for (const auto &candidate_name : candidate_names) {
        if (candidate_name.empty()) {
            continue;
        }

        if (path_is_explicit(candidate_name) || std::filesystem::path(candidate_name).is_absolute()) {
            const auto explicit_match = resolve_explicit_executable_path(candidate_name);
            if (explicit_match.has_value()) {
                return explicit_match;
            }
            continue;
        }

        const auto search_directories = split_string(search_path, kPathDelimiter);
        for (const auto &directory_text : search_directories) {
            if (directory_text.empty()) {
                continue;
            }

            const std::filesystem::path directory_path(directory_text);
#ifdef _WIN32
            const auto candidate_variants = windows_executable_suffixes(candidate_name);
#else
            const std::array<std::string, 1> candidate_variants{candidate_name};
#endif
            for (const auto &candidate_variant : candidate_variants) {
                const auto candidate_path = directory_path / candidate_variant;
                if (!executable_exists(candidate_path)) {
                    continue;
                }

                return normalize_existing_path(candidate_path);
            }
        }
    }

    return std::nullopt;
}

ExternalToolRunResult run_external_tool(const ExternalToolRunRequest &request) noexcept {
    if (request.executable.empty()) {
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = "No executable path was provided for the external tool invocation."
        };
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES pipe_security{};
    pipe_security.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipe_security.bInheritHandle = TRUE;

    HANDLE raw_read_pipe = nullptr;
    HANDLE raw_write_pipe = nullptr;
    if (CreatePipe(&raw_read_pipe, &raw_write_pipe, &pipe_security, 0) == 0) {
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = "Failed to create output capture pipe for external tool invocation."
        };
    }
    WindowsHandle read_pipe(raw_read_pipe);
    WindowsHandle write_pipe(raw_write_pipe);
    SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);

    WindowsHandle stdin_handle(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &pipe_security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    ));

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(STARTUPINFOW);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = stdin_handle.get() != INVALID_HANDLE_VALUE ? stdin_handle.get() : nullptr;
    startup_info.hStdOutput = write_pipe.get();
    startup_info.hStdError = write_pipe.get();

    PROCESS_INFORMATION process_info{};
    auto command_line = build_windows_command_line(request.executable, request.arguments);
    const BOOL created = CreateProcessW(
        request.executable.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup_info,
        &process_info
    );
    if (created == 0) {
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = "CreateProcessW failed for external tool invocation."
        };
    }

    WindowsHandle process_handle(process_info.hProcess);
    WindowsHandle thread_handle(process_info.hThread);
    write_pipe.reset();

    std::string combined_output{};
    for (;;) {
        combined_output += read_available_pipe_output(read_pipe.get());
        const DWORD wait_result = WaitForSingleObject(process_handle.get(), 20);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result != WAIT_TIMEOUT) {
            return ExternalToolRunResult{
                .launched = true,
                .exit_code = -1,
                .failure_message = "Failed while waiting for external tool process.",
                .combined_output = std::move(combined_output)
            };
        }
    }
    combined_output += read_available_pipe_output(read_pipe.get());

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_handle.get(), &exit_code) == 0) {
        return ExternalToolRunResult{
            .launched = true,
            .exit_code = -1,
            .failure_message = "Failed to read external tool exit code.",
            .combined_output = std::move(combined_output)
        };
    }

    return ExternalToolRunResult{
        .launched = true,
        .exit_code = static_cast<int>(exit_code),
        .failure_message = {},
        .combined_output = std::move(combined_output)
    };
#else
    int output_pipe[2]{-1, -1};
    if (pipe(output_pipe) != 0) {
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = std::strerror(errno)
        };
    }

    std::vector<std::string> argument_storage{};
    argument_storage.reserve(request.arguments.size() + 1U);
    argument_storage.push_back(request.executable.string());
    argument_storage.insert(argument_storage.end(), request.arguments.begin(), request.arguments.end());

    std::vector<char *> argument_pointers{};
    argument_pointers.reserve(argument_storage.size() + 1U);
    for (auto &argument : argument_storage) {
        argument_pointers.push_back(argument.data());
    }
    argument_pointers.push_back(nullptr);

    posix_spawn_file_actions_t file_actions{};
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, output_pipe[0]);

    pid_t child_pid = 0;
    const int spawn_result = posix_spawnp(
        &child_pid,
        request.executable.c_str(),
        &file_actions,
        nullptr,
        argument_pointers.data(),
        environ
    );
    posix_spawn_file_actions_destroy(&file_actions);
    close(output_pipe[1]);
    if (spawn_result != 0) {
        close(output_pipe[0]);
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = std::strerror(spawn_result)
        };
    }

    std::string combined_output{};
    std::array<char, 4096> output_buffer{};
    for (;;) {
        const ssize_t bytes_read = read(output_pipe[0], output_buffer.data(), output_buffer.size());
        if (bytes_read > 0) {
            combined_output.append(output_buffer.data(), output_buffer.data() + bytes_read);
            continue;
        }
        break;
    }
    close(output_pipe[0]);

    int wait_status = 0;
    if (waitpid(child_pid, &wait_status, 0) < 0) {
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = std::strerror(errno),
            .combined_output = std::move(combined_output)
        };
    }

    if (WIFEXITED(wait_status)) {
        return ExternalToolRunResult{
            .launched = true,
            .exit_code = WEXITSTATUS(wait_status),
            .failure_message = {},
            .combined_output = std::move(combined_output)
        };
    }

    if (WIFSIGNALED(wait_status)) {
        return ExternalToolRunResult{
            .launched = true,
            .exit_code = 128 + WTERMSIG(wait_status),
            .failure_message = {},
            .combined_output = std::move(combined_output)
        };
    }

    return ExternalToolRunResult{
        .launched = true,
        .exit_code = wait_status,
        .failure_message = {},
        .combined_output = std::move(combined_output)
    };
#endif
}

}  // namespace utsure::core::process
