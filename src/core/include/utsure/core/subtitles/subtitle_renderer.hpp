#pragma once

#include "utsure/core/media/media_info.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace utsure::core::subtitles {

enum class SubtitleBitmapPixelFormat : std::uint8_t {
    unknown = 0,
    rgba8_premultiplied
};

struct SubtitleRenderSessionCreateRequest final {
    std::filesystem::path subtitle_path{};
    std::string format_hint{"auto"};
    int canvas_width{0};
    int canvas_height{0};
    media::Rational sample_aspect_ratio{1, 1};
};

struct SubtitleRenderRequest final {
    std::int64_t timestamp_microseconds{0};
};

struct SubtitleBitmap final {
    int origin_x{0};
    int origin_y{0};
    int width{0};
    int height{0};
    SubtitleBitmapPixelFormat pixel_format{SubtitleBitmapPixelFormat::unknown};
    int line_stride_bytes{0};
    std::vector<std::uint8_t> bytes{};
};

struct RenderedSubtitleFrame final {
    std::int64_t timestamp_microseconds{0};
    int canvas_width{0};
    int canvas_height{0};
    std::vector<SubtitleBitmap> bitmaps{};
};

struct SubtitleRendererError final {
    std::string subtitle_path{};
    std::string message{};
    std::string actionable_hint{};
};

class SubtitleRenderSession;

struct SubtitleRenderSessionResult final {
    std::unique_ptr<SubtitleRenderSession> session{};
    std::optional<SubtitleRendererError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct SubtitleRenderResult final {
    std::optional<RenderedSubtitleFrame> rendered_frame{};
    std::optional<SubtitleRendererError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

class SubtitleRenderSession {
public:
    virtual ~SubtitleRenderSession() = default;

    [[nodiscard]] virtual SubtitleRenderResult render(const SubtitleRenderRequest &request) noexcept = 0;
};

class SubtitleRenderer {
public:
    virtual ~SubtitleRenderer() = default;

    [[nodiscard]] virtual SubtitleRenderSessionResult create_session(
        const SubtitleRenderSessionCreateRequest &request
    ) noexcept = 0;
};

[[nodiscard]] const char *to_string(SubtitleBitmapPixelFormat pixel_format) noexcept;

}  // namespace utsure::core::subtitles
