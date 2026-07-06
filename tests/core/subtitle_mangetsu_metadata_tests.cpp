#include "utsure/core/subtitles/subtitle_mangetsu_metadata.hpp"

#include <iostream>
#include <string_view>

namespace {

using utsure::core::subtitles::kMangetsuActorColorcodingAppliedStylesName;
using utsure::core::subtitles::kMangetsuActorColorcodingEffect;
using utsure::core::subtitles::scan_ass_mangetsu_actor_colorcoding_metadata;

int fail(const std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int assert_top_block_and_reordered_format() {
    constexpr std::string_view kAss = R"([Script Info]
Title: reordered format

[Events]
Format: Start, End, Name, Effect, Text
Comment: 0:00:00.00,9:59:59.99,mangetsu-colorcode-applied-styles,mangetsu-colorcoding,{Default}{Alt}
Comment: 0:00:00.00,9:59:59.99,Nene,mangetsu-colorcoding,{\1c&HFFB6D9&\2bs3\2bc&H7161DF&,keeps,commas}
Dialogue: 0:00:01.00,0:00:04.00,Nene,,Hello.
)";

    const auto metadata = scan_ass_mangetsu_actor_colorcoding_metadata(kAss);
    if (!metadata.scan_completed ||
        !metadata.events_section_found ||
        !metadata.format_found ||
        !metadata.required_fields_found ||
        !metadata.whitelist_found ||
        metadata.lines.size() != 2U) {
        return fail("Scanner did not accept the expected top-block Mangetsu metadata lines.");
    }

    if (metadata.lines[0].name != kMangetsuActorColorcodingAppliedStylesName ||
        metadata.lines[0].effect != kMangetsuActorColorcodingEffect ||
        metadata.lines[0].text != "{Default}{Alt}") {
        return fail("Scanner did not preserve the applied-style whitelist metadata line.");
    }

    if (metadata.lines[1].name != "Nene" ||
        metadata.lines[1].effect != kMangetsuActorColorcodingEffect ||
        metadata.lines[1].text != R"({\1c&HFFB6D9&\2bs3\2bc&H7161DF&,keeps,commas})") {
        return fail("Scanner did not map Name/Effect/Text or preserve comma-bearing Text.");
    }

    std::cout << "mangetsu_metadata.top_block=ok\n";
    return 0;
}

int assert_late_metadata_is_ignored() {
    constexpr std::string_view kAss = R"([Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:04.00,Default,Nene,0,0,0,,Hello.
Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\1c&HFFB6D9&}
)";

    const auto metadata = scan_ass_mangetsu_actor_colorcoding_metadata(kAss);
    if (!metadata.lines.empty() || !metadata.late_match_ignored || metadata.debug_notes.empty()) {
        return fail("Scanner did not ignore late Mangetsu metadata after the first nonmatching event.");
    }

    std::cout << "mangetsu_metadata.late_ignored=ok\n";
    return 0;
}

int assert_normal_comments_are_not_metadata() {
    constexpr std::string_view kAss = R"([Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,,mangetsu-colorcoding text only must not match
Comment: 0,0:00:00.00,9:59:59.99,Default,Nene,0,0,0,mangetsu-colorcoding,{\1c&HFFB6D9&}
)";

    const auto metadata = scan_ass_mangetsu_actor_colorcoding_metadata(kAss);
    if (!metadata.lines.empty() || !metadata.late_match_ignored) {
        return fail("Scanner treated a normal commented-out line as Mangetsu metadata.");
    }

    std::cout << "mangetsu_metadata.no_false_positive=ok\n";
    return 0;
}

int assert_missing_events_or_format_is_reported() {
    const auto missing_events = scan_ass_mangetsu_actor_colorcoding_metadata("[Script Info]\nTitle: none\n");
    if (missing_events.events_section_found || missing_events.warnings.empty()) {
        return fail("Scanner did not report a missing [Events] section.");
    }

    const auto missing_format = scan_ass_mangetsu_actor_colorcoding_metadata("[Events]\nDialogue: x\n");
    if (!missing_format.events_section_found || missing_format.format_found || missing_format.warnings.empty()) {
        return fail("Scanner did not report a missing [Events] Format line.");
    }

    std::cout << "mangetsu_metadata.missing_events_or_format=ok\n";
    return 0;
}

}  // namespace

int main() {
    if (assert_top_block_and_reordered_format() != 0 ||
        assert_late_metadata_is_ignored() != 0 ||
        assert_normal_comments_are_not_metadata() != 0 ||
        assert_missing_events_or_format_is_reported() != 0) {
        return 1;
    }

    return 0;
}
