#pragma once

#include "utsure/core/media/media_info.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace utsure::core::media::ffmpeg_support {

struct FormatContextDeleter final {
    void operator()(AVFormatContext *format_context) const noexcept;
};

using FormatContextHandle = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

[[nodiscard]] std::string ffmpeg_error_to_string(int error_code);
[[nodiscard]] Rational to_rational(AVRational value);
[[nodiscard]] std::optional<std::int64_t> to_optional_pts(std::int64_t value);
[[nodiscard]] std::string codec_name_from_parameters(const AVCodecParameters &parameters);
[[nodiscard]] std::string pixel_format_name_from_parameters(const AVCodecParameters &parameters);
[[nodiscard]] std::string sample_format_name_from_parameters(const AVCodecParameters &parameters);
[[nodiscard]] std::string channel_layout_name_from_parameters(const AVCodecParameters &parameters);
[[nodiscard]] std::string channel_layout_name_from_layout(const AVChannelLayout &channel_layout);
[[nodiscard]] TimestampInfo build_timestamp_info(const AVStream &stream);
[[nodiscard]] VideoStreamInfo build_video_stream_info(const AVStream &stream);
[[nodiscard]] AudioStreamInfo build_audio_stream_info(const AVStream &stream);
[[nodiscard]] MediaSourceInfo build_media_source_info(
    const std::filesystem::path &input_path,
    const AVFormatContext &format_context,
    int primary_video_stream_index
);

}  // namespace utsure::core::media::ffmpeg_support
