extern "C" {
#include <ass/ass.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef UTSURE_LIBASSMOD_REF
#define UTSURE_LIBASSMOD_REF "unknown"
#endif

namespace {

struct LibraryDeleter final {
    void operator()(ASS_Library *library) const noexcept {
        if (library != nullptr) {
            ass_library_done(library);
        }
    }
};

struct RendererDeleter final {
    void operator()(ASS_Renderer *renderer) const noexcept {
        if (renderer != nullptr) {
            ass_renderer_done(renderer);
        }
    }
};

struct TrackDeleter final {
    void operator()(ASS_Track *track) const noexcept {
        if (track != nullptr) {
            ass_free_track(track);
        }
    }
};

using LibraryHandle = std::unique_ptr<ASS_Library, LibraryDeleter>;
using RendererHandle = std::unique_ptr<ASS_Renderer, RendererDeleter>;
using TrackHandle = std::unique_ptr<ASS_Track, TrackDeleter>;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

std::string path_to_utf8_string(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto normalized = path.lexically_normal().u8string();
    return std::string(reinterpret_cast<const char *>(normalized.c_str()), normalized.size());
#else
    return path.lexically_normal().string();
#endif
}

int parse_int(std::string_view value, std::string_view label) {
    std::size_t parsed = 0;
    int result = 0;
    try {
        result = std::stoi(std::string(value), &parsed, 10);
    } catch (const std::exception &) {
        throw std::runtime_error("Invalid integer for " + std::string(label) + ": " + std::string(value));
    }

    if (parsed != value.size()) {
        throw std::runtime_error("Invalid integer for " + std::string(label) + ": " + std::string(value));
    }

    return result;
}

long long parse_long_long(std::string_view value, std::string_view label) {
    std::size_t parsed = 0;
    long long result = 0;
    try {
        result = std::stoll(std::string(value), &parsed, 10);
    } catch (const std::exception &) {
        throw std::runtime_error("Invalid integer for " + std::string(label) + ": " + std::string(value));
    }

    if (parsed != value.size()) {
        throw std::runtime_error("Invalid integer for " + std::string(label) + ": " + std::string(value));
    }

    return result;
}

int repeat_count_from_environment() {
    const char *value = std::getenv("UTSURE_LIBASSMOD_REPRO_REPEAT");
    if (value == nullptr || value[0] == '\0') {
        return 1;
    }

    return std::max(1, parse_int(value, "UTSURE_LIBASSMOD_REPRO_REPEAT"));
}

std::optional<std::uint64_t> estimate_rgba_bytes(const ASS_ImageRGBA &image) noexcept {
    if (image.w <= 0 || image.h <= 0 || image.stride <= 0) {
        return std::nullopt;
    }

    const auto minimum_stride = static_cast<std::int64_t>(image.w) * 4LL;
    if (minimum_stride <= 0 || static_cast<std::int64_t>(image.stride) < minimum_stride) {
        return std::nullopt;
    }

    const auto stride = static_cast<std::uint64_t>(image.stride);
    const auto height = static_cast<std::uint64_t>(image.h);
    if (height != 0U && stride > (std::numeric_limits<std::uint64_t>::max() / height)) {
        return std::nullopt;
    }

    return stride * height;
}

std::optional<std::uint64_t> sum_alpha(const ASS_ImageRGBA &image) noexcept {
    constexpr std::uint64_t kMaximumDiagnosticAlphaScanBytes = 512ULL * 1024ULL * 1024ULL;

    const auto estimated_bytes = estimate_rgba_bytes(image);
    if (!estimated_bytes.has_value() || *estimated_bytes > kMaximumDiagnosticAlphaScanBytes ||
        image.rgba == nullptr) {
        return std::nullopt;
    }

    std::uint64_t alpha_sum = 0;
    for (int row = 0; row < image.h; ++row) {
        const auto *source_row = image.rgba +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(image.stride);
        for (int column = 0; column < image.w; ++column) {
            alpha_sum += source_row[static_cast<std::size_t>(column) * 4U + 3U];
        }
    }

    return alpha_sum;
}

void log_rgba_node(const ASS_ImageRGBA &image, const std::size_t index) {
    const auto alpha_sum = sum_alpha(image);
    const auto estimated_bytes = estimate_rgba_bytes(image);

    std::cout << "rgba_node[" << index << "]"
              << ".type=" << image.type
              << ", dst_x=" << image.dst_x
              << ", dst_y=" << image.dst_y
              << ", w=" << image.w
              << ", h=" << image.h
              << ", stride=" << image.stride
              << ", rgba=" << static_cast<const void *>(image.rgba)
              << ", alpha_sum=";
    if (alpha_sum.has_value()) {
        std::cout << *alpha_sum;
    } else {
        std::cout << "unavailable";
    }

    std::cout << ", estimated_bytes=";
    if (estimated_bytes.has_value()) {
        std::cout << *estimated_bytes;
    } else {
        std::cout << "unavailable";
    }
    std::cout << '\n';
}

int run_rgba_reproducer(
    const std::filesystem::path &subtitle_path,
    const int frame_width,
    const int frame_height,
    const int sar_numerator,
    const int sar_denominator,
    const long long timestamp_microseconds,
    const std::optional<std::filesystem::path> &font_directory
) {
    if (frame_width <= 0 || frame_height <= 0) {
        return fail("Frame size must be positive.");
    }

    if (sar_numerator <= 0 || sar_denominator <= 0) {
        return fail("Sample aspect ratio must be positive.");
    }

    LibraryHandle library(ass_library_init());
    if (!library) {
        return fail("ass_library_init failed.");
    }

    ass_set_extract_fonts(library.get(), 1);
    if (font_directory.has_value()) {
        const auto font_directory_utf8 = path_to_utf8_string(*font_directory);
        ass_set_fonts_dir(library.get(), font_directory_utf8.c_str());
    }

    RendererHandle renderer(ass_renderer_init(library.get()));
    if (!renderer) {
        return fail("ass_renderer_init failed.");
    }

    const double pixel_aspect =
        static_cast<double>(sar_numerator) / static_cast<double>(sar_denominator);
    ass_set_frame_size(renderer.get(), frame_width, frame_height);
    ass_set_storage_size(renderer.get(), frame_width, frame_height);
    ass_set_pixel_aspect(renderer.get(), pixel_aspect);
    ass_set_margins(renderer.get(), 0, 0, 0, 0);
    ass_set_use_margins(renderer.get(), 0);
    ass_set_fonts(renderer.get(), nullptr, "Arial", ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);

    const auto subtitle_path_utf8 = path_to_utf8_string(subtitle_path);
    TrackHandle track(ass_read_file(library.get(), subtitle_path_utf8.c_str(), nullptr));
    if (!track) {
        return fail("ass_read_file failed for subtitle input.");
    }

    std::cout << "libassmod.ref=" << UTSURE_LIBASSMOD_REF << '\n';
    std::cout << "input.subtitle=" << subtitle_path.lexically_normal().string() << '\n';
    std::cout << "setup.frame_size=" << frame_width << 'x' << frame_height << '\n';
    std::cout << "setup.storage_size=" << frame_width << 'x' << frame_height << '\n';
    std::cout << "setup.pixel_aspect=" << pixel_aspect << '\n';
    std::cout << "setup.margins=0,0,0,0\n";
    std::cout << "setup.use_margins=0\n";
    std::cout << "setup.font.default_family=Arial\n";
    std::cout << "setup.font.provider=autodetect\n";
    if (font_directory.has_value()) {
        std::cout << "setup.font.directory=" << font_directory->lexically_normal().string() << '\n';
    } else {
        std::cout << "setup.font.directory=none\n";
    }

    const int repeat_count = repeat_count_from_environment();
    const long long timestamp_milliseconds = timestamp_microseconds / 1000LL;
    std::size_t last_node_count = 0;
    int last_detect_change = 0;
    for (int iteration = 0; iteration < repeat_count; ++iteration) {
        int detect_change = 0;
        ASS_ImageRGBA *images = ass_render_frame_rgba(
            renderer.get(),
            track.get(),
            timestamp_milliseconds,
            &detect_change
        );

        std::size_t node_count = 0;
        for (ASS_ImageRGBA *image = images; image != nullptr; image = image->next) {
            if (iteration == 0 || iteration + 1 == repeat_count) {
                log_rgba_node(*image, node_count);
            }
            ++node_count;
        }

        last_node_count = node_count;
        last_detect_change = detect_change;
        ass_free_images_rgba(images);
    }

    std::cout << "render.timestamp_us=" << timestamp_microseconds << '\n';
    std::cout << "render.timestamp_ms=" << timestamp_milliseconds << '\n';
    std::cout << "render.detect_change=" << last_detect_change << '\n';
    std::cout << "render.node_count=" << last_node_count << '\n';
    std::cout << "render.repeat_count=" << repeat_count << '\n';
    std::cout << "free.completed=yes\n";
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 8 && argc != 9) {
        return fail(
            "Usage: utsure_core_libassmod_rgba_reproducer "
            "--rgba <subtitle> <frame-width> <frame-height> <sar-num> <sar-den> <timestamp-us> [font-dir]"
        );
    }

    try {
        const std::string_view mode(argv[1]);
        if (mode != "--rgba") {
            return fail("Unknown mode for utsure_core_libassmod_rgba_reproducer.");
        }

        return run_rgba_reproducer(
            std::filesystem::path(argv[2]),
            parse_int(argv[3], "frame-width"),
            parse_int(argv[4], "frame-height"),
            parse_int(argv[5], "sar-num"),
            parse_int(argv[6], "sar-den"),
            parse_long_long(argv[7], "timestamp-us"),
            argc == 9 ? std::optional<std::filesystem::path>(std::filesystem::path(argv[8])) : std::nullopt
        );
    } catch (const std::exception &exception) {
        return fail(exception.what());
    }
}
