#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace utsure::core::subtitles {

inline constexpr std::string_view kMangetsuActorColorcodingEffect = "mangetsu-colorcoding";
inline constexpr std::string_view kMangetsuActorColorcodingAppliedStylesName =
    "mangetsu-colorcode-applied-styles";

struct MangetsuActorColorcodingMetadataLine final {
    std::string name{};
    std::string effect{};
    std::string text{};
    std::size_t source_line_number{0};
};

struct MangetsuActorColorcodingMetadata final {
    std::vector<MangetsuActorColorcodingMetadataLine> lines{};
    std::vector<std::string> accepted_names{};
    std::vector<std::string> warnings{};
    std::vector<std::string> debug_notes{};
    bool scan_completed{false};
    bool events_section_found{false};
    bool format_found{false};
    bool required_fields_found{false};
    bool whitelist_found{false};
    bool late_match_ignored{false};
};

[[nodiscard]] MangetsuActorColorcodingMetadata scan_ass_mangetsu_actor_colorcoding_metadata(
    std::string_view ass_text
);

[[nodiscard]] MangetsuActorColorcodingMetadata load_ass_mangetsu_actor_colorcoding_metadata(
    const std::filesystem::path &subtitle_path
);

}  // namespace utsure::core::subtitles
