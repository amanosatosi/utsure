#pragma once

#include "utsure/core/media/media_encoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

namespace utsure::core::media::detail {

struct CodecThreadingSelection final {
    int thread_count{0};
    int thread_type{0};
};

inline std::uint32_t detect_logical_core_count() noexcept {
    return std::thread::hardware_concurrency();
}

inline std::uint32_t effective_logical_core_count(const std::uint32_t detected_logical_core_count) noexcept {
    return detected_logical_core_count > 0U ? detected_logical_core_count : 1U;
}

inline int sanitize_thread_override(const std::optional<int> thread_count_override) noexcept {
    if (!thread_count_override.has_value()) {
        return -1;
    }

    return std::max(*thread_count_override, 1);
}

inline int resolve_requested_ffmpeg_thread_count_impl(
    const CpuUsageMode mode,
    const bool allow_ffmpeg_auto_threads,
    const std::uint32_t logical_core_count
) noexcept {
    switch (mode) {
    case CpuUsageMode::auto_select:
        if (allow_ffmpeg_auto_threads) {
            return 0;
        }
        // Auto is the long-running GUI/batch default, so keep it explicit and bounded.
        return std::clamp(static_cast<int>(logical_core_count / 2U), 1, 4);
    case CpuUsageMode::conservative:
        return std::clamp(static_cast<int>(logical_core_count / 2U), 1, 3);
    case CpuUsageMode::aggressive:
        return std::max(1, static_cast<int>(logical_core_count > 0U ? (logical_core_count - 1U) : 1U));
    default:
        return 0;
    }
}

inline int resolve_requested_ffmpeg_thread_count_impl(
    const CpuUsageMode mode,
    const std::optional<int> thread_count_override,
    const bool allow_ffmpeg_auto_threads,
    const std::uint32_t logical_core_count
) noexcept {
    const int explicit_override = sanitize_thread_override(thread_count_override);
    return explicit_override > 0
        ? explicit_override
        : resolve_requested_ffmpeg_thread_count_impl(mode, allow_ffmpeg_auto_threads, logical_core_count);
}

inline int choose_codec_thread_type(const AVCodec &codec) noexcept {
    int thread_type = 0;
    if ((codec.capabilities & AV_CODEC_CAP_FRAME_THREADS) != 0) {
        thread_type |= FF_THREAD_FRAME;
    }

    if ((codec.capabilities & AV_CODEC_CAP_SLICE_THREADS) != 0) {
        thread_type |= FF_THREAD_SLICE;
    }

    return thread_type;
}

inline CodecThreadingSelection choose_decoder_threading(
    const TranscodeThreadingSettings &settings,
    const AVCodec &decoder,
    const std::uint32_t logical_core_count
) noexcept {
    return CodecThreadingSelection{
        .thread_count = sanitize_thread_override(settings.decoder_thread_count_override) > 0
            ? sanitize_thread_override(settings.decoder_thread_count_override)
            : resolve_requested_ffmpeg_thread_count_impl(
                settings.cpu_usage_mode,
                std::nullopt,
                settings.allow_ffmpeg_auto_threads,
                logical_core_count
            ),
        .thread_type = choose_codec_thread_type(decoder)
    };
}

inline CodecThreadingSelection choose_encoder_threading(
    const TranscodeThreadingSettings &settings,
    const AVCodec &encoder,
    const std::uint32_t logical_core_count
) noexcept {
    const int explicit_override = sanitize_thread_override(settings.encoder_thread_count_override);
    return CodecThreadingSelection{
        .thread_count = explicit_override > 0
            ? explicit_override
            : resolve_requested_ffmpeg_thread_count_impl(
                settings.cpu_usage_mode,
                std::nullopt,
                settings.allow_ffmpeg_auto_threads,
                logical_core_count
            ),
        .thread_type = choose_codec_thread_type(encoder)
    };
}

inline int active_codec_thread_type(const AVCodecContext &codec_context) noexcept {
    return codec_context.active_thread_type != 0 ? codec_context.active_thread_type : codec_context.thread_type;
}

inline std::string format_ffmpeg_thread_type_detail(const int thread_type) {
    const bool frame_threads = (thread_type & FF_THREAD_FRAME) != 0;
    const bool slice_threads = (thread_type & FF_THREAD_SLICE) != 0;
    if (frame_threads && slice_threads) {
        return "frame+slice";
    }

    if (frame_threads) {
        return "frame";
    }

    if (slice_threads) {
        return "slice";
    }

    return "none";
}

inline std::size_t choose_video_processing_worker_count(
    const TranscodeThreadingSettings &settings,
    const std::uint32_t logical_core_count
) noexcept {
    const std::uint32_t available_logical_core_count =
        effective_logical_core_count(settings.logical_core_count_override.value_or(logical_core_count));

    switch (settings.cpu_usage_mode) {
    case CpuUsageMode::conservative:
        return 1U;
    case CpuUsageMode::aggressive:
        // Decoder and encoder already fan out inside libavcodec, so keep the host-side frame workers bounded.
        return std::clamp<std::size_t>(std::max<std::uint32_t>(2U, available_logical_core_count / 4U), 1U, 4U);
    case CpuUsageMode::auto_select:
    default:
        return available_logical_core_count >= 8U ? 2U : 1U;
    }
}

}  // namespace utsure::core::media::detail
