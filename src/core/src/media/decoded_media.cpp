#include "utsure/core/media/decoded_media.hpp"

namespace utsure::core::media {

const char *to_string(const NormalizedVideoPixelFormat pixel_format) noexcept {
    switch (pixel_format) {
    case NormalizedVideoPixelFormat::rgba8:
        return "rgba8";
    case NormalizedVideoPixelFormat::unknown:
    default:
        return "unknown";
    }
}

const char *to_string(const NormalizedAudioSampleFormat sample_format) noexcept {
    switch (sample_format) {
    case NormalizedAudioSampleFormat::f32_planar:
        return "f32_planar";
    case NormalizedAudioSampleFormat::unknown:
    default:
        return "unknown";
    }
}

const char *to_string(const TimestampOrigin timestamp_origin) noexcept {
    switch (timestamp_origin) {
    case TimestampOrigin::decoded_pts:
        return "decoded_pts";
    case TimestampOrigin::best_effort_pts:
        return "best_effort_pts";
    case TimestampOrigin::stream_cursor:
        return "stream_cursor";
    default:
        return "unknown";
    }
}

}  // namespace utsure::core::media
