#include "utsure/core/subtitles/subtitle_renderer.hpp"

extern "C" {
#include <ass/ass.h>
}

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace utsure::core::subtitles {

namespace {

struct LibraryDeleter final {
    void operator()(ASS_Library *library) const noexcept {
        if (library != nullptr) {
            ass_library_done(library);
        }
    }
};

using LibraryHandle = std::unique_ptr<ASS_Library, LibraryDeleter>;

struct RendererDeleter final {
    void operator()(ASS_Renderer *renderer) const noexcept {
        if (renderer != nullptr) {
            ass_renderer_done(renderer);
        }
    }
};

using RendererHandle = std::unique_ptr<ASS_Renderer, RendererDeleter>;

struct TrackDeleter final {
    void operator()(ASS_Track *track) const noexcept {
        if (track != nullptr) {
            ass_free_track(track);
        }
    }
};

using TrackHandle = std::unique_ptr<ASS_Track, TrackDeleter>;

std::string path_to_utf8_string(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto normalized = path.lexically_normal().u8string();
    return std::string(reinterpret_cast<const char *>(normalized.c_str()), normalized.size());
#else
    return path.lexically_normal().string();
#endif
}

SubtitleRenderSessionResult make_session_error(
    const SubtitleRenderSessionCreateRequest &request,
    std::string message,
    std::string actionable_hint
) {
    return SubtitleRenderSessionResult{
        .session = nullptr,
        .error = SubtitleRendererError{
            .subtitle_path = request.subtitle_path.lexically_normal().string(),
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

SubtitleRenderResult make_render_error(
    const std::string &subtitle_path,
    std::string message,
    std::string actionable_hint
) {
    return SubtitleRenderResult{
        .rendered_frame = std::nullopt,
        .error = SubtitleRendererError{
            .subtitle_path = subtitle_path,
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

bool is_supported_format_hint(const std::string &format_hint) {
    return format_hint == "auto" || format_hint == "ass" || format_hint == "ssa";
}

double choose_pixel_aspect_ratio(const media::Rational &sample_aspect_ratio) {
    if (!sample_aspect_ratio.is_valid() || sample_aspect_ratio.numerator <= 0 || sample_aspect_ratio.denominator <= 0) {
        return 1.0;
    }

    return static_cast<double>(sample_aspect_ratio.numerator) /
        static_cast<double>(sample_aspect_ratio.denominator);
}

LibraryHandle create_library() {
    LibraryHandle library(ass_library_init());
    if (!library) {
        throw std::runtime_error("libassmod failed to initialize the subtitle library.");
    }

    ass_set_extract_fonts(library.get(), 1);
    return library;
}

RendererHandle create_renderer(
    ASS_Library &library,
    const SubtitleRenderSessionCreateRequest &request
) {
    RendererHandle renderer(ass_renderer_init(&library));
    if (!renderer) {
        throw std::runtime_error("libassmod failed to initialize the subtitle renderer.");
    }

    ass_set_frame_size(renderer.get(), request.canvas_width, request.canvas_height);
    ass_set_storage_size(renderer.get(), request.canvas_width, request.canvas_height);
    ass_set_pixel_aspect(renderer.get(), choose_pixel_aspect_ratio(request.sample_aspect_ratio));
    ass_set_margins(renderer.get(), 0, 0, 0, 0);
    ass_set_use_margins(renderer.get(), 0);
    ass_set_fonts(renderer.get(), nullptr, "Arial", ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);

    return renderer;
}

TrackHandle load_track(
    ASS_Library &library,
    const SubtitleRenderSessionCreateRequest &request
) {
    const auto subtitle_path_utf8 = path_to_utf8_string(request.subtitle_path);
    TrackHandle track(ass_read_file(&library, subtitle_path_utf8.c_str(), nullptr));
    if (!track) {
        throw std::runtime_error(
            "libassmod failed to parse subtitle script '" + request.subtitle_path.lexically_normal().string() + "'."
        );
    }

    return track;
}

std::optional<SubtitleBitmap> convert_ass_image(const ASS_Image &image) {
    if (image.w <= 0 || image.h <= 0 || image.bitmap == nullptr) {
        return std::nullopt;
    }

    const std::uint8_t red = static_cast<std::uint8_t>((image.color >> 24) & 0xFFU);
    const std::uint8_t green = static_cast<std::uint8_t>((image.color >> 16) & 0xFFU);
    const std::uint8_t blue = static_cast<std::uint8_t>((image.color >> 8) & 0xFFU);
    const std::uint8_t transparency = static_cast<std::uint8_t>(image.color & 0xFFU);
    const std::uint8_t opacity = static_cast<std::uint8_t>(255U - transparency);

    const int line_stride_bytes = image.w * 4;
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(line_stride_bytes) * static_cast<std::size_t>(image.h),
        0U
    );

    for (int row = 0; row < image.h; ++row) {
        const auto *source_row = image.bitmap + static_cast<std::size_t>(row) * static_cast<std::size_t>(image.stride);
        auto *destination_row = bytes.data() +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(line_stride_bytes);

        for (int column = 0; column < image.w; ++column) {
            const std::uint8_t mask_alpha = source_row[column];
            const std::uint8_t final_alpha = static_cast<std::uint8_t>(
                (static_cast<std::uint16_t>(mask_alpha) * static_cast<std::uint16_t>(opacity) + 127U) / 255U
            );
            const auto destination_offset = static_cast<std::size_t>(column) * 4U;

            destination_row[destination_offset + 0U] = static_cast<std::uint8_t>(
                (static_cast<std::uint16_t>(red) * static_cast<std::uint16_t>(final_alpha) + 127U) / 255U
            );
            destination_row[destination_offset + 1U] = static_cast<std::uint8_t>(
                (static_cast<std::uint16_t>(green) * static_cast<std::uint16_t>(final_alpha) + 127U) / 255U
            );
            destination_row[destination_offset + 2U] = static_cast<std::uint8_t>(
                (static_cast<std::uint16_t>(blue) * static_cast<std::uint16_t>(final_alpha) + 127U) / 255U
            );
            destination_row[destination_offset + 3U] = final_alpha;
        }
    }

    return SubtitleBitmap{
        .origin_x = image.dst_x,
        .origin_y = image.dst_y,
        .width = image.w,
        .height = image.h,
        .pixel_format = SubtitleBitmapPixelFormat::rgba8_premultiplied,
        .line_stride_bytes = line_stride_bytes,
        .bytes = std::move(bytes)
    };
}

class LibassmodSubtitleRenderSession final : public SubtitleRenderSession {
public:
    LibassmodSubtitleRenderSession(
        SubtitleRenderSessionCreateRequest create_request,
        std::string subtitle_path_string,
        LibraryHandle library,
        RendererHandle renderer,
        TrackHandle track
    )
        : create_request_(std::move(create_request)),
          subtitle_path_string_(std::move(subtitle_path_string)),
          library_(std::move(library)),
          renderer_(std::move(renderer)),
          track_(std::move(track)) {
    }

    [[nodiscard]] SubtitleRenderResult render(const SubtitleRenderRequest &request) noexcept override {
        try {
            int detect_change = 0;
            const long long timestamp_milliseconds = static_cast<long long>(request.timestamp_microseconds / 1000);
            ASS_Image *images = ass_render_frame(renderer_.get(), track_.get(), timestamp_milliseconds, &detect_change);

            std::vector<SubtitleBitmap> bitmaps{};
            for (ASS_Image *image = images; image != nullptr; image = image->next) {
                const auto bitmap = convert_ass_image(*image);
                if (bitmap.has_value()) {
                    bitmaps.push_back(*bitmap);
                }
            }

            return SubtitleRenderResult{
                .rendered_frame = RenderedSubtitleFrame{
                    .timestamp_microseconds = request.timestamp_microseconds,
                    .canvas_width = create_request_.canvas_width,
                    .canvas_height = create_request_.canvas_height,
                    .bitmaps = std::move(bitmaps)
                },
                .error = std::nullopt
            };
        } catch (const std::exception &exception) {
            return make_render_error(
                subtitle_path_string_,
                "libassmod subtitle rendering aborted because an unexpected exception was raised.",
                exception.what()
            );
        }
    }

private:
    SubtitleRenderSessionCreateRequest create_request_{};
    std::string subtitle_path_string_{};
    LibraryHandle library_{};
    RendererHandle renderer_{};
    TrackHandle track_{};
};

class LibassmodSubtitleRenderer final : public SubtitleRenderer {
public:
    [[nodiscard]] SubtitleRenderSessionResult create_session(
        const SubtitleRenderSessionCreateRequest &request
    ) noexcept override {
        try {
            if (request.subtitle_path.empty()) {
                return make_session_error(
                    request,
                    "Cannot create a subtitle render session because no subtitle file path was provided.",
                    "Provide an ASS or SSA subtitle file path before starting subtitle burn-in."
                );
            }

            if (!is_supported_format_hint(request.format_hint)) {
                return make_session_error(
                    request,
                    "Unsupported subtitle format hint '" + request.format_hint + "' was requested.",
                    "Use 'ass', 'ssa', or 'auto' for the current libassmod-backed renderer."
                );
            }

            if (request.canvas_width <= 0 || request.canvas_height <= 0) {
                return make_session_error(
                    request,
                    "Cannot create a subtitle render session because the target canvas size is invalid.",
                    "Provide the decoded output frame size before creating a subtitle render session."
                );
            }

            std::error_code filesystem_error;
            const auto normalized_path = request.subtitle_path.lexically_normal();
            const bool subtitle_exists = std::filesystem::exists(normalized_path, filesystem_error);
            if (filesystem_error) {
                return make_session_error(
                    request,
                    "Cannot create a subtitle render session because the subtitle file system path could not be queried.",
                    "The operating system reported: " + filesystem_error.message()
                );
            }

            if (!subtitle_exists) {
                return make_session_error(
                    request,
                    "Cannot create a subtitle render session because the subtitle file does not exist.",
                    "Check that the ASS subtitle path is correct before starting burn-in."
                );
            }

            auto library = create_library();
            auto renderer = create_renderer(*library, request);
            auto track = load_track(*library, request);

            return SubtitleRenderSessionResult{
                .session = std::make_unique<LibassmodSubtitleRenderSession>(
                    request,
                    normalized_path.string(),
                    std::move(library),
                    std::move(renderer),
                    std::move(track)
                ),
                .error = std::nullopt
            };
        } catch (const std::exception &exception) {
            return make_session_error(
                request,
                "libassmod subtitle session creation aborted because an unexpected exception was raised.",
                exception.what()
            );
        }
    }
};

}  // namespace

std::unique_ptr<SubtitleRenderer> create_default_subtitle_renderer() {
    return std::make_unique<LibassmodSubtitleRenderer>();
}

}  // namespace utsure::core::subtitles
