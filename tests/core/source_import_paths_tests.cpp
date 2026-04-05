#include "utsure/core/media/source_import_paths.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using utsure::core::media::SourceImportPathExpansionResult;
using utsure::core::media::SourceImportPaths;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

struct TempDirectoryGuard final {
    explicit TempDirectoryGuard(std::filesystem::path value) : path(std::move(value)) {}

    ~TempDirectoryGuard() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path{};
};

std::filesystem::path make_temp_directory() {
    const auto unique_suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root =
        std::filesystem::temp_directory_path() / ("utsure-source-import-path-tests-" + unique_suffix);
    std::filesystem::create_directories(root);
    return root;
}

void touch_file(const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << "test";
}

std::filesystem::path normalize_path(const std::filesystem::path &path) {
    std::error_code error{};
    const auto absolute_path = std::filesystem::absolute(path, error);
    return (error ? path : absolute_path).lexically_normal();
}

int assert_supported_candidate_detection(const std::filesystem::path &root) {
    const auto source_file = root / "Episode 01.MP4";
    const auto unsupported_file = root / "notes.txt";
    const auto source_folder = root / "batch";

    touch_file(source_file);
    touch_file(unsupported_file);
    std::filesystem::create_directories(source_folder);

    if (!SourceImportPaths::is_supported_video_file(source_file)) {
        return fail("The source import helper should accept supported video extensions case-insensitively.");
    }

    if (SourceImportPaths::is_supported_video_file(unsupported_file)) {
        return fail("The source import helper should reject unsupported file extensions.");
    }

    if (!SourceImportPaths::is_supported_drop_candidate(source_file) ||
        !SourceImportPaths::is_supported_drop_candidate(source_folder) ||
        SourceImportPaths::is_supported_drop_candidate(unsupported_file)) {
        return fail("The source import helper did not classify file and folder drop candidates correctly.");
    }

    std::cout << "supported.file=" << source_file.filename().string() << '\n';
    return 0;
}

int assert_folder_expansion_is_non_recursive_and_deduplicated(const std::filesystem::path &root) {
    const auto folder = root / "drop-folder";
    const auto explicit_file = folder / "beta.MP4";
    const auto extra_video = folder / "alpha.mkv";
    const auto ignored_text = folder / "readme.txt";
    const auto nested_video = folder / "nested" / "gamma.mov";

    touch_file(explicit_file);
    touch_file(extra_video);
    touch_file(ignored_text);
    touch_file(nested_video);

    const SourceImportPathExpansionResult result = SourceImportPaths::expand_drop_candidates({
        explicit_file,
        folder,
        ignored_text,
        extra_video
    });

    const std::vector<std::filesystem::path> expected_paths{
        normalize_path(explicit_file),
        normalize_path(extra_video)
    };

    if (result.accepted_source_paths != expected_paths) {
        std::cerr << "Expected paths:\n";
        for (const auto &path : expected_paths) {
            std::cerr << "  " << path.string() << '\n';
        }
        std::cerr << "Actual paths:\n";
        for (const auto &path : result.accepted_source_paths) {
            std::cerr << "  " << path.string() << '\n';
        }
        return fail("Folder expansion should stay one level deep, keep stable order, and deduplicate repeats.");
    }

    std::cout << "folder.paths=" << result.accepted_source_paths.size() << '\n';
    return 0;
}

int assert_multiple_directories_preserve_input_order(const std::filesystem::path &root) {
    const auto first_folder = root / "B-folder";
    const auto second_folder = root / "A-folder";
    const auto first_file = first_folder / "episode-02.webm";
    const auto second_file = second_folder / "episode-01.m2ts";

    touch_file(first_file);
    touch_file(second_file);

    const SourceImportPathExpansionResult result = SourceImportPaths::expand_drop_candidates({
        second_folder,
        first_folder
    });

    const std::vector<std::filesystem::path> expected_paths{
        normalize_path(second_file),
        normalize_path(first_file)
    };

    if (result.accepted_source_paths != expected_paths) {
        return fail("Dropped folders should expand in the same order they were provided.");
    }

    std::cout << "multi-folder.paths=" << result.accepted_source_paths.size() << '\n';
    return 0;
}

}  // namespace

int main() {
    const auto root = make_temp_directory();
    const TempDirectoryGuard cleanup(root);

    if (assert_supported_candidate_detection(root) != 0) {
        return 1;
    }

    if (assert_folder_expansion_is_non_recursive_and_deduplicated(root) != 0) {
        return 1;
    }

    if (assert_multiple_directories_preserve_input_order(root) != 0) {
        return 1;
    }

    return 0;
}
