#include "utsure/core/media/preview_trim.hpp"

#include <libavutil/avutil.h>
#include <libavutil/rational.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <stdexcept>

namespace utsure::core::media {
namespace {

AVRational to_av_rational(const Rational &value) {
    return AVRational{value.numerator, value.denominator};
}

std::int64_t frame_coverage_end_us(const DecodedVideoFrame &frame) {
    return frame.timestamp.duration_microseconds.has_value()
        ? frame.timestamp.start_microseconds + *frame.timestamp.duration_microseconds
        : frame.timestamp.start_microseconds;
}

std::int64_t audio_block_end_us(const DecodedAudioSamples &audio_block) {
    if (audio_block.timestamp.duration_microseconds.has_value()) {
        return audio_block.timestamp.start_microseconds + *audio_block.timestamp.duration_microseconds;
    }

    if (audio_block.sample_rate <= 0 || audio_block.samples_per_channel <= 0) {
        return audio_block.timestamp.start_microseconds;
    }

    return audio_block.timestamp.start_microseconds + av_rescale_q(
               audio_block.samples_per_channel,
               AVRational{1, audio_block.sample_rate},
               AV_TIME_BASE_Q
           );
}

std::int64_t effective_preview_video_selection_time(
    const std::int64_t requested_time_microseconds,
    const PreviewTrimRange &trim_range
) {
    std::int64_t effective_time = std::max<std::int64_t>(requested_time_microseconds, trim_range.trim_in_microseconds);
    if (trim_range.trim_out_microseconds.has_value() &&
        effective_time >= *trim_range.trim_out_microseconds) {
        effective_time = std::max<std::int64_t>(
            trim_range.trim_in_microseconds,
            *trim_range.trim_out_microseconds - 1
        );
    }

    return effective_time;
}

std::vector<std::vector<float>> copy_audio_block_range(
    const std::vector<std::vector<float>> &channel_samples,
    const int start_sample_index,
    const int samples_per_channel
) {
    if (start_sample_index < 0 || samples_per_channel < 0) {
        throw std::runtime_error("Preview trim encountered an invalid trimmed audio sample range.");
    }

    std::vector<std::vector<float>> slice{};
    slice.reserve(channel_samples.size());
    for (const auto &channel : channel_samples) {
        const auto start = static_cast<std::size_t>(start_sample_index);
        const auto end = static_cast<std::size_t>(start_sample_index + samples_per_channel);
        if (end > channel.size()) {
            throw std::runtime_error("Preview trim encountered a truncated normalized audio block.");
        }

        slice.emplace_back(
            channel.begin() + static_cast<std::ptrdiff_t>(start),
            channel.begin() + static_cast<std::ptrdiff_t>(end)
        );
    }

    return slice;
}

}  // namespace

PreviewTrimRange normalize_preview_trim_range(
    const std::int64_t trim_in_microseconds,
    const std::optional<std::int64_t> trim_out_microseconds
) {
    PreviewTrimRange trim_range{};
    trim_range.trim_in_microseconds = std::max<std::int64_t>(trim_in_microseconds, 0);
    if (trim_out_microseconds.has_value()) {
        trim_range.trim_out_microseconds = std::max(*trim_out_microseconds, trim_range.trim_in_microseconds);
    }

    return trim_range;
}

std::int64_t effective_trimmed_preview_frame_time(
    const std::int64_t requested_time_microseconds,
    const PreviewTrimRange &trim_range
) {
    return effective_preview_video_selection_time(requested_time_microseconds, trim_range);
}

std::vector<DecodedVideoFrame> trim_preview_video_frames(
    std::vector<DecodedVideoFrame> frames,
    const PreviewTrimRange &trim_range
) {
    if (!trim_range.has_trim()) {
        return frames;
    }

    frames.erase(
        std::remove_if(
            frames.begin(),
            frames.end(),
            [&trim_range](const DecodedVideoFrame &frame) {
                if (frame.timestamp.start_microseconds < trim_range.trim_in_microseconds) {
                    return true;
                }

                return trim_range.trim_out_microseconds.has_value() &&
                    frame.timestamp.start_microseconds >= *trim_range.trim_out_microseconds;
            }
        ),
        frames.end()
    );

    if (trim_range.trim_out_microseconds.has_value()) {
        for (auto it = frames.begin(); it != frames.end();) {
            const auto trimmed_end_us = std::min(frame_coverage_end_us(*it), *trim_range.trim_out_microseconds);
            if (trimmed_end_us <= it->timestamp.start_microseconds) {
                it = frames.erase(it);
                continue;
            }

            if (it->timestamp.duration_microseconds.has_value()) {
                it->timestamp.duration_microseconds = trimmed_end_us - it->timestamp.start_microseconds;
            }
            ++it;
        }
    }

    return frames;
}

bool trimmed_preview_frames_cover_time(
    const std::vector<DecodedVideoFrame> &frames,
    const std::int64_t requested_time_microseconds,
    const PreviewTrimRange &trim_range
) {
    if (frames.empty()) {
        return false;
    }

    const auto effective_time = effective_preview_video_selection_time(requested_time_microseconds, trim_range);
    const auto &first_frame = frames.front();
    const auto &last_frame = frames.back();

    if (trim_range.has_trim() &&
        effective_time >= trim_range.trim_in_microseconds &&
        effective_time <= first_frame.timestamp.start_microseconds) {
        return true;
    }

    return effective_time >= first_frame.timestamp.start_microseconds &&
        effective_time < frame_coverage_end_us(last_frame);
}

const DecodedVideoFrame *select_trimmed_preview_frame(
    const std::vector<DecodedVideoFrame> &frames,
    const std::int64_t requested_time_microseconds,
    const PreviewTrimRange &trim_range
) {
    if (frames.empty()) {
        return nullptr;
    }

    const auto effective_time = effective_preview_video_selection_time(requested_time_microseconds, trim_range);
    const auto upper_bound = std::upper_bound(
        frames.begin(),
        frames.end(),
        effective_time,
        [](const std::int64_t timestamp_microseconds, const DecodedVideoFrame &frame) {
            return timestamp_microseconds < frame.timestamp.start_microseconds;
        }
    );

    if (upper_bound == frames.begin()) {
        return &frames.front();
    }

    const auto &previous_frame = *(upper_bound - 1);
    if (effective_time < frame_coverage_end_us(previous_frame)) {
        return &previous_frame;
    }

    if (upper_bound == frames.end()) {
        return nullptr;
    }

    return &(*upper_bound);
}

std::vector<DecodedAudioSamples> trim_preview_audio_blocks(
    std::vector<DecodedAudioSamples> audio_blocks,
    const PreviewTrimRange &trim_range
) {
    if (!trim_range.has_trim()) {
        return audio_blocks;
    }

    std::vector<DecodedAudioSamples> trimmed_blocks{};
    trimmed_blocks.reserve(audio_blocks.size());

    for (auto &audio_block : audio_blocks) {
        if (audio_block.sample_rate <= 0 || audio_block.samples_per_channel <= 0) {
            continue;
        }

        const auto sample_time_base = Rational{
            .numerator = 1,
            .denominator = audio_block.sample_rate
        };
        const auto block_start_us = audio_block.timestamp.start_microseconds;
        const auto block_end_us = audio_block_end_us(audio_block);
        const auto overlap_start_us = std::max(block_start_us, trim_range.trim_in_microseconds);
        const auto overlap_end_us = trim_range.trim_out_microseconds.has_value()
            ? std::min(block_end_us, *trim_range.trim_out_microseconds)
            : block_end_us;
        if (overlap_end_us <= overlap_start_us) {
            continue;
        }

        int sample_offset = static_cast<int>(av_rescale_q_rnd(
            overlap_start_us - block_start_us,
            AV_TIME_BASE_Q,
            to_av_rational(sample_time_base),
            AV_ROUND_UP
        ));
        sample_offset = std::clamp(sample_offset, 0, audio_block.samples_per_channel);

        const auto overlap_end_sample_index = av_rescale_q_rnd(
            overlap_end_us - block_start_us,
            AV_TIME_BASE_Q,
            to_av_rational(sample_time_base),
            AV_ROUND_DOWN
        );
        int emitted_samples = static_cast<int>(std::max<std::int64_t>(
            overlap_end_sample_index - sample_offset,
            0
        ));
        emitted_samples = std::min(emitted_samples, audio_block.samples_per_channel - sample_offset);
        if (emitted_samples <= 0) {
            continue;
        }

        audio_block.channel_samples = copy_audio_block_range(
            audio_block.channel_samples,
            sample_offset,
            emitted_samples
        );
        audio_block.samples_per_channel = emitted_samples;
        audio_block.timestamp.start_microseconds = block_start_us + av_rescale_q(
                                                       sample_offset,
                                                       to_av_rational(sample_time_base),
                                                       AV_TIME_BASE_Q
                                                   );
        audio_block.timestamp.duration_microseconds = av_rescale_q(
            emitted_samples,
            to_av_rational(sample_time_base),
            AV_TIME_BASE_Q
        );
        if (audio_block.timestamp.source_pts.has_value() &&
            audio_block.timestamp.source_time_base.is_valid()) {
            audio_block.timestamp.source_pts = *audio_block.timestamp.source_pts + av_rescale_q(
                                                  sample_offset,
                                                  to_av_rational(sample_time_base),
                                                  to_av_rational(audio_block.timestamp.source_time_base)
                                              );
        }
        if (audio_block.timestamp.source_duration.has_value() &&
            audio_block.timestamp.source_time_base.is_valid()) {
            audio_block.timestamp.source_duration = av_rescale_q(
                emitted_samples,
                to_av_rational(sample_time_base),
                to_av_rational(audio_block.timestamp.source_time_base)
            );
        }

        trimmed_blocks.push_back(std::move(audio_block));
    }

    return trimmed_blocks;
}

}  // namespace utsure::core::media
