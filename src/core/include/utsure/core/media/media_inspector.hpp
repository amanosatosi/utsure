#pragma once

#include "utsure/core/media/media_info.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace utsure::core::media {

struct MediaInspectionError final {
    std::string input_path{};
    std::string message{};
    std::string actionable_hint{};
};

struct MediaInspectionResult final {
    std::optional<MediaSourceInfo> media_source_info{};
    std::optional<MediaInspectionError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

class MediaInspector final {
public:
    [[nodiscard]] static MediaInspectionResult inspect(const std::filesystem::path &input_path) noexcept;
};

}  // namespace utsure::core::media
