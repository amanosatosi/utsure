#include "batch_import_planner.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

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
        std::filesystem::temp_directory_path() / ("utsure-batch-import-planner-tests-" + unique_suffix);
    std::filesystem::create_directories(root);
    return root;
}

void touch_file(const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << "test";
}

int assert_order_detection() {
    const auto s1e2 = BatchImportPlanner::detect_order_info("Show.S01E02.1080p.x265.10bit.mkv");
    if (s1e2.season_number != 1 || s1e2.episode_number != 2) {
        return fail("S01E02 order detection failed.");
    }

    const auto ep2 = BatchImportPlanner::detect_order_info("Show Ep.02 [A1B2C3D4].mkv");
    if (ep2.episode_number != 2) {
        return fail("Ep.02 order detection failed.");
    }

    const auto dash2 = BatchImportPlanner::detect_order_info("Show - 02v2 1080p.mkv");
    if (dash2.episode_number != 2 || dash2.version_number != 2) {
        return fail("Show - 02v2 episode/version detection failed.");
    }

    const auto noise = BatchImportPlanner::detect_order_info("Movie 2024 1080p x265 10bit 5.1.mkv");
    if (noise.episode_number.has_value()) {
        return fail("Noise tokens were incorrectly detected as episode numbers.");
    }

    std::cout << "batch_import.detect=ok\n";
    return 0;
}

int assert_natural_sort() {
    std::vector<std::string> names{"Episode 10.mkv", "episode 2.mkv", "Episode 1.mkv"};
    std::stable_sort(names.begin(), names.end(), [](const auto &left, const auto &right) {
        return BatchImportPlanner::natural_compare(left, right) < 0;
    });

    if (names[0] != "Episode 1.mkv" || names[1] != "episode 2.mkv" || names[2] != "Episode 10.mkv") {
        return fail("Natural filename sort did not order 1, 2, 10.");
    }

    std::cout << "batch_import.natural=ok\n";
    return 0;
}

int assert_plan_sort_and_sidecars(const std::filesystem::path &root) {
    const auto input = root / "input";
    touch_file(input / "Show - 10.mkv");
    touch_file(input / "Show - 02.mkv");
    touch_file(input / "Show - 01.mkv");
    touch_file(input / "Show - 02.ass");
    touch_file(input / "OP.txt");

    const auto plan = BatchImportPlanner::plan({input});
    if (plan.jobs.size() != 3U ||
        plan.jobs[0].source_path.filename() != "Show - 01.mkv" ||
        plan.jobs[1].source_path.filename() != "Show - 02.mkv" ||
        plan.jobs[2].source_path.filename() != "Show - 10.mkv" ||
        plan.jobs[0].queue_order_index != 0 ||
        plan.jobs[1].queue_order_index != 1 ||
        plan.jobs[2].queue_order_index != 2) {
        return fail("Batch import planner did not produce deterministic detected-episode order.");
    }
    if (!plan.jobs[1].subtitle_path.has_value() ||
        plan.jobs[1].subtitle_path->filename() != "Show - 02.ass") {
        return fail("Exact sidecar subtitle matching failed.");
    }

    std::cout << "batch_import.plan=ok\n";
    return 0;
}

int assert_ambiguous_episode_sidecars_warn(const std::filesystem::path &root) {
    const auto input = root / "ambiguous";
    touch_file(input / "Show - 03.mkv");
    touch_file(input / "Subtitle EP03.ass");
    touch_file(input / "Other EP03.ssa");

    const auto plan = BatchImportPlanner::plan({input});
    if (plan.jobs.size() != 1U ||
        plan.jobs[0].subtitle_path.has_value() ||
        plan.jobs[0].warning.empty()) {
        return fail("Ambiguous episode subtitle sidecars did not leave the job unset with a warning.");
    }

    std::cout << "batch_import.ambiguous_sidecar=ok\n";
    return 0;
}

}  // namespace

int main() {
    const auto root = make_temp_directory();
    const TempDirectoryGuard cleanup(root);

    if (assert_order_detection() != 0) {
        return 1;
    }
    if (assert_natural_sort() != 0) {
        return 1;
    }
    if (assert_plan_sort_and_sidecars(root) != 0) {
        return 1;
    }
    if (assert_ambiguous_episode_sidecars_warn(root) != 0) {
        return 1;
    }
    return 0;
}
