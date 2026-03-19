#include "utsure/core/subtitles/subtitle_renderer.hpp"

extern "C" {
#include <ass/ass.h>
}

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
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

enum class SessionRenderMode : std::uint8_t {
    legacy_ass_image = 0,
    auto_rgba
};

struct ScriptFeatureScan final {
    bool references_tag_images{false};
};

struct LoadedTrack final {
    TrackHandle track{};
    SessionRenderMode render_mode{SessionRenderMode::legacy_ass_image};
};

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

ScriptFeatureScan scan_script_features(const std::filesystem::path &subtitle_path) {
    std::ifstream stream(subtitle_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(
            "Failed to open subtitle script '" + subtitle_path.lexically_normal().string() +
            "' for libassmod feature scanning."
        );
    }

    std::string script_text{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
    std::transform(script_text.begin(), script_text.end(), script_text.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });

    const bool references_tag_images =
        script_text.find("\\img(") != std::string::npos ||
        script_text.find("\\1img(") != std::string::npos ||
        script_text.find("\\2img(") != std::string::npos ||
        script_text.find("\\3img(") != std::string::npos ||
        script_text.find("\\4img(") != std::string::npos;

    return ScriptFeatureScan{
        .references_tag_images = references_tag_images
    };
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

LoadedTrack load_track(
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

    const auto render_mode = ass_track_has_rgba(track.get()) != 0
        ? SessionRenderMode::auto_rgba
        : SessionRenderMode::legacy_ass_image;

    return LoadedTrack{
        .track = std::move(track),
        .render_mode = render_mode
    };
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

std::optional<SubtitleBitmap> convert_ass_image_rgba(const ASS_ImageRGBA &image) {
    if (image.w <= 0 || image.h <= 0 || image.rgba == nullptr || image.stride < (image.w * 4)) {
        return std::nullopt;
    }

    const int line_stride_bytes = image.w * 4;
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(line_stride_bytes) * static_cast<std::size_t>(image.h),
        0U
    );

    for (int row = 0; row < image.h; ++row) {
        const auto *source_row = image.rgba + static_cast<std::size_t>(row) * static_cast<std::size_t>(image.stride);
        auto *destination_row = bytes.data() +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(line_stride_bytes);
        std::copy_n(source_row, line_stride_bytes, destination_row);
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

std::vector<SubtitleBitmap> convert_ass_image_list(ASS_Image *images) {
    std::vector<SubtitleBitmap> bitmaps{};
    for (ASS_Image *image = images; image != nullptr; image = image->next) {
        const auto bitmap = convert_ass_image(*image);
        if (bitmap.has_value()) {
            bitmaps.push_back(*bitmap);
        }
    }

    return bitmaps;
}

std::vector<SubtitleBitmap> convert_ass_image_rgba_list(ASS_ImageRGBA *images) {
    std::vector<SubtitleBitmap> bitmaps{};
    for (ASS_ImageRGBA *image = images; image != nullptr; image = image->next) {
        const auto bitmap = convert_ass_image_rgba(*image);
        if (bitmap.has_value()) {
            bitmaps.push_back(*bitmap);
        }
    }

    return bitmaps;
}

class LibassmodSubtitleRenderSession final : public SubtitleRenderSession {
public:
    LibassmodSubtitleRenderSession(
        SubtitleRenderSessionCreateRequest create_request,
        std::string subtitle_path_string,
        SessionRenderMode render_mode,
        LibraryHandle library,
        RendererHandle renderer,
        TrackHandle track
    )
        : create_request_(std::move(create_request)),
          subtitle_path_string_(std::move(subtitle_path_string)),
          render_mode_(render_mode),
          library_(std::move(library)),
          renderer_(std::move(renderer)),
          track_(std::move(track)) {
    }

    ~LibassmodSubtitleRenderSession() override {
        if (renderer_) {
            ass_clear_tag_images(renderer_.get());
        }
    }

    [[nodiscard]] SubtitleRenderResult render(const SubtitleRenderRequest &request) noexcept override {
        try {
            int detect_change = 0;
            const long long timestamp_milliseconds = static_cast<long long>(request.timestamp_microseconds / 1000);
            std::vector<SubtitleBitmap> bitmaps{};
            // Keep plain ASS scripts on the legacy list path, but let libassmod switch RGBA-capable
            // tracks to premultiplied tiles on the frames that actually require them.
            if (render_mode_ == SessionRenderMode::legacy_ass_image) {
                ASS_Image *images =
                    ass_render_frame(renderer_.get(), track_.get(), timestamp_milliseconds, &detect_change);
                bitmaps = convert_ass_image_list(images);
            } else {
                ASS_RenderResult render_result =
                    ass_render_frame_auto(renderer_.get(), track_.get(), timestamp_milliseconds, &detect_change);
                const bool frame_requires_rgba =
                    render_result.use_rgba != 0 ||
                    (ass_frame_needs_rgba(renderer_.get()) != 0 && render_result.imgs_rgba != nullptr);
                bitmaps = frame_requires_rgba
                    ? convert_ass_image_rgba_list(render_result.imgs_rgba)
                    : convert_ass_image_list(render_result.imgs);
                ass_render_result_free(&render_result);
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
    SessionRenderMode render_mode_{SessionRenderMode::legacy_ass_image};
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
            const auto script_feature_scan = scan_script_features(normalized_path);
            if (script_feature_scan.references_tag_images) {
                return make_session_error(
                    request,
                    "The subtitle script uses libassmod \\img tags, but this build does not register host-side RGBA "
                    "image resources.",
                    "Remove \\img usage or add the libassmod tag-image registration path before burn-in."
                );
            }

            auto loaded_track = load_track(*library, request);

            return SubtitleRenderSessionResult{
                .session = std::make_unique<LibassmodSubtitleRenderSession>(
                    request,
                    normalized_path.string(),
                    loaded_track.render_mode,
                    std::move(library),
                    std::move(renderer),
                    std::move(loaded_track.track)
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
