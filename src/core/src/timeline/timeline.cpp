#include "utsure/core/timeline/timeline.hpp"

#include "utsure/core/media/media_inspector.hpp"

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace utsure::core::timeline {

namespace {

using utsure::core::media::AudioStreamInfo;
using utsure::core::media::DecodedAudioSamples;
using utsure::core::media::DecodedMediaSource;
using utsure::core::media::DecodedVideoFrame;
using utsure::core::media::MediaInspectionResult;
using utsure::core::media::MediaInspector;
using utsure::core::media::MediaSourceInfo;
using utsure::core::media::Rational;
using utsure::core::media::TimestampOrigin;

struct VideoCadence final {
    int width{0};
    int height{0};
    Rational sample_aspect_ratio{1, 1};
    std::int64_t frame_duration_pts{0};
    std::int64_t frame_duration_microseconds{0};
};

TimelineAssemblyResult make_assembly_error(std::string message, std::string actionable_hint);
TimelineCompositionResult make_composition_error(std::string message, std::string actionable_hint);
AVRational to_av_rational(const Rational &value);
std::int64_t rescale_value(
    std::int64_t value,
    const Rational &source_time_base,
    const Rational &target_time_base
);
std::int64_t rescale_to_microseconds(std::int64_t value, const Rational &time_base);
bool rational_is_positive(const Rational &value);
bool rationals_equal(const Rational &left, const Rational &right);
std::optional<std::filesystem::path> normalize_optional_path(const std::optional<std::filesystem::path> &path);
MediaInspectionResult inspect_segment(TimelineSegmentKind kind, const std::filesystem::path &source_path);
Rational choose_output_video_time_base(const media::VideoStreamInfo &video_stream);
Rational choose_output_frame_rate(const media::VideoStreamInfo &video_stream, const Rational &output_video_time_base);
void validate_video_stream_presence(TimelineSegmentKind kind, const MediaSourceInfo &segment_info);
void validate_video_compatibility(
    TimelineSegmentKind kind,
    const media::VideoStreamInfo &main_video,
    const media::VideoStreamInfo &candidate_video
);
void validate_audio_compatibility(
    TimelineSegmentKind kind,
    const std::optional<AudioStreamInfo> &main_audio,
    const std::optional<AudioStreamInfo> &candidate_audio
);
VideoCadence derive_video_cadence(const DecodedMediaSource &main_segment, const Rational &output_video_time_base);
void validate_segment_frames(
    TimelineSegmentKind kind,
    const DecodedMediaSource &decoded_segment,
    const VideoCadence &video_cadence,
    const Rational &output_video_time_base
);
std::int64_t count_audio_samples(const std::vector<DecodedAudioSamples> &audio_blocks);
DecodedAudioSamples make_audio_block(
    const AudioStreamInfo &output_audio_stream,
    const media::DecodeNormalizationPolicy &normalization_policy,
    std::int64_t block_index,
    std::int64_t source_pts,
    int samples_per_channel,
    bool silent,
    const std::vector<std::vector<float>> &channel_samples
);
void append_audio_segment(
    const TimelineSegmentPlan &segment_plan,
    const DecodedMediaSource &decoded_segment,
    std::int64_t expected_segment_samples,
    const AudioStreamInfo &output_audio_stream,
    std::vector<DecodedAudioSamples> &output_audio_blocks,
    TimelineSegmentSummary &segment_summary,
    std::int64_t &next_output_audio_pts
);
DecodedMediaSource build_composed_media_source(
    const TimelinePlan &timeline_plan,
    const TimelineCompositionSummary &timeline_summary,
    const VideoCadence &video_cadence,
    std::vector<DecodedVideoFrame> video_frames,
    std::vector<DecodedAudioSamples> audio_blocks,
    const media::DecodeNormalizationPolicy &normalization_policy
);

}  // namespace

bool TimelineAssemblyResult::succeeded() const noexcept {
    return timeline_plan.has_value() && !error.has_value();
}

bool TimelineCompositionResult::succeeded() const noexcept {
    return output.has_value() && !error.has_value();
}

const char *to_string(const TimelineSegmentKind kind) noexcept {
    switch (kind) {
    case TimelineSegmentKind::intro:
        return "intro";
    case TimelineSegmentKind::main:
        return "main";
    case TimelineSegmentKind::outro:
        return "outro";
    default:
        return "unknown";
    }
}

const char *to_string(const SubtitleTimingMode mode) noexcept {
    switch (mode) {
    case SubtitleTimingMode::main_segment_only:
        return "main_segment_only";
    case SubtitleTimingMode::full_output_timeline:
        return "full_output_timeline";
    default:
        return "unknown";
    }
}

TimelineAssemblyResult TimelineAssembler::assemble(const TimelineAssemblyRequest &request) noexcept {
    try {
        if (request.main_source_path.empty()) {
            return make_assembly_error(
                "Timeline assembly requires a main source path.",
                "Provide the main source clip before requesting intro or outro composition."
            );
        }

        const auto main_source_path = request.main_source_path.lexically_normal();
        const auto main_inspection_result = inspect_segment(TimelineSegmentKind::main, main_source_path);
        if (!main_inspection_result.succeeded()) {
            return make_assembly_error(
                main_inspection_result.error->message,
                main_inspection_result.error->actionable_hint
            );
        }

        const auto &main_segment_info = *main_inspection_result.media_source_info;
        validate_video_stream_presence(TimelineSegmentKind::main, main_segment_info);

        const auto main_video_stream = *main_segment_info.primary_video_stream;
        if (!rational_is_positive(main_video_stream.average_frame_rate)) {
            return make_assembly_error(
                "The main segment does not expose a usable average frame rate.",
                "Use a main clip with a readable constant frame cadence before adding intro or outro segments."
            );
        }

        std::optional<AudioStreamInfo> output_audio_stream = main_segment_info.primary_audio_stream;
        if (output_audio_stream.has_value()) {
            if (output_audio_stream->sample_rate <= 0 || output_audio_stream->channel_count <= 0) {
                return make_assembly_error(
                    "The main segment audio stream does not expose a usable sample rate and channel count.",
                    "Use a main clip with a readable audio layout or remove audio from the job for this milestone."
                );
            }

            if (!rational_is_positive(output_audio_stream->timestamps.time_base)) {
                output_audio_stream->timestamps.time_base = Rational{
                    .numerator = 1,
                    .denominator = output_audio_stream->sample_rate
                };
            }
        }

        std::vector<TimelineSegmentPlan> segments{};
        segments.reserve(3);

        if (const auto intro_path = normalize_optional_path(request.intro_source_path); intro_path.has_value()) {
            const auto intro_inspection_result = inspect_segment(TimelineSegmentKind::intro, *intro_path);
            if (!intro_inspection_result.succeeded()) {
                return make_assembly_error(
                    intro_inspection_result.error->message,
                    intro_inspection_result.error->actionable_hint
                );
            }

            const auto &intro_info = *intro_inspection_result.media_source_info;
            validate_video_stream_presence(TimelineSegmentKind::intro, intro_info);
            validate_video_compatibility(TimelineSegmentKind::intro, main_video_stream, *intro_info.primary_video_stream);
            validate_audio_compatibility(TimelineSegmentKind::intro, output_audio_stream, intro_info.primary_audio_stream);

            segments.push_back(TimelineSegmentPlan{
                .kind = TimelineSegmentKind::intro,
                .source_path = *intro_path,
                .inspected_source_info = intro_info,
                .subtitles_enabled = request.subtitles_present &&
                    request.subtitle_timing_mode == SubtitleTimingMode::full_output_timeline
            });
        }

        const std::size_t main_segment_index = segments.size();
        segments.push_back(TimelineSegmentPlan{
            .kind = TimelineSegmentKind::main,
            .source_path = main_source_path,
            .inspected_source_info = main_segment_info,
            .subtitles_enabled = request.subtitles_present
        });

        if (const auto outro_path = normalize_optional_path(request.outro_source_path); outro_path.has_value()) {
            const auto outro_inspection_result = inspect_segment(TimelineSegmentKind::outro, *outro_path);
            if (!outro_inspection_result.succeeded()) {
                return make_assembly_error(
                    outro_inspection_result.error->message,
                    outro_inspection_result.error->actionable_hint
                );
            }

            const auto &outro_info = *outro_inspection_result.media_source_info;
            validate_video_stream_presence(TimelineSegmentKind::outro, outro_info);
            validate_video_compatibility(TimelineSegmentKind::outro, main_video_stream, *outro_info.primary_video_stream);
            validate_audio_compatibility(TimelineSegmentKind::outro, output_audio_stream, outro_info.primary_audio_stream);

            segments.push_back(TimelineSegmentPlan{
                .kind = TimelineSegmentKind::outro,
                .source_path = *outro_path,
                .inspected_source_info = outro_info,
                .subtitles_enabled = request.subtitles_present &&
                    request.subtitle_timing_mode == SubtitleTimingMode::full_output_timeline
            });
        }

        const auto output_video_time_base = choose_output_video_time_base(main_video_stream);

        return TimelineAssemblyResult{
            .timeline_plan = TimelinePlan{
                .segments = std::move(segments),
                .main_segment_index = main_segment_index,
                .output_video_time_base = output_video_time_base,
                .output_frame_rate = choose_output_frame_rate(main_video_stream, output_video_time_base),
                .output_audio_stream = output_audio_stream
            },
            .error = std::nullopt
        };
    } catch (const std::runtime_error &exception) {
        return make_assembly_error(
            exception.what(),
            "Adjust the intro/outro clips so their formats and cadence match the supported timeline rules."
        );
    } catch (const std::exception &exception) {
        return make_assembly_error(
            "Timeline assembly aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

TimelineCompositionResult TimelineComposer::compose(
    const TimelinePlan &timeline_plan,
    const std::vector<DecodedMediaSource> &decoded_segments
) noexcept {
    try {
        if (timeline_plan.segments.empty()) {
            return make_composition_error(
                "Timeline composition requires at least one timeline segment.",
                "Provide a main segment before composing intro or outro clips."
            );
        }

        if (timeline_plan.segments.size() != decoded_segments.size()) {
            return make_composition_error(
                "Timeline composition received a decoded segment count that does not match the timeline plan.",
                "Decode every assembled timeline segment before composing the output timeline."
            );
        }

        if (timeline_plan.main_segment_index >= decoded_segments.size()) {
            return make_composition_error(
                "Timeline composition received an invalid main-segment index.",
                "Rebuild the timeline plan before composing the decoded output."
            );
        }

        const auto &main_segment = decoded_segments[timeline_plan.main_segment_index];
        const auto video_cadence = derive_video_cadence(main_segment, timeline_plan.output_video_time_base);

        TimelineCompositionSummary timeline_summary{
            .segments = {},
            .output_video_time_base = timeline_plan.output_video_time_base,
            .output_frame_rate = timeline_plan.output_frame_rate,
            .output_audio_time_base = timeline_plan.output_audio_stream.has_value()
                ? std::optional<Rational>(timeline_plan.output_audio_stream->timestamps.time_base)
                : std::nullopt,
            .output_duration_microseconds = 0,
            .output_video_frame_count = 0,
            .output_audio_block_count = 0
        };
        timeline_summary.segments.reserve(timeline_plan.segments.size());

        std::vector<DecodedVideoFrame> output_video_frames{};
        std::vector<DecodedAudioSamples> output_audio_blocks{};
        std::int64_t next_output_video_pts = 0;
        std::int64_t next_output_audio_pts = 0;

        for (std::size_t segment_index = 0; segment_index < timeline_plan.segments.size(); ++segment_index) {
            const auto &segment_plan = timeline_plan.segments[segment_index];
            const auto &decoded_segment = decoded_segments[segment_index];

            validate_segment_frames(
                segment_plan.kind,
                decoded_segment,
                video_cadence,
                timeline_plan.output_video_time_base
            );

            TimelineSegmentSummary segment_summary{
                .kind = segment_plan.kind,
                .source_path = segment_plan.source_path,
                .start_microseconds = rescale_to_microseconds(next_output_video_pts, timeline_plan.output_video_time_base),
                .duration_microseconds = 0,
                .video_frame_count = static_cast<std::int64_t>(decoded_segment.video_frames.size()),
                .audio_block_count = 0,
                .subtitles_enabled = segment_plan.subtitles_enabled,
                .inserted_silence = false
            };

            for (const auto &frame : decoded_segment.video_frames) {
                auto output_frame = frame;
                output_frame.stream_index = main_segment.source_info.primary_video_stream->stream_index;
                output_frame.frame_index = static_cast<std::int64_t>(output_video_frames.size());
                output_frame.timestamp.source_time_base = timeline_plan.output_video_time_base;
                output_frame.timestamp.source_pts = next_output_video_pts;
                output_frame.timestamp.source_duration = video_cadence.frame_duration_pts;
                output_frame.timestamp.origin = TimestampOrigin::stream_cursor;
                output_frame.timestamp.start_microseconds =
                    rescale_to_microseconds(next_output_video_pts, timeline_plan.output_video_time_base);
                output_frame.timestamp.duration_microseconds = video_cadence.frame_duration_microseconds;
                output_frame.sample_aspect_ratio = video_cadence.sample_aspect_ratio;
                output_video_frames.push_back(std::move(output_frame));
                next_output_video_pts += video_cadence.frame_duration_pts;
            }

            segment_summary.duration_microseconds = rescale_to_microseconds(
                segment_summary.video_frame_count * video_cadence.frame_duration_pts,
                timeline_plan.output_video_time_base
            );

            if (timeline_plan.output_audio_stream.has_value()) {
                const auto expected_segment_samples = rescale_value(
                    segment_summary.video_frame_count * video_cadence.frame_duration_pts,
                    timeline_plan.output_video_time_base,
                    timeline_plan.output_audio_stream->timestamps.time_base
                );
                append_audio_segment(
                    segment_plan,
                    decoded_segment,
                    expected_segment_samples,
                    *timeline_plan.output_audio_stream,
                    output_audio_blocks,
                    segment_summary,
                    next_output_audio_pts
                );
            }

            timeline_summary.segments.push_back(std::move(segment_summary));
        }

        timeline_summary.output_video_frame_count = static_cast<std::int64_t>(output_video_frames.size());
        timeline_summary.output_audio_block_count = static_cast<std::int64_t>(output_audio_blocks.size());
        timeline_summary.output_duration_microseconds = rescale_to_microseconds(
            next_output_video_pts,
            timeline_plan.output_video_time_base
        );

        return TimelineCompositionResult{
            .output = TimelineCompositionOutput{
                .decoded_media_source = build_composed_media_source(
                    timeline_plan,
                    timeline_summary,
                    video_cadence,
                    std::move(output_video_frames),
                    std::move(output_audio_blocks),
                    decoded_segments[timeline_plan.main_segment_index].normalization_policy
                ),
                .timeline_summary = std::move(timeline_summary)
            },
            .error = std::nullopt
        };
    } catch (const std::runtime_error &exception) {
        return make_composition_error(
            exception.what(),
            "Adjust the decoded segment cadence or audio layout so every composed segment matches the main timeline."
        );
    } catch (const std::exception &exception) {
        return make_composition_error(
            "Timeline composition aborted because an unexpected exception was raised.",
            exception.what()
        );
    }
}

namespace {

TimelineAssemblyResult make_assembly_error(std::string message, std::string actionable_hint) {
    return TimelineAssemblyResult{
        .timeline_plan = std::nullopt,
        .error = TimelineAssemblyError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

TimelineCompositionResult make_composition_error(std::string message, std::string actionable_hint) {
    return TimelineCompositionResult{
        .output = std::nullopt,
        .error = TimelineCompositionError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint)
        }
    };
}

AVRational to_av_rational(const Rational &value) {
    return AVRational{
        .num = static_cast<int>(value.numerator),
        .den = static_cast<int>(value.denominator)
    };
}

std::int64_t rescale_value(
    const std::int64_t value,
    const Rational &source_time_base,
    const Rational &target_time_base
) {
    return av_rescale_q(value, to_av_rational(source_time_base), to_av_rational(target_time_base));
}

std::int64_t rescale_to_microseconds(const std::int64_t value, const Rational &time_base) {
    return av_rescale_q(value, to_av_rational(time_base), AV_TIME_BASE_Q);
}

bool rational_is_positive(const Rational &value) {
    return value.is_valid() && value.numerator > 0 && value.denominator > 0;
}

bool rationals_equal(const Rational &left, const Rational &right) {
    if (!left.is_valid() || !right.is_valid()) {
        return false;
    }

    return (left.numerator * right.denominator) == (right.numerator * left.denominator);
}

std::optional<std::filesystem::path> normalize_optional_path(const std::optional<std::filesystem::path> &path) {
    if (!path.has_value() || path->empty()) {
        return std::nullopt;
    }

    return path->lexically_normal();
}

MediaInspectionResult inspect_segment(
    const TimelineSegmentKind kind,
    const std::filesystem::path &source_path
) {
    const auto inspection_result = MediaInspector::inspect(source_path);
    if (inspection_result.succeeded()) {
        return inspection_result;
    }

    return MediaInspectionResult{
        .media_source_info = std::nullopt,
        .error = media::MediaInspectionError{
            .input_path = inspection_result.error->input_path,
            .message = "Failed to inspect the " + std::string(to_string(kind)) + " segment. " +
                inspection_result.error->message,
            .actionable_hint = inspection_result.error->actionable_hint
        }
    };
}

Rational choose_output_video_time_base(const media::VideoStreamInfo &video_stream) {
    if (rational_is_positive(video_stream.timestamps.time_base)) {
        return video_stream.timestamps.time_base;
    }

    if (rational_is_positive(video_stream.average_frame_rate)) {
        return Rational{
            .numerator = video_stream.average_frame_rate.denominator,
            .denominator = video_stream.average_frame_rate.numerator
        };
    }

    throw std::runtime_error("The main video stream does not expose a usable time base for timeline composition.");
}

Rational choose_output_frame_rate(
    const media::VideoStreamInfo &video_stream,
    const Rational &output_video_time_base
) {
    if (rational_is_positive(video_stream.average_frame_rate)) {
        return video_stream.average_frame_rate;
    }

    if (rational_is_positive(output_video_time_base)) {
        return Rational{
            .numerator = output_video_time_base.denominator,
            .denominator = output_video_time_base.numerator
        };
    }

    throw std::runtime_error("The main video stream does not expose a usable frame rate for timeline composition.");
}

void validate_video_stream_presence(
    const TimelineSegmentKind kind,
    const MediaSourceInfo &segment_info
) {
    if (segment_info.primary_video_stream.has_value()) {
        return;
    }

    throw std::runtime_error(
        "The " + std::string(to_string(kind)) + " segment does not contain a primary video stream."
    );
}

void validate_video_compatibility(
    const TimelineSegmentKind kind,
    const media::VideoStreamInfo &main_video,
    const media::VideoStreamInfo &candidate_video
) {
    if (candidate_video.width != main_video.width || candidate_video.height != main_video.height) {
        throw std::runtime_error(
            "The " + std::string(to_string(kind)) + " segment resolution " +
            std::to_string(candidate_video.width) + "x" + std::to_string(candidate_video.height) +
            " does not match the main segment resolution " +
            std::to_string(main_video.width) + "x" + std::to_string(main_video.height) + "."
        );
    }

    if (rational_is_positive(main_video.sample_aspect_ratio) &&
        rational_is_positive(candidate_video.sample_aspect_ratio) &&
        !rationals_equal(candidate_video.sample_aspect_ratio, main_video.sample_aspect_ratio)) {
        throw std::runtime_error(
            "The " + std::string(to_string(kind)) + " segment sample aspect ratio " +
            std::to_string(candidate_video.sample_aspect_ratio.numerator) + "/" +
            std::to_string(candidate_video.sample_aspect_ratio.denominator) +
            " does not match the main segment sample aspect ratio " +
            std::to_string(main_video.sample_aspect_ratio.numerator) + "/" +
            std::to_string(main_video.sample_aspect_ratio.denominator) + "."
        );
    }

    if (!rational_is_positive(candidate_video.average_frame_rate)) {
        throw std::runtime_error(
            "The " + std::string(to_string(kind)) + " segment does not expose a usable average frame rate."
        );
    }

    if (!rationals_equal(candidate_video.average_frame_rate, main_video.average_frame_rate)) {
        throw std::runtime_error(
            "The " + std::string(to_string(kind)) + " segment frame rate " +
            std::to_string(candidate_video.average_frame_rate.numerator) + "/" +
            std::to_string(candidate_video.average_frame_rate.denominator) +
            " does not match the main segment frame rate " +
            std::to_string(main_video.average_frame_rate.numerator) + "/" +
            std::to_string(main_video.average_frame_rate.denominator) + "."
        );
    }
}

bool channel_layouts_conflict(const AudioStreamInfo &left, const AudioStreamInfo &right) {
    if (left.channel_layout_name.empty() || left.channel_layout_name == "unknown") {
        return false;
    }

    if (right.channel_layout_name.empty() || right.channel_layout_name == "unknown") {
        return false;
    }

    return left.channel_layout_name != right.channel_layout_name;
}

void validate_audio_compatibility(
    const TimelineSegmentKind kind,
    const std::optional<AudioStreamInfo> &main_audio,
    const std::optional<AudioStreamInfo> &candidate_audio
) {
    if (!main_audio.has_value()) {
        if (candidate_audio.has_value()) {
            throw std::runtime_error(
                "The " + std::string(to_string(kind)) +
                " segment contains audio but the main segment does not define output audio for this milestone."
            );
        }

        return;
    }

    if (!candidate_audio.has_value()) {
        return;
    }

    if (candidate_audio->sample_rate != main_audio->sample_rate) {
        throw std::runtime_error(
            "The " + std::string(to_string(kind)) + " segment audio sample rate " +
            std::to_string(candidate_audio->sample_rate) +
            " does not match the main segment audio sample rate " +
            std::to_string(main_audio->sample_rate) + "."
        );
    }

    if (candidate_audio->channel_count != main_audio->channel_count) {
        throw std::runtime_error(
            "The " + std::string(to_string(kind)) + " segment audio channel count " +
            std::to_string(candidate_audio->channel_count) +
            " does not match the main segment audio channel count " +
            std::to_string(main_audio->channel_count) + "."
        );
    }

    if (channel_layouts_conflict(*main_audio, *candidate_audio)) {
        throw std::runtime_error(
            "The " + std::string(to_string(kind)) + " segment audio channel layout '" +
            candidate_audio->channel_layout_name +
            "' does not match the main segment audio channel layout '" +
            main_audio->channel_layout_name + "'."
        );
    }
}

VideoCadence derive_video_cadence(
    const DecodedMediaSource &main_segment,
    const Rational &output_video_time_base
) {
    if (main_segment.video_frames.empty()) {
        throw std::runtime_error("The main segment decode did not produce any video frames.");
    }

    const auto &first_frame = main_segment.video_frames.front();
    if (first_frame.pixel_format != media::NormalizedVideoPixelFormat::rgba8 || first_frame.planes.size() != 1) {
        throw std::runtime_error("Timeline composition requires decoded rgba8 video frames with a single plane.");
    }

    const auto first_frame_duration = first_frame.timestamp.source_duration.value_or(0);
    if (first_frame_duration <= 0 || !rational_is_positive(first_frame.timestamp.source_time_base)) {
        throw std::runtime_error(
            "The main segment decode did not expose a usable frame duration for timeline composition."
        );
    }

    const std::int64_t frame_duration_pts = rescale_value(
        first_frame_duration,
        first_frame.timestamp.source_time_base,
        output_video_time_base
    );
    if (frame_duration_pts <= 0) {
        throw std::runtime_error("The main segment frame duration could not be converted into the output time base.");
    }

    for (std::size_t index = 0; index < main_segment.video_frames.size(); ++index) {
        const auto &frame = main_segment.video_frames[index];
        if (frame.pixel_format != media::NormalizedVideoPixelFormat::rgba8 || frame.planes.size() != 1) {
            throw std::runtime_error("Timeline composition encountered an unsupported decoded video frame layout.");
        }

        if (frame.width != first_frame.width || frame.height != first_frame.height) {
            throw std::runtime_error("Timeline composition requires every segment to keep one constant resolution.");
        }

        if (!rationals_equal(frame.sample_aspect_ratio, first_frame.sample_aspect_ratio)) {
            throw std::runtime_error(
                "Timeline composition requires every segment to keep one constant sample aspect ratio."
            );
        }

        const auto frame_duration = frame.timestamp.source_duration.value_or(0);
        if (frame_duration <= 0 || !rational_is_positive(frame.timestamp.source_time_base)) {
            throw std::runtime_error("A decoded main-segment frame is missing timing information.");
        }

        const auto converted_duration = rescale_value(
            frame_duration,
            frame.timestamp.source_time_base,
            output_video_time_base
        );
        if (converted_duration != frame_duration_pts) {
            throw std::runtime_error(
                "The main segment uses variable or incompatible frame durations. This milestone requires constant cadence."
            );
        }

        if (index == 0) {
            continue;
        }

        const auto &previous_frame = main_segment.video_frames[index - 1];
        const auto previous_pts = rescale_value(
            previous_frame.timestamp.source_pts.value_or(0),
            previous_frame.timestamp.source_time_base,
            output_video_time_base
        );
        const auto current_pts = rescale_value(
            frame.timestamp.source_pts.value_or(0),
            frame.timestamp.source_time_base,
            output_video_time_base
        );

        if ((current_pts - previous_pts) != frame_duration_pts) {
            throw std::runtime_error(
                "The main segment cadence is not strictly constant after normalization. VFR composition is not supported yet."
            );
        }
    }

    return VideoCadence{
        .width = first_frame.width,
        .height = first_frame.height,
        .sample_aspect_ratio = first_frame.sample_aspect_ratio,
        .frame_duration_pts = frame_duration_pts,
        .frame_duration_microseconds = rescale_to_microseconds(frame_duration_pts, output_video_time_base)
    };
}

void validate_segment_frames(
    const TimelineSegmentKind kind,
    const DecodedMediaSource &decoded_segment,
    const VideoCadence &video_cadence,
    const Rational &output_video_time_base
) {
    if (decoded_segment.video_frames.empty()) {
        throw std::runtime_error(
            "The " + std::string(to_string(kind)) + " segment decode did not produce any video frames."
        );
    }

    for (std::size_t index = 0; index < decoded_segment.video_frames.size(); ++index) {
        const auto &frame = decoded_segment.video_frames[index];
        if (frame.pixel_format != media::NormalizedVideoPixelFormat::rgba8 || frame.planes.size() != 1) {
            throw std::runtime_error(
                "The " + std::string(to_string(kind)) + " segment contains an unsupported decoded video frame layout."
            );
        }

        if (frame.width != video_cadence.width || frame.height != video_cadence.height) {
            throw std::runtime_error(
                "The " + std::string(to_string(kind)) +
                " segment decoded into a resolution that does not match the main segment."
            );
        }

        if (!rationals_equal(frame.sample_aspect_ratio, video_cadence.sample_aspect_ratio)) {
            throw std::runtime_error(
                "The " + std::string(to_string(kind)) +
                " segment decoded with a sample aspect ratio that does not match the main segment."
            );
        }

        const auto frame_duration = frame.timestamp.source_duration.value_or(0);
        if (frame_duration <= 0 || !rational_is_positive(frame.timestamp.source_time_base)) {
            throw std::runtime_error(
                "The " + std::string(to_string(kind)) + " segment contains a decoded frame with missing timing data."
            );
        }

        const auto converted_duration = rescale_value(
            frame_duration,
            frame.timestamp.source_time_base,
            output_video_time_base
        );
        if (converted_duration != video_cadence.frame_duration_pts) {
            throw std::runtime_error(
                "The " + std::string(to_string(kind)) +
                " segment frame cadence does not match the main segment cadence."
            );
        }

        if (index == 0) {
            continue;
        }

        const auto &previous_frame = decoded_segment.video_frames[index - 1];
        const auto previous_pts = rescale_value(
            previous_frame.timestamp.source_pts.value_or(0),
            previous_frame.timestamp.source_time_base,
            output_video_time_base
        );
        const auto current_pts = rescale_value(
            frame.timestamp.source_pts.value_or(0),
            frame.timestamp.source_time_base,
            output_video_time_base
        );

        if ((current_pts - previous_pts) != video_cadence.frame_duration_pts) {
            throw std::runtime_error(
                "The " + std::string(to_string(kind)) +
                " segment timestamps do not advance at the main segment cadence."
            );
        }
    }
}

std::int64_t count_audio_samples(const std::vector<DecodedAudioSamples> &audio_blocks) {
    std::int64_t total_samples = 0;
    for (const auto &audio_block : audio_blocks) {
        total_samples += audio_block.samples_per_channel;
    }

    return total_samples;
}

DecodedAudioSamples make_audio_block(
    const AudioStreamInfo &output_audio_stream,
    const media::DecodeNormalizationPolicy &normalization_policy,
    const std::int64_t block_index,
    const std::int64_t source_pts,
    const int samples_per_channel,
    const bool silent,
    const std::vector<std::vector<float>> &channel_samples
) {
    std::vector<std::vector<float>> output_channels{};
    if (silent) {
        output_channels.resize(
            static_cast<std::size_t>(output_audio_stream.channel_count),
            std::vector<float>(static_cast<std::size_t>(samples_per_channel), 0.0F)
        );
    } else {
        output_channels = channel_samples;
    }

    return DecodedAudioSamples{
        .stream_index = output_audio_stream.stream_index,
        .block_index = block_index,
        .timestamp = {
            .source_time_base = output_audio_stream.timestamps.time_base,
            .source_pts = source_pts,
            .source_duration = samples_per_channel,
            .origin = TimestampOrigin::stream_cursor,
            .start_microseconds = rescale_to_microseconds(source_pts, output_audio_stream.timestamps.time_base),
            .duration_microseconds = rescale_to_microseconds(samples_per_channel, output_audio_stream.timestamps.time_base)
        },
        .sample_rate = output_audio_stream.sample_rate,
        .channel_count = output_audio_stream.channel_count,
        .channel_layout_name = output_audio_stream.channel_layout_name,
        .sample_format = normalization_policy.audio_sample_format,
        .samples_per_channel = samples_per_channel,
        .channel_samples = std::move(output_channels)
    };
}

void append_audio_segment(
    const TimelineSegmentPlan &segment_plan,
    const DecodedMediaSource &decoded_segment,
    const std::int64_t expected_segment_samples,
    const AudioStreamInfo &output_audio_stream,
    std::vector<DecodedAudioSamples> &output_audio_blocks,
    TimelineSegmentSummary &segment_summary,
    std::int64_t &next_output_audio_pts
) {
    if (!decoded_segment.source_info.primary_audio_stream.has_value()) {
        const auto starting_block_count = static_cast<std::int64_t>(output_audio_blocks.size());
        segment_summary.inserted_silence = expected_segment_samples > 0;
        if (decoded_segment.normalization_policy.audio_block_samples <= 0) {
            throw std::runtime_error("Timeline composition requires a positive normalized audio block size.");
        }

        int samples_remaining = static_cast<int>(expected_segment_samples);
        while (samples_remaining > 0) {
            const int block_size = std::min(samples_remaining, decoded_segment.normalization_policy.audio_block_samples);
            output_audio_blocks.push_back(make_audio_block(
                output_audio_stream,
                decoded_segment.normalization_policy,
                static_cast<std::int64_t>(output_audio_blocks.size()),
                next_output_audio_pts,
                block_size,
                true,
                {}
            ));
            next_output_audio_pts += block_size;
            samples_remaining -= block_size;
        }

        segment_summary.audio_block_count = static_cast<std::int64_t>(output_audio_blocks.size()) - starting_block_count;
        return;
    }

    const auto &segment_audio_stream = *decoded_segment.source_info.primary_audio_stream;
    if (segment_audio_stream.sample_rate != output_audio_stream.sample_rate ||
        segment_audio_stream.channel_count != output_audio_stream.channel_count) {
        throw std::runtime_error(
            "The decoded " + std::string(to_string(segment_plan.kind)) +
            " segment audio layout no longer matches the main segment."
        );
    }

    const auto actual_segment_samples = count_audio_samples(decoded_segment.audio_blocks);
    if (actual_segment_samples != expected_segment_samples) {
        throw std::runtime_error(
            "The decoded " + std::string(to_string(segment_plan.kind)) +
            " segment audio duration does not match its video duration at the main cadence."
        );
    }

    const auto starting_block_count = static_cast<std::int64_t>(output_audio_blocks.size());

    for (const auto &audio_block : decoded_segment.audio_blocks) {
        if (audio_block.sample_format != decoded_segment.normalization_policy.audio_sample_format) {
            throw std::runtime_error(
                "The decoded " + std::string(to_string(segment_plan.kind)) +
                " segment audio format changed unexpectedly during composition."
            );
        }

        if (audio_block.channel_count != output_audio_stream.channel_count ||
            audio_block.sample_rate != output_audio_stream.sample_rate) {
            throw std::runtime_error(
                "The decoded " + std::string(to_string(segment_plan.kind)) +
                " segment audio block shape does not match the main segment."
            );
        }

        output_audio_blocks.push_back(make_audio_block(
            output_audio_stream,
            decoded_segment.normalization_policy,
            static_cast<std::int64_t>(output_audio_blocks.size()),
            next_output_audio_pts,
            audio_block.samples_per_channel,
            false,
            audio_block.channel_samples
        ));
        next_output_audio_pts += audio_block.samples_per_channel;
    }

    segment_summary.audio_block_count = static_cast<std::int64_t>(output_audio_blocks.size()) - starting_block_count;
}

DecodedMediaSource build_composed_media_source(
    const TimelinePlan &timeline_plan,
    const TimelineCompositionSummary &timeline_summary,
    const VideoCadence &video_cadence,
    std::vector<DecodedVideoFrame> video_frames,
    std::vector<DecodedAudioSamples> audio_blocks,
    const media::DecodeNormalizationPolicy &normalization_policy
) {
    const auto &main_segment_plan = timeline_plan.segments[timeline_plan.main_segment_index];
    const auto &main_video_stream = *main_segment_plan.inspected_source_info.primary_video_stream;

    MediaSourceInfo source_info{
        .input_name = "timeline",
        .container_format_name = "timeline",
        .container_duration_microseconds = timeline_summary.output_duration_microseconds,
        .primary_video_stream = media::VideoStreamInfo{
            .stream_index = main_video_stream.stream_index,
            .codec_name = main_video_stream.codec_name,
            .width = video_cadence.width,
            .height = video_cadence.height,
            .sample_aspect_ratio = video_cadence.sample_aspect_ratio,
            .pixel_format_name = "rgba",
            .average_frame_rate = timeline_plan.output_frame_rate,
            .timestamps = {
                .time_base = timeline_plan.output_video_time_base,
                .start_pts = 0,
                .duration_pts = static_cast<std::int64_t>(video_frames.size()) * video_cadence.frame_duration_pts
            },
            .frame_count = static_cast<std::int64_t>(video_frames.size())
        },
        .primary_audio_stream = std::nullopt
    };

    if (timeline_plan.output_audio_stream.has_value()) {
        auto output_audio_stream = *timeline_plan.output_audio_stream;
        output_audio_stream.timestamps.start_pts = 0;
        output_audio_stream.timestamps.duration_pts = count_audio_samples(audio_blocks);
        output_audio_stream.frame_count = count_audio_samples(audio_blocks);
        source_info.primary_audio_stream = output_audio_stream;
    }

    return DecodedMediaSource{
        .source_info = std::move(source_info),
        .normalization_policy = normalization_policy,
        .video_frames = std::move(video_frames),
        .audio_blocks = std::move(audio_blocks)
    };
}

}  // namespace

}  // namespace utsure::core::timeline
