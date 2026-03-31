#include "utsure/core/subtitles/subtitle_auto_selection.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <system_error>
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

int extension_priority(const std::string_view extension) {
    return extension == ".ass" ? 0 : 1;
}

struct RankedCandidate final {
    SubtitleAutoSelectionCandidate candidate{};
    int extension_rank{1};
    std::string normalized_file_name{};
};

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
        .normalized_file_name = lowercase_ascii(candidate_path.filename().string())
    };
}

std::string build_no_match_summary(const std::filesystem::path &source_path) {
    return "Automatic subtitle selection for '" + format_path_leaf(source_path) +
        "': no exact '.ass' or '.ssa' match was found beside the source.";
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

}  // namespace

bool SubtitleAutoSelectionResult::has_selection() const noexcept {
    return selected_candidate.has_value() && decision == SubtitleAutoSelectionDecisionCode::selected;
}

SubtitleAutoSelectionResult SubtitleAutoSelector::select(const std::filesystem::path &source_path) {
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

        const auto candidate = classify_candidate(source_path, iterator->path());
        if (!candidate.has_value()) {
            continue;
        }

        matched_candidates.push_back(*candidate);
    }

    if (matched_candidates.empty()) {
        return SubtitleAutoSelectionResult{
            .decision = SubtitleAutoSelectionDecisionCode::no_match,
            .matched_candidate_count = 0,
            .decision_summary = build_no_match_summary(source_path)
        };
    }

    std::sort(
        matched_candidates.begin(),
        matched_candidates.end(),
        [](const RankedCandidate &left, const RankedCandidate &right) {
            if (left.candidate.match_kind != right.candidate.match_kind) {
                return left.candidate.match_kind < right.candidate.match_kind;
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
        .decision_summary = build_selected_summary(source_path, selected_candidate, used_fx_priority_rule)
    };
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
