#include "utsure/core/subtitles/subtitle_auto_selection.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using utsure::core::subtitles::SubtitleAutoSelectionDecisionCode;
using utsure::core::subtitles::SubtitleAutoSelectionMatchKind;
using utsure::core::subtitles::SubtitleAutoSelector;
using utsure::core::subtitles::SubtitleSelectedTextSelectionRequest;

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

int assert_selected_text_match_uses_current_subtitle_stem(const std::filesystem::path &root) {
    const auto source_path = root / "Toshi" / "video.mkv";
    const auto current_subtitle_path = source_path.parent_path() / "Episode 01.ass";
    touch_file(source_path);
    touch_file(current_subtitle_path);
    touch_file(source_path.parent_path() / "Episode 01 OP.ass");
    touch_file(source_path.parent_path() / "video OP.ass");

    const auto result = SubtitleAutoSelector::select_for_selected_text(SubtitleSelectedTextSelectionRequest{
        .source_path = source_path,
        .current_subtitle_path = current_subtitle_path,
        .selected_text = "OP"
    });
    if (!result.has_selection() ||
        result.selected_candidate->subtitle_path.filename() != "Episode 01 OP.ass" ||
        result.selected_candidate->format_hint != "ass") {
        return fail("Selected-text subtitle selection did not prefer the current subtitle stem.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

int assert_selected_text_match_accepts_spacing_variants(const std::filesystem::path &root) {
    const auto source_path = root / "Toshi Spacing" / "Episode 02.mkv";
    const auto current_subtitle_path = source_path.parent_path() / "Episode 02.ass";
    touch_file(source_path);
    touch_file(current_subtitle_path);
    touch_file(source_path.parent_path() / "Episode 02  OP.ass");

    const auto result = SubtitleAutoSelector::select_for_selected_text(SubtitleSelectedTextSelectionRequest{
        .source_path = source_path,
        .current_subtitle_path = current_subtitle_path,
        .selected_text = " OP "
    });
    if (!result.has_selection() ||
        result.selected_candidate->subtitle_path.filename() != "Episode 02  OP.ass") {
        return fail("Selected-text subtitle selection did not tolerate a two-space filename variant.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

int assert_selected_text_candidate_variants(const std::filesystem::path &root) {
    const std::vector<std::string> candidate_names{
        "Episode 07 OP.ass",
        "Episode 07  OP.ass",
        "Episode 07OP.ass",
        "OP Episode 07.ass",
        "OP.ass"
    };

    for (std::size_t index = 0; index < candidate_names.size(); ++index) {
        const auto source_path = root / ("Toshi Variant " + std::to_string(index)) / "Episode 07.mkv";
        const auto current_subtitle_path = source_path.parent_path() / "Episode 07.ass";
        touch_file(source_path);
        touch_file(current_subtitle_path);
        touch_file(source_path.parent_path() / candidate_names[index]);

        const auto result = SubtitleAutoSelector::select_for_selected_text(SubtitleSelectedTextSelectionRequest{
            .source_path = source_path,
            .current_subtitle_path = current_subtitle_path,
            .selected_text = "OP"
        });
        if (!result.has_selection() ||
            result.selected_candidate->subtitle_path.filename() != candidate_names[index]) {
            return fail("Selected-text subtitle selection missed a required candidate-stem variant.");
        }
    }

    std::cout << "selected_text.variants=ok\n";
    return 0;
}

int assert_selected_text_match_handles_invalid_characters(const std::filesystem::path &root) {
    const auto source_path = root / "Toshi Invalid" / "Episode 03.mkv";
    const auto current_subtitle_path = source_path.parent_path() / "Episode 03.ass";
    touch_file(source_path);
    touch_file(current_subtitle_path);
    touch_file(source_path.parent_path() / "Episode 03 O P.ass");

    const auto result = SubtitleAutoSelector::select_for_selected_text(SubtitleSelectedTextSelectionRequest{
        .source_path = source_path,
        .current_subtitle_path = current_subtitle_path,
        .selected_text = "O:P*"
    });
    if (!result.has_selection() ||
        result.selected_candidate->subtitle_path.filename() != "Episode 03 O P.ass") {
        return fail("Selected-text subtitle selection did not safely normalize invalid filename characters.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

int assert_selected_text_whitespace_only_does_nothing(const std::filesystem::path &root) {
    const auto source_path = root / "Toshi Empty" / "Episode 04.mkv";
    const auto current_subtitle_path = source_path.parent_path() / "Episode 04.ass";
    touch_file(source_path);
    touch_file(current_subtitle_path);
    touch_file(source_path.parent_path() / "Episode 04 OP.ass");

    const auto result = SubtitleAutoSelector::select_for_selected_text(SubtitleSelectedTextSelectionRequest{
        .source_path = source_path,
        .current_subtitle_path = current_subtitle_path,
        .selected_text = "   "
    });
    if (result.has_selection() || result.decision != SubtitleAutoSelectionDecisionCode::no_match) {
        return fail("Whitespace-only selected text should not produce a selected-text subtitle match.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

int assert_selected_text_no_match_is_not_an_error(const std::filesystem::path &root) {
    const auto source_path = root / "Toshi Missing" / "Episode 05.mkv";
    const auto current_subtitle_path = source_path.parent_path() / "Episode 05.ass";
    touch_file(source_path);
    touch_file(current_subtitle_path);
    touch_file(source_path.parent_path() / "Unrelated OP.ass");

    const auto result = SubtitleAutoSelector::select_for_selected_text(SubtitleSelectedTextSelectionRequest{
        .source_path = source_path,
        .current_subtitle_path = current_subtitle_path,
        .selected_text = "OP"
    });
    if (result.has_selection() || result.decision != SubtitleAutoSelectionDecisionCode::no_match) {
        return fail("Selected-text subtitle selection should leave no-match as a recoverable no-op.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

int assert_selected_text_source_directory_unavailable_uses_selected_text_summary(const std::filesystem::path &root) {
    const auto missing_source_path = root / "Missing Source" / "Episode 08.mkv";
    const auto result = SubtitleAutoSelector::select_for_selected_text(SubtitleSelectedTextSelectionRequest{
        .source_path = missing_source_path,
        .current_subtitle_path = std::nullopt,
        .selected_text = "OP"
    });
    if (result.decision != SubtitleAutoSelectionDecisionCode::source_directory_unavailable ||
        result.decision_summary.find("Selected-text subtitle selection") == std::string::npos) {
        return fail("Selected-text subtitle selection used the generic source-directory-unavailable diagnostic.");
    }

    std::cout << result.decision_summary << '\n';
    return 0;
}

int assert_selected_text_keeps_fx_priority(const std::filesystem::path &root) {
    const auto source_path = root / "Toshi Fx" / "Episode 06.mkv";
    const auto current_subtitle_path = source_path.parent_path() / "Episode 06.ass";
    touch_file(source_path);
    touch_file(current_subtitle_path);
    touch_file(source_path.parent_path() / "Episode 06 OP.ass");
    touch_file(source_path.parent_path() / "Episode 06 OP.fx.ass");

    const auto result = SubtitleAutoSelector::select_for_selected_text(SubtitleSelectedTextSelectionRequest{
        .source_path = source_path,
        .current_subtitle_path = current_subtitle_path,
        .selected_text = "OP"
    });
    if (!result.has_selection() ||
        result.selected_candidate->subtitle_path.filename() != "Episode 06 OP.fx.ass" ||
        result.selected_candidate->match_kind != SubtitleAutoSelectionMatchKind::exact_fx ||
        !result.used_fx_priority_rule) {
        return fail("Selected-text subtitle selection did not preserve existing .fx priority.");
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

    if (assert_selected_text_match_uses_current_subtitle_stem(root) != 0) {
        return 1;
    }

    if (assert_selected_text_match_accepts_spacing_variants(root) != 0) {
        return 1;
    }

    if (assert_selected_text_candidate_variants(root) != 0) {
        return 1;
    }

    if (assert_selected_text_match_handles_invalid_characters(root) != 0) {
        return 1;
    }

    if (assert_selected_text_whitespace_only_does_nothing(root) != 0) {
        return 1;
    }

    if (assert_selected_text_no_match_is_not_an_error(root) != 0) {
        return 1;
    }

    if (assert_selected_text_source_directory_unavailable_uses_selected_text_summary(root) != 0) {
        return 1;
    }

    if (assert_selected_text_keeps_fx_priority(root) != 0) {
        return 1;
    }

    return 0;
}
