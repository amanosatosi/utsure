#include "utsure/core/subtitles/subtitle_auto_selection.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using utsure::core::subtitles::SubtitleAutoSelectionDecisionCode;
using utsure::core::subtitles::SubtitleAutoSelectionMatchKind;
using utsure::core::subtitles::SubtitleAutoSelector;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

struct TempDirectoryGuard final {
    explicit TempDirectoryGuard(std::filesystem::path value) : path(std::move(value)) {}

    ~TempDirectoryGuard() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path{};
};

std::filesystem::path make_temp_directory() {
    const auto unique_suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root =
        std::filesystem::temp_directory_path() / ("utsure-subtitle-auto-selection-tests-" + unique_suffix);
    std::filesystem::create_directories(root);
    return root;
}

void touch_file(const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << "test";
}

int assert_no_match_behavior(const std::filesystem::path &root) {
    const auto source_path = root / "Show" / "episode01.mkv";
    touch_file(source_path);
    touch_file(source_path.parent_path() / "episode02.ass");
    touch_file(source_path.parent_path() / "episode01-commentary.ass");

    const auto result = SubtitleAutoSelector::select(source_path);
    if (result.decision != SubtitleAutoSelectionDecisionCode::no_match || result.has_selection() ||
        result.matched_candidate_count != 0) {
        return fail("Subtitle auto-selection should leave the subtitle unset when no exact match exists.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

int assert_single_plain_match_behavior(const std::filesystem::path &root) {
    const auto source_path = root / "Show" / "episode02.mkv";
    touch_file(source_path);
    touch_file(source_path.parent_path() / "episode02.ass");

    const auto result = SubtitleAutoSelector::select(source_path);
    if (!result.has_selection() || !result.selected_candidate.has_value() ||
        result.selected_candidate->subtitle_path.filename() != "episode02.ass" ||
        result.selected_candidate->format_hint != "ass" ||
        result.selected_candidate->match_kind != SubtitleAutoSelectionMatchKind::exact_plain ||
        result.matched_candidate_count != 1 ||
        result.used_fx_priority_rule) {
        return fail("Subtitle auto-selection did not pick the only exact .ass match.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

int assert_fx_priority_behavior(const std::filesystem::path &root) {
    const auto source_path = root / "Show" / "episode03.mkv";
    touch_file(source_path);
    touch_file(source_path.parent_path() / "episode03.ass");
    touch_file(source_path.parent_path() / "episode03.fx.ass");
    touch_file(source_path.parent_path() / "episode03.ssa");

    const auto result = SubtitleAutoSelector::select(source_path);
    if (!result.has_selection() || !result.selected_candidate.has_value() ||
        result.selected_candidate->subtitle_path.filename() != "episode03.fx.ass" ||
        result.selected_candidate->match_kind != SubtitleAutoSelectionMatchKind::exact_fx ||
        result.matched_candidate_count != 3 ||
        !result.used_fx_priority_rule) {
        return fail("Subtitle auto-selection did not prefer the .fx subtitle over the plain subtitle match.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

}  // namespace

int main() {
    const auto root = make_temp_directory();
    const TempDirectoryGuard cleanup(root);

    if (assert_no_match_behavior(root) != 0) {
        return 1;
    }

    if (assert_single_plain_match_behavior(root) != 0) {
        return 1;
    }

    if (assert_fx_priority_behavior(root) != 0) {
        return 1;
    }

    return 0;
}
