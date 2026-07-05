#include "utsure/core/subtitles/subtitle_mangetsu_metadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace utsure::core::subtitles {

namespace {

constexpr std::string_view kEventsSectionName = "events";
constexpr std::string_view kMangetsuEffectMarker = "mangetsu-colorcoding";

struct ActiveEventFormat final {
    std::vector<std::string> field_names{};
    std::optional<std::size_t> name_index{};
    std::optional<std::size_t> effect_index{};
    std::optional<std::size_t> text_index{};

    [[nodiscard]] bool has_required_fields() const noexcept {
        return name_index.has_value() && effect_index.has_value() && text_index.has_value();
    }
};

struct EventLine final {
    std::string_view kind{};
    std::string_view payload{};
};

[[nodiscard]] bool is_ascii_space(const char character) noexcept {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

[[nodiscard]] std::string_view trim_ascii_view(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::string_view trim_left_ascii_view(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    return value;
}

[[nodiscard]] std::string trim_ascii_copy(const std::string_view value) {
    const auto trimmed = trim_ascii_view(value);
    return std::string(trimmed);
}

[[nodiscard]] std::string lowercase_ascii(std::string value) {
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

[[nodiscard]] bool equals_ascii_case_insensitive(std::string_view left, std::string_view right) {
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

[[nodiscard]] bool starts_with_ascii_case_insensitive(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }

    return equals_ascii_case_insensitive(value.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool contains_ascii_case_insensitive(std::string_view value, std::string_view needle) {
    const std::string lowered_value = lowercase_ascii(std::string(value));
    const std::string lowered_needle = lowercase_ascii(std::string(needle));
    return lowered_value.find(lowered_needle) != std::string::npos;
}

[[nodiscard]] std::string_view strip_trailing_cr(std::string_view line) noexcept {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines{};
    while (!text.empty()) {
        const auto newline_position = text.find('\n');
        if (newline_position == std::string_view::npos) {
            lines.push_back(strip_trailing_cr(text));
            break;
        }

        lines.push_back(strip_trailing_cr(text.substr(0, newline_position)));
        text.remove_prefix(newline_position + 1U);
    }
    if (lines.empty()) {
        lines.push_back({});
    }
    return lines;
}

[[nodiscard]] std::optional<std::string> parse_section_name(std::string_view line) {
    const auto trimmed = trim_ascii_view(line);
    if (trimmed.size() < 2U || trimmed.front() != '[' || trimmed.back() != ']') {
        return std::nullopt;
    }

    auto name = trim_ascii_copy(trimmed.substr(1U, trimmed.size() - 2U));
    return lowercase_ascii(std::move(name));
}

[[nodiscard]] std::vector<std::string_view> split_comma_fields(std::string_view payload) {
    std::vector<std::string_view> fields{};
    for (;;) {
        const auto comma_position = payload.find(',');
        if (comma_position == std::string_view::npos) {
            fields.push_back(payload);
            return fields;
        }

        fields.push_back(payload.substr(0, comma_position));
        payload.remove_prefix(comma_position + 1U);
    }
}

[[nodiscard]] ActiveEventFormat parse_format_line(std::string_view line) {
    ActiveEventFormat format{};
    const auto payload = trim_ascii_view(line.substr(std::string_view("Format:").size()));
    const auto fields = split_comma_fields(payload);
    format.field_names.reserve(fields.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
        auto field_name = lowercase_ascii(trim_ascii_copy(fields[index]));
        if (field_name == "name") {
            format.name_index = index;
        } else if (field_name == "effect") {
            format.effect_index = index;
        } else if (field_name == "text") {
            format.text_index = index;
        }
        format.field_names.push_back(std::move(field_name));
    }

    return format;
}

[[nodiscard]] bool is_supported_event_kind(std::string_view kind) {
    static constexpr std::array<std::string_view, 6> kEventKinds{
        "comment",
        "dialogue",
        "picture",
        "sound",
        "movie",
        "command"
    };

    return std::any_of(
        kEventKinds.begin(),
        kEventKinds.end(),
        [kind](const std::string_view candidate) {
            return equals_ascii_case_insensitive(kind, candidate);
        }
    );
}

[[nodiscard]] std::optional<EventLine> parse_event_line(std::string_view line) {
    line = trim_ascii_view(line);
    const auto colon_position = line.find(':');
    if (colon_position == std::string_view::npos) {
        return std::nullopt;
    }

    const auto kind = trim_ascii_view(line.substr(0, colon_position));
    if (!is_supported_event_kind(kind)) {
        return std::nullopt;
    }

    return EventLine{
        .kind = kind,
        .payload = trim_left_ascii_view(line.substr(colon_position + 1U))
    };
}

[[nodiscard]] std::optional<std::vector<std::string_view>> split_event_fields(
    std::string_view payload,
    const std::size_t field_count,
    const std::size_t text_index
) {
    if (field_count == 0U || text_index >= field_count) {
        return std::nullopt;
    }

    std::vector<std::string_view> fields(field_count);
    std::size_t front_begin = 0;
    for (std::size_t index = 0; index < text_index; ++index) {
        const auto comma_position = payload.find(',', front_begin);
        if (comma_position == std::string_view::npos) {
            return std::nullopt;
        }
        fields[index] = payload.substr(front_begin, comma_position - front_begin);
        front_begin = comma_position + 1U;
    }

    std::size_t tail_end = payload.size();
    for (std::size_t reverse_index = field_count - 1U; reverse_index > text_index; --reverse_index) {
        if (tail_end == 0U) {
            return std::nullopt;
        }

        const auto comma_position = payload.rfind(',', tail_end - 1U);
        if (comma_position == std::string_view::npos || comma_position < front_begin) {
            return std::nullopt;
        }

        fields[reverse_index] = payload.substr(comma_position + 1U, tail_end - comma_position - 1U);
        tail_end = comma_position;
    }

    if (front_begin > tail_end) {
        return std::nullopt;
    }
    fields[text_index] = payload.substr(front_begin, tail_end - front_begin);
    return fields;
}

void append_warning(
    MangetsuColorcodingMetadata &metadata,
    const std::size_t line_number,
    const std::string_view message
) {
    std::ostringstream stream;
    stream << "ASS Mangetsu metadata line " << line_number << ": " << message;
    metadata.warnings.push_back(stream.str());
}

}  // namespace

MangetsuColorcodingMetadata scan_ass_mangetsu_colorcoding_metadata(const std::string_view ass_text) {
    MangetsuColorcodingMetadata metadata{};
    metadata.scan_completed = true;

    bool inside_events_section = false;
    std::optional<ActiveEventFormat> active_format{};

    const auto lines = split_lines(ass_text);
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        const auto line_number = line_index + 1U;
        const std::string_view raw_line = lines[line_index];
        const auto trimmed = trim_ascii_view(raw_line);

        if (const auto section_name = parse_section_name(trimmed); section_name.has_value()) {
            if (*section_name == kEventsSectionName) {
                inside_events_section = true;
                metadata.events_section_found = true;
                continue;
            }

            if (inside_events_section) {
                break;
            }
            continue;
        }

        if (!inside_events_section) {
            continue;
        }

        if (trimmed.empty()) {
            continue;
        }

        if (starts_with_ascii_case_insensitive(trimmed, "Format:")) {
            active_format = parse_format_line(trimmed);
            metadata.format_found = active_format->has_required_fields();
            if (!metadata.format_found) {
                append_warning(
                    metadata,
                    line_number,
                    "active Format line does not contain Name, Effect, and Text fields"
                );
            }
            continue;
        }

        const auto event_line = parse_event_line(trimmed);
        if (!event_line.has_value()) {
            continue;
        }

        if (!equals_ascii_case_insensitive(event_line->kind, "comment")) {
            break;
        }

        if (!active_format.has_value() || !active_format->has_required_fields()) {
            if (contains_ascii_case_insensitive(event_line->payload, kMangetsuEffectMarker)) {
                append_warning(
                    metadata,
                    line_number,
                    "matching Comment event appeared before a usable Format line"
                );
            }
            break;
        }

        const auto split_fields = split_event_fields(
            event_line->payload,
            active_format->field_names.size(),
            *active_format->text_index
        );
        if (!split_fields.has_value()) {
            if (contains_ascii_case_insensitive(event_line->payload, kMangetsuEffectMarker)) {
                append_warning(
                    metadata,
                    line_number,
                    "matching Comment event could not be split according to the active Format line"
                );
                continue;
            }
            break;
        }

        const auto &fields = *split_fields;
        const std::string name = trim_ascii_copy(fields[*active_format->name_index]);
        const std::string effect = trim_ascii_copy(fields[*active_format->effect_index]);
        if (!contains_ascii_case_insensitive(effect, kMangetsuEffectMarker)) {
            break;
        }

        if (name.empty()) {
            append_warning(
                metadata,
                line_number,
                "matching Comment event has an empty Name field and will not be fed to libassmod"
            );
            continue;
        }

        metadata.lines.push_back(MangetsuColorcodingMetadataLine{
            .name = name,
            .effect = effect,
            .text = std::string(fields[*active_format->text_index]),
            .is_comment = true,
            .is_top_block = true,
            .source_line_number = line_number
        });
    }

    return metadata;
}

MangetsuColorcodingMetadata load_ass_mangetsu_colorcoding_metadata(
    const std::filesystem::path &subtitle_path
) {
    MangetsuColorcodingMetadata metadata{};
    metadata.scan_completed = true;
    if (subtitle_path.empty()) {
        return metadata;
    }

    std::ifstream stream(subtitle_path, std::ios::binary);
    if (!stream) {
        metadata.warnings.push_back(
            "ASS Mangetsu metadata scan could not read subtitle file: " +
            subtitle_path.lexically_normal().string()
        );
        return metadata;
    }

    const std::string text{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
    return scan_ass_mangetsu_colorcoding_metadata(text);
}

}  // namespace utsure::core::subtitles
