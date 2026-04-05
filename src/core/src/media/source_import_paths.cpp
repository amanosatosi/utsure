#include "utsure/core/media/source_import_paths.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace utsure::core::media {

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

std::filesystem::path normalize_path(const std::filesystem::path &path) {
    std::error_code absolute_error{};
    const auto absolute_path = std::filesystem::absolute(path, absolute_error);
    return (absolute_error ? path : absolute_path).lexically_normal();
}

std::string normalized_path_key(const std::filesystem::path &path) {
    const auto normalized_path = normalize_path(path);
    auto key = normalized_path.generic_u8string();
    std::string normalized_key(key.begin(), key.end());
#ifdef _WIN32
    normalized_key = lowercase_ascii(std::move(normalized_key));
#endif
    return normalized_key;
}

bool is_supported_video_extension(const std::string_view extension) {
    return extension == ".mp4" || extension == ".mkv" || extension == ".mov" || extension == ".m4v" ||
        extension == ".avi" || extension == ".wmv" || extension == ".webm" || extension == ".ts" ||
        extension == ".m2ts" || extension == ".mts" || extension == ".flv";
}

void append_path_if_unique(
    const std::filesystem::path &path,
    std::unordered_set<std::string> &seen_paths,
    std::vector<std::filesystem::path> &accepted_paths
) {
    const auto normalized_path = normalize_path(path);
    const auto [_, inserted] = seen_paths.insert(normalized_path_key(normalized_path));
    if (!inserted) {
        return;
    }

    accepted_paths.push_back(normalized_path);
}

std::vector<std::filesystem::path> collect_supported_directory_files(const std::filesystem::path &directory_path) {
    std::vector<std::filesystem::path> supported_paths{};
    std::error_code iteration_error{};
    for (std::filesystem::directory_iterator iterator(directory_path, iteration_error), end;
         iterator != end;
         iterator.increment(iteration_error)) {
        if (iteration_error) {
            break;
        }

        std::error_code entry_error{};
        if (!iterator->is_regular_file(entry_error) || entry_error) {
            continue;
        }

        if (!SourceImportPaths::is_supported_video_file(iterator->path())) {
            continue;
        }

        supported_paths.push_back(normalize_path(iterator->path()));
    }

    std::sort(
        supported_paths.begin(),
        supported_paths.end(),
        [](const std::filesystem::path &left, const std::filesystem::path &right) {
            return normalized_path_key(left) < normalized_path_key(right);
        }
    );
    return supported_paths;
}

}  // namespace

bool SourceImportPaths::is_supported_video_file(const std::filesystem::path &path) {
    std::error_code status_error{};
    if (!std::filesystem::is_regular_file(path, status_error) || status_error) {
        return false;
    }

    return is_supported_video_extension(lowercase_ascii(path.extension().string()));
}

bool SourceImportPaths::is_supported_drop_candidate(const std::filesystem::path &path) {
    std::error_code status_error{};
    if (std::filesystem::is_directory(path, status_error) && !status_error) {
        return true;
    }

    return is_supported_video_file(path);
}

SourceImportPathExpansionResult SourceImportPaths::expand_drop_candidates(
    const std::vector<std::filesystem::path> &paths
) {
    SourceImportPathExpansionResult result{};
    std::unordered_set<std::string> seen_paths{};
    for (const auto &path : paths) {
        if (path.empty()) {
            continue;
        }

        std::error_code status_error{};
        if (std::filesystem::is_directory(path, status_error) && !status_error) {
            for (const auto &directory_file : collect_supported_directory_files(path)) {
                append_path_if_unique(directory_file, seen_paths, result.accepted_source_paths);
            }
            continue;
        }

        if (!is_supported_video_file(path)) {
            continue;
        }

        append_path_if_unique(path, seen_paths, result.accepted_source_paths);
    }

    return result;
}

}  // namespace utsure::core::media
