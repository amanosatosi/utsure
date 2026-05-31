#include "utsure/core/subtitles/subtitle_image_assets.hpp"

#include "utsure/core/media/media_decoder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace utsure::core::subtitles {

namespace {

constexpr int kMaximumAssetDimension = 8192;
constexpr std::int64_t kMaximumAssetPixels = 64LL * 1024LL * 1024LL;

std::string trim_ascii(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool starts_img_tag_name(std::string_view value, std::size_t index) {
    if (index >= value.size() || value[index] != '\\') {
        return false;
    }

    ++index;
    if (index < value.size() && value[index] >= '1' && value[index] <= '4') {
        ++index;
    }
    if (index + 4U > value.size()) {
        return false;
    }

    return std::tolower(static_cast<unsigned char>(value[index])) == 'i' &&
        std::tolower(static_cast<unsigned char>(value[index + 1U])) == 'm' &&
        std::tolower(static_cast<unsigned char>(value[index + 2U])) == 'g' &&
        value[index + 3U] == '(';
}

std::size_t img_args_start(std::string_view value, std::size_t index) {
    ++index;
    if (index < value.size() && value[index] >= '1' && value[index] <= '4') {
        ++index;
    }
    return index + 4U;
}

std::optional<std::string> extract_img_path_argument(std::string_view args) {
    args = std::string_view(args.data(), args.find(')') == std::string_view::npos ? args.size() : args.find(')'));
    const auto trimmed = trim_ascii(args);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::string_view value{trimmed};
    if ((value.front() == '"' || value.front() == '\'') && value.size() >= 2U) {
        const char quote = value.front();
        const auto end_quote = value.find(quote, 1U);
        if (end_quote != std::string_view::npos && end_quote > 1U) {
            return trim_ascii(value.substr(1U, end_quote - 1U));
        }
    }

    const auto comma = value.find(',');
    return trim_ascii(comma == std::string_view::npos ? value : value.substr(0, comma));
}

std::string read_text_file_or_throw(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Could not read subtitle script '" + path.lexically_normal().string() + "'.");
    }

    return std::string{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
}

bool is_regular_file_quiet(const std::filesystem::path &path) {
    std::error_code error{};
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::optional<SubtitleImageAssetFormat> format_from_extension(const std::filesystem::path &path) {
    const auto extension = lowercase_ascii(path.extension().string());
    if (extension == ".png") {
        return SubtitleImageAssetFormat::png;
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return SubtitleImageAssetFormat::jpeg;
    }
    if (extension == ".webp") {
        return SubtitleImageAssetFormat::webp;
    }
    return std::nullopt;
}

bool path_has_parent_traversal(const std::filesystem::path &path) {
    for (const auto &component : path) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

bool is_safe_relative_reference(const std::filesystem::path &path) {
    return !path.empty() && !path.is_absolute() && !path_has_parent_traversal(path);
}

std::vector<std::filesystem::path> build_asset_candidates(
    const std::filesystem::path &subtitle_directory,
    const std::filesystem::path &reference_path
) {
    std::vector<std::filesystem::path> candidates{};
    const std::vector<std::filesystem::path> extension_guesses{
        ".png",
        ".webp",
        ".jpg",
        ".jpeg"
    };

    const auto add_reference_candidates = [&candidates, &extension_guesses](const std::filesystem::path &base) {
        candidates.push_back(base);
        if (!base.has_extension()) {
            for (const auto &extension : extension_guesses) {
                candidates.push_back(base.string() + extension.string());
            }
        }
    };

    add_reference_candidates(subtitle_directory / reference_path);
    if (!reference_path.has_parent_path()) {
        for (const auto &sidecar_directory : { "assets", "images", "img" }) {
            add_reference_candidates(subtitle_directory / sidecar_directory / reference_path);
        }
    }

    return candidates;
}

std::optional<std::filesystem::path> resolve_asset_path(
    const std::filesystem::path &subtitle_directory,
    const std::string &name,
    std::string *error_message
) {
    const std::filesystem::path reference_path{name};
    if (!is_safe_relative_reference(reference_path)) {
        if (error_message != nullptr) {
            *error_message = "Unsafe subtitle image asset path rejected: " + name;
        }
        return std::nullopt;
    }

    for (const auto &candidate : build_asset_candidates(subtitle_directory, reference_path)) {
        const auto normalized = candidate.lexically_normal();
        if (!is_regular_file_quiet(normalized)) {
            continue;
        }
        if (!format_from_extension(normalized).has_value()) {
            if (error_message != nullptr) {
                *error_message = "Unsupported subtitle image asset format: " + normalized.lexically_normal().string();
            }
            return std::nullopt;
        }
        return normalized;
    }

    if (error_message != nullptr) {
        *error_message = "Missing subtitle image asset: " + name;
    }
    return std::nullopt;
}

std::optional<SubtitleImageAssetError> validate_decoded_asset_frame(
    const std::string &name,
    const std::filesystem::path &source_path,
    const media::DecodedVideoFrame &frame
) {
    if (frame.width <= 0 || frame.height <= 0) {
        return SubtitleImageAssetError{
            .message = "Invalid subtitle image asset dimensions: " + name,
            .actionable_hint = "Replace '" + source_path.lexically_normal().string() + "' with a readable non-empty image."
        };
    }
    if (frame.width > kMaximumAssetDimension || frame.height > kMaximumAssetDimension) {
        return SubtitleImageAssetError{
            .message = "Subtitle image asset is too large: " + name,
            .actionable_hint = "Use an image no larger than 8192 pixels in either dimension."
        };
    }
    if (static_cast<std::int64_t>(frame.width) * static_cast<std::int64_t>(frame.height) > kMaximumAssetPixels) {
        return SubtitleImageAssetError{
            .message = "Subtitle image asset has too many pixels: " + name,
            .actionable_hint = "Use a smaller subtitle image asset."
        };
    }
    if (frame.planes.empty() || frame.planes[0].line_stride_bytes < frame.width * 4) {
        return SubtitleImageAssetError{
            .message = "Subtitle image asset did not decode to a valid RGBA surface: " + name,
            .actionable_hint = "Replace '" + source_path.lexically_normal().string() + "' with a valid PNG, JPEG, or WebP image."
        };
    }

    const auto required_size =
        static_cast<std::uint64_t>(frame.planes[0].line_stride_bytes) * static_cast<std::uint64_t>(frame.height);
    if (required_size > frame.planes[0].bytes.size()) {
        return SubtitleImageAssetError{
            .message = "Subtitle image asset decoded to a truncated RGBA buffer: " + name,
            .actionable_hint = "Replace '" + source_path.lexically_normal().string() + "' with a valid image file."
        };
    }

    return std::nullopt;
}

SubtitleImageAssetLoadResult make_asset_error(
    std::vector<SubtitleImageAssetReference> references,
    std::vector<std::string> diagnostics,
    std::string message,
    std::string actionable_hint
) {
    return SubtitleImageAssetLoadResult{
        .references = std::move(references),
        .assets = {},
        .diagnostics = std::move(diagnostics),
        .error = SubtitleImageAssetError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

}  // namespace

bool SubtitleImageAssetLoadResult::succeeded() const noexcept {
    return !error.has_value();
}

std::vector<SubtitleImageAssetReference> find_subtitle_image_asset_references_in_text(
    const std::string_view script_text
) {
    std::vector<SubtitleImageAssetReference> references{};
    std::set<std::string> seen{};

    std::size_t index = 0;
    while ((index = script_text.find('{', index)) != std::string_view::npos) {
        const auto block_end = script_text.find('}', index + 1U);
        if (block_end == std::string_view::npos) {
            break;
        }

        const auto block = script_text.substr(index + 1U, block_end - index - 1U);
        for (std::size_t block_index = 0; block_index < block.size(); ++block_index) {
            if (!starts_img_tag_name(block, block_index)) {
                continue;
            }
            const auto path_start = img_args_start(block, block_index);
            if (path_start > block.size()) {
                continue;
            }
            const auto extracted = extract_img_path_argument(block.substr(path_start));
            if (!extracted.has_value() || extracted->empty()) {
                continue;
            }
            if (seen.insert(*extracted).second) {
                references.push_back(SubtitleImageAssetReference{.name = *extracted});
            }
        }

        index = block_end + 1U;
    }

    return references;
}

SubtitleImageAssetLoadResult load_subtitle_image_assets(const std::filesystem::path &subtitle_path) {
    std::vector<std::string> diagnostics{};
    std::vector<SubtitleImageAssetReference> references{};
    try {
        references = find_subtitle_image_asset_references_in_text(read_text_file_or_throw(subtitle_path));
    } catch (const std::exception &exception) {
        return make_asset_error(
            {},
            {},
            "Could not inspect subtitle image asset references.",
            exception.what()
        );
    }

    diagnostics.push_back(
        "Subtitle image asset references detected: " + std::to_string(references.size())
    );
    if (references.empty()) {
        return SubtitleImageAssetLoadResult{
            .references = {},
            .assets = {},
            .diagnostics = std::move(diagnostics),
            .error = std::nullopt
        };
    }

    const auto subtitle_directory = subtitle_path.parent_path();
    std::vector<SubtitleImageAsset> assets{};
    assets.reserve(references.size());
    for (const auto &reference : references) {
        std::string resolve_error{};
        const auto resolved_path = resolve_asset_path(subtitle_directory, reference.name, &resolve_error);
        if (!resolved_path.has_value()) {
            return make_asset_error(
                references,
                std::move(diagnostics),
                resolve_error,
                "Place the referenced image beside the .ass file or in an assets/images/img sidecar folder."
            );
        }

        const auto format = format_from_extension(*resolved_path);
        if (!format.has_value()) {
            return make_asset_error(
                references,
                std::move(diagnostics),
                "Unsupported subtitle image asset format: " + resolved_path->lexically_normal().string(),
                "Use PNG, JPEG, or WebP subtitle image assets."
            );
        }

        auto frame_result = media::MediaDecoder::decode_video_frame_at_time(
            *resolved_path,
            0,
            media::DecodeNormalizationPolicy{
                .video_pixel_format = media::NormalizedVideoPixelFormat::rgba8,
                .audio_sample_format = media::NormalizedAudioSampleFormat::f32_planar,
                .audio_block_samples = 1024,
                .video_max_width = 0,
                .video_max_height = 0
            }
        );
        if (!frame_result.succeeded()) {
            return make_asset_error(
                references,
                std::move(diagnostics),
                "Failed to decode subtitle image asset: " + reference.name,
                frame_result.error.has_value() ? frame_result.error->message : "FFmpeg could not decode the image."
            );
        }

        const auto validation_error = validate_decoded_asset_frame(reference.name, *resolved_path, *frame_result.video_frame);
        if (validation_error.has_value()) {
            return make_asset_error(
                references,
                std::move(diagnostics),
                validation_error->message,
                validation_error->actionable_hint
            );
        }

        const auto &frame = *frame_result.video_frame;
        const auto &plane = frame.planes[0];
        diagnostics.push_back(
            "Subtitle image asset loaded: " + reference.name + " -> " +
            resolved_path->lexically_normal().string() + " (" +
            std::to_string(frame.width) + "x" + std::to_string(frame.height) + ")"
        );
        assets.push_back(SubtitleImageAsset{
            .name = reference.name,
            .source_path = *resolved_path,
            .format = *format,
            .width = frame.width,
            .height = frame.height,
            .stride = plane.line_stride_bytes,
            .rgba = plane.bytes
        });
    }

    return SubtitleImageAssetLoadResult{
        .references = std::move(references),
        .assets = std::move(assets),
        .diagnostics = std::move(diagnostics),
        .error = std::nullopt
    };
}

const char *to_string(const SubtitleImageAssetFormat format) noexcept {
    switch (format) {
    case SubtitleImageAssetFormat::png:
        return "png";
    case SubtitleImageAssetFormat::jpeg:
        return "jpeg";
    case SubtitleImageAssetFormat::webp:
        return "webp";
    }
    return "unknown";
}

}  // namespace utsure::core::subtitles
