#include "utsure/core/subtitles/subtitle_renderer.hpp"
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

#ifndef UTSURE_LIBASSMOD_REF
#define UTSURE_LIBASSMOD_REF "unknown"
#endif

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

struct AutoRenderResultDeleter final {
    void operator()(ASS_RenderResult *result) const noexcept {
        if (result != nullptr) {
            if (result->imgs_rgba != nullptr) {
                ass_free_images_rgba(result->imgs_rgba);
                result->imgs_rgba = nullptr;
            }
            delete result;
        }
    }
};

using AutoRenderResultHandle = std::unique_ptr<ASS_RenderResult, AutoRenderResultDeleter>;

struct ScriptFeatureScan final {
    bool references_tag_images{false};
};

std::vector<std::string> build_script_feature_quirk_messages(const ScriptFeatureScan &scan) {
    std::vector<std::string> messages{};
    if (scan.references_tag_images) {
        messages.push_back(
            runtime_policy::format_operation_message(
                runtime_policy::RuntimeAnomalyClass::reduced_fidelity,
                "subtitle rendering",
                "script references \\img tags, but this build does not register host-side RGBA tag images; "
                "unsupported effect details may be skipped."
            )
        );
    }

    return messages;
}

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
            .subtitle_path = request.subtitle_path.lexically_normal().string(),
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

double choose_pixel_aspect_ratio(const media::Rational &sample_aspect_ratio) {
    if (!sample_aspect_ratio.is_valid() || sample_aspect_ratio.numerator <= 0 || sample_aspect_ratio.denominator <= 0) {
        return 1.0;
    }

    return static_cast<double>(sample_aspect_ratio.numerator) /
        static_cast<double>(sample_aspect_ratio.denominator);
}

std::string format_renderer_setup_diagnostics(
    const SubtitleRenderSessionCreateRequest &request
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
            << ", libassmod_ref=" << UTSURE_LIBASSMOD_REF;
    if (request.font_search_directory.has_value()) {
        message << ", font.directory=" << request.font_search_directory->lexically_normal().string();
    } else {
        message << ", font.directory=none";
    }

    return message.str();
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
        const ASS_Image &image = *image_nodes[bitmap_index];
        if (image.w <= 0 || image.h <= 0) {
            maybe_log_ass_image_collection_decision(request, image, bitmap_index, "rejected", "empty");
            continue;
        }

        if (image.stride <= 0 || image.stride < image.w || image.bitmap == nullptr) {
            maybe_log_ass_image_collection_decision(request, image, bitmap_index, "rejected", "unsafe");
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

        maybe_log_ass_image_collection_decision(request, image, bitmap_index, "accepted", "drawable");
        drawable_bitmaps.push_back(image_nodes[bitmap_index]);
    }

    return drawable_bitmaps;
}

bool should_use_rgba_images(const ASS_RenderResult &render_result) noexcept {
    return render_result.use_rgba != 0 && render_result.imgs_rgba != nullptr;
}

class LibassmodSubtitleRenderSession final : public SubtitleRenderSession {
public:
    LibassmodSubtitleRenderSession(
        SubtitleRenderSessionCreateRequest create_request,
        std::string subtitle_path_string,
        std::vector<std::string> quirk_messages,
        LibraryHandle library,
        RendererHandle renderer,
        TrackHandle track
    )
        : create_request_(std::move(create_request)),
          subtitle_path_string_(std::move(subtitle_path_string)),
          quirk_messages_(std::move(quirk_messages)),
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
            maybe_log_renderer_setup_diagnostics(request);
            maybe_log_quirk_diagnostics(request);
            auto render_result = render_images_auto(request);
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
            [[maybe_unused]] const auto access_guard = begin_session_access("compose");
            detail::validate_rgba_frame_surface(video_frame, "Subtitle composition");
            maybe_log_renderer_setup_diagnostics(request);
            maybe_log_quirk_diagnostics(request);

            auto render_result = render_images_auto(request);
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

        request.debug_context->log_callback(format_renderer_setup_diagnostics(create_request_));
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

    [[nodiscard]] AutoRenderResultHandle render_images_auto(const SubtitleRenderRequest &request) const {
        int detect_change = 0;
        const long long timestamp_milliseconds = static_cast<long long>(request.timestamp_microseconds / 1000);
        return AutoRenderResultHandle(
            new ASS_RenderResult(ass_render_frame_auto(renderer_.get(), track_.get(), timestamp_milliseconds, &detect_change))
        );
    }

    SubtitleRenderSessionCreateRequest create_request_{};
    std::string subtitle_path_string_{};
    std::vector<std::string> quirk_messages_{};
    LibraryHandle library_{};
    RendererHandle renderer_{};
    TrackHandle track_{};
    runtime::SubtitleRuntimeOptions runtime_options_{};
    int session_instance_id_{0};
    std::atomic<bool> in_use_{false};
    std::atomic<bool> destroyed_{false};
    bool renderer_setup_diagnostics_logged_{false};
    bool quirk_diagnostics_logged_{false};
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
            auto quirk_messages = build_script_feature_quirk_messages(script_feature_scan);

            auto track = load_track(*library, request);

            return SubtitleRenderSessionResult{
                .session = std::make_unique<LibassmodSubtitleRenderSession>(
                    request,
                    normalized_path.string(),
                    std::move(quirk_messages),
                    std::move(library),
                    std::move(renderer),
                    std::move(track)
                ),
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
