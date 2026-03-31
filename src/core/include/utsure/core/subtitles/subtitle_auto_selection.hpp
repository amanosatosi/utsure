#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace utsure::core::subtitles {

enum class SubtitleAutoSelectionDecisionCode : std::uint8_t {
    selected = 0,
    no_match,
    source_directory_unavailable
};

enum class SubtitleAutoSelectionMatchKind : std::uint8_t {
    exact_fx = 0,
    exact_plain
};

struct SubtitleAutoSelectionCandidate final {
    std::filesystem::path subtitle_path{};
    std::string format_hint{};
    SubtitleAutoSelectionMatchKind match_kind{SubtitleAutoSelectionMatchKind::exact_plain};
};

struct SubtitleAutoSelectionResult final {
    SubtitleAutoSelectionDecisionCode decision{SubtitleAutoSelectionDecisionCode::no_match};
    std::optional<SubtitleAutoSelectionCandidate> selected_candidate{};
    std::size_t matched_candidate_count{0};
    bool used_fx_priority_rule{false};
    std::string decision_summary{};

    [[nodiscard]] bool has_selection() const noexcept;
};

class SubtitleAutoSelector final {
public:
    [[nodiscard]] static SubtitleAutoSelectionResult select(const std::filesystem::path &source_path);
};

[[nodiscard]] const char *to_string(SubtitleAutoSelectionDecisionCode code) noexcept;
[[nodiscard]] const char *to_string(SubtitleAutoSelectionMatchKind kind) noexcept;

}  // namespace utsure::core::subtitles
