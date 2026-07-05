#include "utsure/core/subtitles/subtitle_mangetsu_metadata.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using utsure::core::subtitles::scan_ass_mangetsu_colorcoding_metadata;

int fail(const std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

bool contains_text(const std::string &text, const std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

bool warnings_contain_text(
    const std::vector<std::string> &warnings,
    const std::string_view needle
) {
    for (const auto &warning : warnings) {
        if (contains_text(warning, needle)) {
            return true;
        }
    }
    return false;
}

int assert_standard_top_block_and_late_comment_behavior() {
    constexpr std::string_view kAss = R"ass([Script Info]
Title: Mangetsu sample

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Comment: 0,0:00:00.00,0:00:00.00,Default,mangetsu-colorcode-applied-styles,0,0,0,mangetsu-colorcoding,{Default}{Signs}
Comment: 0,0:00:00.00,0:00:00.00,Default,Alice,0,0,0,mangetsu-colorcoding,{\1c&H66CCFF&\3c&H003366&}
Dialogue: 0,0:00:01.00,0:00:04.00,Default,Alice,0,0,0,,Alice should use actor colorcoding.
Comment: 0,0:00:04.00,0:00:05.00,Default,Charlie,0,0,0,mangetsu-colorcoding,Later comment must be ignored.
)ass";

    const auto metadata = scan_ass_mangetsu_colorcoding_metadata(kAss);
    if (!metadata.scan_completed || !metadata.events_section_found || !metadata.format_found) {
        return fail("Standard metadata scan did not report the expected section and format state.");
    }
    if (metadata.lines.size() != 2U) {
        return fail("Standard metadata scan did not accept exactly the top-block lines before Dialogue.");
    }
    if (metadata.lines[0].name != "mangetsu-colorcode-applied-styles" ||
        metadata.lines[0].text != "{Default}{Signs}") {
        return fail("Applied-styles Mangetsu line was not preserved.");
    }
    if (metadata.lines[1].name != "Alice" ||
        metadata.lines[1].effect != "mangetsu-colorcoding" ||
        metadata.lines[1].text != R"({\1c&H66CCFF&\3c&H003366&})") {
        return fail("Actor Mangetsu line was not extracted exactly.");
    }

    std::cout << "standard.accepted_lines=" << metadata.lines.size() << '\n';
    return 0;
}

int assert_reordered_fields_and_comma_text_behavior() {
    constexpr std::string_view kAss = R"ass([Events]
Format: Text, Effect, Name, End, Start
Comment: {\1c&H66CCFF&\3c&H003366&, keep,this,comma text},mangetsu-colorcoding,Alice,0:00:00.00,0:00:00.00
Comment: ignored because normal top block ends here,,Note,0:00:00.00,0:00:00.00
Comment: {\1c&HFFAA66&},mangetsu-colorcoding,Bob,0:00:00.00,0:00:00.00
)ass";

    const auto metadata = scan_ass_mangetsu_colorcoding_metadata(kAss);
    if (!metadata.format_found || metadata.lines.size() != 1U) {
        return fail("Reordered metadata scan did not accept exactly the first matching comment.");
    }
    if (metadata.lines.front().name != "Alice" ||
        metadata.lines.front().effect != "mangetsu-colorcoding" ||
        metadata.lines.front().text != R"({\1c&H66CCFF&\3c&H003366&, keep,this,comma text})") {
        return fail("Reordered metadata scan did not preserve the comma-bearing Text field.");
    }

    std::cout << "reordered.text=" << metadata.lines.front().text << '\n';
    return 0;
}

int assert_missing_sections_are_nonfatal() {
    const auto missing_events = scan_ass_mangetsu_colorcoding_metadata("[Script Info]\nTitle: none\n");
    if (!missing_events.scan_completed || missing_events.events_section_found ||
        missing_events.format_found || !missing_events.lines.empty()) {
        return fail("Missing [Events] should scan cleanly with no metadata.");
    }

    constexpr std::string_view kMissingFormat = R"ass([Events]
Comment: 0,0:00:00.00,0:00:00.00,Default,Alice,0,0,0,mangetsu-colorcoding,{\1c&H66CCFF&}
)ass";
    const auto missing_format = scan_ass_mangetsu_colorcoding_metadata(kMissingFormat);
    if (!missing_format.scan_completed || !missing_format.events_section_found ||
        missing_format.format_found || !missing_format.lines.empty()) {
        return fail("Missing Format should not produce metadata or crash.");
    }
    if (!warnings_contain_text(missing_format.warnings, "before a usable Format line")) {
        return fail("Missing Format scan did not warn about the matching metadata line.");
    }

    std::cout << "missing_events.accepted_lines=" << missing_events.lines.size() << '\n';
    std::cout << "missing_format.warning_count=" << missing_format.warnings.size() << '\n';
    return 0;
}

int assert_empty_name_is_recoverable() {
    constexpr std::string_view kAss = R"ass([Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-colorcoding,{\1c&H66CCFF&}
Comment: 0,0:00:00.00,0:00:00.00,Default,Bob,0,0,0,mangetsu-colorcoding,{\1c&HFFAA66&}
)ass";

    const auto metadata = scan_ass_mangetsu_colorcoding_metadata(kAss);
    if (metadata.lines.size() != 1U || metadata.lines.front().name != "Bob") {
        return fail("Empty Name should warn and continue scanning later top-block metadata.");
    }
    if (!warnings_contain_text(metadata.warnings, "empty Name")) {
        return fail("Empty Name metadata line did not emit a warning.");
    }

    std::cout << "empty_name.accepted_lines=" << metadata.lines.size() << '\n';
    return 0;
}

}  // namespace

int main() {
    if (assert_standard_top_block_and_late_comment_behavior() != 0 ||
        assert_reordered_fields_and_comma_text_behavior() != 0 ||
        assert_missing_sections_are_nonfatal() != 0 ||
        assert_empty_name_is_recoverable() != 0) {
        return 1;
    }

    return 0;
}
