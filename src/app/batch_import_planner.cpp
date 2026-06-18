#include "batch_import_planner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

namespace {

std::string path_component_to_string(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char *>(value.c_str()), value.size());
#else
    return path.string();
#endif
}

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

std::string normalize_stem_text(std::string value) {
    value = lowercase_ascii(std::move(value));
    std::string normalized{};
    normalized.reserve(value.size());
    bool previous_separator = false;
    bool in_crc_block = false;
    std::string bracket_text{};
    for (const unsigned char character : value) {
        if (character == '[') {
            in_crc_block = true;
            bracket_text.clear();
            continue;
        }
        if (in_crc_block) {
            if (character == ']') {
                const bool looks_crc = bracket_text.size() == 8 &&
                    std::all_of(bracket_text.begin(), bracket_text.end(), [](const unsigned char value) {
                        return std::isxdigit(value) != 0;
                    });
                if (!looks_crc) {
                    if (!normalized.empty() && !previous_separator) {
                        normalized.push_back(' ');
                    }
                    normalized += bracket_text;
                    previous_separator = false;
                }
                in_crc_block = false;
            } else {
                bracket_text.push_back(static_cast<char>(character));
            }
            continue;
        }

        if (std::isalnum(character)) {
            normalized.push_back(static_cast<char>(character));
            previous_separator = false;
            continue;
        }

        if (!normalized.empty() && !previous_separator) {
            normalized.push_back(' ');
            previous_separator = true;
        }
    }

    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

std::optional<int> parse_int_token(const std::string &value) {
    if (value.empty() || value.size() > 6 ||
        !std::all_of(value.begin(), value.end(), [](const unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        return std::nullopt;
    }
    return std::stoi(value);
}

bool is_noise_number_context(const std::string &text, const std::size_t begin, const std::size_t end) {
    const std::string before = begin >= 4 ? text.substr(begin - 4, 4) : text.substr(0, begin);
    const std::string after = text.substr(end, std::min<std::size_t>(4, text.size() - end));
    const auto value = parse_int_token(text.substr(begin, end - begin)).value_or(0);
    if (value == 720 || value == 1080 || value == 2160 || value == 480) {
        return true;
    }
    if (value == 264 || value == 265) {
        return before.ends_with('x') || before.ends_with('h');
    }
    if ((value == 8 || value == 10) && after.starts_with("bit")) {
        return true;
    }
    if ((value == 5 || value == 2 || value == 7) &&
        (after.starts_with(" 1") || after.starts_with(" 0"))) {
        return true;
    }
    if ((value == 1 || value == 0) && begin >= 2 && std::isdigit(static_cast<unsigned char>(text[begin - 2])) &&
        text[begin - 1] == ' ') {
        return true;
    }
    if (value >= 1900 && value <= 2099) {
        return true;
    }
    return false;
}

std::string normalized_absolute_key(const std::filesystem::path &path) {
    std::error_code error{};
    auto absolute_path = std::filesystem::absolute(path, error);
    if (error) {
        absolute_path = path;
    }
    std::string key = absolute_path.lexically_normal().generic_string();
#if defined(_WIN32)
    key = lowercase_ascii(std::move(key));
#endif
    return key;
}

void collect_input_paths(
    const std::filesystem::path &path,
    std::vector<std::filesystem::path> &video_paths,
    std::vector<std::filesystem::path> &subtitle_paths
) {
    std::error_code status_error{};
    if (std::filesystem::is_directory(path, status_error) && !status_error) {
        std::vector<std::filesystem::path> children{};
        std::error_code iteration_error{};
        for (std::filesystem::recursive_directory_iterator iterator(path, iteration_error), end;
             iterator != end;
             iterator.increment(iteration_error)) {
            if (iteration_error) {
                break;
            }
            std::error_code entry_error{};
            if (!iterator->is_regular_file(entry_error) || entry_error) {
                continue;
            }
            children.push_back(iterator->path());
        }
        std::stable_sort(children.begin(), children.end(), [](const auto &left, const auto &right) {
            return BatchImportPlanner::natural_compare(
                path_component_to_string(left.lexically_normal()),
                path_component_to_string(right.lexically_normal())
            ) < 0;
        });
        for (const auto &child : children) {
            if (BatchImportPlanner::is_supported_video_path(child)) {
                video_paths.push_back(child);
            } else if (BatchImportPlanner::is_supported_subtitle_path(child)) {
                subtitle_paths.push_back(child);
            }
        }
        return;
    }

    if (BatchImportPlanner::is_supported_video_path(path)) {
        video_paths.push_back(path);
    } else if (BatchImportPlanner::is_supported_subtitle_path(path)) {
        subtitle_paths.push_back(path);
    }
}

bool order_less(const BatchImportPlannedJob &left, const BatchImportPlannedJob &right) {
    if (left.order.season_number != right.order.season_number) {
        if (!left.order.season_number.has_value()) {
            return false;
        }
        if (!right.order.season_number.has_value()) {
            return true;
        }
        return *left.order.season_number < *right.order.season_number;
    }
    if (left.order.episode_number != right.order.episode_number) {
        if (!left.order.episode_number.has_value()) {
            return false;
        }
        if (!right.order.episode_number.has_value()) {
            return true;
        }
        return *left.order.episode_number < *right.order.episode_number;
    }
    const int natural = BatchImportPlanner::natural_compare(
        path_component_to_string(left.source_path.filename()),
        path_component_to_string(right.source_path.filename())
    );
    if (natural != 0) {
        return natural < 0;
    }
    return left.original_import_index < right.original_import_index;
}

}  // namespace

bool BatchImportPlanner::is_supported_video_path(const std::filesystem::path &path) {
    const std::string extension = lowercase_ascii(path.extension().string());
    static const std::set<std::string> kExtensions{
        ".avi", ".m2ts", ".m4v", ".mkv", ".mov", ".mp4", ".mpeg", ".mpg", ".mts", ".ts", ".webm", ".wmv"
    };
    return kExtensions.contains(extension);
}

bool BatchImportPlanner::is_supported_subtitle_path(const std::filesystem::path &path) {
    const std::string extension = lowercase_ascii(path.extension().string());
    return extension == ".ass" || extension == ".ssa";
}

BatchImportOrderInfo BatchImportPlanner::detect_order_info(const std::filesystem::path &path) {
    const std::string stem = normalize_stem_text(path_component_to_string(path.stem()));
    BatchImportOrderInfo result{};

    const std::vector<std::regex> season_episode_patterns{
        std::regex(R"(\bs\s*0*([0-9]{1,2})\s*e\s*0*([0-9]{1,4})\b)", std::regex_constants::icase),
        std::regex(R"(\b([0-9]{1,2})\s*x\s*0*([0-9]{1,4})\b)", std::regex_constants::icase)
    };
    for (const auto &pattern : season_episode_patterns) {
        std::smatch match{};
        if (std::regex_search(stem, match, pattern)) {
            result.season_number = parse_int_token(match[1].str());
            result.episode_number = parse_int_token(match[2].str());
            break;
        }
    }

    if (!result.episode_number.has_value()) {
        const std::vector<std::regex> episode_patterns{
            std::regex(R"(\bep(?:isode)?\s*0*([0-9]{1,4})\b)", std::regex_constants::icase),
            std::regex(R"(\b-\s*0*([0-9]{1,4})(?:\s|$|v[0-9]+\b))", std::regex_constants::icase),
            std::regex(R"(\b0*([0-9]{1,4})v[0-9]+\b)", std::regex_constants::icase)
        };
        for (const auto &pattern : episode_patterns) {
            std::smatch match{};
            if (std::regex_search(stem, match, pattern)) {
                result.episode_number = parse_int_token(match[1].str());
                break;
            }
        }
    }

    if (!result.episode_number.has_value()) {
        std::regex token_pattern(R"(\b([0-9]{1,4})\b)");
        for (std::sregex_iterator iterator(stem.begin(), stem.end(), token_pattern), end; iterator != end; ++iterator) {
            const auto begin = static_cast<std::size_t>(iterator->position(1));
            const auto token = (*iterator)[1].str();
            const auto parsed = parse_int_token(token);
            if (!parsed.has_value()) {
                continue;
            }
            if (is_noise_number_context(stem, begin, begin + token.size())) {
                continue;
            }
            result.episode_number = parsed;
            break;
        }
    }

    std::smatch version_match{};
    if (std::regex_search(stem, version_match, std::regex(R"(\bv\s*([0-9]+)\b)", std::regex_constants::icase))) {
        result.version_number = parse_int_token(version_match[1].str());
    }

    return result;
}

int BatchImportPlanner::natural_compare(std::string left, std::string right) {
    left = lowercase_ascii(std::move(left));
    right = lowercase_ascii(std::move(right));
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        const unsigned char left_char = static_cast<unsigned char>(left[left_index]);
        const unsigned char right_char = static_cast<unsigned char>(right[right_index]);
        if (std::isdigit(left_char) && std::isdigit(right_char)) {
            const std::size_t left_start = left_index;
            const std::size_t right_start = right_index;
            while (left_index < left.size() && std::isdigit(static_cast<unsigned char>(left[left_index]))) {
                ++left_index;
            }
            while (right_index < right.size() && std::isdigit(static_cast<unsigned char>(right[right_index]))) {
                ++right_index;
            }
            std::string left_number = left.substr(left_start, left_index - left_start);
            std::string right_number = right.substr(right_start, right_index - right_start);
            left_number.erase(0, left_number.find_first_not_of('0'));
            right_number.erase(0, right_number.find_first_not_of('0'));
            if (left_number.empty()) {
                left_number = "0";
            }
            if (right_number.empty()) {
                right_number = "0";
            }
            if (left_number.size() != right_number.size()) {
                return left_number.size() < right_number.size() ? -1 : 1;
            }
            if (left_number != right_number) {
                return left_number < right_number ? -1 : 1;
            }
            continue;
        }
        if (left_char != right_char) {
            return left_char < right_char ? -1 : 1;
        }
        ++left_index;
        ++right_index;
    }
    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

BatchImportPlan BatchImportPlanner::plan(const std::vector<std::filesystem::path> &input_paths) {
    std::vector<std::filesystem::path> video_paths{};
    std::vector<std::filesystem::path> subtitle_paths{};
    video_paths.reserve(input_paths.size());
    subtitle_paths.reserve(input_paths.size());
    for (const auto &path : input_paths) {
        collect_input_paths(path, video_paths, subtitle_paths);
    }

    std::set<std::string> seen_video_paths{};
    std::vector<BatchImportPlannedJob> jobs{};
    jobs.reserve(video_paths.size());
    int original_index = 0;
    for (const auto &path : video_paths) {
        const std::string key = normalized_absolute_key(path);
        if (!seen_video_paths.insert(key).second) {
            continue;
        }
        jobs.push_back(BatchImportPlannedJob{
            .source_path = path.lexically_normal(),
            .order = detect_order_info(path),
            .original_import_index = original_index++
        });
    }

    std::stable_sort(jobs.begin(), jobs.end(), order_less);

    std::multimap<std::string, std::filesystem::path> subtitle_by_stem{};
    std::multimap<int, std::filesystem::path> subtitle_by_episode{};
    std::set<std::string> seen_subtitle_paths{};
    for (const auto &subtitle_path : subtitle_paths) {
        const std::string key = normalized_absolute_key(subtitle_path);
        if (!seen_subtitle_paths.insert(key).second) {
            continue;
        }
        subtitle_by_stem.emplace(
            normalize_stem_text(path_component_to_string(subtitle_path.stem())),
            subtitle_path.lexically_normal()
        );
        const auto order = detect_order_info(subtitle_path);
        if (order.episode_number.has_value()) {
            subtitle_by_episode.emplace(*order.episode_number, subtitle_path.lexically_normal());
        }
    }

    for (int index = 0; index < static_cast<int>(jobs.size()); ++index) {
        auto &job = jobs[static_cast<std::size_t>(index)];
        job.queue_order_index = index;

        const std::string source_stem = normalize_stem_text(path_component_to_string(job.source_path.stem()));
        auto exact_range = subtitle_by_stem.equal_range(source_stem);
        const auto exact_count = std::distance(exact_range.first, exact_range.second);
        if (exact_count == 1) {
            job.subtitle_path = exact_range.first->second;
            continue;
        }
        if (exact_count > 1) {
            job.warning = "Multiple exact sidecar subtitles matched; subtitle was left unset.";
            continue;
        }

        if (!job.order.episode_number.has_value()) {
            continue;
        }
        auto episode_range = subtitle_by_episode.equal_range(*job.order.episode_number);
        const auto episode_count = std::distance(episode_range.first, episode_range.second);
        if (episode_count == 1) {
            job.subtitle_path = episode_range.first->second;
        } else if (episode_count > 1) {
            job.warning = "Multiple episode sidecar subtitles matched; subtitle was left unset.";
        }
    }

    return BatchImportPlan{.jobs = std::move(jobs)};
}
