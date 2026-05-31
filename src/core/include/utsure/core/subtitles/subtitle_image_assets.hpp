#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace utsure::core::subtitles {

enum class SubtitleImageAssetFormat {
    png,
    jpeg,
    webp
};

struct SubtitleImageAssetReference final {
    std::string name{};
};

struct SubtitleImageAsset final {
    std::string name{};
    std::filesystem::path source_path{};
    SubtitleImageAssetFormat format{SubtitleImageAssetFormat::png};
    int width{0};
    int height{0};
    int stride{0};
    std::vector<std::uint8_t> rgba{};
};

struct SubtitleImageAssetError final {
    std::string message{};
    std::string actionable_hint{};
};

struct SubtitleImageAssetLoadResult final {
    std::vector<SubtitleImageAssetReference> references{};
    std::vector<SubtitleImageAsset> assets{};
    std::vector<std::string> diagnostics{};
    std::optional<SubtitleImageAssetError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

[[nodiscard]] std::vector<SubtitleImageAssetReference> find_subtitle_image_asset_references_in_text(
    std::string_view script_text
);

[[nodiscard]] SubtitleImageAssetLoadResult load_subtitle_image_assets(
    const std::filesystem::path &subtitle_path
);

[[nodiscard]] const char *to_string(SubtitleImageAssetFormat format) noexcept;

}  // namespace utsure::core::subtitles
