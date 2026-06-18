#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct BatchImportOrderInfo final {
    std::optional<int> season_number{};
    std::optional<int> episode_number{};
    std::optional<int> version_number{};
};

struct BatchImportPlannedJob final {
    std::filesystem::path source_path{};
    std::optional<std::filesystem::path> subtitle_path{};
    BatchImportOrderInfo order{};
    int original_import_index{0};
    int queue_order_index{0};
    std::string warning{};
};

struct BatchImportPlan final {
    std::vector<BatchImportPlannedJob> jobs{};
    std::vector<std::string> warnings{};
};

class BatchImportPlanner final {
public:
    [[nodiscard]] static bool is_supported_video_path(const std::filesystem::path &path);
    [[nodiscard]] static bool is_supported_subtitle_path(const std::filesystem::path &path);
    [[nodiscard]] static BatchImportOrderInfo detect_order_info(const std::filesystem::path &path);
    [[nodiscard]] static int natural_compare(std::string left, std::string right);
    [[nodiscard]] static BatchImportPlan plan(const std::vector<std::filesystem::path> &input_paths);
};
