#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace utsure::core::subtitles {

enum class ThumbnailPrerollDecisionCode {
    disabled = 0,
    no_subtitle_folder,
    no_accepted_thumbnail,
    missing_overlay_script,
    missing_title_event,
    ready
};

struct ThumbnailPrerollResolveRequest final {
    bool enabled{false};
    bool auto_select{true};
    std::filesystem::path subtitle_path{};
    std::optional<std::filesystem::path> explicit_image_path{};
    std::optional<std::filesystem::path> explicit_overlay_ass_path{};
    int required_width{0};
    int required_height{0};
};

struct ThumbnailPrerollAssets final {
    std::filesystem::path image_path{};
    std::filesystem::path overlay_ass_path{};
    std::string title_text{};
};

struct ThumbnailPrerollResolveResult final {
    ThumbnailPrerollDecisionCode decision{ThumbnailPrerollDecisionCode::disabled};
    std::optional<ThumbnailPrerollAssets> assets{};
    std::string decision_summary{};
    std::vector<std::string> diagnostics{};

    [[nodiscard]] bool has_assets() const noexcept;
};

class ThumbnailPrerollResolver final {
public:
    [[nodiscard]] static ThumbnailPrerollResolveResult resolve(const ThumbnailPrerollResolveRequest &request);
    [[nodiscard]] static std::optional<std::string> extract_epnumber_text(const std::filesystem::path &overlay_ass_path);
    [[nodiscard]] static std::string replace_epnumber_text(
        const std::string &ass_script_text,
        const std::string &replacement_text
    );
};

[[nodiscard]] const char *to_string(ThumbnailPrerollDecisionCode decision) noexcept;

}  // namespace utsure::core::subtitles
