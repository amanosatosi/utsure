#include "utsure/core/subtitles/thumbnail_preroll.hpp"

#include "utsure/core/filesystem/path_format.hpp"
#include "utsure/core/media/media_inspector.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace utsure::core::subtitles {

namespace {

constexpr std::string_view kThumbnailImageStem{"thumbnail"};
constexpr std::string_view kThumbnailDataActor{"utsure_data"};

struct AssDialogueFormat final {
    std::size_t actor_index{4};
    std::size_t text_index{9};
    std::size_t field_count{10};
};

struct AssLineSplit final {
    std::string line{};
    std::string newline{};
};

struct AssDialogueTextReplacement final {
    std::string line{};
    bool matched{false};
};

std::string lowercase_ascii(std::string value) {
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

std::string trim_ascii(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }

    return std::string(value);
}

bool iequals_ascii(const std::string_view left, const std::string_view right) {
    return lowercase_ascii(std::string(left)) == lowercase_ascii(std::string(right));
}

bool starts_with_case_insensitive(const std::string_view value, const std::string_view prefix) {
    return value.size() >= prefix.size() && iequals_ascii(value.substr(0, prefix.size()), prefix);
}

bool supported_thumbnail_extension(const std::filesystem::path &path) {
    const auto extension = lowercase_ascii(path.extension().string());
    return extension == ".png" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".webp" ||
        extension == ".bmp" ||
        extension == ".tif" ||
        extension == ".tiff";
}

int thumbnail_extension_priority(const std::filesystem::path &path) {
    const auto extension = lowercase_ascii(path.extension().string());
    if (extension == ".png") {
        return 0;
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return 1;
    }
    if (extension == ".webp") {
        return 2;
    }
    if (extension == ".bmp") {
        return 3;
    }
    if (extension == ".tif" || extension == ".tiff") {
        return 4;
    }
    return 100;
}

std::string read_text_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }

    return std::string{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
}

bool is_regular_file_quiet(const std::filesystem::path &path) {
    std::error_code error{};
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::vector<std::string> split_ass_fields(const std::string_view payload, const std::size_t field_count) {
    std::vector<std::string> fields{};
    if (field_count == 0U) {
        return fields;
    }

    fields.reserve(field_count);
    std::size_t field_start = 0;
    for (std::size_t field_index = 0; field_index + 1U < field_count; ++field_index) {
        const auto comma = payload.find(',', field_start);
        if (comma == std::string_view::npos) {
            fields.emplace_back(payload.substr(field_start));
            field_start = payload.size();
            break;
        }

        fields.emplace_back(payload.substr(field_start, comma - field_start));
        field_start = comma + 1U;
    }

    fields.emplace_back(payload.substr(std::min(field_start, payload.size())));
    while (fields.size() < field_count) {
        fields.emplace_back();
    }

    return fields;
}

std::optional<AssDialogueFormat> parse_format_line(const std::string_view line) {
    if (!starts_with_case_insensitive(line, "Format:")) {
        return std::nullopt;
    }

    const auto payload = line.substr(std::string_view("Format:").size());
    std::vector<std::string> names{};
    std::size_t field_start = 0;
    while (field_start <= payload.size()) {
        const auto comma = payload.find(',', field_start);
        const auto field_end = comma == std::string_view::npos ? payload.size() : comma;
        names.push_back(lowercase_ascii(trim_ascii(payload.substr(field_start, field_end - field_start))));
        if (comma == std::string_view::npos) {
            break;
        }
        field_start = comma + 1U;
    }

    AssDialogueFormat format{};
    format.field_count = names.size();
    bool found_actor = false;
    bool found_text = false;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == "name" || names[index] == "actor") {
            format.actor_index = index;
            found_actor = true;
        } else if (names[index] == "text") {
            format.text_index = index;
            found_text = true;
        }
    }

    return found_actor && found_text ? std::optional<AssDialogueFormat>(format) : std::nullopt;
}

std::optional<std::string> extract_utsure_data_text_from_script(const std::string &script_text) {
    bool in_events_section = false;
    AssDialogueFormat format{};

    std::istringstream stream(script_text);
    std::string line{};
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const auto trimmed_line = trim_ascii(line);
        if (trimmed_line.empty() || trimmed_line.front() == ';') {
            continue;
        }

        if (trimmed_line.front() == '[' && trimmed_line.back() == ']') {
            in_events_section = iequals_ascii(trimmed_line, "[Events]");
            continue;
        }

        if (!in_events_section) {
            continue;
        }

        if (const auto parsed_format = parse_format_line(trimmed_line); parsed_format.has_value()) {
            format = *parsed_format;
            continue;
        }

        if (!starts_with_case_insensitive(trimmed_line, "Dialogue:")) {
            continue;
        }

        const auto payload = trimmed_line.substr(std::string_view("Dialogue:").size());
        auto fields = split_ass_fields(payload, format.field_count);
        if (format.actor_index >= fields.size() || format.text_index >= fields.size()) {
            continue;
        }

        if (iequals_ascii(trim_ascii(fields[format.actor_index]), kThumbnailDataActor)) {
            return fields[format.text_index];
        }
    }

    return std::nullopt;
}

std::vector<AssLineSplit> split_lines_preserving_newlines(const std::string &text) {
    std::vector<AssLineSplit> lines{};
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        const auto line_end = text.find_first_of("\r\n", line_start);
        if (line_end == std::string::npos) {
            lines.push_back(AssLineSplit{
                .line = text.substr(line_start),
                .newline = {}
            });
            return lines;
        }

        std::string newline{text.substr(line_end, 1U)};
        std::size_t next_line = line_end + 1U;
        if (text[line_end] == '\r' && next_line < text.size() && text[next_line] == '\n') {
            newline = "\r\n";
            ++next_line;
        }

        lines.push_back(AssLineSplit{
            .line = text.substr(line_start, line_end - line_start),
            .newline = std::move(newline)
        });
        line_start = next_line;
    }

    if (text.empty()) {
        return lines;
    }

    return lines;
}

AssDialogueTextReplacement rebuild_dialogue_line_with_text(
    const std::string &original_line,
    const AssDialogueFormat &format,
    const std::string &replacement_text
) {
    const auto colon = original_line.find(':');
    if (colon == std::string::npos) {
        return AssDialogueTextReplacement{.line = original_line};
    }

    const std::string prefix = original_line.substr(0, colon + 1U);
    const std::string payload = original_line.substr(colon + 1U);
    auto fields = split_ass_fields(payload, format.field_count);
    if (format.actor_index >= fields.size() || format.text_index >= fields.size() ||
        !iequals_ascii(trim_ascii(fields[format.actor_index]), kThumbnailDataActor)) {
        return AssDialogueTextReplacement{.line = original_line};
    }

    fields[format.text_index] = replacement_text;
    std::ostringstream rebuilt;
    rebuilt << prefix;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0U) {
            rebuilt << ',';
        }
        rebuilt << fields[index];
    }

    return AssDialogueTextReplacement{
        .line = rebuilt.str(),
        .matched = true
    };
}

std::filesystem::path find_case_insensitive_file(
    const std::filesystem::path &directory,
    const std::string_view stem,
    const std::string_view extension
) {
    std::error_code error{};
    if (!std::filesystem::is_directory(directory, error) || error) {
        return {};
    }

    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file(error) || error) {
            error.clear();
            continue;
        }

        const auto path = entry.path();
        if (iequals_ascii(path.stem().string(), stem) &&
            iequals_ascii(path.extension().string(), extension)) {
            return path.lexically_normal();
        }
    }

    return {};
}

std::vector<std::filesystem::path> find_thumbnail_candidates(const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> candidates{};
    std::error_code error{};
    if (!std::filesystem::is_directory(directory, error) || error) {
        return candidates;
    }

    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file(error) || error) {
            error.clear();
            continue;
        }

        const auto path = entry.path();
        if (iequals_ascii(path.stem().string(), kThumbnailImageStem) && supported_thumbnail_extension(path)) {
            candidates.push_back(path.lexically_normal());
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
        const int left_priority = thumbnail_extension_priority(left);
        const int right_priority = thumbnail_extension_priority(right);
        if (left_priority != right_priority) {
            return left_priority < right_priority;
        }

        return lowercase_ascii(left.filename().string()) < lowercase_ascii(right.filename().string());
    });

    return candidates;
}

bool dimensions_match_resize_aspect_tolerance(
    const int source_width,
    const int source_height,
    const int target_width,
    const int target_height,
    bool *used_tolerance = nullptr
) {
    if (used_tolerance != nullptr) {
        *used_tolerance = false;
    }

    if (source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0) {
        return false;
    }

    if (static_cast<long long>(source_width) * static_cast<long long>(target_height) ==
        static_cast<long long>(target_width) * static_cast<long long>(source_height)) {
        return true;
    }

    constexpr long double kMaxEncoderSafeRoundingErrorPixels = 1.0L;
    const auto ideal_target_width =
        static_cast<long double>(target_height) * static_cast<long double>(source_width) /
        static_cast<long double>(source_height);
    const auto ideal_target_height =
        static_cast<long double>(target_width) * static_cast<long double>(source_height) /
        static_cast<long double>(source_width);
    const bool within_resize_tolerance =
        (std::fabs(static_cast<long double>(target_width) - ideal_target_width) <=
            kMaxEncoderSafeRoundingErrorPixels) &&
        (std::fabs(static_cast<long double>(target_height) - ideal_target_height) <=
            kMaxEncoderSafeRoundingErrorPixels);

    if (within_resize_tolerance && used_tolerance != nullptr) {
        *used_tolerance = true;
    }
    return within_resize_tolerance;
}

bool image_dimensions_are_usable(
    const std::filesystem::path &image_path,
    const int required_width,
    const int required_height,
    std::string *diagnostic
) {
    if (required_width <= 0 || required_height <= 0) {
        if (diagnostic != nullptr) {
            *diagnostic = "thumbnail dimensions could not be checked because the final output size is unknown.";
        }
        return false;
    }

    const auto inspection_result = media::MediaInspector::inspect(image_path);
    if (!inspection_result.succeeded()) {
        if (diagnostic != nullptr) {
            *diagnostic = inspection_result.error->message + " Hint: " + inspection_result.error->actionable_hint;
        }
        return false;
    }

    if (!inspection_result.media_source_info->primary_video_stream.has_value()) {
        if (diagnostic != nullptr) {
            *diagnostic = "thumbnail candidate has no readable image/video stream.";
        }
        return false;
    }

    const auto &video_stream = *inspection_result.media_source_info->primary_video_stream;
    if (video_stream.width == required_width && video_stream.height == required_height) {
        if (diagnostic != nullptr) {
            *diagnostic = "thumbnail candidate '" + filesystem::path_to_utf8_string(image_path.lexically_normal()) +
                "' already matches " + std::to_string(required_width) + "x" +
                std::to_string(required_height) + ".";
        }
        return true;
    }

    bool used_resize_tolerance = false;
    if (dimensions_match_resize_aspect_tolerance(
            video_stream.width,
            video_stream.height,
            required_width,
            required_height,
            &used_resize_tolerance)) {
        if (video_stream.width < required_width || video_stream.height < required_height) {
            if (diagnostic != nullptr) {
                *diagnostic = "thumbnail candidate '" + filesystem::path_to_utf8_string(image_path.lexically_normal()) +
                    "' is " + std::to_string(video_stream.width) + "x" +
                    std::to_string(video_stream.height) + ", which is smaller than the final output size " +
                    std::to_string(required_width) + "x" + std::to_string(required_height) +
                    "; thumbnail upscaling is not enabled.";
            }
            return false;
        }

        if (diagnostic != nullptr) {
            *diagnostic = "thumbnail candidate '" + filesystem::path_to_utf8_string(image_path.lexically_normal()) +
                "' normalized thumbnail source from " + std::to_string(video_stream.width) + "x" +
                std::to_string(video_stream.height) + " to " + std::to_string(required_width) + "x" +
                std::to_string(required_height) +
                (used_resize_tolerance ? " within resize aspect tolerance." : ".");
        }
        return true;
    }

    if (diagnostic != nullptr) {
        *diagnostic = "thumbnail candidate '" + filesystem::path_to_utf8_string(image_path.lexically_normal()) + "' is " +
            std::to_string(video_stream.width) + "x" + std::to_string(video_stream.height) +
            ", whose aspect mismatch exceeds resize tolerance for final output " + std::to_string(required_width) +
            "x" + std::to_string(required_height) + ".";
    }
    return false;
}

ThumbnailPrerollResolveResult make_result(
    const ThumbnailPrerollDecisionCode decision,
    std::string summary,
    std::vector<std::string> diagnostics = {}
) {
    return ThumbnailPrerollResolveResult{
        .decision = decision,
        .assets = std::nullopt,
        .decision_summary = std::move(summary),
        .diagnostics = std::move(diagnostics)
    };
}

}  // namespace

bool ThumbnailPrerollResolveResult::has_assets() const noexcept {
    return assets.has_value();
}

const char *to_string(const ThumbnailPrerollDecisionCode decision) noexcept {
    switch (decision) {
    case ThumbnailPrerollDecisionCode::disabled:
        return "disabled";
    case ThumbnailPrerollDecisionCode::no_subtitle_folder:
        return "no_subtitle_folder";
    case ThumbnailPrerollDecisionCode::no_accepted_thumbnail:
        return "no_accepted_thumbnail";
    case ThumbnailPrerollDecisionCode::missing_overlay_script:
        return "missing_overlay_script";
    case ThumbnailPrerollDecisionCode::missing_title_event:
        return "missing_title_event";
    case ThumbnailPrerollDecisionCode::ready:
        return "ready";
    default:
        return "unknown";
    }
}

std::optional<std::string> ThumbnailPrerollResolver::extract_utsure_data_text(
    const std::filesystem::path &overlay_ass_path
) {
    const auto script_text = read_text_file(overlay_ass_path);
    if (script_text.empty()) {
        return std::nullopt;
    }

    return extract_utsure_data_text_from_script(script_text);
}

std::string ThumbnailPrerollResolver::replace_utsure_data_text(
    const std::string &ass_script_text,
    const std::string &replacement_text
) {
    bool in_events_section = false;
    AssDialogueFormat format{};
    bool replaced = false;
    std::ostringstream output;

    for (const auto &split_line : split_lines_preserving_newlines(ass_script_text)) {
        std::string line = split_line.line;
        const auto trimmed_line = trim_ascii(line);

        if (!trimmed_line.empty() && trimmed_line.front() == '[' && trimmed_line.back() == ']') {
            in_events_section = iequals_ascii(trimmed_line, "[Events]");
        } else if (in_events_section) {
            if (const auto parsed_format = parse_format_line(trimmed_line); parsed_format.has_value()) {
                format = *parsed_format;
            } else if (!replaced && starts_with_case_insensitive(trimmed_line, "Dialogue:")) {
                auto replacement = rebuild_dialogue_line_with_text(line, format, replacement_text);
                line = std::move(replacement.line);
                if (replacement.matched) {
                    replaced = true;
                }
            }
        }

        output << line << split_line.newline;
    }

    return output.str();
}

ThumbnailPrerollResolveResult ThumbnailPrerollResolver::resolve(
    const ThumbnailPrerollResolveRequest &request
) {
    if (!request.enabled) {
        return make_result(ThumbnailPrerollDecisionCode::disabled, "Thumbnail pre-roll is disabled.");
    }

    std::vector<std::string> diagnostics{};
    const auto subtitle_path = request.subtitle_path.lexically_normal();
    const auto subtitle_directory = subtitle_path.parent_path();
    if (subtitle_directory.empty()) {
        return make_result(
            ThumbnailPrerollDecisionCode::no_subtitle_folder,
            "Thumbnail pre-roll could not auto-select assets because the selected subtitle has no parent folder."
        );
    }

    std::optional<std::filesystem::path> selected_image_path{};
    if (request.explicit_image_path.has_value() && !request.explicit_image_path->empty()) {
        std::string diagnostic{};
        if (!image_dimensions_are_usable(*request.explicit_image_path, request.required_width, request.required_height, &diagnostic)) {
            diagnostics.push_back(std::move(diagnostic));
            return make_result(
                ThumbnailPrerollDecisionCode::no_accepted_thumbnail,
                "Thumbnail pre-roll rejected the manually selected image because it cannot be normalized to the final output size.",
                std::move(diagnostics)
            );
        }
        diagnostics.push_back(std::move(diagnostic));
        selected_image_path = request.explicit_image_path->lexically_normal();
    } else if (!request.auto_select) {
        return make_result(
            ThumbnailPrerollDecisionCode::no_accepted_thumbnail,
            "Thumbnail pre-roll needs a manually selected image because automatic thumbnail.* selection is disabled."
        );
    } else {
        for (const auto &candidate : find_thumbnail_candidates(subtitle_directory)) {
            std::string diagnostic{};
            if (image_dimensions_are_usable(candidate, request.required_width, request.required_height, &diagnostic)) {
                diagnostics.push_back(std::move(diagnostic));
                selected_image_path = candidate;
                break;
            }
            diagnostics.push_back(std::move(diagnostic));
        }
    }

    if (!selected_image_path.has_value()) {
        return make_result(
            ThumbnailPrerollDecisionCode::no_accepted_thumbnail,
            "Thumbnail pre-roll did not find a thumbnail.* image beside the selected subtitle that can be normalized to the final output size.",
            std::move(diagnostics)
        );
    }

    const auto overlay_ass_path =
        request.explicit_overlay_ass_path.has_value() && !request.explicit_overlay_ass_path->empty()
            ? request.explicit_overlay_ass_path->lexically_normal()
            : find_case_insensitive_file(
                  selected_image_path->parent_path(),
                  selected_image_path->stem().string(),
                  ".ass"
              );
    if (overlay_ass_path.empty()) {
        return make_result(
            ThumbnailPrerollDecisionCode::missing_overlay_script,
            "Thumbnail pre-roll found a thumbnail image but did not find a same-stem .ass overlay beside it.",
            std::move(diagnostics)
        );
    }
    if (!is_regular_file_quiet(overlay_ass_path)) {
        return make_result(
            ThumbnailPrerollDecisionCode::missing_overlay_script,
            "Thumbnail pre-roll found a thumbnail image but the selected same-stem .ass overlay is not a readable file.",
            std::move(diagnostics)
        );
    }

    auto title_text = ThumbnailPrerollResolver::extract_utsure_data_text(overlay_ass_path);
    if (!title_text.has_value()) {
        return make_result(
            ThumbnailPrerollDecisionCode::missing_title_event,
            "Thumbnail pre-roll found the same-stem .ass overlay but did not find a Dialogue line with actor/name utsure_data.",
            std::move(diagnostics)
        );
    }

    ThumbnailPrerollResolveResult result{
        .decision = ThumbnailPrerollDecisionCode::ready,
        .assets = ThumbnailPrerollAssets{
            .image_path = selected_image_path->lexically_normal(),
            .overlay_ass_path = overlay_ass_path.lexically_normal(),
            .title_text = std::move(*title_text)
        },
        .decision_summary = "Thumbnail pre-roll selected '" + selected_image_path->filename().string() +
            "' and '" + overlay_ass_path.filename().string() + "'.",
        .diagnostics = std::move(diagnostics)
    };
    return result;
}

}  // namespace utsure::core::subtitles
