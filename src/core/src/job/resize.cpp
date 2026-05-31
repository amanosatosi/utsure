#include "utsure/core/job/resize.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace utsure::core::job {
namespace {

constexpr int kMaximumEncoderDimension = 8192;

[[nodiscard]] bool rational_is_positive(const media::Rational &value) noexcept {
    return value.numerator > 0 && value.denominator > 0;
}

[[nodiscard]] int round_to_even(const double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }

    int rounded = static_cast<int>(std::lround(value));
    if (rounded < 2) {
        rounded = 2;
    }
    if ((rounded % 2) != 0) {
        ++rounded;
    }
    return rounded;
}

[[nodiscard]] int floor_to_even(const int value) noexcept {
    if (value <= 0) {
        return 0;
    }
    const int rounded = value - (value % 2);
    return rounded >= 2 ? rounded : 0;
}

[[nodiscard]] std::optional<ResizeOutputDimensions> validate_source_dimensions_preserve_exact(
    const int width,
    const int height,
    const media::Rational sample_aspect_ratio
) noexcept {
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    if (width > kMaximumEncoderDimension || height > kMaximumEncoderDimension) {
        return std::nullopt;
    }

    return ResizeOutputDimensions{
        .width = width,
        .height = height,
        .sample_aspect_ratio = sample_aspect_ratio
    };
}

[[nodiscard]] std::optional<ResizeOutputDimensions> validate_encode_dimensions(
    const int width,
    const int height,
    const media::Rational sample_aspect_ratio
) noexcept {
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    if (width > kMaximumEncoderDimension || height > kMaximumEncoderDimension) {
        return std::nullopt;
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        return std::nullopt;
    }

    return ResizeOutputDimensions{
        .width = width,
        .height = height,
        .sample_aspect_ratio = sample_aspect_ratio
    };
}

}  // namespace

ResizeCalculationResult calculate_resize_dimensions(
    const ResizeSourceDimensions &source,
    const EncodeResizeSettings &settings
) noexcept {
    const auto source_dimensions =
        validate_source_dimensions_preserve_exact(source.width, source.height, source.sample_aspect_ratio);
    if (!source_dimensions.has_value()) {
        return ResizeCalculationResult{
            .dimensions = std::nullopt,
            .error_message = "Source video dimensions are invalid for resize calculation."
        };
    }

    if (settings.mode == EncodeResizeMode::source) {
        return ResizeCalculationResult{
            .dimensions = *source_dimensions
        };
    }

    if (settings.mode != EncodeResizeMode::target_height) {
        return ResizeCalculationResult{
            .dimensions = std::nullopt,
            .error_message = "Unsupported resize mode."
        };
    }

    if (settings.target_height <= 0 || settings.target_height > kMaximumEncoderDimension) {
        return ResizeCalculationResult{
            .dimensions = std::nullopt,
            .error_message = "Target resize height must be a positive encoder-safe value."
        };
    }

    const int requested_target_height = !settings.allow_upscale && source.height <= settings.target_height
        ? floor_to_even(source.height)
        : round_to_even(static_cast<double>(settings.target_height));
    const int target_height = requested_target_height;
    if (target_height <= 0 || target_height > kMaximumEncoderDimension) {
        return ResizeCalculationResult{
            .dimensions = std::nullopt,
            .error_message = "Target resize height must be a positive encoder-safe value."
        };
    }

    const double sar_multiplier = rational_is_positive(source.sample_aspect_ratio)
        ? static_cast<double>(source.sample_aspect_ratio.numerator) /
            static_cast<double>(source.sample_aspect_ratio.denominator)
        : 1.0;
    const double display_aspect_width = static_cast<double>(source.width) * sar_multiplier;
    const double target_width_value =
        static_cast<double>(target_height) * display_aspect_width / static_cast<double>(source.height);
    const int target_width = round_to_even(target_width_value);

    const auto target_dimensions = validate_encode_dimensions(target_width, target_height, media::Rational{1, 1});
    if (!target_dimensions.has_value()) {
        return ResizeCalculationResult{
            .dimensions = std::nullopt,
            .error_message = "Calculated resize dimensions are invalid or too large."
        };
    }

    return ResizeCalculationResult{
        .dimensions = *target_dimensions
    };
}

const char *to_string(const EncodeResizeMode mode) noexcept {
    switch (mode) {
    case EncodeResizeMode::source:
        return "source";
    case EncodeResizeMode::target_height:
        return "targetHeight";
    default:
        return "unknown";
    }
}

std::optional<EncodeResizeMode> resize_mode_from_string(const std::string_view text) noexcept {
    if (text == "source" || text == "none" || text == "noResize") {
        return EncodeResizeMode::source;
    }
    if (text == "targetHeight" || text == "target_height") {
        return EncodeResizeMode::target_height;
    }
    return std::nullopt;
}

}  // namespace utsure::core::job
