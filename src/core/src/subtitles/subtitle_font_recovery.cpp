#include "utsure/core/subtitles/subtitle_font_recovery.hpp"

#include "../process/external_tool_runner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace utsure::core::subtitles {

namespace {

constexpr std::string_view kFontCollectorEnvVar = "UTSURE_FONTCOLLECTOR_PATH";

std::string lowercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

std::string path_to_utf8_string(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto normalized = path.lexically_normal().u8string();
    return std::string(reinterpret_cast<const char *>(normalized.c_str()), normalized.size());
#else
    return path.lexically_normal().string();
#endif
}

std::string format_path_leaf(const std::filesystem::path &path) {
    const auto leaf = path.filename();
    return leaf.empty() ? path.lexically_normal().string() : leaf.string();
}

std::string join_font_file_names(const std::vector<std::filesystem::path> &font_files) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < font_files.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << format_path_leaf(font_files[index]);
    }
    return stream.str();
}

bool file_has_font_extension(const std::filesystem::path &path) {
    static constexpr std::array<std::string_view, 6> kFontExtensions{
        ".ttf",
        ".otf",
        ".ttc",
        ".otc",
        ".woff",
        ".woff2"
    };

    const std::string extension = lowercase_ascii(path.extension().string());
    return std::any_of(
        kFontExtensions.begin(),
        kFontExtensions.end(),
        [&extension](const std::string_view candidate) {
            return extension == candidate;
        }
    );
}

bool request_uses_ass_subtitles(const SubtitleRenderSessionCreateRequest &request) {
    const std::string normalized_hint = lowercase_ascii(request.format_hint);
    if (normalized_hint == "ass") {
        return true;
    }

    if (normalized_hint == "ssa") {
        return false;
    }

    const std::string extension = lowercase_ascii(request.subtitle_path.extension().string());
    return extension == ".ass";
}

std::uint64_t current_process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::filesystem::path make_temporary_root() {
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root_name =
        "utsure-fontcollector-" + std::to_string(current_process_id()) + "-" + std::to_string(timestamp);
    return std::filesystem::temp_directory_path() / root_name;
}

std::vector<std::filesystem::path> collect_recovered_font_files(const std::filesystem::path &root_directory) {
    std::vector<std::filesystem::path> recovered_files{};
    std::error_code iteration_error{};
    for (std::filesystem::recursive_directory_iterator iterator(root_directory, iteration_error), end;
         iterator != end;
         iterator.increment(iteration_error)) {
        if (iteration_error) {
            break;
        }

        std::error_code entry_error{};
        if (!iterator->is_regular_file(entry_error) || entry_error) {
            continue;
        }

        if (!file_has_font_extension(iterator->path())) {
            continue;
        }

        recovered_files.push_back(iterator->path().lexically_normal());
    }

    std::sort(recovered_files.begin(), recovered_files.end());
    return recovered_files;
}

std::optional<std::filesystem::path> resolve_fontcollector_executable(const SubtitleFontRecoveryOptions &options) {
    if (options.fontcollector_executable_override.has_value()) {
        return process::find_executable_on_path(
            {path_to_utf8_string(*options.fontcollector_executable_override)}
        );
    }

    const char *fontcollector_env = std::getenv(kFontCollectorEnvVar.data());
    if (fontcollector_env != nullptr && fontcollector_env[0] != '\0') {
        const auto resolved_env = process::find_executable_on_path({fontcollector_env});
        if (resolved_env.has_value()) {
            return resolved_env;
        }
        return std::nullopt;
    }

    return process::find_executable_on_path({
        "fontcollector",
        "fontcollector.exe",
        "fontcollector.cmd",
        "fontcollector.bat"
    });
}

SubtitleFontRecoveryReport make_tool_unavailable_report(const std::filesystem::path &subtitle_path) {
    return SubtitleFontRecoveryReport{
        .outcome = SubtitleFontRecoveryOutcome::tool_unavailable,
        .subtitle_path = subtitle_path,
        .message = "FontCollector fallback is unavailable for '" + format_path_leaf(subtitle_path) +
            "'. Continuing with the normal subtitle font resolution path.",
        .actionable_hint =
            "Install FontCollector or set " + std::string(kFontCollectorEnvVar) +
            " to a valid executable path to enable recovered-font staging."
    };
}

SubtitleFontRecoveryReport make_failed_report(
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &executable_path,
    const std::filesystem::path &font_directory,
    const std::filesystem::path &tool_log_path,
    const std::string &message,
    const std::string &actionable_hint
) {
    return SubtitleFontRecoveryReport{
        .outcome = SubtitleFontRecoveryOutcome::failed,
        .subtitle_path = subtitle_path,
        .fontcollector_executable = executable_path,
        .staged_font_directory = font_directory,
        .tool_log_path = tool_log_path,
        .message = std::move(message),
        .actionable_hint = std::move(actionable_hint),
        .recovered_fonts_applied = false
    };
}

}  // namespace

SubtitleFontRecoveryArtifacts::~SubtitleFontRecoveryArtifacts() {
    if (temporary_root_directory_.empty()) {
        return;
    }

    std::error_code error;
    std::filesystem::remove_all(temporary_root_directory_, error);
}

const std::filesystem::path &SubtitleFontRecoveryArtifacts::temporary_root_directory() const noexcept {
    return temporary_root_directory_;
}

const std::filesystem::path &SubtitleFontRecoveryArtifacts::font_directory() const noexcept {
    return font_directory_;
}

const std::vector<std::filesystem::path> &SubtitleFontRecoveryArtifacts::recovered_font_files() const noexcept {
    return recovered_font_files_;
}

bool SubtitleFontRecoveryReport::attempted() const noexcept {
    return outcome == SubtitleFontRecoveryOutcome::recovered_fonts ||
        outcome == SubtitleFontRecoveryOutcome::no_additional_fonts ||
        outcome == SubtitleFontRecoveryOutcome::failed;
}

PreparedSubtitleRenderSessionRequest prepare_subtitle_render_session_request(
    SubtitleRenderSessionCreateRequest session_request,
    const SubtitleFontRecoveryOptions &options
) {
    PreparedSubtitleRenderSessionRequest prepared_request{
        .session_request = std::move(session_request),
        .font_recovery_artifacts = nullptr,
        .font_recovery_report = SubtitleFontRecoveryReport{}
    };

    const auto normalized_subtitle_path = prepared_request.session_request.subtitle_path.lexically_normal();
    prepared_request.font_recovery_report.subtitle_path = normalized_subtitle_path;

    if (normalized_subtitle_path.empty() || !request_uses_ass_subtitles(prepared_request.session_request)) {
        prepared_request.font_recovery_report.outcome = SubtitleFontRecoveryOutcome::not_applicable;
        return prepared_request;
    }

    std::error_code subtitle_status_error{};
    if (!std::filesystem::exists(normalized_subtitle_path, subtitle_status_error) ||
        subtitle_status_error ||
        !std::filesystem::is_regular_file(normalized_subtitle_path, subtitle_status_error) ||
        subtitle_status_error) {
        prepared_request.font_recovery_report.outcome = SubtitleFontRecoveryOutcome::not_applicable;
        return prepared_request;
    }

    const auto fontcollector_executable = resolve_fontcollector_executable(options);
    if (!fontcollector_executable.has_value()) {
        prepared_request.font_recovery_report = make_tool_unavailable_report(normalized_subtitle_path);
        return prepared_request;
    }

    auto artifacts = std::make_shared<SubtitleFontRecoveryArtifacts>();
    artifacts->temporary_root_directory_ = make_temporary_root();
    artifacts->font_directory_ = artifacts->temporary_root_directory_ / "fonts";

    std::error_code create_error{};
    std::filesystem::create_directories(artifacts->font_directory_, create_error);
    if (create_error) {
        prepared_request.font_recovery_artifacts = std::move(artifacts);
        prepared_request.font_recovery_report = make_failed_report(
            normalized_subtitle_path,
            *fontcollector_executable,
            prepared_request.font_recovery_artifacts->font_directory(),
            prepared_request.font_recovery_artifacts->temporary_root_directory() / "fontcollector.log",
            "FontCollector fallback could not create a temporary staging directory for '" +
                format_path_leaf(normalized_subtitle_path) +
                "'. Continuing with the normal subtitle font resolution path.",
            "The operating system reported: " + create_error.message()
        );
        return prepared_request;
    }

    prepared_request.font_recovery_artifacts = artifacts;
    prepared_request.font_recovery_report.fontcollector_executable = *fontcollector_executable;
    prepared_request.font_recovery_report.staged_font_directory = artifacts->font_directory_;
    prepared_request.font_recovery_report.tool_log_path =
        prepared_request.font_recovery_artifacts->temporary_root_directory() / "fontcollector.log";

    const auto tool_run_result = process::run_external_tool(process::ExternalToolRunRequest{
        .executable = *fontcollector_executable,
        .arguments = {
            "--input",
            path_to_utf8_string(normalized_subtitle_path),
            "--output",
            path_to_utf8_string(artifacts->font_directory_),
            "--logging",
            path_to_utf8_string(prepared_request.font_recovery_report.tool_log_path)
        }
    });
    if (!tool_run_result.launched) {
        prepared_request.font_recovery_report = make_failed_report(
            normalized_subtitle_path,
            *fontcollector_executable,
            artifacts->font_directory_,
            prepared_request.font_recovery_report.tool_log_path,
            "FontCollector fallback could not be launched for '" + format_path_leaf(normalized_subtitle_path) +
                "'. Continuing with the normal subtitle font resolution path.",
            tool_run_result.failure_message.empty()
                ? "The tool launch failed before FontCollector could inspect the subtitle script."
                : tool_run_result.failure_message
        );
        return prepared_request;
    }

    artifacts->recovered_font_files_ = collect_recovered_font_files(artifacts->font_directory_);
    prepared_request.font_recovery_report.recovered_font_files = artifacts->recovered_font_files_;

    if (tool_run_result.exit_code != 0) {
        prepared_request.font_recovery_report = make_failed_report(
            normalized_subtitle_path,
            *fontcollector_executable,
            artifacts->font_directory_,
            prepared_request.font_recovery_report.tool_log_path,
            "FontCollector fallback failed for '" + format_path_leaf(normalized_subtitle_path) + "' with exit code " +
                std::to_string(tool_run_result.exit_code) +
                ". Continuing with the normal subtitle font resolution path.",
            "Review the FontCollector invocation and subtitle script compatibility if the wrong fonts persist."
        );
        prepared_request.font_recovery_report.recovered_font_files = artifacts->recovered_font_files_;
        return prepared_request;
    }

    if (artifacts->recovered_font_files_.empty()) {
        prepared_request.font_recovery_report = SubtitleFontRecoveryReport{
            .outcome = SubtitleFontRecoveryOutcome::no_additional_fonts,
            .subtitle_path = normalized_subtitle_path,
            .fontcollector_executable = *fontcollector_executable,
            .staged_font_directory = artifacts->font_directory_,
            .tool_log_path = prepared_request.font_recovery_report.tool_log_path,
            .recovered_font_files = {},
            .message = "FontCollector ran for '" + format_path_leaf(normalized_subtitle_path) +
                "' but did not recover any additional font files. Continuing with the normal subtitle font resolution "
                "path.",
            .actionable_hint =
                "If the rendered subtitle fonts are still wrong, confirm that the ASS script references installed fonts "
                "and that FontCollector can access them on this machine.",
            .recovered_fonts_applied = false
        };
        return prepared_request;
    }

    prepared_request.session_request.font_search_directory = artifacts->font_directory_;
    prepared_request.font_recovery_report = SubtitleFontRecoveryReport{
        .outcome = SubtitleFontRecoveryOutcome::recovered_fonts,
        .subtitle_path = normalized_subtitle_path,
        .fontcollector_executable = *fontcollector_executable,
        .staged_font_directory = artifacts->font_directory_,
        .tool_log_path = prepared_request.font_recovery_report.tool_log_path,
        .recovered_font_files = artifacts->recovered_font_files_,
        .message = "FontCollector recovered " + std::to_string(artifacts->recovered_font_files_.size()) +
            " font file(s) for '" + format_path_leaf(normalized_subtitle_path) + "': " +
            join_font_file_names(artifacts->recovered_font_files_) +
            ". The recovered font directory will be applied to subtitle rendering.",
        .actionable_hint = {},
        .recovered_fonts_applied = true
    };
    return prepared_request;
}

const char *to_string(const SubtitleFontRecoveryOutcome outcome) noexcept {
    switch (outcome) {
    case SubtitleFontRecoveryOutcome::not_applicable:
        return "not_applicable";
    case SubtitleFontRecoveryOutcome::recovered_fonts:
        return "recovered_fonts";
    case SubtitleFontRecoveryOutcome::no_additional_fonts:
        return "no_additional_fonts";
    case SubtitleFontRecoveryOutcome::tool_unavailable:
        return "tool_unavailable";
    case SubtitleFontRecoveryOutcome::failed:
        return "failed";
    default:
        return "unknown";
    }
}

}  // namespace utsure::core::subtitles
