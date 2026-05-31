#pragma once

#include "utsure/core/media/media_info.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace utsure::core::job {

enum class EncodeResizeMode : std::uint8_t {
    source = 0,
    target_height
};

struct EncodeResizeSettings final {
    EncodeResizeMode mode{EncodeResizeMode::source};
    int target_height{0};
    bool allow_upscale{false};
};

struct ResizeSourceDimensions final {
    int width{0};
    int height{0};
    media::Rational sample_aspect_ratio{1, 1};
};

struct ResizeOutputDimensions final {
    int width{0};
    int height{0};
    media::Rational sample_aspect_ratio{1, 1};
};

struct ResizeCalculationResult final {
    std::optional<ResizeOutputDimensions> dimensions{};
    std::string error_message{};

    [[nodiscard]] bool succeeded() const noexcept {
        return dimensions.has_value() && error_message.empty();
    }
};

[[nodiscard]] ResizeCalculationResult calculate_resize_dimensions(
    const ResizeSourceDimensions &source,
    const EncodeResizeSettings &settings
) noexcept;

[[nodiscard]] const char *to_string(EncodeResizeMode mode) noexcept;
[[nodiscard]] std::optional<EncodeResizeMode> resize_mode_from_string(std::string_view text) noexcept;

}  // namespace utsure::core::job
