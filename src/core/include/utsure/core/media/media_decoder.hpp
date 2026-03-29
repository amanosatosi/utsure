#pragma once

#include "utsure/core/media/decoded_media.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace utsure::core::media {

struct MediaDecodeError final {
    std::string input_path{};
    std::string message{};
    std::string actionable_hint{};
};

struct MediaDecodeResult final {
    std::optional<DecodedMediaSource> decoded_media_source{};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct VideoFrameDecodeResult final {
    std::optional<DecodedVideoFrame> video_frame{};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct VideoFrameWindowDecodeResult final {
    std::optional<std::vector<DecodedVideoFrame>> video_frames{};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct AudioBlockWindowDecodeResult final {
    std::optional<std::vector<DecodedAudioSamples>> audio_blocks{};
    bool exhausted{false};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

class VideoPreviewSession;
class AudioPreviewSession;

struct VideoPreviewSessionCreateResult final {
    std::unique_ptr<VideoPreviewSession> session{};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct AudioPreviewSessionCreateResult final {
    std::unique_ptr<AudioPreviewSession> session{};
    std::optional<MediaDecodeError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct DecodeStreamSelection final {
    bool decode_video{true};
    bool decode_audio{true};
};

struct AudioPreviewOutputConfig final {
    int sample_rate_hz{0};
    int channel_count{0};
};

class VideoPreviewSession final {
public:
    struct Impl;

    VideoPreviewSession(VideoPreviewSession &&) noexcept;
    VideoPreviewSession &operator=(VideoPreviewSession &&) noexcept;
    ~VideoPreviewSession();

    VideoPreviewSession(const VideoPreviewSession &) = delete;
    VideoPreviewSession &operator=(const VideoPreviewSession &) = delete;

    [[nodiscard]] VideoFrameWindowDecodeResult seek_and_decode_window_at_time(
        std::int64_t requested_time_microseconds,
        std::size_t maximum_frame_count
    ) noexcept;

    [[nodiscard]] VideoFrameWindowDecodeResult decode_next_window(std::size_t maximum_frame_count) noexcept;

private:
    explicit VideoPreviewSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_{};

    friend class MediaDecoder;
};

class AudioPreviewSession final {
public:
    struct Impl;

    AudioPreviewSession(AudioPreviewSession &&) noexcept;
    AudioPreviewSession &operator=(AudioPreviewSession &&) noexcept;
    ~AudioPreviewSession();

    AudioPreviewSession(const AudioPreviewSession &) = delete;
    AudioPreviewSession &operator=(const AudioPreviewSession &) = delete;

    [[nodiscard]] AudioBlockWindowDecodeResult seek_and_decode_window_at_time(
        std::int64_t requested_time_microseconds,
        std::int64_t minimum_duration_microseconds
    ) noexcept;

    [[nodiscard]] AudioBlockWindowDecodeResult decode_next_window(
        std::int64_t minimum_duration_microseconds
    ) noexcept;

private:
    explicit AudioPreviewSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_{};

    friend class MediaDecoder;
};

class MediaDecoder final {
public:
    [[nodiscard]] static MediaDecodeResult decode(
        const std::filesystem::path &input_path,
        const DecodeNormalizationPolicy &normalization_policy = {},
        const DecodeStreamSelection &stream_selection = {}
    ) noexcept;

    [[nodiscard]] static VideoFrameDecodeResult decode_video_frame_at_time(
        const std::filesystem::path &input_path,
        std::int64_t requested_time_microseconds,
        const DecodeNormalizationPolicy &normalization_policy = {}
    ) noexcept;

    [[nodiscard]] static VideoFrameWindowDecodeResult decode_video_frame_window_at_time(
        const std::filesystem::path &input_path,
        std::int64_t requested_time_microseconds,
        std::size_t maximum_frame_count,
        const DecodeNormalizationPolicy &normalization_policy = {}
    ) noexcept;

    [[nodiscard]] static VideoPreviewSessionCreateResult create_video_preview_session(
        const std::filesystem::path &input_path,
        const DecodeNormalizationPolicy &normalization_policy = {}
    ) noexcept;

    [[nodiscard]] static AudioPreviewSessionCreateResult create_audio_preview_session(
        const std::filesystem::path &input_path,
        const AudioPreviewOutputConfig &output_config = {},
        const DecodeNormalizationPolicy &normalization_policy = {}
    ) noexcept;
};

}  // namespace utsure::core::media
