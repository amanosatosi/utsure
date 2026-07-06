#include "utsure/core/subtitles/subtitle_mangetsu_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace utsure::core::subtitles {

namespace {

struct EventFormatMap final {
    std::vector<std::string> fields{};
    std::size_t name_index{0};
    std::size_t effect_index{0};
    std::size_t text_index{0};
    bool has_name{false};
    bool has_effect{false};
    bool has_text{false};
};

struct ParsedEventLine final {
    std::string type{};
    std::vector<std::string_view> fields{};
    bool valid{false};
};

[[nodiscard]] std::string_view trim_view(std::string_view text) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string to_trimmed_string(const std::string_view text) {
    const auto trimmed = trim_view(text);
    return std::string(trimmed.data(), trimmed.size());
}

[[nodiscard]] std::string to_lower_ascii(std::string_view text) {
    std::string lowered{};
    lowered.reserve(text.size());
    for (const unsigned char character : text) {
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    return lowered;
}

[[nodiscard]] bool ascii_equals_ignore_case(std::string_view left, std::string_view right) {
    left = trim_view(left);
    right = trim_view(right);
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_character = static_cast<unsigned char>(left[index]);
        const auto right_character = static_cast<unsigned char>(right[index]);
        if (std::tolower(left_character) != std::tolower(right_character)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool starts_with_keyword(std::string_view line, std::string_view keyword) {
    line = trim_view(line);
    const auto colon_position = line.find(':');
    if (colon_position == std::string_view::npos) {
        return false;
    }
    return ascii_equals_ignore_case(line.substr(0, colon_position), keyword);
}

[[nodiscard]] bool is_events_section_header(std::string_view line) {
    line = trim_view(line);
    return line.size() == 8U &&
        line.front() == '[' &&
        line.back() == ']' &&
        ascii_equals_ignore_case(line.substr(1, 6), "Events");
}

[[nodiscard]] bool is_section_header(std::string_view line) {
    line = trim_view(line);
    return line.size() >= 2U && line.front() == '[' && line.back() == ']';
}

[[nodiscard]] std::vector<std::string_view> split_csv_preserving_final_field(
    std::string_view text,
    const std::size_t expected_field_count
) {
    std::vector<std::string_view> fields{};
    fields.reserve(expected_field_count);
    std::size_t field_start = 0;
    for (std::size_t index = 0; index < expected_field_count; ++index) {
        if (index + 1U == expected_field_count) {
            fields.push_back(text.substr(field_start));
            return fields;
        }

        const auto comma_position = text.find(',', field_start);
        if (comma_position == std::string_view::npos) {
            return {};
        }

        fields.push_back(text.substr(field_start, comma_position - field_start));
        field_start = comma_position + 1U;
    }

    return fields;
}

[[nodiscard]] EventFormatMap parse_event_format_line(std::string_view line) {
    EventFormatMap format{};
    line = trim_view(line);
    const auto colon_position = line.find(':');
    if (colon_position == std::string_view::npos) {
        return format;
    }

    const auto field_text = line.substr(colon_position + 1U);
    std::size_t field_start = 0;
    while (field_start <= field_text.size()) {
        const auto comma_position = field_text.find(',', field_start);
        const auto field_view = comma_position == std::string_view::npos
            ? field_text.substr(field_start)
            : field_text.substr(field_start, comma_position - field_start);
        const auto normalized_field = to_lower_ascii(trim_view(field_view));
        const std::size_t field_index = format.fields.size();
        format.fields.push_back(normalized_field);
        if (normalized_field == "name") {
            format.name_index = field_index;
            format.has_name = true;
        } else if (normalized_field == "effect") {
            format.effect_index = field_index;
            format.has_effect = true;
        } else if (normalized_field == "text") {
            format.text_index = field_index;
            format.has_text = true;
        }

        if (comma_position == std::string_view::npos) {
            break;
        }
        field_start = comma_position + 1U;
    }

    return format;
}

[[nodiscard]] bool is_known_ass_event_type(const std::string_view type) {
    return ascii_equals_ignore_case(type, "Comment") ||
        ascii_equals_ignore_case(type, "Dialogue") ||
        ascii_equals_ignore_case(type, "Picture") ||
        ascii_equals_ignore_case(type, "Sound") ||
        ascii_equals_ignore_case(type, "Movie") ||
        ascii_equals_ignore_case(type, "Command");
}

[[nodiscard]] ParsedEventLine parse_event_line(std::string_view line, const EventFormatMap &format) {
    ParsedEventLine parsed{};
    line = trim_view(line);
    const auto colon_position = line.find(':');
    if (colon_position == std::string_view::npos) {
        return parsed;
    }

    const auto type = trim_view(line.substr(0, colon_position));
    if (!is_known_ass_event_type(type)) {
        return parsed;
    }

    parsed.type = std::string(type.data(), type.size());
    parsed.fields = split_csv_preserving_final_field(line.substr(colon_position + 1U), format.fields.size());
    parsed.valid = parsed.fields.size() == format.fields.size();
    return parsed;
}

[[nodiscard]] bool event_line_has_mangetsu_effect(const ParsedEventLine &event, const EventFormatMap &format) {
    return event.valid &&
        event.fields.size() > format.effect_index &&
        trim_view(event.fields[format.effect_index]) == kMangetsuActorColorcodingEffect;
}

[[nodiscard]] std::optional<MangetsuActorColorcodingMetadataLine> accepted_metadata_line(
    const ParsedEventLine &event,
    const EventFormatMap &format,
    const std::size_t source_line_number
) {
    if (!ascii_equals_ignore_case(event.type, "Comment") || !event_line_has_mangetsu_effect(event, format)) {
        return std::nullopt;
    }

    const auto name = to_trimmed_string(event.fields[format.name_index]);
    if (name.empty()) {
        return std::nullopt;
    }

    return MangetsuActorColorcodingMetadataLine{
        .name = name,
        .effect = to_trimmed_string(event.fields[format.effect_index]),
        .text = std::string(event.fields[format.text_index].data(), event.fields[format.text_index].size()),
        .source_line_number = source_line_number
    };
}

void push_unique_name(std::vector<std::string> &names, const std::string &name) {
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
    }
}

void record_accepted_metadata_line(
    MangetsuActorColorcodingMetadata &metadata,
    MangetsuActorColorcodingMetadataLine line
) {
    if (line.name == kMangetsuActorColorcodingAppliedStylesName) {
        metadata.whitelist_found = true;
    }
    push_unique_name(metadata.accepted_names, line.name);
    metadata.lines.push_back(std::move(line));
}

void record_late_ignored_match(
    MangetsuActorColorcodingMetadata &metadata,
    const MangetsuActorColorcodingMetadataLine &line
) {
    metadata.late_match_ignored = true;
    metadata.debug_notes.push_back(
        "Ignored late Mangetsu actor colorcoding metadata at ASS line " +
        std::to_string(line.source_line_number) +
        " for Name='" + line.name + "' because it is outside the top contiguous metadata block."
    );
}

[[nodiscard]] std::string_view strip_utf8_bom_if_present(std::string_view line) {
    if (line.size() >= 3U &&
        static_cast<unsigned char>(line[0]) == 0xEFU &&
        static_cast<unsigned char>(line[1]) == 0xBBU &&
        static_cast<unsigned char>(line[2]) == 0xBFU) {
        line.remove_prefix(3);
    }
    return line;
}

}  // namespace

MangetsuActorColorcodingMetadata scan_ass_mangetsu_actor_colorcoding_metadata(
    const std::string_view ass_text
) {
    MangetsuActorColorcodingMetadata metadata{};
    metadata.scan_completed = true;

    bool in_events_section = false;
    bool top_block_closed = false;
    EventFormatMap format{};

    std::size_t line_start = 0;
    std::size_t source_line_number = 1;
    while (line_start <= ass_text.size()) {
        const auto line_end = ass_text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
            ? ass_text.substr(line_start)
            : ass_text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (source_line_number == 1U) {
            line = strip_utf8_bom_if_present(line);
        }

        const auto trimmed_line = trim_view(line);
        if (is_section_header(trimmed_line)) {
            if (metadata.events_section_found && !is_events_section_header(trimmed_line)) {
                break;
            }
            if (is_events_section_header(trimmed_line)) {
                metadata.events_section_found = true;
                in_events_section = true;
            }
        } else if (in_events_section && !metadata.format_found) {
            if (starts_with_keyword(trimmed_line, "Format")) {
                format = parse_event_format_line(trimmed_line);
                metadata.format_found = true;
                metadata.required_fields_found = format.has_name && format.has_effect && format.has_text;
                if (!metadata.required_fields_found) {
                    metadata.warnings.push_back(
                        "Mangetsu actor colorcoding scan skipped: [Events] Format is missing Name, Effect, or Text."
                    );
                } else if (format.text_index + 1U != format.fields.size()) {
                    metadata.warnings.push_back(
                        "Mangetsu actor colorcoding scan warning: [Events] Text is not the final Format field; "
                        "comma-bearing Text values may not be preserved."
                    );
                }
            }
        } else if (in_events_section && metadata.required_fields_found) {
            if (trimmed_line.empty() || starts_with_keyword(trimmed_line, "Format")) {
                // Empty lines and duplicate Format lines are not events; only event lines close the top block.
            } else {
                const auto event = parse_event_line(trimmed_line, format);
                if (event.type.empty()) {
                    // Non-event content in [Events] does not create metadata and does not close the event block.
                } else if (!top_block_closed) {
                    if (!event.valid) {
                        top_block_closed = true;
                        metadata.warnings.push_back(
                            "Mangetsu actor colorcoding scan stopped at malformed ASS event line " +
                            std::to_string(source_line_number) + "."
                        );
                    } else if (auto accepted_line = accepted_metadata_line(event, format, source_line_number)) {
                        record_accepted_metadata_line(metadata, std::move(*accepted_line));
                    } else {
                        if (event_line_has_mangetsu_effect(event, format) &&
                            ascii_equals_ignore_case(event.type, "Comment") &&
                            trim_view(event.fields[format.name_index]).empty()) {
                            metadata.warnings.push_back(
                                "Mangetsu actor colorcoding scan stopped at ASS line " +
                                std::to_string(source_line_number) +
                                " because the metadata Comment has an empty Name field."
                            );
                        }
                        top_block_closed = true;
                    }
                } else if (auto accepted_line = accepted_metadata_line(event, format, source_line_number)) {
                    record_late_ignored_match(metadata, *accepted_line);
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1U;
        ++source_line_number;
    }

    if (!metadata.events_section_found) {
        metadata.warnings.push_back("Mangetsu actor colorcoding scan skipped: [Events] section was not found.");
    } else if (!metadata.format_found) {
        metadata.warnings.push_back("Mangetsu actor colorcoding scan skipped: [Events] Format line was not found.");
    }

    return metadata;
}

MangetsuActorColorcodingMetadata load_ass_mangetsu_actor_colorcoding_metadata(
    const std::filesystem::path &subtitle_path
) {
    std::ifstream input(subtitle_path, std::ios::binary);
    if (!input) {
        MangetsuActorColorcodingMetadata metadata{};
        metadata.scan_completed = true;
        metadata.warnings.push_back(
            "Mangetsu actor colorcoding scan skipped: subtitle file could not be opened."
        );
        return metadata;
    }

    const std::string text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    return scan_ass_mangetsu_actor_colorcoding_metadata(text);
}

}  // namespace utsure::core::subtitles
