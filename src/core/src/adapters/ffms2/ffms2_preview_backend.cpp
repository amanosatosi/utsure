#include "ffms2_preview_backend.hpp"

#include "../../media/ffmpeg_media_support.hpp"

extern "C" {
#include <ffms.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace utsure::core::media::ffms2_preview {

namespace {

using ffmpeg_support::ffmpeg_error_to_string;

constexpr std::size_t kFfmsErrorBufferSize = 1024;
constexpr std::int64_t kMicrosecondsPerSecond = 1000000;
constexpr std::int64_t kAudioPrerollUs = 100000;
constexpr std::int64_t kAudioWindowMarginUs = 500000;

struct FfmsErrorBuffer final {
    std::array<char, kFfmsErrorBufferSize> storage{};
    FFMS_ErrorInfo info{};

    FfmsErrorBuffer() {
        info.ErrorType = FFMS_ERROR_SUCCESS;
        info.SubType = FFMS_ERROR_SUCCESS;
        info.BufferSize = static_cast<int>(storage.size());
        info.Buffer = storage.data();
        storage.front() = '\0';
    }

    [[nodiscard]] std::string message() const {
        return std::string(storage.data());
    }
};

struct IndexDeleter final {
    void operator()(FFMS_Index *index) const noexcept {
        if (index != nullptr) {
            FFMS_DestroyIndex(index);
        }
    }
};

struct VideoSourceDeleter final {
    void operator()(FFMS_VideoSource *source) const noexcept {
        if (source != nullptr) {
            FFMS_DestroyVideoSource(source);
        }
    }
};

struct AudioSourceDeleter final {
    void operator()(FFMS_AudioSource *source) const noexcept {
        if (source != nullptr) {
            FFMS_DestroyAudioSource(source);
        }
    }
};

struct ResampleOptionsDeleter final {
    void operator()(FFMS_ResampleOptions *options) const noexcept {
        if (options != nullptr) {
            FFMS_DestroyResampleOptions(options);
        }
    }
};

struct SwrContextDeleter final {
    void operator()(SwrContext *resample_context) const noexcept {
        if (resample_context != nullptr) {
            swr_free(&resample_context);
        }
    }
};

using IndexHandle = std::unique_ptr<FFMS_Index, IndexDeleter>;
using VideoSourceHandle = std::unique_ptr<FFMS_VideoSource, VideoSourceDeleter>;
using AudioSourceHandle = std::unique_ptr<FFMS_AudioSource, AudioSourceDeleter>;
using ResampleOptionsHandle = std::unique_ptr<FFMS_ResampleOptions, ResampleOptionsDeleter>;
using SwrContextHandle = std::unique_ptr<SwrContext, SwrContextDeleter>;

struct PreviewIndexLoadResult final {
    IndexHandle index{};
};

struct FrameTiming final {
    std::int64_t source_pts{0};
    std::optional<std::int64_t> source_duration_pts{};
    std::int64_t start_microseconds{0};
    std::int64_t duration_microseconds{0};
    bool key_frame{false};
};

struct AudioResamplePlan final {
    int source_sample_rate{0};
    int source_channel_count{0};
    std::uint64_t source_channel_layout_mask{0};
    int output_sample_rate{0};
    int output_channel_count{0};
    std::uint64_t output_channel_layout_mask{0};
    bool needs_resample{false};
    std::string output_channel_layout_name{"unknown"};
};

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path) {
#ifdef _WIN32
    const auto utf8_text = path.u8string();
    return std::string(reinterpret_cast<const char *>(utf8_text.c_str()), utf8_text.size());
#else
    return path.string();
#endif
}

[[nodiscard]] std::string display_path_string(const std::filesystem::path &path) {
    return path.lexically_normal().string();
}

[[nodiscard]] std::string stable_source_identity(const std::filesystem::path &input_path) {
    std::error_code error_code;
    auto normalized_path = std::filesystem::weakly_canonical(input_path, error_code);
    if (error_code) {
        normalized_path = input_path.lexically_normal();
    }

    auto identity = path_to_utf8(normalized_path);
#ifdef _WIN32
    std::transform(identity.begin(), identity.end(), identity.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
#endif
    return identity;
}

[[nodiscard]] std::uint64_t fnv1a_64(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : text) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

[[nodiscard]] std::string hex_string(const std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int index = 15; index >= 0; --index) {
        text[static_cast<std::size_t>(index)] = kDigits[(value >> ((15 - index) * 4)) & 0x0f];
    }
    return text;
}

[[nodiscard]] std::filesystem::path preview_cache_root() {
#ifdef _WIN32
    if (const char *local_app_data = std::getenv("LOCALAPPDATA");
        local_app_data != nullptr && local_app_data[0] != '\0') {
        return std::filesystem::path(local_app_data) / "utsure" / "ffms2-preview-indexes";
    }
#endif

    if (const char *xdg_cache_home = std::getenv("XDG_CACHE_HOME");
        xdg_cache_home != nullptr && xdg_cache_home[0] != '\0') {
        return std::filesystem::path(xdg_cache_home) / "utsure" / "ffms2-preview-indexes";
    }

    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".cache" / "utsure" / "ffms2-preview-indexes";
    }

    return std::filesystem::temp_directory_path() / "utsure" / "ffms2-preview-indexes";
}

void initialize_ffms() {
    static std::once_flag once_flag{};
    std::call_once(once_flag, []() {
        FFMS_Init(0, 0);
        FFMS_SetLogLevel(FFMS_LOG_QUIET);
    });
}

[[nodiscard]] std::filesystem::path make_preview_index_temp_path(const std::filesystem::path &final_path) {
    return final_path.parent_path() /
        (final_path.filename().string() + ".tmp-" + std::to_string(std::rand()));
}

void write_index_atomically(
    const std::filesystem::path &index_path,
    FFMS_Index &index
) {
    std::error_code error_code;
    std::filesystem::create_directories(index_path.parent_path(), error_code);
    if (error_code) {
        return;
    }

    const auto temporary_path = make_preview_index_temp_path(index_path);
    FfmsErrorBuffer error_buffer{};
    if (FFMS_WriteIndex(path_to_utf8(temporary_path).c_str(), &index, &error_buffer.info) != FFMS_ERROR_SUCCESS) {
        std::filesystem::remove(temporary_path, error_code);
        return;
    }

    std::filesystem::remove(index_path, error_code);
    error_code.clear();
    std::filesystem::rename(temporary_path, index_path, error_code);
    if (error_code) {
        std::filesystem::remove(temporary_path, error_code);
    }
}

[[nodiscard]] PreviewIndexLoadResult load_or_build_preview_index(const std::filesystem::path &input_path) {
    initialize_ffms();

    const auto source_path_utf8 = path_to_utf8(input_path);
    const auto index_path = preview_index_path_for_source(input_path);
    FfmsErrorBuffer error_buffer{};

    if (std::filesystem::exists(index_path)) {
        IndexHandle cached_index(FFMS_ReadIndex(path_to_utf8(index_path).c_str(), &error_buffer.info));
        if (cached_index != nullptr) {
            error_buffer = FfmsErrorBuffer{};
            if (FFMS_IndexBelongsToFile(cached_index.get(), source_path_utf8.c_str(), &error_buffer.info) ==
                FFMS_ERROR_SUCCESS) {
                return PreviewIndexLoadResult{
                    .index = std::move(cached_index)
                };
            }
        }

        std::error_code remove_error;
        std::filesystem::remove(index_path, remove_error);
    }

    FFMS_Indexer *indexer = FFMS_CreateIndexer(source_path_utf8.c_str(), &error_buffer.info);
    if (indexer == nullptr) {
        throw std::runtime_error("FFMS2 could not create a preview indexer: " + error_buffer.message());
    }

    FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_VIDEO, 1, 0);
    FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_AUDIO, 1, 0);
    FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_DATA, 0, 0);
    FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_SUBTITLE, 0, 0);
    FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_ATTACHMENT, 0, 0);

    error_buffer = FfmsErrorBuffer{};
    IndexHandle built_index(FFMS_DoIndexing2(indexer, FFMS_IEH_ABORT, &error_buffer.info));
    if (built_index == nullptr) {
        throw std::runtime_error("FFMS2 preview indexing failed: " + error_buffer.message());
    }

    write_index_atomically(index_path, *built_index);
    return PreviewIndexLoadResult{
        .index = std::move(built_index)
    };
}

[[nodiscard]] int popcount_layout_mask(const std::uint64_t channel_layout_mask) {
    return std::popcount(channel_layout_mask);
}

[[nodiscard]] std::uint64_t default_channel_layout_mask(const int channel_count) {
    AVChannelLayout layout{};
    if (channel_count <= 0) {
        return 0;
    }

    av_channel_layout_default(&layout, channel_count);

    const std::uint64_t mask =
        layout.order == AV_CHANNEL_ORDER_NATIVE ? layout.u.mask : static_cast<std::uint64_t>(0);
    av_channel_layout_uninit(&layout);
    return mask;
}

[[nodiscard]] AVChannelLayout av_channel_layout_from_mask(
    const std::uint64_t channel_layout_mask,
    const int channel_count
) {
    AVChannelLayout layout{};
    if (channel_layout_mask != 0 && popcount_layout_mask(channel_layout_mask) == channel_count) {
        layout.order = AV_CHANNEL_ORDER_NATIVE;
        layout.nb_channels = channel_count;
        layout.u.mask = channel_layout_mask;
        return layout;
    }

    if (channel_count > 0) {
        av_channel_layout_default(&layout, channel_count);
    }

    if (layout.nb_channels > 0) {
        return layout;
    }

    layout.order = AV_CHANNEL_ORDER_UNSPEC;
    layout.nb_channels = std::max(channel_count, 0);
    layout.u.mask = 0;
    return layout;
}

[[nodiscard]] std::string describe_channel_layout(
    const std::uint64_t channel_layout_mask,
    const int channel_count
) {
    AVChannelLayout layout = av_channel_layout_from_mask(channel_layout_mask, channel_count);
    if (layout.nb_channels <= 0) {
        return "unknown";
    }

    char description[128]{};
    const int describe_result = av_channel_layout_describe(&layout, description, sizeof(description));
    av_channel_layout_uninit(&layout);
    if (describe_result < 0) {
        return "unknown";
    }

    return std::string(description);
}

[[nodiscard]] AVRational ffms_time_base_to_av_rational(const FFMS_TrackTimeBase &time_base) {
    return AVRational{
        .num = static_cast<int>(time_base.Num),
        .den = static_cast<int>(time_base.Den * 1000)
    };
}

[[nodiscard]] Rational ffms_time_base_to_rational(const FFMS_TrackTimeBase &time_base) {
    return Rational{
        .numerator = time_base.Num,
        .denominator = time_base.Den * 1000
    };
}

[[nodiscard]] std::int64_t pts_to_microseconds(const std::int64_t pts, const FFMS_TrackTimeBase &time_base) {
    return av_rescale_q(pts, ffms_time_base_to_av_rational(time_base), AV_TIME_BASE_Q);
}

[[nodiscard]] std::int64_t seconds_to_microseconds(const double seconds) {
    return static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kMicrosecondsPerSecond)));
}

[[nodiscard]] std::int64_t duration_to_samples(const std::int64_t duration_microseconds, const int sample_rate) {
    if (duration_microseconds <= 0 || sample_rate <= 0) {
        return 0;
    }

    return std::max<std::int64_t>(
        av_rescale_rnd(duration_microseconds, sample_rate, kMicrosecondsPerSecond, AV_ROUND_UP),
        1
    );
}

[[nodiscard]] std::int64_t samples_to_microseconds(const std::int64_t sample_count, const int sample_rate) {
    if (sample_count <= 0 || sample_rate <= 0) {
        return 0;
    }

    return av_rescale_q(sample_count, AVRational{1, sample_rate}, AV_TIME_BASE_Q);
}

[[nodiscard]] std::pair<int, int> choose_normalized_video_dimensions(
    const int source_width,
    const int source_height,
    const DecodeNormalizationPolicy &normalization_policy
) {
    if (source_width <= 0 || source_height <= 0) {
        throw std::runtime_error("The preview source did not expose valid frame dimensions.");
    }

    const int max_width = normalization_policy.video_max_width;
    const int max_height = normalization_policy.video_max_height;
    if (max_width <= 0 && max_height <= 0) {
        return {source_width, source_height};
    }

    const double width_scale = max_width > 0
        ? static_cast<double>(max_width) / static_cast<double>(source_width)
        : 1.0;
    const double height_scale = max_height > 0
        ? static_cast<double>(max_height) / static_cast<double>(source_height)
        : 1.0;
    const double scale = std::min(width_scale, height_scale);
    if (scale >= 1.0) {
        return {source_width, source_height};
    }

    return {
        std::max(1, static_cast<int>(std::lround(static_cast<double>(source_width) * scale))),
        std::max(1, static_cast<int>(std::lround(static_cast<double>(source_height) * scale)))
    };
}

[[nodiscard]] std::vector<FrameTiming> build_frame_timings(
    FFMS_Track &track,
    const FFMS_TrackTimeBase &time_base,
    const FFMS_VideoProperties &video_properties
) {
    const int frame_count = FFMS_GetNumFrames(&track);
    if (frame_count <= 0) {
        throw std::runtime_error("The selected source does not expose any FFMS2 preview frames.");
    }

    std::vector<FrameTiming> timings(static_cast<std::size_t>(frame_count));
    const auto *first_info = FFMS_GetFrameInfo(&track, 0);
    if (first_info == nullptr) {
        throw std::runtime_error("FFMS2 did not expose metadata for the first preview frame.");
    }

    const std::int64_t timeline_zero_us = pts_to_microseconds(first_info->PTS, time_base);
    std::int64_t fallback_duration_us = 0;
    if (video_properties.FPSNumerator > 0 && video_properties.FPSDenominator > 0) {
        fallback_duration_us = av_rescale_q(
            1,
            AVRational{video_properties.FPSDenominator, video_properties.FPSNumerator},
            AV_TIME_BASE_Q
        );
    }

    for (int index = 0; index < frame_count; ++index) {
        const auto *frame_info = FFMS_GetFrameInfo(&track, index);
        if (frame_info == nullptr) {
            throw std::runtime_error("FFMS2 did not expose metadata for preview frame " + std::to_string(index) + ".");
        }

        const std::int64_t absolute_start_us = pts_to_microseconds(frame_info->PTS, time_base);
        auto &timing = timings[static_cast<std::size_t>(index)];
        timing.source_pts = frame_info->PTS;
        timing.start_microseconds = absolute_start_us - timeline_zero_us;
        timing.key_frame = frame_info->KeyFrame != 0;

        if (index + 1 < frame_count) {
            const auto *next_frame_info = FFMS_GetFrameInfo(&track, index + 1);
            if (next_frame_info != nullptr && next_frame_info->PTS > frame_info->PTS) {
                timing.source_duration_pts = next_frame_info->PTS - frame_info->PTS;
                timing.duration_microseconds = pts_to_microseconds(*timing.source_duration_pts, time_base);
            }
        }
    }

    const std::int64_t video_end_us = video_properties.LastEndTime > 0.0
        ? seconds_to_microseconds(video_properties.LastEndTime) - timeline_zero_us
        : 0;

    for (std::size_t index = 0; index < timings.size(); ++index) {
        auto &timing = timings[index];
        if (timing.duration_microseconds > 0) {
            continue;
        }

        if (index + 1 < timings.size()) {
            const auto candidate = timings[index + 1].start_microseconds - timing.start_microseconds;
            if (candidate > 0) {
                timing.duration_microseconds = candidate;
                continue;
            }
        }

        if (index + 1 == timings.size() && video_end_us > timing.start_microseconds) {
            const auto candidate = video_end_us - timing.start_microseconds;
            if (candidate > 0) {
                timing.duration_microseconds = candidate;
                continue;
            }
        }

        timing.duration_microseconds = std::max<std::int64_t>(fallback_duration_us, 1);
    }

    return timings;
}

[[nodiscard]] std::size_t find_preview_frame_index_for_time(
    const std::vector<FrameTiming> &timings,
    const std::int64_t requested_time_microseconds
) {
    if (timings.empty()) {
        return 0;
    }

    const auto upper_bound = std::upper_bound(
        timings.begin(),
        timings.end(),
        requested_time_microseconds,
        [](const std::int64_t requested_time, const FrameTiming &timing) {
            return requested_time < timing.start_microseconds;
        }
    );

    if (upper_bound == timings.begin()) {
        return 0;
    }

    const std::size_t previous_index = static_cast<std::size_t>(std::distance(timings.begin(), upper_bound) - 1);
    if (requested_time_microseconds <
        timings[previous_index].start_microseconds + timings[previous_index].duration_microseconds) {
        return previous_index;
    }

    if (upper_bound == timings.end()) {
        return timings.size() - 1;
    }

    return static_cast<std::size_t>(std::distance(timings.begin(), upper_bound));
}

}  // namespace

struct VideoPreviewBackend::Impl final {
    std::filesystem::path input_path{};
    std::string input_path_string{};
    DecodeNormalizationPolicy normalization_policy{};
    IndexHandle index{};
    VideoSourceHandle video_source{};
    FFMS_Track *video_track{nullptr};
    FFMS_TrackTimeBase video_time_base{};
    Rational sample_aspect_ratio{1, 1};
    int stream_index{-1};
    int frame_count{0};
    std::vector<FrameTiming> frame_timings{};
    std::size_t next_frame_index{0};
};

struct AudioPreviewBackend::Impl final {
    std::filesystem::path input_path{};
    std::string input_path_string{};
    DecodeNormalizationPolicy normalization_policy{};
    AudioPreviewOutputConfig output_config{};
    IndexHandle index{};
    AudioSourceHandle audio_source{};
    int stream_index{-1};
    std::int64_t total_source_samples{0};
    std::int64_t total_output_samples{0};
    std::int64_t next_output_sample_index{0};
    int block_samples{1024};
    AudioResamplePlan resample_plan{};
};

namespace {

[[nodiscard]] std::vector<std::vector<float>> deinterleave_float_samples(
    const std::vector<float> &interleaved_samples,
    const int channel_count
) {
    if (channel_count <= 0 || interleaved_samples.empty()) {
        return {};
    }

    const auto samples_per_channel = static_cast<int>(interleaved_samples.size() / static_cast<std::size_t>(channel_count));
    std::vector<std::vector<float>> channel_samples(
        static_cast<std::size_t>(channel_count),
        std::vector<float>(static_cast<std::size_t>(samples_per_channel), 0.0f)
    );

    for (int sample_index = 0; sample_index < samples_per_channel; ++sample_index) {
        for (int channel_index = 0; channel_index < channel_count; ++channel_index) {
            channel_samples[static_cast<std::size_t>(channel_index)][static_cast<std::size_t>(sample_index)] =
                interleaved_samples[static_cast<std::size_t>(sample_index * channel_count + channel_index)];
        }
    }

    return channel_samples;
}

[[nodiscard]] std::vector<float> read_ffms_float_audio_samples(
    FFMS_AudioSource &audio_source,
    const int channel_count,
    const std::int64_t start_sample,
    const std::int64_t sample_count
) {
    if (sample_count <= 0 || channel_count <= 0) {
        return {};
    }

    std::vector<float> interleaved_samples(
        static_cast<std::size_t>(sample_count) * static_cast<std::size_t>(channel_count),
        0.0f
    );
    FfmsErrorBuffer error_buffer{};
    if (FFMS_GetAudio(
            &audio_source,
            interleaved_samples.data(),
            start_sample,
            sample_count,
            &error_buffer.info
        ) != FFMS_ERROR_SUCCESS) {
        throw std::runtime_error("FFMS2 could not decode preview audio samples: " + error_buffer.message());
    }

    return interleaved_samples;
}

[[nodiscard]] SwrContextHandle make_audio_resample_context(const AudioResamplePlan &plan) {
    SwrContext *raw_context = nullptr;

    AVChannelLayout source_layout = av_channel_layout_from_mask(
        plan.source_channel_layout_mask,
        plan.source_channel_count
    );
    AVChannelLayout output_layout = av_channel_layout_from_mask(
        plan.output_channel_layout_mask,
        plan.output_channel_count
    );

    const int allocate_result = swr_alloc_set_opts2(
        &raw_context,
        &output_layout,
        AV_SAMPLE_FMT_FLTP,
        plan.output_sample_rate,
        &source_layout,
        AV_SAMPLE_FMT_FLT,
        plan.source_sample_rate,
        0,
        nullptr
    );
    av_channel_layout_uninit(&source_layout);
    av_channel_layout_uninit(&output_layout);
    if (allocate_result < 0 || raw_context == nullptr) {
        throw std::runtime_error(
            "Failed to allocate the FFMS2 preview audio resampler. FFmpeg reported: " +
            ffmpeg_error_to_string(allocate_result)
        );
    }

    SwrContextHandle resample_context(raw_context);
    const int init_result = swr_init(resample_context.get());
    if (init_result < 0) {
        throw std::runtime_error(
            "Failed to initialize the FFMS2 preview audio resampler. FFmpeg reported: " +
            ffmpeg_error_to_string(init_result)
        );
    }

    return resample_context;
}

[[nodiscard]] std::vector<std::vector<float>> resample_interleaved_float_audio(
    const std::vector<float> &input_samples,
    const std::int64_t input_sample_count,
    const std::int64_t discard_output_samples,
    const std::int64_t requested_output_samples,
    const AudioResamplePlan &plan
) {
    auto resample_context = make_audio_resample_context(plan);
    const int maximum_output_samples = static_cast<int>(std::max<std::int64_t>(
        av_rescale_rnd(
            input_sample_count + swr_get_delay(resample_context.get(), plan.source_sample_rate),
            plan.output_sample_rate,
            plan.source_sample_rate,
            AV_ROUND_UP
        ) + 32,
        32
    ));

    std::vector<std::vector<float>> output_channels(
        static_cast<std::size_t>(plan.output_channel_count),
        std::vector<float>(static_cast<std::size_t>(maximum_output_samples), 0.0f)
    );

    std::vector<std::uint8_t *> output_planes(static_cast<std::size_t>(plan.output_channel_count), nullptr);
    for (int channel_index = 0; channel_index < plan.output_channel_count; ++channel_index) {
        output_planes[static_cast<std::size_t>(channel_index)] = reinterpret_cast<std::uint8_t *>(
            output_channels[static_cast<std::size_t>(channel_index)].data()
        );
    }

    const std::uint8_t *input_plane = reinterpret_cast<const std::uint8_t *>(input_samples.data());
    const int converted_samples = swr_convert(
        resample_context.get(),
        output_planes.data(),
        maximum_output_samples,
        &input_plane,
        static_cast<int>(input_sample_count)
    );
    if (converted_samples < 0) {
        throw std::runtime_error(
            "The FFMS2 preview audio resampler failed while converting decoded samples. FFmpeg reported: " +
            ffmpeg_error_to_string(converted_samples)
        );
    }

    const int remaining_capacity = maximum_output_samples - converted_samples;
    int flushed_samples = 0;
    if (remaining_capacity > 0) {
        std::vector<std::uint8_t *> flush_planes(output_planes);
        for (int channel_index = 0; channel_index < plan.output_channel_count; ++channel_index) {
            flush_planes[static_cast<std::size_t>(channel_index)] +=
                static_cast<std::size_t>(converted_samples) * sizeof(float);
        }

        flushed_samples = swr_convert(
            resample_context.get(),
            flush_planes.data(),
            remaining_capacity,
            nullptr,
            0
        );
        if (flushed_samples < 0) {
            throw std::runtime_error(
                "The FFMS2 preview audio resampler failed while draining buffered samples. FFmpeg reported: " +
                ffmpeg_error_to_string(flushed_samples)
            );
        }
    }

    const std::int64_t total_output_samples = static_cast<std::int64_t>(converted_samples + flushed_samples);
    const std::int64_t available_output_samples = std::max<std::int64_t>(total_output_samples - discard_output_samples, 0);
    const std::int64_t emitted_output_samples = requested_output_samples > 0
        ? std::min<std::int64_t>(available_output_samples, requested_output_samples)
        : available_output_samples;

    std::vector<std::vector<float>> sliced_channels(
        static_cast<std::size_t>(plan.output_channel_count),
        std::vector<float>(static_cast<std::size_t>(emitted_output_samples), 0.0f)
    );
    for (int channel_index = 0; channel_index < plan.output_channel_count; ++channel_index) {
        const auto &channel = output_channels[static_cast<std::size_t>(channel_index)];
        std::copy_n(
            channel.begin() + static_cast<std::ptrdiff_t>(discard_output_samples),
            static_cast<std::ptrdiff_t>(emitted_output_samples),
            sliced_channels[static_cast<std::size_t>(channel_index)].begin()
        );
    }

    return sliced_channels;
}

[[nodiscard]] DecodedVideoFrame build_decoded_video_frame(
    const FFMS_Frame &frame,
    const FrameTiming &timing,
    const FFMS_TrackTimeBase &time_base,
    const int stream_index,
    const std::int64_t frame_index,
    const Rational sample_aspect_ratio
) {
    if (frame.Data[0] == nullptr || frame.Linesize[0] <= 0 || frame.ScaledWidth <= 0 || frame.ScaledHeight <= 0) {
        throw std::runtime_error("FFMS2 returned an incomplete RGBA preview frame.");
    }

    VideoPlane plane{
        .line_stride_bytes = frame.Linesize[0],
        .visible_width = frame.ScaledWidth,
        .visible_height = frame.ScaledHeight,
        .bytes = std::vector<std::uint8_t>(
            static_cast<std::size_t>(frame.Linesize[0]) * static_cast<std::size_t>(frame.ScaledHeight),
            std::uint8_t{0}
        )
    };

    for (int row = 0; row < frame.ScaledHeight; ++row) {
        std::memcpy(
            plane.bytes.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(frame.Linesize[0]),
            frame.Data[0] + static_cast<std::size_t>(row) * static_cast<std::size_t>(frame.Linesize[0]),
            static_cast<std::size_t>(frame.Linesize[0])
        );
    }

    return DecodedVideoFrame{
        .stream_index = stream_index,
        .frame_index = frame_index,
        .timestamp = MediaTimestamp{
            .source_time_base = ffms_time_base_to_rational(time_base),
            .source_pts = timing.source_pts,
            .source_duration = timing.source_duration_pts,
            .origin = TimestampOrigin::decoded_pts,
            .start_microseconds = timing.start_microseconds,
            .duration_microseconds = timing.duration_microseconds
        },
        .width = frame.ScaledWidth,
        .height = frame.ScaledHeight,
        .sample_aspect_ratio = sample_aspect_ratio,
        .pixel_format = NormalizedVideoPixelFormat::rgba8,
        .planes = {std::move(plane)}
    };
}

[[nodiscard]] DecodedAudioSamples build_decoded_audio_block(
    const std::vector<std::vector<float>> &channel_samples,
    const std::int64_t start_sample,
    const std::int64_t block_index,
    const AudioResamplePlan &plan,
    const int stream_index
) {
    const int samples_per_channel = channel_samples.empty()
        ? 0
        : static_cast<int>(channel_samples.front().size());
    const std::int64_t duration_microseconds = samples_to_microseconds(samples_per_channel, plan.output_sample_rate);

    return DecodedAudioSamples{
        .stream_index = stream_index,
        .block_index = block_index,
        .timestamp = MediaTimestamp{
            .source_time_base = Rational{1, plan.output_sample_rate},
            .source_pts = start_sample,
            .source_duration = samples_per_channel,
            .origin = TimestampOrigin::stream_cursor,
            .start_microseconds = samples_to_microseconds(start_sample, plan.output_sample_rate),
            .duration_microseconds = duration_microseconds
        },
        .sample_rate = plan.output_sample_rate,
        .channel_count = plan.output_channel_count,
        .channel_layout_name = plan.output_channel_layout_name,
        .sample_format = NormalizedAudioSampleFormat::f32_planar,
        .samples_per_channel = samples_per_channel,
        .channel_samples = channel_samples
    };
}

[[nodiscard]] MediaDecodeError make_error(
    const std::filesystem::path &input_path,
    std::string message,
    std::string actionable_hint
) {
    return MediaDecodeError{
        .input_path = display_path_string(input_path),
        .message = std::move(message),
        .actionable_hint = std::move(actionable_hint)
    };
}

[[nodiscard]] VideoFrameWindowDecodeResult make_video_error_result(
    const std::filesystem::path &input_path,
    std::string message,
    std::string actionable_hint
) {
    return VideoFrameWindowDecodeResult{
        .video_frames = std::nullopt,
        .error = make_error(input_path, std::move(message), std::move(actionable_hint))
    };
}

[[nodiscard]] AudioBlockWindowDecodeResult make_audio_error_result(
    const std::filesystem::path &input_path,
    std::string message,
    std::string actionable_hint
) {
    return AudioBlockWindowDecodeResult{
        .audio_blocks = std::nullopt,
        .exhausted = false,
        .error = make_error(input_path, std::move(message), std::move(actionable_hint))
    };
}

[[nodiscard]] std::string ffms_error_message(
    std::string_view prefix,
    const FfmsErrorBuffer &error_buffer
) {
    return std::string(prefix) + error_buffer.message();
}

}  // namespace

std::filesystem::path preview_index_path_for_source(const std::filesystem::path &input_path) {
    const auto identity = stable_source_identity(input_path);
    const auto hash = hex_string(fnv1a_64(identity));
    return preview_cache_root() / ("preview-" + hash + ".ffindex");
}

VideoPreviewBackend::VideoPreviewBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VideoPreviewBackend::VideoPreviewBackend(VideoPreviewBackend &&) noexcept = default;
VideoPreviewBackend &VideoPreviewBackend::operator=(VideoPreviewBackend &&) noexcept = default;
VideoPreviewBackend::~VideoPreviewBackend() = default;

VideoFrameWindowDecodeResult VideoPreviewBackend::seek_and_decode_window_at_time(
    const std::int64_t requested_time_microseconds,
    const std::size_t maximum_frame_count
) noexcept {
    try {
        if (!impl_) {
            return make_video_error_result(
                {},
                "The FFMS2 preview backend is not initialized.",
                "Create a preview session before requesting preview frames."
            );
        }

        if (maximum_frame_count == 0) {
            return make_video_error_result(
                impl_->input_path,
                "Preview frame window decode requires at least one frame.",
                "Request one or more preview frames when seeking."
            );
        }

        const std::size_t start_index = find_preview_frame_index_for_time(
            impl_->frame_timings,
            std::max<std::int64_t>(requested_time_microseconds, 0)
        );
        const std::size_t end_index = std::min<std::size_t>(
            impl_->frame_timings.size(),
            start_index + maximum_frame_count
        );

        std::vector<DecodedVideoFrame> frames{};
        frames.reserve(end_index - start_index);
        for (std::size_t frame_index = start_index; frame_index < end_index; ++frame_index) {
            FfmsErrorBuffer error_buffer{};
            const auto *frame = FFMS_GetFrame(
                impl_->video_source.get(),
                static_cast<int>(frame_index),
                &error_buffer.info
            );
            if (frame == nullptr) {
                return make_video_error_result(
                    impl_->input_path,
                    ffms_error_message("FFMS2 could not decode a preview frame: ", error_buffer),
                    "Try seeking again or choose a different source clip."
                );
            }

            frames.push_back(build_decoded_video_frame(
                *frame,
                impl_->frame_timings[frame_index],
                impl_->video_time_base,
                impl_->stream_index,
                static_cast<std::int64_t>(frame_index),
                impl_->sample_aspect_ratio
            ));
        }

        impl_->next_frame_index = end_index;
        return VideoFrameWindowDecodeResult{
            .video_frames = std::move(frames),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_video_error_result(
            impl_ != nullptr ? impl_->input_path : std::filesystem::path{},
            "Preview frame window decode aborted because FFMS2 raised an unexpected exception.",
            exception.what()
        );
    }
}

VideoFrameWindowDecodeResult VideoPreviewBackend::decode_next_window(const std::size_t maximum_frame_count) noexcept {
    try {
        if (!impl_) {
            return make_video_error_result(
                {},
                "The FFMS2 preview backend is not initialized.",
                "Create a preview session before requesting preview frames."
            );
        }

        if (maximum_frame_count == 0) {
            return make_video_error_result(
                impl_->input_path,
                "Preview frame window decode requires at least one frame.",
                "Request one or more preview frames when reading sequentially."
            );
        }

        if (impl_->next_frame_index >= impl_->frame_timings.size()) {
            return make_video_error_result(
                impl_->input_path,
                "The preview session has no additional frames available.",
                "Seek to another position or choose a longer source clip."
            );
        }

        const std::size_t end_index = std::min<std::size_t>(
            impl_->frame_timings.size(),
            impl_->next_frame_index + maximum_frame_count
        );

        std::vector<DecodedVideoFrame> frames{};
        frames.reserve(end_index - impl_->next_frame_index);
        for (std::size_t frame_index = impl_->next_frame_index; frame_index < end_index; ++frame_index) {
            FfmsErrorBuffer error_buffer{};
            const auto *frame = FFMS_GetFrame(
                impl_->video_source.get(),
                static_cast<int>(frame_index),
                &error_buffer.info
            );
            if (frame == nullptr) {
                return make_video_error_result(
                    impl_->input_path,
                    ffms_error_message("FFMS2 could not decode a sequential preview frame: ", error_buffer),
                    "Seek to another position or choose a different source clip."
                );
            }

            frames.push_back(build_decoded_video_frame(
                *frame,
                impl_->frame_timings[frame_index],
                impl_->video_time_base,
                impl_->stream_index,
                static_cast<std::int64_t>(frame_index),
                impl_->sample_aspect_ratio
            ));
        }

        impl_->next_frame_index = end_index;
        return VideoFrameWindowDecodeResult{
            .video_frames = std::move(frames),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_video_error_result(
            impl_ != nullptr ? impl_->input_path : std::filesystem::path{},
            "Preview frame window decode aborted because FFMS2 raised an unexpected exception.",
            exception.what()
        );
    }
}

AudioPreviewBackend::AudioPreviewBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AudioPreviewBackend::AudioPreviewBackend(AudioPreviewBackend &&) noexcept = default;
AudioPreviewBackend &AudioPreviewBackend::operator=(AudioPreviewBackend &&) noexcept = default;
AudioPreviewBackend::~AudioPreviewBackend() = default;

AudioBlockWindowDecodeResult AudioPreviewBackend::seek_and_decode_window_at_time(
    const std::int64_t requested_time_microseconds,
    const std::int64_t minimum_duration_microseconds
) noexcept {
    try {
        if (!impl_) {
            return make_audio_error_result(
                {},
                "The FFMS2 preview audio backend is not initialized.",
                "Create a preview audio session before requesting preview audio blocks."
            );
        }

        if (minimum_duration_microseconds <= 0) {
            return make_audio_error_result(
                impl_->input_path,
                "Preview audio window decode requires a positive duration.",
                "Request a positive preview-audio chunk duration."
            );
        }

        const std::int64_t requested_output_start_sample = std::clamp<std::int64_t>(
            av_rescale_rnd(
                std::max<std::int64_t>(requested_time_microseconds, 0),
                impl_->resample_plan.output_sample_rate,
                kMicrosecondsPerSecond,
                AV_ROUND_DOWN
            ),
            0,
            impl_->total_output_samples
        );

        if (requested_output_start_sample >= impl_->total_output_samples) {
            impl_->next_output_sample_index = impl_->total_output_samples;
            return AudioBlockWindowDecodeResult{
                .audio_blocks = std::vector<DecodedAudioSamples>{},
                .exhausted = true,
                .error = std::nullopt
            };
        }

        const std::int64_t requested_output_samples = std::min<std::int64_t>(
            duration_to_samples(minimum_duration_microseconds, impl_->resample_plan.output_sample_rate),
            impl_->total_output_samples - requested_output_start_sample
        );

        std::vector<std::vector<float>> converted_channels{};
        if (!impl_->resample_plan.needs_resample) {
            const auto interleaved_samples = read_ffms_float_audio_samples(
                *impl_->audio_source,
                impl_->resample_plan.source_channel_count,
                requested_output_start_sample,
                requested_output_samples
            );
            converted_channels = deinterleave_float_samples(
                interleaved_samples,
                impl_->resample_plan.output_channel_count
            );
        } else {
            const std::int64_t nominal_source_start = av_rescale_rnd(
                requested_output_start_sample,
                impl_->resample_plan.source_sample_rate,
                impl_->resample_plan.output_sample_rate,
                AV_ROUND_DOWN
            );
            const std::int64_t source_preroll_samples = std::min<std::int64_t>(
                duration_to_samples(kAudioPrerollUs, impl_->resample_plan.source_sample_rate),
                nominal_source_start
            );
            const std::int64_t actual_source_start = nominal_source_start - source_preroll_samples;
            const std::int64_t source_window_samples = av_rescale_rnd(
                requested_output_samples,
                impl_->resample_plan.source_sample_rate,
                impl_->resample_plan.output_sample_rate,
                AV_ROUND_UP
            );
            const std::int64_t source_margin_samples = duration_to_samples(
                kAudioWindowMarginUs,
                impl_->resample_plan.source_sample_rate
            );
            const std::int64_t source_samples_to_read = std::min<std::int64_t>(
                impl_->total_source_samples - actual_source_start,
                source_preroll_samples + source_window_samples + source_margin_samples
            );

            const auto interleaved_samples = read_ffms_float_audio_samples(
                *impl_->audio_source,
                impl_->resample_plan.source_channel_count,
                actual_source_start,
                source_samples_to_read
            );

            const std::int64_t discard_output_samples = av_rescale_rnd(
                source_preroll_samples,
                impl_->resample_plan.output_sample_rate,
                impl_->resample_plan.source_sample_rate,
                AV_ROUND_DOWN
            );
            converted_channels = resample_interleaved_float_audio(
                interleaved_samples,
                source_samples_to_read,
                discard_output_samples,
                requested_output_samples,
                impl_->resample_plan
            );
        }

        if (converted_channels.empty() || converted_channels.front().empty()) {
            impl_->next_output_sample_index = impl_->total_output_samples;
            return AudioBlockWindowDecodeResult{
                .audio_blocks = std::vector<DecodedAudioSamples>{},
                .exhausted = true,
                .error = std::nullopt
            };
        }

        std::vector<DecodedAudioSamples> audio_blocks{};
        std::int64_t block_start_sample = requested_output_start_sample;
        std::int64_t block_index = requested_output_start_sample / std::max<std::int64_t>(impl_->block_samples, 1);
        const int total_samples_per_channel = static_cast<int>(converted_channels.front().size());
        for (int sample_offset = 0; sample_offset < total_samples_per_channel; sample_offset += impl_->block_samples) {
            const int block_samples = std::min(impl_->block_samples, total_samples_per_channel - sample_offset);
            std::vector<std::vector<float>> block_channels(
                static_cast<std::size_t>(impl_->resample_plan.output_channel_count),
                std::vector<float>(static_cast<std::size_t>(block_samples), 0.0f)
            );

            for (int channel_index = 0; channel_index < impl_->resample_plan.output_channel_count; ++channel_index) {
                std::copy_n(
                    converted_channels[static_cast<std::size_t>(channel_index)].begin() + sample_offset,
                    block_samples,
                    block_channels[static_cast<std::size_t>(channel_index)].begin()
                );
            }

            audio_blocks.push_back(build_decoded_audio_block(
                block_channels,
                block_start_sample,
                block_index,
                impl_->resample_plan,
                impl_->stream_index
            ));
            block_start_sample += block_samples;
            ++block_index;
        }

        impl_->next_output_sample_index = block_start_sample;
        return AudioBlockWindowDecodeResult{
            .audio_blocks = std::move(audio_blocks),
            .exhausted = impl_->next_output_sample_index >= impl_->total_output_samples,
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_audio_error_result(
            impl_ != nullptr ? impl_->input_path : std::filesystem::path{},
            "Preview audio window decode aborted because FFMS2 raised an unexpected exception.",
            exception.what()
        );
    }
}

AudioBlockWindowDecodeResult AudioPreviewBackend::decode_next_window(
    const std::int64_t minimum_duration_microseconds
) noexcept {
    return seek_and_decode_window_at_time(
        impl_ != nullptr
            ? samples_to_microseconds(impl_->next_output_sample_index, impl_->resample_plan.output_sample_rate)
            : 0,
        minimum_duration_microseconds
    );
}

VideoPreviewBackendCreateResult create_video_preview_backend(
    const std::filesystem::path &input_path,
    const DecodeNormalizationPolicy &normalization_policy
) noexcept {
    try {
        const auto normalized_input_path = input_path.lexically_normal();
        const auto input_path_string = display_path_string(normalized_input_path);

        if (input_path_string.empty()) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "Cannot create a preview session because no file path was provided.",
                    "Provide a path to a readable media file before requesting preview."
                )
            };
        }

        std::error_code filesystem_error;
        const bool input_exists = std::filesystem::exists(normalized_input_path, filesystem_error);
        if (filesystem_error) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "Cannot create preview media input '" + input_path_string + "': the file system could not be queried.",
                    "The operating system reported: " + filesystem_error.message()
                )
            };
        }

        if (!input_exists) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "Cannot create preview media input '" + input_path_string + "': the file does not exist.",
                    "Check that the path is correct and that the file has been created before requesting preview."
                )
            };
        }

        if (normalization_policy.video_pixel_format != NormalizedVideoPixelFormat::rgba8) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "The FFMS2 preview backend only supports rgba8 preview output.",
                    "Keep the preview decode path normalized to rgba8."
                )
            };
        }

        auto index_result = load_or_build_preview_index(normalized_input_path);
        FfmsErrorBuffer error_buffer{};
        const int video_track_index = FFMS_GetFirstIndexedTrackOfType(
            index_result.index.get(),
            FFMS_TYPE_VIDEO,
            &error_buffer.info
        );
        if (video_track_index < 0) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "No previewable video stream was found in '" + input_path_string + "'.",
                    "Provide a media file that contains a decodable video stream before enabling Preview."
                )
            };
        }

        VideoSourceHandle video_source(FFMS_CreateVideoSource(
            path_to_utf8(normalized_input_path).c_str(),
            video_track_index,
            index_result.index.get(),
            1,
            FFMS_SEEK_NORMAL,
            &error_buffer.info
        ));
        if (video_source == nullptr) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 could not open the preview video stream for '" + input_path_string + "'.",
                    error_buffer.message()
                )
            };
        }

        FFMS_Track *video_track = FFMS_GetTrackFromVideo(video_source.get());
        const auto *video_properties = FFMS_GetVideoProperties(video_source.get());
        if (video_track == nullptr || video_properties == nullptr || video_properties->NumFrames <= 0) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "The selected source does not expose a primary video stream for preview.",
                    "Choose a source file with a decodable video stream before enabling Preview."
                )
            };
        }

        error_buffer = FfmsErrorBuffer{};
        const auto *first_frame = FFMS_GetFrame(video_source.get(), 0, &error_buffer.info);
        if (first_frame == nullptr) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 could not decode the first preview frame.",
                    error_buffer.message()
                )
            };
        }

        const auto [target_width, target_height] = choose_normalized_video_dimensions(
            first_frame->EncodedWidth,
            first_frame->EncodedHeight,
            normalization_policy
        );
        const int rgba_pixel_format = FFMS_GetPixFmt("rgba");
        if (rgba_pixel_format < 0) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 could not resolve an RGBA preview pixel format.",
                    "Keep the preview backend built against a swscale-capable FFMS2/FFmpeg stack."
                )
            };
        }

        const int target_formats[2] = {rgba_pixel_format, -1};
        error_buffer = FfmsErrorBuffer{};
        if (FFMS_SetOutputFormatV2(
                video_source.get(),
                target_formats,
                target_width,
                target_height,
                FFMS_RESIZER_BILINEAR,
                &error_buffer.info
            ) != FFMS_ERROR_SUCCESS) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 could not normalize preview frames to RGBA output.",
                    error_buffer.message()
                )
            };
        }

        const auto *time_base = FFMS_GetTimeBase(video_track);
        if (time_base == nullptr || time_base->Num <= 0 || time_base->Den <= 0) {
            return VideoPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 did not expose a usable preview video time base.",
                    "Choose a source file with readable video timestamps before enabling Preview."
                )
            };
        }

        auto impl = std::make_unique<VideoPreviewBackend::Impl>();
        impl->input_path = normalized_input_path;
        impl->input_path_string = input_path_string;
        impl->normalization_policy = normalization_policy;
        impl->stream_index = video_track_index;
        impl->index = std::move(index_result.index);
        impl->video_source = std::move(video_source);
        impl->video_track = video_track;
        impl->video_time_base = *time_base;
        impl->sample_aspect_ratio = Rational{
            .numerator = std::max(video_properties->SARNum, 1),
            .denominator = std::max(video_properties->SARDen, 1)
        };
        impl->frame_count = video_properties->NumFrames;
        impl->frame_timings = build_frame_timings(*video_track, *time_base, *video_properties);

        return VideoPreviewBackendCreateResult{
            .backend = std::unique_ptr<VideoPreviewBackend>(new VideoPreviewBackend(std::move(impl))),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return VideoPreviewBackendCreateResult{
            .backend = nullptr,
            .error = make_error(
                input_path,
                "Preview session creation aborted because FFMS2 raised an unexpected exception.",
                exception.what()
            )
        };
    }
}

AudioPreviewBackendCreateResult create_audio_preview_backend(
    const std::filesystem::path &input_path,
    const AudioPreviewOutputConfig &output_config,
    const DecodeNormalizationPolicy &normalization_policy
) noexcept {
    try {
        const auto normalized_input_path = input_path.lexically_normal();
        const auto input_path_string = display_path_string(normalized_input_path);

        if (input_path_string.empty()) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "Cannot create a preview audio session because no file path was provided.",
                    "Provide a path to a readable media file before requesting preview audio."
                )
            };
        }

        std::error_code filesystem_error;
        const bool input_exists = std::filesystem::exists(normalized_input_path, filesystem_error);
        if (filesystem_error) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "Cannot create preview media input '" + input_path_string + "': the file system could not be queried.",
                    "The operating system reported: " + filesystem_error.message()
                )
            };
        }

        if (!input_exists) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "Cannot create preview media input '" + input_path_string + "': the file does not exist.",
                    "Check that the path is correct and that the file has been created before requesting preview audio."
                )
            };
        }

        auto index_result = load_or_build_preview_index(normalized_input_path);
        FfmsErrorBuffer error_buffer{};
        const int audio_track_index = FFMS_GetFirstIndexedTrackOfType(
            index_result.index.get(),
            FFMS_TYPE_AUDIO,
            &error_buffer.info
        );
        if (audio_track_index < 0) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "No previewable audio stream was found in '" + input_path_string + "'.",
                    "Choose a source file with a decodable audio stream before expecting preview audio."
                )
            };
        }

        const int video_track_index = FFMS_GetFirstIndexedTrackOfType(
            index_result.index.get(),
            FFMS_TYPE_VIDEO,
            &error_buffer.info
        );
        const int delay_mode = video_track_index >= 0 ? FFMS_DELAY_FIRST_VIDEO_TRACK : FFMS_DELAY_TIME_ZERO;

        AudioSourceHandle audio_source(FFMS_CreateAudioSource2(
            path_to_utf8(normalized_input_path).c_str(),
            audio_track_index,
            index_result.index.get(),
            delay_mode,
            FFMS_GAP_FILL_AUTO,
            0.0,
            &error_buffer.info
        ));
        if (audio_source == nullptr) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 could not open the preview audio stream for '" + input_path_string + "'.",
                    error_buffer.message()
                )
            };
        }

        const auto *source_audio_properties = FFMS_GetAudioProperties(audio_source.get());
        if (source_audio_properties == nullptr ||
            source_audio_properties->SampleRate <= 0 ||
            source_audio_properties->Channels <= 0 ||
            source_audio_properties->NumSamples <= 0) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "The selected source does not expose a usable preview audio sample rate and channel count.",
                    "Choose a source clip with readable audio stream metadata before expecting preview audio."
                )
            };
        }

        ResampleOptionsHandle ffms_output_options(FFMS_CreateResampleOptions(audio_source.get()));
        if (ffms_output_options == nullptr) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 could not allocate preview audio output-format options.",
                    "Try another source clip or rebuild the preview index."
                )
            };
        }

        ffms_output_options->SampleFormat = FFMS_FMT_FLT;
        ffms_output_options->SampleRate = source_audio_properties->SampleRate;
        ffms_output_options->ChannelLayout = source_audio_properties->ChannelLayout;

        error_buffer = FfmsErrorBuffer{};
        if (FFMS_SetOutputFormatA(audio_source.get(), ffms_output_options.get(), &error_buffer.info) !=
            FFMS_ERROR_SUCCESS) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 could not normalize preview audio to float samples.",
                    error_buffer.message()
                )
            };
        }

        const auto *ffms_output_properties = FFMS_GetAudioProperties(audio_source.get());
        if (ffms_output_properties == nullptr ||
            ffms_output_properties->SampleRate <= 0 ||
            ffms_output_properties->Channels <= 0) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "FFMS2 did not expose usable preview audio output properties.",
                    "Try another source clip or rebuild the preview index."
                )
            };
        }

        const int requested_sample_rate = output_config.sample_rate_hz > 0
            ? output_config.sample_rate_hz
            : ffms_output_properties->SampleRate;
        const int requested_channel_count = output_config.channel_count > 0
            ? output_config.channel_count
            : ffms_output_properties->Channels;
        if (requested_sample_rate <= 0 || requested_channel_count <= 0) {
            return AudioPreviewBackendCreateResult{
                .backend = nullptr,
                .error = make_error(
                    normalized_input_path,
                    "The selected source does not expose a usable preview audio sample rate and channel count.",
                    "Choose a source clip with readable audio stream metadata before expecting preview audio."
                )
            };
        }

        auto impl = std::make_unique<AudioPreviewBackend::Impl>();
        impl->input_path = normalized_input_path;
        impl->input_path_string = input_path_string;
        impl->normalization_policy = normalization_policy;
        impl->output_config = output_config;
        impl->index = std::move(index_result.index);
        impl->audio_source = std::move(audio_source);
        impl->stream_index = audio_track_index;
        impl->total_source_samples = ffms_output_properties->NumSamples;
        impl->block_samples = std::max(normalization_policy.audio_block_samples, 1);
        impl->resample_plan = AudioResamplePlan{
            .source_sample_rate = ffms_output_properties->SampleRate,
            .source_channel_count = ffms_output_properties->Channels,
            .source_channel_layout_mask = static_cast<std::uint64_t>(
                ffms_output_properties->ChannelLayout != 0
                    ? ffms_output_properties->ChannelLayout
                    : default_channel_layout_mask(ffms_output_properties->Channels)
            ),
            .output_sample_rate = requested_sample_rate,
            .output_channel_count = requested_channel_count,
            .output_channel_layout_mask = default_channel_layout_mask(requested_channel_count),
            .needs_resample = requested_sample_rate != ffms_output_properties->SampleRate ||
                requested_channel_count != ffms_output_properties->Channels,
            .output_channel_layout_name = describe_channel_layout(
                default_channel_layout_mask(requested_channel_count),
                requested_channel_count
            )
        };
        impl->total_output_samples = av_rescale_rnd(
            impl->total_source_samples,
            impl->resample_plan.output_sample_rate,
            impl->resample_plan.source_sample_rate,
            AV_ROUND_UP
        );

        return AudioPreviewBackendCreateResult{
            .backend = std::unique_ptr<AudioPreviewBackend>(new AudioPreviewBackend(std::move(impl))),
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return AudioPreviewBackendCreateResult{
            .backend = nullptr,
            .error = make_error(
                input_path,
                "Preview audio session creation aborted because FFMS2 raised an unexpected exception.",
                exception.what()
            )
        };
    }
}

}  // namespace utsure::core::media::ffms2_preview
