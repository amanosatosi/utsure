#pragma once

#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace utsure::core::subtitles {

enum class SubtitleFontRecoveryOutcome : std::uint8_t {
    not_applicable = 0,
    recovered_fonts,
    no_additional_fonts,
    tool_unavailable,
    failed
};

struct SubtitleFontRecoveryOptions;
struct PreparedSubtitleRenderSessionRequest;

class SubtitleFontRecoveryArtifacts final {
public:
    SubtitleFontRecoveryArtifacts() = default;
    ~SubtitleFontRecoveryArtifacts();

    SubtitleFontRecoveryArtifacts(const SubtitleFontRecoveryArtifacts &) = delete;
    SubtitleFontRecoveryArtifacts &operator=(const SubtitleFontRecoveryArtifacts &) = delete;
    SubtitleFontRecoveryArtifacts(SubtitleFontRecoveryArtifacts &&) = delete;
    SubtitleFontRecoveryArtifacts &operator=(SubtitleFontRecoveryArtifacts &&) = delete;

    [[nodiscard]] const std::filesystem::path &temporary_root_directory() const noexcept;
    [[nodiscard]] const std::filesystem::path &font_directory() const noexcept;
    [[nodiscard]] const std::vector<std::filesystem::path> &recovered_font_files() const noexcept;

private:
    friend PreparedSubtitleRenderSessionRequest prepare_subtitle_render_session_request(
        SubtitleRenderSessionCreateRequest session_request,
        const SubtitleFontRecoveryOptions &options
    );

    std::filesystem::path temporary_root_directory_{};
    std::filesystem::path font_directory_{};
    std::vector<std::filesystem::path> recovered_font_files_{};
};

struct SubtitleFontRecoveryReport final {
    SubtitleFontRecoveryOutcome outcome{SubtitleFontRecoveryOutcome::not_applicable};
    std::filesystem::path subtitle_path{};
    std::filesystem::path fontcollector_executable{};
    std::filesystem::path staged_font_directory{};
    std::filesystem::path tool_log_path{};
    std::vector<std::filesystem::path> recovered_font_files{};
    std::string message{};
    std::string actionable_hint{};
    bool recovered_fonts_applied{false};

    [[nodiscard]] bool attempted() const noexcept;
};

struct SubtitleFontRecoveryOptions final {
    std::optional<std::filesystem::path> fontcollector_executable_override{};
};

struct PreparedSubtitleRenderSessionRequest final {
    SubtitleRenderSessionCreateRequest session_request{};
    std::shared_ptr<SubtitleFontRecoveryArtifacts> font_recovery_artifacts{};
    SubtitleFontRecoveryReport font_recovery_report{};
};

[[nodiscard]] PreparedSubtitleRenderSessionRequest prepare_subtitle_render_session_request(
    SubtitleRenderSessionCreateRequest session_request,
    const SubtitleFontRecoveryOptions &options = {}
);

[[nodiscard]] const char *to_string(SubtitleFontRecoveryOutcome outcome) noexcept;

}  // namespace utsure::core::subtitles
