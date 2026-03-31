#include "external_tool_runner.hpp"

#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
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
    std::vector<std::wstring> argument_storage{};
    argument_storage.reserve(request.arguments.size() + 1U);
    argument_storage.push_back(request.executable.wstring());
    for (const auto &argument : request.arguments) {
        argument_storage.push_back(utf8_to_wstring(argument));
    }

    std::vector<wchar_t *> argument_pointers{};
    argument_pointers.reserve(argument_storage.size() + 1U);
    for (auto &argument : argument_storage) {
        argument_pointers.push_back(argument.data());
    }
    argument_pointers.push_back(nullptr);

    errno = 0;
    const intptr_t spawn_result = _wspawnvp(_P_WAIT, request.executable.c_str(), argument_pointers.data());
    if (spawn_result == -1) {
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = std::strerror(errno)
        };
    }

    return ExternalToolRunResult{
        .launched = true,
        .exit_code = static_cast<int>(spawn_result),
        .failure_message = {}
    };
#else
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

    pid_t child_pid = 0;
    const int spawn_result = posix_spawnp(
        &child_pid,
        request.executable.c_str(),
        nullptr,
        nullptr,
        argument_pointers.data(),
        environ
    );
    if (spawn_result != 0) {
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = std::strerror(spawn_result)
        };
    }

    int wait_status = 0;
    if (waitpid(child_pid, &wait_status, 0) < 0) {
        return ExternalToolRunResult{
            .launched = false,
            .exit_code = -1,
            .failure_message = std::strerror(errno)
        };
    }

    if (WIFEXITED(wait_status)) {
        return ExternalToolRunResult{
            .launched = true,
            .exit_code = WEXITSTATUS(wait_status),
            .failure_message = {}
        };
    }

    if (WIFSIGNALED(wait_status)) {
        return ExternalToolRunResult{
            .launched = true,
            .exit_code = 128 + WTERMSIG(wait_status),
            .failure_message = {}
        };
    }

    return ExternalToolRunResult{
        .launched = true,
        .exit_code = wait_status,
        .failure_message = {}
    };
#endif
}

}  // namespace utsure::core::process
