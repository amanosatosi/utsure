#include "utsure/core/subtitles/subtitle_renderer.hpp"
#include "../../subtitles/subtitle_bitmap_compositor.hpp"
#include "../../subtitles/subtitle_composition_diagnostics.hpp"
#include "../../subtitles/subtitle_runtime_options.hpp"

extern "C" {
#include <ass/ass.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
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

struct ImageRgbaListDeleter final {
    void operator()(ASS_ImageRGBA *images) const noexcept {
        if (images != nullptr) {
            ass_free_images_rgba(images);
        }
    }
};

using ImageRgbaListHandle = std::unique_ptr<ASS_ImageRGBA, ImageRgbaListDeleter>;

struct ScriptFeatureScan final {
    bool references_tag_images{false};
};

enum class AssImageRgbaValidationResult : std::uint8_t {
    empty = 0,
    drawable
};

struct DrawableAssImageRgba final {
    std::size_t bitmap_index{0};
    const ASS_ImageRGBA *image{nullptr};
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

SubtitleFrameComposeResult make_compose_error(std::string message, std::string actionable_hint) {
    return SubtitleFrameComposeResult{
        .subtitles_applied = false,
        .error = SubtitleFrameComposeError{
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

void configure_library_fonts(
    ASS_Library &library,
    const SubtitleRenderSessionCreateRequest &request
) {
    if (!request.font_search_directory.has_value()) {
        return;
    }

    const auto font_directory_utf8 = path_to_utf8_string(*request.font_search_directory);
    ass_set_fonts_dir(&library, font_directory_utf8.c_str());
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

[[nodiscard]] AssImageRgbaValidationResult validate_ass_image_rgba(
    const ASS_ImageRGBA &image,
    const std::size_t bitmap_index,
    const std::string &subtitle_path_string,
    const int session_instance_id
) {
    if (image.w <= 0 || image.h <= 0) {
        return AssImageRgbaValidationResult::empty;
    }

    const auto minimum_stride = static_cast<std::int64_t>(image.w) * 4LL;
    if (minimum_stride > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        std::ostringstream message;
        message << "libassmod produced a subtitle bitmap whose row size overflowed the host stride range"
                << " for '" << subtitle_path_string << "'"
                << " (session " << session_instance_id << ", bitmap " << bitmap_index << "): origin="
                << image.dst_x << ',' << image.dst_y
                << ", width=" << image.w << ", height=" << image.h << '.';
        throw std::runtime_error(message.str());
    }

    if (image.stride <= 0 || static_cast<std::int64_t>(image.stride) < minimum_stride) {
        std::ostringstream message;
        message << "libassmod produced an invalid RGBA subtitle bitmap stride"
                << " for '" << subtitle_path_string << "'"
                << " (session " << session_instance_id << ", bitmap " << bitmap_index << "): origin="
                << image.dst_x << ',' << image.dst_y
                << ", width=" << image.w << ", height=" << image.h
                << ", stride=" << image.stride << '.';
        throw std::runtime_error(message.str());
    }

    if (image.rgba == nullptr) {
        std::ostringstream message;
        message << "libassmod produced a subtitle bitmap with null RGBA bytes"
                << " for '" << subtitle_path_string << "'"
                << " (session " << session_instance_id << ", bitmap " << bitmap_index << "): origin="
                << image.dst_x << ',' << image.dst_y
                << ", width=" << image.w << ", height=" << image.h
                << ", stride=" << image.stride << '.';
        throw std::runtime_error(message.str());
    }

    return AssImageRgbaValidationResult::drawable;
}

SubtitleBitmap copy_ass_image_rgba(const ASS_ImageRGBA &image) {
    const int line_stride_bytes = image.w * 4;
    std::vector<std::uint8_t> bytes(detail::required_rgba_buffer_size(
        image.w,
        image.h,
        line_stride_bytes,
        "bitmap"
    ), 0U);

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

detail::PremultipliedRgbaBitmapView make_ass_image_rgba_view(const ASS_ImageRGBA &image) {
    return detail::PremultipliedRgbaBitmapView{
        .origin_x = image.dst_x,
        .origin_y = image.dst_y,
        .width = image.w,
        .height = image.h,
        .line_stride_bytes = image.stride,
        .bytes = image.rgba
    };
}

std::vector<ASS_ImageRGBA *> collect_ass_image_rgba_nodes(ASS_ImageRGBA *images) {
    std::vector<ASS_ImageRGBA *> bitmaps{};
    for (ASS_ImageRGBA *image = images; image != nullptr; image = image->next) {
        bitmaps.push_back(image);
    }

    return bitmaps;
}

std::vector<DrawableAssImageRgba> collect_drawable_ass_image_rgba_nodes(
    const std::vector<ASS_ImageRGBA *> &image_nodes,
    const SubtitleRenderRequest &request,
    const std::string_view bitmap_mode,
    const std::string &subtitle_path_string,
    const int session_instance_id
) {
    std::vector<DrawableAssImageRgba> drawable_bitmaps{};
    drawable_bitmaps.reserve(image_nodes.size());
    for (std::size_t bitmap_index = 0; bitmap_index < image_nodes.size(); ++bitmap_index) {
        const ASS_ImageRGBA &image = *image_nodes[bitmap_index];
        const auto validation_result = validate_ass_image_rgba(
            image,
            bitmap_index,
            subtitle_path_string,
            session_instance_id
        );
        if (validation_result == AssImageRgbaValidationResult::empty) {
            detail::maybe_log_skipped_empty_subtitle_bitmap_diagnostics(
                request,
                bitmap_index,
                image.dst_x,
                image.dst_y,
                image.w,
                image.h,
                image.stride,
                bitmap_mode
            );
            continue;
        }

        drawable_bitmaps.push_back(DrawableAssImageRgba{
            .bitmap_index = bitmap_index,
            .image = &image
        });
    }

    return drawable_bitmaps;
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
          track_(std::move(track)),
          runtime_options_(runtime::resolve_subtitle_runtime_options()),
          session_instance_id_(next_session_instance_id()) {
    }

    ~LibassmodSubtitleRenderSession() override {
        destroyed_.store(true, std::memory_order_release);
        if (renderer_) {
            ass_clear_tag_images(renderer_.get());
        }
    }

    [[nodiscard]] SubtitleRenderResult render(const SubtitleRenderRequest &request) noexcept override {
        try {
            [[maybe_unused]] const auto access_guard = begin_session_access("render");
            auto images_rgba = render_images_rgba(request);
            const auto image_nodes = collect_ass_image_rgba_nodes(images_rgba.get());
            const auto drawable_image_nodes = collect_drawable_ass_image_rgba_nodes(
                image_nodes,
                request,
                "copied",
                subtitle_path_string_,
                session_instance_id_
            );
            std::vector<SubtitleBitmap> bitmaps{};
            bitmaps.reserve(drawable_image_nodes.size());
            for (const auto &drawable_image : drawable_image_nodes) {
                if (drawable_image.image == nullptr) {
                    continue;
                }

                const ASS_ImageRGBA &bitmap = *drawable_image.image;
                bitmaps.push_back(copy_ass_image_rgba(bitmap));
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

    [[nodiscard]] SubtitleFrameComposeResult compose_into_frame(
        media::DecodedVideoFrame &video_frame,
        const SubtitleRenderRequest &request
    ) noexcept override {
        try {
            [[maybe_unused]] const auto access_guard = begin_session_access("compose");
            detail::validate_rgba_frame_surface(video_frame, "Subtitle composition");

            auto images_rgba = render_images_rgba(request);
            const auto image_nodes = collect_ass_image_rgba_nodes(images_rgba.get());
            const auto drawable_image_nodes = collect_drawable_ass_image_rgba_nodes(
                image_nodes,
                request,
                runtime::to_string(runtime_options_.bitmap_transfer_mode),
                subtitle_path_string_,
                session_instance_id_
            );
            detail::maybe_log_subtitle_frame_diagnostics(
                request,
                video_frame,
                drawable_image_nodes.size(),
                runtime::to_string(runtime_options_.bitmap_transfer_mode)
            );
            bool subtitles_applied = false;
            for (const auto &drawable_image : drawable_image_nodes) {
                if (drawable_image.image == nullptr) {
                    continue;
                }

                const ASS_ImageRGBA &image = *drawable_image.image;
                detail::maybe_log_subtitle_bitmap_diagnostics(
                    request,
                    drawable_image.bitmap_index,
                    image.dst_x,
                    image.dst_y,
                    image.w,
                    image.h,
                    image.stride,
                    runtime::to_string(runtime_options_.bitmap_transfer_mode)
                );

                if (runtime_options_.bitmap_transfer_mode == runtime::SubtitleBitmapTransferMode::direct) {
                    detail::composite_premultiplied_rgba_bitmap_into_frame(
                        video_frame,
                        make_ass_image_rgba_view(image)
                    );
                } else {
                    detail::composite_bitmap_into_frame(video_frame, copy_ass_image_rgba(image));
                }
                subtitles_applied = true;
            }

            return SubtitleFrameComposeResult{
                .subtitles_applied = subtitles_applied,
                .error = std::nullopt
            };
        } catch (const std::exception &exception) {
            return make_compose_error(
                "libassmod subtitle composition aborted because an unexpected exception was raised.",
                std::string(exception.what()) + " Context: " +
                    detail::format_subtitle_frame_diagnostics(
                        request,
                        video_frame,
                        0U,
                        runtime::to_string(runtime_options_.bitmap_transfer_mode)
                    )
            );
        }
    }

private:
    class SessionAccessGuard final {
    public:
        explicit SessionAccessGuard(std::atomic<bool> &in_use) noexcept
            : in_use_(&in_use) {
        }

        SessionAccessGuard(const SessionAccessGuard &) = delete;
        SessionAccessGuard &operator=(const SessionAccessGuard &) = delete;

        SessionAccessGuard(SessionAccessGuard &&other) noexcept
            : in_use_(std::exchange(other.in_use_, nullptr)) {
        }

        SessionAccessGuard &operator=(SessionAccessGuard &&other) noexcept {
            if (this == &other) {
                return *this;
            }

            release();
            in_use_ = std::exchange(other.in_use_, nullptr);
            return *this;
        }

        ~SessionAccessGuard() {
            release();
        }

    private:
        void release() noexcept {
            if (in_use_ != nullptr) {
                in_use_->store(false, std::memory_order_release);
                in_use_ = nullptr;
            }
        }

        std::atomic<bool> *in_use_{nullptr};
    };

    [[nodiscard]] static int next_session_instance_id() noexcept {
        static std::atomic<int> next_session_id{1};
        return next_session_id.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] SessionAccessGuard begin_session_access(const char *operation) {
        if (destroyed_.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "Attempted to " + std::string(operation) + " with a destroyed libassmod subtitle session " +
                std::to_string(session_instance_id_) + '.'
            );
        }

        bool expected = false;
        if (!in_use_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            throw std::runtime_error(
                "Concurrent " + std::string(operation) + " attempted against libassmod subtitle session " +
                std::to_string(session_instance_id_) + " for '" + subtitle_path_string_ + "'."
            );
        }

        try {
            if (!library_ || !renderer_ || !track_) {
                throw std::runtime_error(
                    "libassmod subtitle session " + std::to_string(session_instance_id_) +
                    " is missing active renderer state for '" + subtitle_path_string_ + "'."
                );
            }
        } catch (...) {
            in_use_.store(false, std::memory_order_release);
            throw;
        }

        return SessionAccessGuard(in_use_);
    }

    [[nodiscard]] ImageRgbaListHandle render_images_rgba(const SubtitleRenderRequest &request) const {
        int detect_change = 0;
        const long long timestamp_milliseconds = static_cast<long long>(request.timestamp_microseconds / 1000);
        return ImageRgbaListHandle(
            ass_render_frame_rgba(renderer_.get(), track_.get(), timestamp_milliseconds, &detect_change)
        );
    }

    SubtitleRenderSessionCreateRequest create_request_{};
    std::string subtitle_path_string_{};
    LibraryHandle library_{};
    RendererHandle renderer_{};
    TrackHandle track_{};
    runtime::SubtitleRuntimeOptions runtime_options_{};
    int session_instance_id_{0};
    std::atomic<bool> in_use_{false};
    std::atomic<bool> destroyed_{false};
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

            if (request.font_search_directory.has_value()) {
                if (request.font_search_directory->empty()) {
                    return make_session_error(
                        request,
                        "Cannot create a subtitle render session because the recovered-font directory is empty.",
                        "Provide an existing directory of recovered font files, or clear the fallback font directory."
                    );
                }

                std::error_code font_directory_error{};
                const bool font_directory_exists =
                    std::filesystem::exists(*request.font_search_directory, font_directory_error);
                if (font_directory_error || !font_directory_exists ||
                    !std::filesystem::is_directory(*request.font_search_directory, font_directory_error) ||
                    font_directory_error) {
                    return make_session_error(
                        request,
                        "Cannot create a subtitle render session because the recovered-font directory is not available.",
                        "Re-run the subtitle font recovery step, or clear the fallback directory and rely on the "
                        "normal system-font path."
                    );
                }
            }

            auto library = create_library();
            configure_library_fonts(*library, request);
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
