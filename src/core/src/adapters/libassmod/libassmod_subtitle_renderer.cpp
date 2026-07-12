#include "utsure/core/subtitles/subtitle_renderer.hpp"
#include "utsure/core/subtitles/subtitle_image_assets.hpp"
#include "libassmod_rgba_bitmap_validation.hpp"
#include "../../runtime_anomaly_policy.hpp"
#include "../../subtitles/subtitle_bitmap_compositor.hpp"
#include "../../subtitles/subtitle_composition_diagnostics.hpp"
#include "../../subtitles/subtitle_runtime_options.hpp"

extern "C" {
#include <ass/ass.h>
}

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef UTSURE_LIBASSMOD_REF
#define UTSURE_LIBASSMOD_REF "unknown"
#endif

namespace utsure::core::subtitles {

namespace {

std::mutex &libassmod_global_mutex();
bool should_serialize_all_libassmod_calls() noexcept;

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

struct AutoRenderResultDeleter final {
    bool strict_same_thread_lifetime{false};
    std::thread::id subtitle_owner_thread_id{};

    void operator()(ASS_RenderResult *result) const noexcept {
        if (result != nullptr) {
            if (result->imgs_rgba != nullptr) {
                if (strict_same_thread_lifetime && std::this_thread::get_id() != subtitle_owner_thread_id) {
                    assert(false && "libassmod RGBA image cleanup ran outside its subtitle-owner thread");
                    std::terminate();
                }
                if (should_serialize_all_libassmod_calls()) {
                    const std::lock_guard lock(libassmod_global_mutex());
                    ass_free_images_rgba(result->imgs_rgba);
                } else {
                    ass_free_images_rgba(result->imgs_rgba);
                }
                result->imgs_rgba = nullptr;
            }
            delete result;
        }
    }
};

using AutoRenderResultHandle = std::unique_ptr<ASS_RenderResult, AutoRenderResultDeleter>;

enum class SubtitleBitmapFrameVisibility : std::uint8_t {
    visible = 0,
    clipped,
    off_frame
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
    std::string actionable_hint,
    const runtime_policy::RuntimeAnomalyClass classification =
        runtime_policy::RuntimeAnomalyClass::unsupported_early
) {
    return SubtitleRenderSessionResult{
        .session = nullptr,
        .error = SubtitleRendererError{
            .subtitle_path = path_to_utf8_string(request.subtitle_path),
            .message = runtime_policy::format_operation_message(
                classification,
                "subtitle session creation",
                message
            ),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

SubtitleRenderResult make_render_error(
    const std::string &subtitle_path,
    std::string message,
    std::string actionable_hint,
    const runtime_policy::RuntimeAnomalyClass classification =
        runtime_policy::RuntimeAnomalyClass::unsafe_or_corrupt
) {
    return SubtitleRenderResult{
        .rendered_frame = std::nullopt,
        .error = SubtitleRendererError{
            .subtitle_path = subtitle_path,
            .message = runtime_policy::format_operation_message(
                classification,
                "subtitle rendering",
                message
            ),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

SubtitleFrameComposeResult make_compose_error(
    std::string message,
    std::string actionable_hint,
    const runtime_policy::RuntimeAnomalyClass classification =
        runtime_policy::RuntimeAnomalyClass::unsafe_or_corrupt
) {
    return SubtitleFrameComposeResult{
        .subtitles_applied = false,
        .error = SubtitleFrameComposeError{
            .message = runtime_policy::format_operation_message(
                classification,
                "subtitle composition",
                message
            ),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

bool is_supported_format_hint(const std::string &format_hint) {
    return format_hint == "auto" || format_hint == "ass" || format_hint == "ssa";
}

SubtitleBitmapFrameVisibility classify_subtitle_bitmap_frame_visibility(
    const media::DecodedVideoFrame &video_frame,
    const int origin_x,
    const int origin_y,
    const int width,
    const int height
) noexcept {
    if (width <= 0 || height <= 0) {
        return SubtitleBitmapFrameVisibility::off_frame;
    }

    const std::int64_t left = static_cast<std::int64_t>(origin_x);
    const std::int64_t top = static_cast<std::int64_t>(origin_y);
    const std::int64_t right = left + static_cast<std::int64_t>(width);
    const std::int64_t bottom = top + static_cast<std::int64_t>(height);
    const std::int64_t clipped_left = std::max<std::int64_t>(0, left);
    const std::int64_t clipped_top = std::max<std::int64_t>(0, top);
    const std::int64_t clipped_right = std::min<std::int64_t>(video_frame.width, right);
    const std::int64_t clipped_bottom = std::min<std::int64_t>(video_frame.height, bottom);

    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
        return SubtitleBitmapFrameVisibility::off_frame;
    }

    if (left != clipped_left || top != clipped_top || right != clipped_right || bottom != clipped_bottom) {
        return SubtitleBitmapFrameVisibility::clipped;
    }

    return SubtitleBitmapFrameVisibility::visible;
}

std::string format_recoverable_subtitle_bitmap_visibility_warning(
    const media::DecodedVideoFrame &video_frame,
    const std::size_t bitmap_index,
    const int origin_x,
    const int origin_y,
    const int width,
    const int height,
    const int stride,
    const std::string_view bitmap_mode,
    const SubtitleBitmapFrameVisibility visibility
) {
    const auto classification = visibility == SubtitleBitmapFrameVisibility::clipped
        ? runtime_policy::RuntimeAnomalyClass::recoverable_normalization
        : runtime_policy::RuntimeAnomalyClass::harmless_noop;
    const std::string action = visibility == SubtitleBitmapFrameVisibility::clipped
        ? "clipped to the output frame"
        : "skipped because it is fully outside the output frame";

    std::ostringstream message;
    message << runtime_policy::format_operation_message(
        classification,
        "subtitle composition",
        "libassmod subtitle bitmap[" + std::to_string(bitmap_index) + "] " + action
    ) << " mode=" << bitmap_mode
        << ", origin=" << origin_x << ',' << origin_y
        << ", size=" << width << 'x' << height
        << ", stride=" << stride
        << ", destination=" << video_frame.width << 'x' << video_frame.height << '.';
    return message.str();
}

void maybe_log_recoverable_subtitle_bitmap_visibility_warning(
    const SubtitleRenderRequest &request,
    const media::DecodedVideoFrame &video_frame,
    const std::size_t bitmap_index,
    const int origin_x,
    const int origin_y,
    const int width,
    const int height,
    const int stride,
    const std::string_view bitmap_mode,
    const SubtitleBitmapFrameVisibility visibility,
    bool &off_frame_warning_logged,
    bool &clipped_warning_logged
) {
    if (visibility == SubtitleBitmapFrameVisibility::visible) {
        return;
    }

    bool &already_logged = visibility == SubtitleBitmapFrameVisibility::clipped
        ? clipped_warning_logged
        : off_frame_warning_logged;
    if (already_logged) {
        return;
    }

    detail::maybe_log_subtitle_warning(
        request,
        format_recoverable_subtitle_bitmap_visibility_warning(
            video_frame,
            bitmap_index,
            origin_x,
            origin_y,
            width,
            height,
            stride,
            bitmap_mode,
            visibility
        )
    );
    already_logged = true;
}

double choose_pixel_aspect_ratio(const media::Rational &sample_aspect_ratio) {
    if (!sample_aspect_ratio.is_valid() || sample_aspect_ratio.numerator <= 0 || sample_aspect_ratio.denominator <= 0) {
        return 1.0;
    }

    return static_cast<double>(sample_aspect_ratio.numerator) /
        static_cast<double>(sample_aspect_ratio.denominator);
}

std::string format_renderer_setup_diagnostics(
    const SubtitleRenderSessionCreateRequest &request,
    const std::size_t image_asset_count
) {
    std::ostringstream message;
    message << "libassmod renderer setup: frame_size="
            << request.canvas_width << 'x' << request.canvas_height
            << ", storage_size=" << request.canvas_width << 'x' << request.canvas_height
            << ", pixel_aspect=" << choose_pixel_aspect_ratio(request.sample_aspect_ratio)
            << ", margins=0,0,0,0"
            << ", use_margins=0"
            << ", font.default_family=Arial"
            << ", font.provider=autodetect"
            << ", setup.thread_id=" << std::this_thread::get_id()
            << ", image_assets=" << image_asset_count
            << ", libassmod_ref=" << UTSURE_LIBASSMOD_REF;
    if (request.font_search_directory.has_value()) {
        message << ", font.directory=" << path_to_utf8_string(*request.font_search_directory);
    } else {
        message << ", font.directory=none";
    }

    return message.str();
}

std::mutex &subtitle_setup_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::mutex &libassmod_global_mutex() {
    static std::mutex mutex;
    return mutex;
}

bool should_serialize_all_libassmod_calls() noexcept {
    return runtime::global_libass_lock_enabled();
}

template <typename Callback>
decltype(auto) with_optional_global_libassmod_lock(Callback &&callback) {
    if (should_serialize_all_libassmod_calls()) {
        const std::lock_guard lock(libassmod_global_mutex());
        return callback();
    }

    return callback();
}

std::string pointer_to_string(const void *pointer) {
    std::ostringstream stream;
    stream << pointer;
    return stream.str();
}

LibraryHandle create_library() {
    LibraryHandle library(with_optional_global_libassmod_lock([]() {
        return ass_library_init();
    }));
    if (!library) {
        throw std::runtime_error("libassmod failed to initialize the subtitle library.");
    }

    with_optional_global_libassmod_lock([&library]() {
        ass_set_extract_fonts(library.get(), 1);
    });
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
    with_optional_global_libassmod_lock([&library, &font_directory_utf8]() {
        ass_set_fonts_dir(&library, font_directory_utf8.c_str());
    });
}

RendererHandle create_renderer(
    ASS_Library &library,
    const SubtitleRenderSessionCreateRequest &request
) {
    RendererHandle renderer(with_optional_global_libassmod_lock([&library]() {
        return ass_renderer_init(&library);
    }));
    if (!renderer) {
        throw std::runtime_error("libassmod failed to initialize the subtitle renderer.");
    }

    with_optional_global_libassmod_lock([&renderer, &request]() {
        ass_set_frame_size(renderer.get(), request.canvas_width, request.canvas_height);
        ass_set_storage_size(renderer.get(), request.canvas_width, request.canvas_height);
        ass_set_pixel_aspect(renderer.get(), choose_pixel_aspect_ratio(request.sample_aspect_ratio));
        ass_set_margins(renderer.get(), 0, 0, 0, 0);
        ass_set_use_margins(renderer.get(), 0);
        ass_set_fonts(renderer.get(), nullptr, "Arial", ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
    });

    return renderer;
}

TrackHandle load_track(
    ASS_Library &library,
    const SubtitleRenderSessionCreateRequest &request
) {
    const auto subtitle_path_utf8 = path_to_utf8_string(request.subtitle_path);
    TrackHandle track(with_optional_global_libassmod_lock([&library, &subtitle_path_utf8]() {
        return ass_read_file(&library, subtitle_path_utf8.c_str(), nullptr);
    }));
    if (!track) {
        throw std::runtime_error(
            "libassmod failed to parse subtitle script '" + path_to_utf8_string(request.subtitle_path) + "'."
        );
    }

    return track;
}

ASS_TagImageFormat to_ass_tag_image_format(const SubtitleImageAssetFormat format) {
    switch (format) {
    case SubtitleImageAssetFormat::png:
        return ASS_TAG_IMAGE_FORMAT_PNG;
    case SubtitleImageAssetFormat::jpeg:
        return ASS_TAG_IMAGE_FORMAT_JPEG;
    case SubtitleImageAssetFormat::webp:
        return ASS_TAG_IMAGE_FORMAT_WEBP;
    }

    throw std::runtime_error("Unsupported subtitle image asset format.");
}

struct SubtitleImageAssetRegistrationResult final {
    std::vector<std::string> diagnostics{};
    std::optional<SubtitleImageAssetError> error{};

    [[nodiscard]] bool succeeded() const noexcept {
        return !error.has_value();
    }
};

SubtitleImageAssetRegistrationResult register_subtitle_image_assets(
    ASS_Renderer &renderer,
    const std::vector<SubtitleImageAsset> &assets
) {
    std::vector<std::string> diagnostics{};
    if (assets.empty()) {
        return SubtitleImageAssetRegistrationResult{.diagnostics = {}, .error = std::nullopt};
    }

    for (const auto &asset : assets) {
        if (asset.name.empty() || asset.width <= 0 || asset.height <= 0 ||
            asset.stride < asset.width * 4 || asset.rgba.empty()) {
            with_optional_global_libassmod_lock([&renderer]() {
                ass_clear_tag_images(&renderer);
            });
            return SubtitleImageAssetRegistrationResult{
                .diagnostics = std::move(diagnostics),
                .error = SubtitleImageAssetError{
                    .message = "Invalid subtitle image asset buffer: " + asset.name,
                    .actionable_hint = "The decoded image asset is not a valid RGBA surface."
                }
            };
        }

        const auto *buffer = asset.rgba.data();
        const int result = with_optional_global_libassmod_lock([&renderer, &asset, buffer]() {
            return ass_set_tag_image_rgba(
                &renderer,
                asset.name.c_str(),
                to_ass_tag_image_format(asset.format),
                asset.width,
                asset.height,
                asset.stride,
                buffer
            );
        });
        if (result != 0) {
            with_optional_global_libassmod_lock([&renderer]() {
                ass_clear_tag_images(&renderer);
            });
            return SubtitleImageAssetRegistrationResult{
                .diagnostics = std::move(diagnostics),
                .error = SubtitleImageAssetError{
                    .message = "libassmod rejected subtitle image asset: " + asset.name,
                    .actionable_hint = "Asset path: " + path_to_utf8_string(asset.source_path)
                }
            };
        }

        diagnostics.push_back(
            "Subtitle image asset registered: " + asset.name + " -> " +
            path_to_utf8_string(asset.source_path) + " (" +
            std::to_string(asset.width) + "x" + std::to_string(asset.height) +
            "), stride=" + std::to_string(asset.stride) +
            ", buffer=" + pointer_to_string(buffer) +
            ", owned_bytes=" + std::to_string(asset.rgba.size())
        );
    }

    return SubtitleImageAssetRegistrationResult{
        .diagnostics = std::move(diagnostics),
        .error = std::nullopt
    };
}

std::uint8_t ass_color_red(const std::uint32_t color) noexcept {
    return static_cast<std::uint8_t>(color >> 24U);
}

std::uint8_t ass_color_green(const std::uint32_t color) noexcept {
    return static_cast<std::uint8_t>((color >> 16U) & 0xFFU);
}

std::uint8_t ass_color_blue(const std::uint32_t color) noexcept {
    return static_cast<std::uint8_t>((color >> 8U) & 0xFFU);
}

std::uint8_t ass_color_opacity(const std::uint32_t color) noexcept {
    return static_cast<std::uint8_t>(255U - (color & 0xFFU));
}

int packed_rgba_stride_bytes(const int width, const std::string_view label) {
    const std::int64_t stride = static_cast<std::int64_t>(width) * 4LL;
    if (width <= 0 || stride <= 0 || stride > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        std::ostringstream message;
        message << "Cannot safely copy a libassmod RGBA subtitle " << label
                << " into app-owned memory because its packed row size is invalid: width="
                << width << '.';
        throw runtime_policy::RuntimeAnomalyError(
            runtime_policy::RuntimeAnomalyClass::unsafe_or_corrupt,
            message.str()
        );
    }

    return static_cast<int>(stride);
}

SubtitleBitmap copy_ass_image_rgba(const ASS_ImageRGBA &image) {
    const int line_stride_bytes = packed_rgba_stride_bytes(image.w, "bitmap");
    if (image.rgba == nullptr || image.stride < line_stride_bytes) {
        std::ostringstream message;
        message << "Cannot safely copy a libassmod RGBA subtitle bitmap into app-owned memory: origin="
                << image.dst_x << ',' << image.dst_y
                << ", width=" << image.w
                << ", height=" << image.h
                << ", stride=" << image.stride
                << ", rgba=" << static_cast<const void *>(image.rgba) << '.';
        throw runtime_policy::RuntimeAnomalyError(
            runtime_policy::RuntimeAnomalyClass::unsafe_or_corrupt,
            message.str()
        );
    }

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

SubtitleBitmap copy_ass_image(const ASS_Image &image) {
    const auto minimum_stride = static_cast<std::int64_t>(image.w);
    if (image.w <= 0 || image.h <= 0 || image.stride <= 0 ||
        static_cast<std::int64_t>(image.stride) < minimum_stride ||
        image.bitmap == nullptr ||
        static_cast<std::int64_t>(image.w) * 4LL > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        std::ostringstream message;
        message << "libassmod produced an invalid legacy subtitle bitmap: origin="
                << image.dst_x << ',' << image.dst_y
                << ", width=" << image.w
                << ", height=" << image.h
                << ", stride=" << image.stride
                << ", bitmap=" << static_cast<const void *>(image.bitmap) << '.';
        throw runtime_policy::RuntimeAnomalyError(
            runtime_policy::RuntimeAnomalyClass::unsafe_or_corrupt,
            message.str()
        );
    }

    const int line_stride_bytes = image.w * 4;
    std::vector<std::uint8_t> bytes(detail::required_rgba_buffer_size(
        image.w,
        image.h,
        line_stride_bytes,
        "bitmap"
    ), 0U);

    const std::uint8_t opacity = ass_color_opacity(image.color);
    const std::uint8_t red = ass_color_red(image.color);
    const std::uint8_t green = ass_color_green(image.color);
    const std::uint8_t blue = ass_color_blue(image.color);

    for (int row = 0; row < image.h; ++row) {
        const auto *source_row = image.bitmap +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(image.stride);
        auto *destination_row = bytes.data() +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(line_stride_bytes);
        for (int column = 0; column < image.w; ++column) {
            const auto offset = static_cast<std::size_t>(column) * 4U;
            const std::uint8_t coverage = source_row[column];
            const std::uint8_t alpha = static_cast<std::uint8_t>(
                (static_cast<unsigned int>(coverage) * static_cast<unsigned int>(opacity) + 127U) / 255U
            );
            destination_row[offset + 0U] = static_cast<std::uint8_t>(
                (static_cast<unsigned int>(red) * static_cast<unsigned int>(alpha) + 127U) / 255U
            );
            destination_row[offset + 1U] = static_cast<std::uint8_t>(
                (static_cast<unsigned int>(green) * static_cast<unsigned int>(alpha) + 127U) / 255U
            );
            destination_row[offset + 2U] = static_cast<std::uint8_t>(
                (static_cast<unsigned int>(blue) * static_cast<unsigned int>(alpha) + 127U) / 255U
            );
            destination_row[offset + 3U] = alpha;
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

std::vector<ASS_Image *> collect_ass_image_nodes(ASS_Image *images) {
    std::vector<ASS_Image *> bitmaps{};
    for (ASS_Image *image = images; image != nullptr; image = image->next) {
        bitmaps.push_back(image);
    }

    return bitmaps;
}

std::string format_ass_image_node_diagnostics(
    const ASS_Image &image,
    const std::size_t bitmap_index,
    const std::string_view phase
) {
    std::ostringstream message;
    message << "libassmod legacy node[" << bitmap_index << "] " << phase
            << ": type=" << image.type
            << ", origin=" << image.dst_x << ',' << image.dst_y
            << ", size=" << image.w << 'x' << image.h
            << ", stride=" << image.stride
            << ", bitmap=" << static_cast<const void *>(image.bitmap)
            << ", color=0x" << std::hex << image.color << std::dec;
    return message.str();
}

void maybe_log_auto_render_result(
    const SubtitleRenderRequest &request,
    const ASS_RenderResult &render_result
) {
    if (!detail::should_log_subtitle_frame_diagnostics(request)) {
        return;
    }

    std::ostringstream message;
    message << "libassmod auto render result: use_rgba=" << render_result.use_rgba
            << ", imgs=" << static_cast<const void *>(render_result.imgs)
            << ", imgs_rgba=" << static_cast<const void *>(render_result.imgs_rgba);
    request.debug_context->log_callback(message.str());
}

void maybe_log_ass_image_nodes_after_render(
    const std::vector<ASS_Image *> &image_nodes,
    const SubtitleRenderRequest &request
) {
    if (!detail::should_log_subtitle_bitmap_diagnostics(request)) {
        return;
    }

    for (std::size_t bitmap_index = 0; bitmap_index < image_nodes.size(); ++bitmap_index) {
        if (image_nodes[bitmap_index] == nullptr) {
            request.debug_context->log_callback(
                "libassmod legacy node[" + std::to_string(bitmap_index) + "] after_render: node=null"
            );
            continue;
        }

        request.debug_context->log_callback(
            format_ass_image_node_diagnostics(*image_nodes[bitmap_index], bitmap_index, "after_render")
        );
    }
}

void maybe_log_ass_image_collection_decision(
    const SubtitleRenderRequest &request,
    const ASS_Image &image,
    const std::size_t bitmap_index,
    const std::string_view decision,
    const std::string_view reason
) {
    if (!detail::should_log_subtitle_bitmap_diagnostics(request)) {
        return;
    }

    request.debug_context->log_callback(
        format_ass_image_node_diagnostics(image, bitmap_index, decision) +
        ", decision_reason=" + std::string(reason)
    );
}

std::vector<ASS_Image *> collect_drawable_ass_image_nodes(
    const std::vector<ASS_Image *> &image_nodes,
    const SubtitleRenderRequest &request
) {
    std::vector<ASS_Image *> drawable_bitmaps{};
    drawable_bitmaps.reserve(image_nodes.size());
    for (std::size_t bitmap_index = 0; bitmap_index < image_nodes.size(); ++bitmap_index) {
        if (image_nodes[bitmap_index] == nullptr) {
            continue;
        }

        const ASS_Image &image = *image_nodes[bitmap_index];
        if (image.w <= 0 || image.h <= 0) {
            maybe_log_ass_image_collection_decision(request, image, bitmap_index, "rejected", "empty");
            continue;
        }

        maybe_log_ass_image_collection_decision(
            request,
            image,
            bitmap_index,
            "accepted",
            "trusted_libassmod_output"
        );
        drawable_bitmaps.push_back(image_nodes[bitmap_index]);
    }

    return drawable_bitmaps;
}

bool should_use_rgba_images(const ASS_RenderResult &render_result) noexcept {
    return render_result.use_rgba != 0 && render_result.imgs_rgba != nullptr;
}

bool should_emit_subtitle_render_trace(const SubtitleRenderRequest &request) noexcept {
    if (request.debug_context == nullptr || !request.debug_context->log_callback) {
        return false;
    }

    if (runtime::environment_flag_enabled("UTSURE_SUBTITLE_RENDER_TRACE_FULL")) {
        return true;
    }

    if (!runtime::environment_flag_enabled("UTSURE_SUBTITLE_RENDER_TRACE")) {
        return false;
    }

    const auto frame_index = request.debug_context->decoded_frame_index;
    return frame_index < 5 || frame_index % 300 == 0;
}

class LibassmodSubtitleRenderSession final : public SubtitleRenderSession {
public:
    LibassmodSubtitleRenderSession(
        SubtitleRenderSessionCreateRequest create_request,
        std::string subtitle_path_string,
        std::vector<std::string> quirk_messages,
        std::vector<SubtitleImageAsset> image_assets,
        LibraryHandle library,
        RendererHandle renderer,
        TrackHandle track
    )
        : create_request_(std::move(create_request)),
          subtitle_path_string_(std::move(subtitle_path_string)),
          quirk_messages_(std::move(quirk_messages)),
          image_assets_(std::move(image_assets)),
          library_(std::move(library)),
          renderer_(std::move(renderer)),
          track_(std::move(track)),
          runtime_options_(runtime::resolve_subtitle_runtime_options()),
          session_instance_id_(next_session_instance_id()),
          strict_same_thread_lifetime_(runtime::strict_same_thread_lifetime_enabled()),
          subtitle_owner_thread_id_(std::this_thread::get_id()),
          subtitle_library_created_thread_id_(std::this_thread::get_id()),
          subtitle_renderer_created_thread_id_(std::this_thread::get_id()),
          subtitle_track_created_thread_id_(std::this_thread::get_id()) {
        assert(library_ != nullptr);
        assert(renderer_ != nullptr);
        assert(track_ != nullptr);
        for (const auto &asset : image_assets_) {
            assert(!asset.name.empty());
            assert(asset.width > 0);
            assert(asset.height > 0);
            assert(asset.stride >= asset.width * 4);
            assert(!asset.rgba.empty());
        }
        std::ostringstream message;
        message << "libassmod session created: session_instance_id=" << session_instance_id_
                << ", renderer=" << pointer_to_string(renderer_.get())
                << ", track=" << pointer_to_string(track_.get())
                << ", library=" << pointer_to_string(library_.get())
                << ", subtitle_strict_same_thread=" << (strict_same_thread_lifetime_ ? 1 : 0)
                << ", subtitle_owner_thread_id=" << subtitle_owner_thread_id_
                << ", subtitle_library_created_thread_id=" << subtitle_library_created_thread_id_
                << ", subtitle_renderer_created_thread_id=" << subtitle_renderer_created_thread_id_
                << ", subtitle_track_created_thread_id=" << subtitle_track_created_thread_id_
                << ", last_subtitle_event_count=" << track_->n_events
                << ", registered_image_asset_count=" << image_assets_.size()
                << ", safe_mode=" << (runtime::environment_flag_enabled("UTSURE_SUBTITLE_SAFE_MODE") ? 1 : 0)
                << ", global_libass_lock=" << (runtime::global_libass_lock_enabled() ? 1 : 0)
                << ", bitmap_transfer_mode=" << runtime::to_string(runtime_options_.bitmap_transfer_mode)
                << ", composition_mode=" << runtime::to_string(runtime_options_.composition_mode);
        quirk_messages_.push_back(message.str());
        quirk_messages_.push_back(
            "ass_read_file success: path=" + subtitle_path_string_ +
            ", track=" + pointer_to_string(track_.get()) +
            ", events=" + std::to_string(track_->n_events)
        );
    }

    ~LibassmodSubtitleRenderSession() override {
        {
            std::unique_lock lock(access_mutex_);
            enforce_owner_thread_for_teardown_or_terminate("begin teardown");
            cleanup_started_.store(true, std::memory_order_release);
            access_available_.wait(lock, [this]() {
                return active_render_count_ == 0;
            });
            assert(active_render_count_ == 0);
        }

        with_optional_global_libassmod_lock([this]() {
            if (renderer_) {
                ass_clear_tag_images(renderer_.get());
            }
            subtitle_track_destroyed_thread_id_ = std::this_thread::get_id();
            track_.reset();
            subtitle_renderer_destroyed_thread_id_ = std::this_thread::get_id();
            renderer_.reset();
            subtitle_library_destroyed_thread_id_ = std::this_thread::get_id();
            library_.reset();
        });
        image_assets_.clear();
        emit_teardown_lifecycle_diagnostic();
    }

    [[nodiscard]] SubtitleImageAssetRegistrationResult register_session_image_assets() {
        std::unique_lock lock(access_mutex_);
        enforce_owner_thread_locked("register image assets");
        if (!renderer_) {
            return SubtitleImageAssetRegistrationResult{
                .diagnostics = {},
                .error = SubtitleImageAssetError{
                    .message = "Cannot register subtitle image assets because the renderer is not available.",
                    .actionable_hint = "Create the libassmod renderer before registering image tag assets."
                }
            };
        }

        const auto registration_result = register_subtitle_image_assets(*renderer_, image_assets_);
        if (registration_result.succeeded()) {
            quirk_messages_.insert(
                quirk_messages_.end(),
                registration_result.diagnostics.begin(),
                registration_result.diagnostics.end()
            );
        }
        return registration_result;
    }

    [[nodiscard]] SubtitleRenderResult render(const SubtitleRenderRequest &request) noexcept override {
        try {
            auto access_guard = begin_session_access("render", request);
            maybe_log_renderer_setup_diagnostics(request);
            maybe_log_quirk_diagnostics(request);
            auto render_result = render_images_auto(request, access_guard);
            maybe_log_auto_render_result(request, *render_result);
            std::vector<SubtitleBitmap> bitmaps{};
            if (should_use_rgba_images(*render_result)) {
                const auto image_nodes = collect_ass_image_rgba_nodes(render_result->imgs_rgba);
                detail::libassmod::maybe_log_ass_image_rgba_nodes_after_render(
                    image_nodes,
                    request,
                    "copied"
                );
                const auto drawable_image_nodes = detail::libassmod::collect_drawable_ass_image_rgba_nodes(
                    image_nodes,
                    request,
                    "copied",
                    subtitle_path_string_,
                    session_instance_id_
                );
                bitmaps.reserve(drawable_image_nodes.size());
                for (const auto &drawable_image : drawable_image_nodes) {
                    if (drawable_image.image == nullptr) {
                        continue;
                    }

                    bitmaps.push_back(copy_ass_image_rgba(*drawable_image.image));
                }
            } else {
                const auto image_nodes = collect_ass_image_nodes(render_result->imgs);
                maybe_log_ass_image_nodes_after_render(image_nodes, request);
                const auto drawable_image_nodes = collect_drawable_ass_image_nodes(image_nodes, request);
                bitmaps.reserve(drawable_image_nodes.size());
                for (const auto *image : drawable_image_nodes) {
                    if (image == nullptr) {
                        continue;
                    }

                    bitmaps.push_back(copy_ass_image(*image));
                }
            }

            access_guard.finish_success(request);
            return SubtitleRenderResult{
                .rendered_frame = RenderedSubtitleFrame{
                    .timestamp_microseconds = request.timestamp_microseconds,
                    .canvas_width = create_request_.canvas_width,
                    .canvas_height = create_request_.canvas_height,
                    .bitmaps = std::move(bitmaps)
                },
                .error = std::nullopt
            };
        } catch (const runtime_policy::RuntimeAnomalyError &exception) {
            return make_render_error(
                subtitle_path_string_,
                exception.what(),
                "classification=" + std::string(runtime_policy::to_string(exception.classification())),
                exception.classification()
            );
        } catch (const std::exception &exception) {
            return make_render_error(
                subtitle_path_string_,
                "libassmod raised an unclassified subtitle-rendering failure.",
                exception.what()
            );
        }
    }

    [[nodiscard]] SubtitleFrameComposeResult compose_into_frame(
        media::DecodedVideoFrame &video_frame,
        const SubtitleRenderRequest &request
    ) noexcept override {
        try {
            auto access_guard = begin_session_access("compose", request);
            detail::validate_rgba_frame_surface(video_frame, "Subtitle composition");
            maybe_log_renderer_setup_diagnostics(request);
            maybe_log_quirk_diagnostics(request);

            auto render_result = render_images_auto(request, access_guard);
            maybe_log_auto_render_result(request, *render_result);
            bool subtitles_applied = false;
            if (should_use_rgba_images(*render_result)) {
                const auto image_nodes = collect_ass_image_rgba_nodes(render_result->imgs_rgba);
                detail::libassmod::maybe_log_ass_image_rgba_nodes_after_render(
                    image_nodes,
                    request,
                    runtime::to_string(runtime_options_.bitmap_transfer_mode)
                );
                const auto drawable_image_nodes = detail::libassmod::collect_drawable_ass_image_rgba_nodes(
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
                for (const auto &drawable_image : drawable_image_nodes) {
                    if (drawable_image.image == nullptr) {
                        continue;
                    }

                    const ASS_ImageRGBA &image = *drawable_image.image;
                    const auto visibility = classify_subtitle_bitmap_frame_visibility(
                        video_frame,
                        image.dst_x,
                        image.dst_y,
                        image.w,
                        image.h
                    );
                    maybe_log_recoverable_subtitle_bitmap_visibility_warning(
                        request,
                        video_frame,
                        drawable_image.bitmap_index,
                        image.dst_x,
                        image.dst_y,
                        image.w,
                        image.h,
                        image.stride,
                        runtime::to_string(runtime_options_.bitmap_transfer_mode),
                        visibility,
                        off_frame_bitmap_warning_logged_,
                        clipped_bitmap_warning_logged_
                    );
                    if (visibility == SubtitleBitmapFrameVisibility::off_frame) {
                        continue;
                    }

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
            } else {
                const auto image_nodes = collect_ass_image_nodes(render_result->imgs);
                maybe_log_ass_image_nodes_after_render(image_nodes, request);
                const auto drawable_image_nodes = collect_drawable_ass_image_nodes(image_nodes, request);
                detail::maybe_log_subtitle_frame_diagnostics(
                    request,
                    video_frame,
                    drawable_image_nodes.size(),
                    "legacy"
                );
                for (std::size_t bitmap_index = 0; bitmap_index < drawable_image_nodes.size(); ++bitmap_index) {
                    const ASS_Image *image = drawable_image_nodes[bitmap_index];
                    if (image == nullptr) {
                        continue;
                    }

                    const auto visibility = classify_subtitle_bitmap_frame_visibility(
                        video_frame,
                        image->dst_x,
                        image->dst_y,
                        image->w,
                        image->h
                    );
                    maybe_log_recoverable_subtitle_bitmap_visibility_warning(
                        request,
                        video_frame,
                        bitmap_index,
                        image->dst_x,
                        image->dst_y,
                        image->w,
                        image->h,
                        image->stride,
                        "legacy",
                        visibility,
                        off_frame_bitmap_warning_logged_,
                        clipped_bitmap_warning_logged_
                    );
                    if (visibility == SubtitleBitmapFrameVisibility::off_frame) {
                        continue;
                    }

                    detail::maybe_log_subtitle_bitmap_diagnostics(
                        request,
                        bitmap_index,
                        image->dst_x,
                        image->dst_y,
                        image->w,
                        image->h,
                        image->stride,
                        "legacy"
                    );
                    detail::composite_bitmap_into_frame(video_frame, copy_ass_image(*image));
                    subtitles_applied = true;
                }
            }

            access_guard.finish_success(request);
            return SubtitleFrameComposeResult{
                .subtitles_applied = subtitles_applied,
                .error = std::nullopt
            };
        } catch (const runtime_policy::RuntimeAnomalyError &exception) {
            return make_compose_error(
                exception.what(),
                std::string("classification=") +
                    runtime_policy::to_string(exception.classification()) + " Context: " +
                    detail::format_subtitle_frame_diagnostics(
                        request,
                        video_frame,
                        0U,
                        runtime::to_string(runtime_options_.bitmap_transfer_mode)
                    ),
                exception.classification()
            );
        } catch (const std::exception &exception) {
            return make_compose_error(
                "libassmod raised an unclassified subtitle-composition failure.",
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
    void maybe_log_renderer_setup_diagnostics(const SubtitleRenderRequest &request) {
        if (renderer_setup_diagnostics_logged_ || !detail::should_log_subtitle_frame_diagnostics(request)) {
            return;
        }

        std::ostringstream message;
        message << format_renderer_setup_diagnostics(create_request_, image_assets_.size())
                << ", renderer=" << pointer_to_string(renderer_.get())
                << ", track=" << pointer_to_string(track_.get())
                << ", library=" << pointer_to_string(library_.get())
                << ", session_instance_id=" << session_instance_id_;
        request.debug_context->log_callback(message.str());
        renderer_setup_diagnostics_logged_ = true;
    }

    void maybe_log_quirk_diagnostics(const SubtitleRenderRequest &request) {
        if (quirk_messages_.empty() || quirk_diagnostics_logged_) {
            return;
        }

        if (!detail::should_log_subtitle_frame_diagnostics(request)) {
            return;
        }

        for (const auto &message : quirk_messages_) {
            detail::maybe_log_subtitle_renderer_quirk_diagnostic(request, message);
        }
        quirk_diagnostics_logged_ = true;
    }

    void remember_lifecycle_callback(const SubtitleRenderRequest &request) {
        if (strict_same_thread_lifetime_ && request.debug_context != nullptr && request.debug_context->lifecycle_callback) {
            lifecycle_callback_ = request.debug_context->lifecycle_callback;
        }
    }

    [[nodiscard]] std::string format_thread_lifetime_diagnostic(
        const std::string_view event,
        const std::string_view operation,
        const std::thread::id actual_thread_id
    ) const {
        std::ostringstream message;
        message << event
                << ": operation=" << operation
                << ", session_instance_id=" << session_instance_id_
                << ", thread_id=" << actual_thread_id
                << ", subtitle_strict_same_thread=" << (strict_same_thread_lifetime_ ? 1 : 0)
                << ", subtitle_owner_thread_id=" << subtitle_owner_thread_id_
                << ", subtitle_library_created_thread_id=" << subtitle_library_created_thread_id_
                << ", subtitle_renderer_created_thread_id=" << subtitle_renderer_created_thread_id_
                << ", subtitle_track_created_thread_id=" << subtitle_track_created_thread_id_
                << ", subtitle_render_thread_id=" << subtitle_render_thread_id_
                << ", subtitle_track_destroyed_thread_id=" << subtitle_track_destroyed_thread_id_
                << ", subtitle_renderer_destroyed_thread_id=" << subtitle_renderer_destroyed_thread_id_
                << ", subtitle_library_destroyed_thread_id=" << subtitle_library_destroyed_thread_id_;
        return message.str();
    }

    void enforce_owner_thread_locked(const std::string_view operation) const {
        if (!strict_same_thread_lifetime_ || std::this_thread::get_id() == subtitle_owner_thread_id_) {
            return;
        }

        const auto diagnostic = format_thread_lifetime_diagnostic(
            "subtitle strict same-thread violation",
            operation,
            std::this_thread::get_id()
        );
        if (lifecycle_callback_) {
            lifecycle_callback_(diagnostic);
        }
        throw std::runtime_error(diagnostic);
    }

    void enforce_owner_thread_for_teardown_or_terminate(const std::string_view operation) const noexcept {
        if (!strict_same_thread_lifetime_ || std::this_thread::get_id() == subtitle_owner_thread_id_) {
            return;
        }

        try {
            const auto diagnostic = format_thread_lifetime_diagnostic(
                "subtitle strict same-thread violation",
                operation,
                std::this_thread::get_id()
            );
            if (lifecycle_callback_) {
                lifecycle_callback_(diagnostic);
            }
        } catch (...) {
        }
        assert(false && "libassmod teardown ran outside its subtitle-owner thread");
        std::terminate();
    }

    void emit_teardown_lifecycle_diagnostic() const noexcept {
        if (!strict_same_thread_lifetime_ || !lifecycle_callback_) {
            return;
        }

        try {
            lifecycle_callback_(format_thread_lifetime_diagnostic(
                "subtitle session destroyed",
                "teardown",
                std::this_thread::get_id()
            ));
        } catch (...) {
        }
    }

    void log_render_lifecycle(
        const SubtitleRenderRequest &request,
        const std::string_view event,
        const std::string_view operation,
        const int active_count
    ) const {
        if (request.debug_context == nullptr) {
            return;
        }

        std::ostringstream message;
        message << event
                << ": operation=" << operation
                << ", session_instance_id=" << session_instance_id_
                << ", frame=" << request.debug_context->decoded_frame_index
                << ", pts_us=" << request.timestamp_microseconds
                << ", renderer_pts_ms="
                << subtitle_timestamp_microseconds_to_renderer_milliseconds(request.timestamp_microseconds)
                << ", thread_id=" << std::this_thread::get_id()
                << ", renderer=" << pointer_to_string(renderer_.get())
                << ", track=" << pointer_to_string(track_.get())
                << ", library=" << pointer_to_string(library_.get())
                << ", active_subtitle_render_count=" << active_count
                << ", subtitle_strict_same_thread=" << (strict_same_thread_lifetime_ ? 1 : 0)
                << ", subtitle_owner_thread_id=" << subtitle_owner_thread_id_
                << ", subtitle_library_created_thread_id=" << subtitle_library_created_thread_id_
                << ", subtitle_renderer_created_thread_id=" << subtitle_renderer_created_thread_id_
                << ", subtitle_track_created_thread_id=" << subtitle_track_created_thread_id_
                << ", subtitle_render_thread_id=" << subtitle_render_thread_id_
                << ", subtitle_track_destroyed_thread_id=" << subtitle_track_destroyed_thread_id_
                << ", subtitle_renderer_destroyed_thread_id=" << subtitle_renderer_destroyed_thread_id_
                << ", subtitle_library_destroyed_thread_id=" << subtitle_library_destroyed_thread_id_
                << ", last_subtitle_event_count=" << (track_ != nullptr ? track_->n_events : 0)
                << ", registered_image_asset_count=" << image_assets_.size();
        message << ", subtitle_cleanup_started=" << (cleanup_started_.load(std::memory_order_acquire) ? 1 : 0)
                << ", safe_mode=" << (runtime::environment_flag_enabled("UTSURE_SUBTITLE_SAFE_MODE") ? 1 : 0)
                << ", global_libass_lock=" << (runtime::global_libass_lock_enabled() ? 1 : 0)
                << ", bitmap_transfer_mode=" << runtime::to_string(runtime_options_.bitmap_transfer_mode)
                << ", composition_mode=" << runtime::to_string(runtime_options_.composition_mode);
        if (!image_assets_.empty()) {
            message << ", last_registered_image_asset_name=" << image_assets_.back().name
                    << ", last_registered_image_asset_path=" << path_to_utf8_string(image_assets_.back().source_path);
        }
        const auto lifecycle_message = message.str();
        if (request.debug_context->lifecycle_callback) {
            request.debug_context->lifecycle_callback(lifecycle_message);
        }
        if (should_emit_subtitle_render_trace(request)) {
            request.debug_context->log_callback(lifecycle_message);
        }
    }

    class SessionAccessGuard final {
    public:
        SessionAccessGuard(
            LibassmodSubtitleRenderSession &session,
            std::string operation,
            std::unique_lock<std::mutex> access_lock
        ) noexcept
            : session_(&session),
              operation_(std::move(operation)),
              access_lock_(std::move(access_lock)) {
        }

        SessionAccessGuard(const SessionAccessGuard &) = delete;
        SessionAccessGuard &operator=(const SessionAccessGuard &) = delete;

        SessionAccessGuard(SessionAccessGuard &&other) noexcept
            : session_(std::exchange(other.session_, nullptr)),
              operation_(std::move(other.operation_)),
              access_lock_(std::move(other.access_lock_)) {
        }

        SessionAccessGuard &operator=(SessionAccessGuard &&other) noexcept {
            if (this == &other) {
                return *this;
            }

            release();
            session_ = std::exchange(other.session_, nullptr);
            operation_ = std::move(other.operation_);
            access_lock_ = std::move(other.access_lock_);
            return *this;
        }

        ~SessionAccessGuard() {
            release();
        }

        void finish_success(const SubtitleRenderRequest &request) noexcept {
            if (session_ == nullptr) {
                return;
            }

            session_->last_subtitle_render_end_pts_.store(request.timestamp_microseconds, std::memory_order_release);
            session_->log_render_lifecycle(
                request,
                "subtitle render end",
                operation_,
                active_count_snapshot()
            );
        }

        [[nodiscard]] ASS_Renderer *renderer() const noexcept {
            return session_ != nullptr ? session_->renderer_.get() : nullptr;
        }

        [[nodiscard]] ASS_Track *track() const noexcept {
            return session_ != nullptr ? session_->track_.get() : nullptr;
        }

    private:
        [[nodiscard]] int active_count_snapshot() const noexcept {
            return session_ != nullptr ? session_->active_subtitle_render_count_.load(std::memory_order_acquire) : 0;
        }

        void release() noexcept {
            if (session_ != nullptr) {
                if (!access_lock_.owns_lock()) {
                    access_lock_.lock();
                }
                assert(session_->active_render_count_ > 0);
                if (session_->active_render_count_ > 0) {
                    --session_->active_render_count_;
                }
                session_->render_in_progress_ = false;
                session_->active_subtitle_render_count_.store(
                    session_->active_render_count_,
                    std::memory_order_release
                );
                auto *session = session_;
                session_ = nullptr;
                access_lock_.unlock();
                session->access_available_.notify_all();
            }
        }

        LibassmodSubtitleRenderSession *session_{nullptr};
        std::string operation_{};
        std::unique_lock<std::mutex> access_lock_{};
    };

    [[nodiscard]] static int next_session_instance_id() noexcept {
        static std::atomic<int> next_session_id{1};
        return next_session_id.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] SessionAccessGuard begin_session_access(
        const char *operation,
        const SubtitleRenderRequest &request
    ) {
        std::unique_lock lock(access_mutex_);
        remember_lifecycle_callback(request);
        if (cleanup_started_.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "Attempted to " + std::string(operation) + " while cleanup had begun for libassmod subtitle session " +
                std::to_string(session_instance_id_) + '.'
            );
        }

        enforce_owner_thread_locked(operation);

        if (render_in_progress_) {
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
            assert(library_ != nullptr);
            assert(renderer_ != nullptr);
            assert(track_ != nullptr);
        } catch (...) {
            throw;
        }

        render_in_progress_ = true;
        ++active_render_count_;
        active_subtitle_render_count_.store(active_render_count_, std::memory_order_release);
        last_subtitle_render_start_pts_.store(request.timestamp_microseconds, std::memory_order_release);
        subtitle_render_thread_id_ = std::this_thread::get_id();
        const int active_count = active_render_count_;
        log_render_lifecycle(request, "subtitle render start", operation, active_count);
        return SessionAccessGuard(*this, operation, std::move(lock));
    }

    [[nodiscard]] AutoRenderResultHandle render_images_auto(
        const SubtitleRenderRequest &request,
        const SessionAccessGuard &access_guard
    ) const {
        enforce_owner_thread_locked("ass_render_frame_auto");
        int detect_change = 0;
        const auto timestamp_milliseconds =
            subtitle_timestamp_microseconds_to_renderer_milliseconds(request.timestamp_microseconds);
        return AutoRenderResultHandle(
            new ASS_RenderResult(with_optional_global_libassmod_lock([&]() {
                return ass_render_frame_auto(
                    access_guard.renderer(),
                    access_guard.track(),
                    static_cast<long long>(timestamp_milliseconds),
                    &detect_change
                );
            })),
            AutoRenderResultDeleter{
                .strict_same_thread_lifetime = strict_same_thread_lifetime_,
                .subtitle_owner_thread_id = subtitle_owner_thread_id_
            }
        );
    }

    SubtitleRenderSessionCreateRequest create_request_{};
    std::string subtitle_path_string_{};
    std::vector<std::string> quirk_messages_{};
    std::vector<SubtitleImageAsset> image_assets_{};
    LibraryHandle library_{};
    RendererHandle renderer_{};
    TrackHandle track_{};
    runtime::SubtitleRuntimeOptions runtime_options_{};
    int session_instance_id_{0};
    bool strict_same_thread_lifetime_{false};
    std::thread::id subtitle_owner_thread_id_{};
    std::thread::id subtitle_library_created_thread_id_{};
    std::thread::id subtitle_renderer_created_thread_id_{};
    std::thread::id subtitle_track_created_thread_id_{};
    std::thread::id subtitle_render_thread_id_{};
    std::thread::id subtitle_track_destroyed_thread_id_{};
    std::thread::id subtitle_renderer_destroyed_thread_id_{};
    std::thread::id subtitle_library_destroyed_thread_id_{};
    mutable std::mutex access_mutex_{};
    std::condition_variable access_available_{};
    int active_render_count_{0};
    bool render_in_progress_{false};
    std::atomic<int> active_subtitle_render_count_{0};
    std::atomic<std::int64_t> last_subtitle_render_start_pts_{0};
    std::atomic<std::int64_t> last_subtitle_render_end_pts_{0};
    std::atomic<bool> cleanup_started_{false};
    std::function<void(const std::string &)> lifecycle_callback_{};
    bool renderer_setup_diagnostics_logged_{false};
    bool quirk_diagnostics_logged_{false};
    bool off_frame_bitmap_warning_logged_{false};
    bool clipped_bitmap_warning_logged_{false};
};

class LibassmodSubtitleRenderer final : public SubtitleRenderer {
public:
    [[nodiscard]] SubtitleRenderSessionResult create_session(
        const SubtitleRenderSessionCreateRequest &request
    ) noexcept override {
        const std::lock_guard lock(subtitle_setup_mutex());
        return create_session_impl(request);
    }

private:
    [[nodiscard]] SubtitleRenderSessionResult create_session_impl(
        const SubtitleRenderSessionCreateRequest &request
    ) noexcept {
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
                        "Provide an existing directory of FontCollector-staged font files."
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
                        "Re-run the FontCollector subtitle font preparation step before creating the renderer session."
                    );
                }
            }

            auto library = create_library();
            configure_library_fonts(*library, request);
            auto renderer = create_renderer(*library, request);
            auto image_asset_result = load_subtitle_image_assets(normalized_path);
            if (!image_asset_result.succeeded()) {
                return make_session_error(
                    request,
                    image_asset_result.error->message,
                    image_asset_result.error->actionable_hint
                );
            }

            auto quirk_messages = image_asset_result.references.empty()
                ? std::vector<std::string>{}
                : std::move(image_asset_result.diagnostics);
            auto track = load_track(*library, request);

            auto session = std::make_unique<LibassmodSubtitleRenderSession>(
                request,
                path_to_utf8_string(normalized_path),
                std::move(quirk_messages),
                std::move(image_asset_result.assets),
                std::move(library),
                std::move(renderer),
                std::move(track)
            );
            const auto registration_result = session->register_session_image_assets();
            if (!registration_result.succeeded()) {
                return make_session_error(
                    request,
                    registration_result.error->message,
                    registration_result.error->actionable_hint
                );
            }

            return SubtitleRenderSessionResult{
                .session = std::move(session),
                .error = std::nullopt
            };
        } catch (const runtime_policy::RuntimeAnomalyError &exception) {
            return make_session_error(
                request,
                exception.what(),
                "classification=" + std::string(runtime_policy::to_string(exception.classification())),
                exception.classification()
            );
        } catch (const std::exception &exception) {
            return make_session_error(
                request,
                "libassmod raised an unclassified subtitle-session setup failure.",
                exception.what(),
                runtime_policy::RuntimeAnomalyClass::unsafe_or_corrupt
            );
        }
    }
};

}  // namespace

std::unique_ptr<SubtitleRenderer> create_default_subtitle_renderer() {
    return std::make_unique<LibassmodSubtitleRenderer>();
}

}  // namespace utsure::core::subtitles
