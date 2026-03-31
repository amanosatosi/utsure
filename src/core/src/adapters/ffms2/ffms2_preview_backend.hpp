#pragma once

#include "utsure/core/media/media_decoder.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace utsure::core::media::ffms2_preview {

struct VideoPreviewBackendCreateResult;
struct AudioPreviewBackendCreateResult;

[[nodiscard]] VideoPreviewBackendCreateResult create_video_preview_backend(
    const std::filesystem::path &input_path,
    const DecodeNormalizationPolicy &normalization_policy
) noexcept;

[[nodiscard]] AudioPreviewBackendCreateResult create_audio_preview_backend(
    const std::filesystem::path &input_path,
    const AudioPreviewOutputConfig &output_config,
    const DecodeNormalizationPolicy &normalization_policy
) noexcept;

class VideoPreviewBackend final {
public:
    struct Impl;

    VideoPreviewBackend(VideoPreviewBackend &&) noexcept;
    VideoPreviewBackend &operator=(VideoPreviewBackend &&) noexcept;
    ~VideoPreviewBackend();

    VideoPreviewBackend(const VideoPreviewBackend &) = delete;
    VideoPreviewBackend &operator=(const VideoPreviewBackend &) = delete;

    [[nodiscard]] VideoFrameWindowDecodeResult seek_and_decode_window_at_time(
        std::int64_t requested_time_microseconds,
        std::size_t maximum_frame_count
    ) noexcept;

    [[nodiscard]] VideoFrameWindowDecodeResult decode_next_window(std::size_t maximum_frame_count) noexcept;

private:
    explicit VideoPreviewBackend(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_{};

    friend VideoPreviewBackendCreateResult create_video_preview_backend(
        const std::filesystem::path &input_path,
        const DecodeNormalizationPolicy &normalization_policy
    ) noexcept;
    friend struct VideoPreviewBackendCreateResult;
};

class AudioPreviewBackend final {
public:
    struct Impl;

    AudioPreviewBackend(AudioPreviewBackend &&) noexcept;
    AudioPreviewBackend &operator=(AudioPreviewBackend &&) noexcept;
    ~AudioPreviewBackend();

    AudioPreviewBackend(const AudioPreviewBackend &) = delete;
    AudioPreviewBackend &operator=(const AudioPreviewBackend &) = delete;

    [[nodiscard]] AudioBlockWindowDecodeResult seek_and_decode_window_at_time(
        std::int64_t requested_time_microseconds,
        std::int64_t minimum_duration_microseconds
    ) noexcept;

    [[nodiscard]] AudioBlockWindowDecodeResult decode_next_window(
        std::int64_t minimum_duration_microseconds
    ) noexcept;

private:
    explicit AudioPreviewBackend(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_{};

    friend AudioPreviewBackendCreateResult create_audio_preview_backend(
        const std::filesystem::path &input_path,
        const AudioPreviewOutputConfig &output_config,
        const DecodeNormalizationPolicy &normalization_policy
    ) noexcept;
    friend struct AudioPreviewBackendCreateResult;
};

struct VideoPreviewBackendCreateResult final {
    std::unique_ptr<VideoPreviewBackend> backend{};
    std::optional<MediaDecodeError> error{};
    std::string diagnostics{};

    [[nodiscard]] bool succeeded() const noexcept {
        return backend != nullptr && !error.has_value();
    }
};

struct AudioPreviewBackendCreateResult final {
    std::unique_ptr<AudioPreviewBackend> backend{};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept {
        return backend != nullptr && !error.has_value();
    }
};

[[nodiscard]] std::filesystem::path preview_index_path_for_source(const std::filesystem::path &input_path);

}  // namespace utsure::core::media::ffms2_preview
