#include "utsure/core/subtitles/subtitle_auto_selection.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace utsure::core::subtitles {

namespace {

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

std::string format_path_leaf(const std::filesystem::path &path) {
    const auto leaf = path.filename().string();
    return leaf.empty() ? path.lexically_normal().string() : leaf;
}

bool is_supported_subtitle_extension(const std::string_view extension) {
    return extension == ".ass" || extension == ".ssa";
}

bool is_supported_selected_text_subtitle_extension(const std::string_view extension) {
    return extension == ".ass";
}

int extension_priority(const std::string_view extension) {
    return extension == ".ass" ? 0 : 1;
}

struct RankedCandidate final {
    SubtitleAutoSelectionCandidate candidate{};
    int extension_rank{1};
    int stem_rank{0};
    std::string normalized_file_name{};
};

bool is_invalid_filename_character(const unsigned char character) {
    return character < 32 || character == '<' || character == '>' || character == ':' || character == '"' ||
        character == '/' || character == '\\' || character == '|' || character == '?' || character == '*';
}

std::string trim_ascii_whitespace(std::string value) {
    const auto not_space = [](const unsigned char character) {
        return !std::isspace(character);
    };

    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    if (begin == value.end()) {
        return {};
    }

    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    return std::string(begin, end);
}

std::string normalize_lookup_text(const std::string_view text) {
    std::string normalized{};
    normalized.reserve(text.size());

    bool previous_was_space = false;
    for (const unsigned char character : text) {
        if (is_invalid_filename_character(character) || std::isspace(character)) {
            if (!normalized.empty() && !previous_was_space) {
                normalized.push_back(' ');
                previous_was_space = true;
            }
            continue;
        }

        normalized.push_back(static_cast<char>(character));
        previous_was_space = false;
    }

    return trim_ascii_whitespace(std::move(normalized));
}

void push_unique_text(std::vector<std::string> &values, std::string value) {
    value = trim_ascii_whitespace(std::move(value));
    if (value.empty()) {
        return;
    }

    const std::string normalized_value = lowercase_ascii(value);
    const bool already_present = std::any_of(
        values.begin(),
        values.end(),
        [&normalized_value](const std::string &existing) {
            return lowercase_ascii(existing) == normalized_value;
        }
    );
    if (!already_present) {
        values.push_back(std::move(value));
    }
}

std::string strip_fx_suffix(std::string stem) {
    const std::string normalized = lowercase_ascii(stem);
    constexpr std::string_view kFxSuffix = ".fx";
    if (normalized.size() > kFxSuffix.size() &&
        normalized.ends_with(kFxSuffix)) {
        stem.resize(stem.size() - kFxSuffix.size());
    }
    return stem;
}

std::vector<std::string> build_selected_text_base_stems(
    const std::filesystem::path &source_path,
    const std::optional<std::filesystem::path> &current_subtitle_path
) {
    std::vector<std::string> base_stems{};
    if (current_subtitle_path.has_value() && !current_subtitle_path->empty()) {
        const std::string current_stem = normalize_lookup_text(current_subtitle_path->stem().string());
        push_unique_text(base_stems, strip_fx_suffix(current_stem));
        push_unique_text(base_stems, current_stem);
    }

    push_unique_text(base_stems, normalize_lookup_text(source_path.stem().string()));
    return base_stems;
}

std::vector<std::string> build_selected_text_candidate_stems(
    const std::filesystem::path &source_path,
    const std::optional<std::filesystem::path> &current_subtitle_path,
    const std::string_view selected_text
) {
    const std::string normalized_selected_text = normalize_lookup_text(selected_text);
    if (normalized_selected_text.empty()) {
        return {};
    }

    std::vector<std::string> candidate_stems{};
    for (const auto &base_stem : build_selected_text_base_stems(source_path, current_subtitle_path)) {
        push_unique_text(candidate_stems, base_stem + " " + normalized_selected_text);
        push_unique_text(candidate_stems, base_stem + "  " + normalized_selected_text);
        push_unique_text(candidate_stems, base_stem + normalized_selected_text);
        push_unique_text(candidate_stems, normalized_selected_text + " " + base_stem);
    }
    push_unique_text(candidate_stems, normalized_selected_text);
    return candidate_stems;
}

std::optional<RankedCandidate> classify_candidate(
    const std::filesystem::path &source_path,
    const std::filesystem::path &candidate_path
) {
    const std::string normalized_extension = lowercase_ascii(candidate_path.extension().string());
    if (!is_supported_subtitle_extension(normalized_extension)) {
        return std::nullopt;
    }

    const std::string normalized_source_stem = lowercase_ascii(source_path.stem().string());
    const std::string normalized_candidate_stem = lowercase_ascii(candidate_path.stem().string());

    SubtitleAutoSelectionMatchKind match_kind{};
    if (normalized_candidate_stem == normalized_source_stem + ".fx") {
        match_kind = SubtitleAutoSelectionMatchKind::exact_fx;
    } else if (normalized_candidate_stem == normalized_source_stem) {
        match_kind = SubtitleAutoSelectionMatchKind::exact_plain;
    } else {
        return std::nullopt;
    }

    return RankedCandidate{
        .candidate = {
            .subtitle_path = candidate_path,
            .format_hint = normalized_extension.substr(1),
            .match_kind = match_kind
        },
        .extension_rank = extension_priority(normalized_extension),
        .stem_rank = 0,
        .normalized_file_name = lowercase_ascii(candidate_path.filename().string())
    };
}

std::optional<RankedCandidate> classify_selected_text_candidate(
    const std::filesystem::path &candidate_path,
    const std::vector<std::string> &candidate_stems
) {
    const std::string normalized_extension = lowercase_ascii(candidate_path.extension().string());
    if (!is_supported_selected_text_subtitle_extension(normalized_extension)) {
        return std::nullopt;
    }

    const std::string normalized_candidate_stem = lowercase_ascii(normalize_lookup_text(candidate_path.stem().string()));
    for (std::size_t index = 0; index < candidate_stems.size(); ++index) {
        const std::string normalized_target_stem = lowercase_ascii(candidate_stems[index]);
        SubtitleAutoSelectionMatchKind match_kind{};
        if (normalized_candidate_stem == normalized_target_stem + ".fx") {
            match_kind = SubtitleAutoSelectionMatchKind::exact_fx;
        } else if (normalized_candidate_stem == normalized_target_stem) {
            match_kind = SubtitleAutoSelectionMatchKind::exact_plain;
        } else {
            continue;
        }

        return RankedCandidate{
            .candidate = {
                .subtitle_path = candidate_path,
                .format_hint = normalized_extension.substr(1),
                .match_kind = match_kind
            },
            .extension_rank = extension_priority(normalized_extension),
            .stem_rank = static_cast<int>(index),
            .normalized_file_name = lowercase_ascii(candidate_path.filename().string())
        };
    }

    return std::nullopt;
}

std::string build_no_match_summary(const std::filesystem::path &source_path) {
    return "Automatic subtitle selection for '" + format_path_leaf(source_path) +
        "': no exact '.ass' or '.ssa' match was found beside the source.";
}

std::string build_selected_text_no_match_summary(const std::filesystem::path &source_path) {
    return "Selected-text subtitle selection for '" + format_path_leaf(source_path) +
        "': no matching '.ass' subtitle was found beside the source.";
}

std::string build_selected_text_ignored_summary(const std::filesystem::path &source_path) {
    return "Selected-text subtitle selection for '" + format_path_leaf(source_path) +
        "': selected text was empty.";
}

std::string build_source_directory_unavailable_summary(const std::filesystem::path &source_path) {
    return "Automatic subtitle selection for '" + format_path_leaf(source_path) +
        "': the source directory was unavailable.";
}

std::string build_selected_summary(
    const std::filesystem::path &source_path,
    const RankedCandidate &selected_candidate,
    const bool used_fx_priority_rule
) {
    std::string summary =
        "Automatic subtitle selection for '" + format_path_leaf(source_path) + "': chose '" +
        format_path_leaf(selected_candidate.candidate.subtitle_path) + "'";
    if (used_fx_priority_rule) {
        summary += " because the exact '.fx' match outranks the plain subtitle match.";
    } else if (selected_candidate.candidate.match_kind == SubtitleAutoSelectionMatchKind::exact_fx) {
        summary += " as the exact '.fx' subtitle match.";
    } else {
        summary += " as the exact subtitle match.";
    }

    return summary;
}

std::string build_selected_text_selected_summary(
    const std::filesystem::path &source_path,
    const RankedCandidate &selected_candidate
) {
    return "Selected-text subtitle selection for '" + format_path_leaf(source_path) + "': chose '" +
        format_path_leaf(selected_candidate.candidate.subtitle_path) + "' as the matching '.ass' subtitle.";
}

SubtitleAutoSelectionResult select_from_directory(
    const std::filesystem::path &source_path,
    const std::function<std::optional<RankedCandidate>(const std::filesystem::path &)> &classifier,
    const std::function<std::string()> &no_match_summary,
    const std::function<std::string(const RankedCandidate &, bool)> &selected_summary
) {
    const auto source_directory = source_path.parent_path();
    std::error_code status_error{};
    if (source_directory.empty() ||
        !std::filesystem::exists(source_directory, status_error) ||
        status_error ||
        !std::filesystem::is_directory(source_directory, status_error) ||
        status_error) {
        return SubtitleAutoSelectionResult{
            .decision = SubtitleAutoSelectionDecisionCode::source_directory_unavailable,
            .decision_summary = build_source_directory_unavailable_summary(source_path)
        };
    }

    std::vector<RankedCandidate> matched_candidates{};
    std::error_code iteration_error{};
    for (std::filesystem::directory_iterator iterator(source_directory, iteration_error), end;
         iterator != end;
         iterator.increment(iteration_error)) {
        if (iteration_error) {
            break;
        }

        std::error_code entry_error{};
        if (!iterator->is_regular_file(entry_error) || entry_error) {
            continue;
        }

        const auto candidate = classifier(iterator->path());
        if (!candidate.has_value()) {
            continue;
        }

        matched_candidates.push_back(*candidate);
    }

    if (matched_candidates.empty()) {
        return SubtitleAutoSelectionResult{
            .decision = SubtitleAutoSelectionDecisionCode::no_match,
            .matched_candidate_count = 0,
            .decision_summary = no_match_summary()
        };
    }

    std::sort(
        matched_candidates.begin(),
        matched_candidates.end(),
        [](const RankedCandidate &left, const RankedCandidate &right) {
            if (left.candidate.match_kind != right.candidate.match_kind) {
                return left.candidate.match_kind < right.candidate.match_kind;
            }
            if (left.stem_rank != right.stem_rank) {
                return left.stem_rank < right.stem_rank;
            }
            if (left.extension_rank != right.extension_rank) {
                return left.extension_rank < right.extension_rank;
            }
            return left.normalized_file_name < right.normalized_file_name;
        }
    );

    const RankedCandidate &selected_candidate = matched_candidates.front();
    const bool has_plain_match = std::any_of(
        matched_candidates.begin(),
        matched_candidates.end(),
        [](const RankedCandidate &candidate) {
            return candidate.candidate.match_kind == SubtitleAutoSelectionMatchKind::exact_plain;
        }
    );
    const bool used_fx_priority_rule =
        selected_candidate.candidate.match_kind == SubtitleAutoSelectionMatchKind::exact_fx && has_plain_match;

    return SubtitleAutoSelectionResult{
        .decision = SubtitleAutoSelectionDecisionCode::selected,
        .selected_candidate = selected_candidate.candidate,
        .matched_candidate_count = matched_candidates.size(),
        .used_fx_priority_rule = used_fx_priority_rule,
        .decision_summary = selected_summary(selected_candidate, used_fx_priority_rule)
    };
}

}  // namespace

bool SubtitleAutoSelectionResult::has_selection() const noexcept {
    return selected_candidate.has_value() && decision == SubtitleAutoSelectionDecisionCode::selected;
}

SubtitleAutoSelectionResult SubtitleAutoSelector::select(const std::filesystem::path &source_path) {
    return select_from_directory(
        source_path,
        [&source_path](const std::filesystem::path &candidate_path) {
            return classify_candidate(source_path, candidate_path);
        },
        [&source_path]() {
            return build_no_match_summary(source_path);
        },
        [&source_path](const RankedCandidate &selected_candidate, const bool used_fx_priority_rule) {
            return build_selected_summary(source_path, selected_candidate, used_fx_priority_rule);
        }
    );
}

SubtitleAutoSelectionResult SubtitleAutoSelector::select_for_selected_text(
    const SubtitleSelectedTextSelectionRequest &request
) {
    const std::vector<std::string> candidate_stems = build_selected_text_candidate_stems(
        request.source_path,
        request.current_subtitle_path,
        request.selected_text
    );
    if (candidate_stems.empty()) {
        return SubtitleAutoSelectionResult{
            .decision = SubtitleAutoSelectionDecisionCode::no_match,
            .matched_candidate_count = 0,
            .decision_summary = build_selected_text_ignored_summary(request.source_path)
        };
    }

    return select_from_directory(
        request.source_path,
        [&candidate_stems](const std::filesystem::path &candidate_path) {
            return classify_selected_text_candidate(candidate_path, candidate_stems);
        },
        [&request]() {
            return build_selected_text_no_match_summary(request.source_path);
        },
        [&request](const RankedCandidate &selected_candidate, bool) {
            return build_selected_text_selected_summary(request.source_path, selected_candidate);
        }
    );
}

const char *to_string(const SubtitleAutoSelectionDecisionCode code) noexcept {
    switch (code) {
    case SubtitleAutoSelectionDecisionCode::selected:
        return "selected";
    case SubtitleAutoSelectionDecisionCode::no_match:
        return "no_match";
    case SubtitleAutoSelectionDecisionCode::source_directory_unavailable:
        return "source_directory_unavailable";
    default:
        return "unknown";
    }
}

const char *to_string(const SubtitleAutoSelectionMatchKind kind) noexcept {
    switch (kind) {
    case SubtitleAutoSelectionMatchKind::exact_fx:
        return "exact_fx";
    case SubtitleAutoSelectionMatchKind::exact_plain:
        return "exact_plain";
    default:
        return "unknown";
    }
}

}  // namespace utsure::core::subtitles
