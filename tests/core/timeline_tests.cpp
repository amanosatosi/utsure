#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using utsure::core::media::DecodedAudioSamples;
using utsure::core::media::DecodedMediaSource;
using utsure::core::media::MediaDecodeResult;
using utsure::core::media::MediaDecoder;
using utsure::core::media::Rational;
using utsure::core::timeline::SubtitleTimingMode;
using utsure::core::timeline::TimelineAssembler;
using utsure::core::timeline::TimelineAssemblyRequest;
using utsure::core::timeline::TimelineAssemblyResult;
using utsure::core::timeline::TimelineComposer;
using utsure::core::timeline::TimelineCompositionOutput;
using utsure::core::timeline::TimelineCompositionResult;
using utsure::core::timeline::TimelinePlan;
using utsure::core::timeline::TimelineSegmentKind;
using utsure::core::timeline::to_string;

struct ComposedTimelineContext final {
    TimelinePlan plan{};
    TimelineCompositionOutput output{};
};

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

std::string format_rational(const Rational &value) {
    if (!value.is_valid()) {
        return "unknown";
    }

    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

bool frames_are_identical(
    const DecodedMediaSource &decoded_media_source,
    const std::size_t left_index,
    const std::size_t right_index
) {
    return decoded_media_source.video_frames[left_index].planes.front().bytes ==
        decoded_media_source.video_frames[right_index].planes.front().bytes;
}

bool audio_block_is_silent(const DecodedAudioSamples &audio_block) {
    for (const auto &channel : audio_block.channel_samples) {
        for (const auto sample : channel) {
            if (sample != 0.0F) {
                return false;
            }
        }
    }

    return true;
}

std::string build_summary(const TimelineCompositionOutput &output) {
    std::string summary;
    summary += "timeline.output.video_time_base=" + format_rational(output.timeline_summary.output_video_time_base);
    summary += "\ntimeline.output.frame_rate=" + format_rational(output.timeline_summary.output_frame_rate);
    summary += "\ntimeline.output.video_frames=" + std::to_string(output.timeline_summary.output_video_frame_count);
    summary += "\ntimeline.output.audio_blocks=" + std::to_string(output.timeline_summary.output_audio_block_count);
    summary += "\ntimeline.output.duration_us=" + std::to_string(output.timeline_summary.output_duration_microseconds);
    for (std::size_t index = 0; index < output.timeline_summary.segments.size(); ++index) {
        const auto &segment = output.timeline_summary.segments[index];
        summary += "\nsegment[" + std::to_string(index) + "].kind=" + std::string(to_string(segment.kind));
        summary += "\nsegment[" + std::to_string(index) + "].start_us=" + std::to_string(segment.start_microseconds);
        summary += "\nsegment[" + std::to_string(index) + "].duration_us=" + std::to_string(segment.duration_microseconds);
        summary += "\nsegment[" + std::to_string(index) + "].video_frames=" + std::to_string(segment.video_frame_count);
        summary += "\nsegment[" + std::to_string(index) + "].audio_blocks=" + std::to_string(segment.audio_block_count);
        summary += "\nsegment[" + std::to_string(index) + "].subtitles=" +
            std::string(segment.subtitles_enabled ? "yes" : "no");
        summary += "\nsegment[" + std::to_string(index) + "].inserted_silence=" +
            std::string(segment.inserted_silence ? "yes" : "no");
    }

    return summary;
}

std::optional<ComposedTimelineContext> compose_timeline(const TimelineAssemblyRequest &request) {
    const TimelineAssemblyResult assembly_result = TimelineAssembler::assemble(request);
    if (!assembly_result.succeeded()) {
        std::cerr << assembly_result.error->message << '\n';
        std::cerr << assembly_result.error->actionable_hint << '\n';
        return std::nullopt;
    }

    std::vector<DecodedMediaSource> decoded_segments{};
    decoded_segments.reserve(assembly_result.timeline_plan->segments.size());

    for (const auto &segment : assembly_result.timeline_plan->segments) {
        const MediaDecodeResult decode_result = MediaDecoder::decode(segment.source_path);
        if (!decode_result.succeeded()) {
            std::cerr << decode_result.error->message << '\n';
            std::cerr << decode_result.error->actionable_hint << '\n';
            return std::nullopt;
        }

        decoded_segments.push_back(*decode_result.decoded_media_source);
    }

    const TimelineCompositionResult composition_result =
        TimelineComposer::compose(*assembly_result.timeline_plan, decoded_segments);
    if (!composition_result.succeeded()) {
        std::cerr << composition_result.error->message << '\n';
        std::cerr << composition_result.error->actionable_hint << '\n';
        return std::nullopt;
    }

    return ComposedTimelineContext{
        .plan = *assembly_result.timeline_plan,
        .output = *composition_result.output
    };
}

int assert_main_only(const std::filesystem::path &main_path) {
    const auto context = compose_timeline(TimelineAssemblyRequest{
        .main_source_path = main_path,
        .subtitles_present = false,
        .subtitle_timing_mode = SubtitleTimingMode::main_segment_only
    });
    if (!context.has_value()) {
        return 1;
    }

    const auto &summary = context->output.timeline_summary;
    const auto &decoded = context->output.decoded_media_source;

    if (summary.segments.size() != 1 || summary.output_video_frame_count != 48 || summary.output_audio_block_count != 94) {
        return fail("Unexpected main-only timeline counts.");
    }

    if (format_rational(summary.output_frame_rate) != "24/1" ||
        format_rational(summary.output_video_time_base) != "1/24") {
        return fail("Unexpected main-only timeline cadence.");
    }

    if (summary.segments[0].kind != TimelineSegmentKind::main ||
        summary.segments[0].start_microseconds != 0 ||
        summary.segments[0].duration_microseconds != 2000000 ||
        summary.segments[0].inserted_silence ||
        summary.segments[0].subtitles_enabled) {
        return fail("Unexpected main-only segment summary.");
    }

    if (decoded.video_frames[0].timestamp.start_microseconds != 0 ||
        decoded.video_frames[1].timestamp.start_microseconds != 41667 ||
        decoded.video_frames[47].timestamp.start_microseconds != 1958333) {
        return fail("Unexpected main-only video timestamps.");
    }

    if (decoded.audio_blocks[0].timestamp.start_microseconds != 0 ||
        decoded.audio_blocks[47].timestamp.start_microseconds != 1002667) {
        return fail("Unexpected main-only audio timestamps.");
    }

    std::cout << build_summary(context->output) << '\n';
    return 0;
}

int assert_intro_main(const std::filesystem::path &intro_path, const std::filesystem::path &main_path) {
    const auto context = compose_timeline(TimelineAssemblyRequest{
        .intro_source_path = intro_path,
        .main_source_path = main_path,
        .subtitles_present = false,
        .subtitle_timing_mode = SubtitleTimingMode::main_segment_only
    });
    if (!context.has_value()) {
        return 1;
    }

    const auto &summary = context->output.timeline_summary;
    const auto &decoded = context->output.decoded_media_source;

    if (summary.segments.size() != 2 || summary.output_video_frame_count != 72 || summary.output_audio_block_count != 141) {
        return fail("Unexpected intro+main timeline counts.");
    }

    if (summary.segments[0].kind != TimelineSegmentKind::intro ||
        summary.segments[0].start_microseconds != 0 ||
        summary.segments[0].duration_microseconds != 1000000 ||
        summary.segments[0].audio_block_count != 47 ||
        summary.segments[0].inserted_silence) {
        return fail("Unexpected intro segment summary.");
    }

    if (summary.segments[1].kind != TimelineSegmentKind::main ||
        summary.segments[1].start_microseconds != 1000000 ||
        summary.segments[1].duration_microseconds != 2000000) {
        return fail("Unexpected intro+main main-segment summary.");
    }

    if (decoded.video_frames[24].timestamp.start_microseconds != 1000000 ||
        frames_are_identical(decoded, 0U, 24U)) {
        return fail("Intro+main video boundary was not stitched correctly.");
    }

    if (decoded.audio_blocks[47].timestamp.start_microseconds != 1000000 ||
        audio_block_is_silent(decoded.audio_blocks[47])) {
        return fail("Intro+main audio boundary was not stitched correctly.");
    }

    std::cout << build_summary(context->output) << '\n';
    return 0;
}

int assert_main_outro(const std::filesystem::path &main_path, const std::filesystem::path &outro_path) {
    const auto context = compose_timeline(TimelineAssemblyRequest{
        .main_source_path = main_path,
        .outro_source_path = outro_path,
        .subtitles_present = false,
        .subtitle_timing_mode = SubtitleTimingMode::main_segment_only
    });
    if (!context.has_value()) {
        return 1;
    }

    const auto &summary = context->output.timeline_summary;
    const auto &decoded = context->output.decoded_media_source;

    if (summary.segments.size() != 2 || summary.output_video_frame_count != 72 || summary.output_audio_block_count != 141) {
        return fail("Unexpected main+outro timeline counts.");
    }

    if (summary.segments[1].kind != TimelineSegmentKind::outro ||
        summary.segments[1].start_microseconds != 2000000 ||
        summary.segments[1].duration_microseconds != 1000000 ||
        !summary.segments[1].inserted_silence ||
        summary.segments[1].audio_block_count != 47) {
        return fail("Unexpected outro segment summary.");
    }

    if (decoded.video_frames[48].timestamp.start_microseconds != 2000000 ||
        frames_are_identical(decoded, 0U, 48U)) {
        return fail("Main+outro video boundary was not stitched correctly.");
    }

    if (decoded.audio_blocks[94].timestamp.start_microseconds != 2000000 ||
        !audio_block_is_silent(decoded.audio_blocks[94])) {
        return fail("Main+outro silence insertion was not aligned correctly.");
    }

    std::cout << build_summary(context->output) << '\n';
    return 0;
}

int assert_intro_main_outro(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path
) {
    const auto context = compose_timeline(TimelineAssemblyRequest{
        .intro_source_path = intro_path,
        .main_source_path = main_path,
        .outro_source_path = outro_path,
        .subtitles_present = false,
        .subtitle_timing_mode = SubtitleTimingMode::main_segment_only
    });
    if (!context.has_value()) {
        return 1;
    }

    const auto &summary = context->output.timeline_summary;
    const auto &decoded = context->output.decoded_media_source;

    if (summary.segments.size() != 3 || summary.output_video_frame_count != 96 || summary.output_audio_block_count != 188) {
        return fail("Unexpected intro+main+outro timeline counts.");
    }

    if (summary.output_duration_microseconds != 4000000 ||
        summary.segments[1].start_microseconds != 1000000 ||
        summary.segments[2].start_microseconds != 3000000) {
        return fail("Unexpected intro+main+outro segment timing.");
    }

    if (!summary.segments[2].inserted_silence ||
        decoded.video_frames[72].timestamp.start_microseconds != 3000000 ||
        decoded.audio_blocks[141].timestamp.start_microseconds != 3000000) {
        return fail("Unexpected intro+main+outro outro alignment.");
    }

    if (frames_are_identical(decoded, 0U, 24U) || frames_are_identical(decoded, 24U, 72U)) {
        return fail("Intro, main, and outro frames were not kept visually distinct.");
    }

    if (audio_block_is_silent(decoded.audio_blocks[0]) ||
        audio_block_is_silent(decoded.audio_blocks[47]) ||
        !audio_block_is_silent(decoded.audio_blocks[141])) {
        return fail("Unexpected intro+main+outro audio carry-through.");
    }

    std::cout << build_summary(context->output) << '\n';
    return 0;
}

int assert_subtitle_scope(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path
) {
    const TimelineAssemblyResult main_only_result = TimelineAssembler::assemble(TimelineAssemblyRequest{
        .intro_source_path = intro_path,
        .main_source_path = main_path,
        .outro_source_path = outro_path,
        .subtitles_present = true,
        .subtitle_timing_mode = SubtitleTimingMode::main_segment_only
    });
    if (!main_only_result.succeeded()) {
        return fail("Main-only subtitle-scope assembly failed unexpectedly.");
    }

    if (main_only_result.timeline_plan->segments.size() != 3 ||
        main_only_result.timeline_plan->segments[0].subtitles_enabled ||
        !main_only_result.timeline_plan->segments[1].subtitles_enabled ||
        main_only_result.timeline_plan->segments[2].subtitles_enabled) {
        return fail("Main-only subtitle scope did not stay on the main segment.");
    }

    const TimelineAssemblyResult full_timeline_result = TimelineAssembler::assemble(TimelineAssemblyRequest{
        .intro_source_path = intro_path,
        .main_source_path = main_path,
        .outro_source_path = outro_path,
        .subtitles_present = true,
        .subtitle_timing_mode = SubtitleTimingMode::full_output_timeline
    });
    if (!full_timeline_result.succeeded()) {
        return fail("Full-timeline subtitle-scope assembly failed unexpectedly.");
    }

    if (!full_timeline_result.timeline_plan->segments[0].subtitles_enabled ||
        !full_timeline_result.timeline_plan->segments[1].subtitles_enabled ||
        !full_timeline_result.timeline_plan->segments[2].subtitles_enabled) {
        return fail("Full-output subtitle scope did not enable every segment.");
    }

    std::cout << "subtitle.scope.main_only=intro:no,main:yes,outro:no\n";
    std::cout << "subtitle.scope.full_output=intro:yes,main:yes,outro:yes\n";
    return 0;
}

int assert_normalized_fps_intro(const std::filesystem::path &intro_path, const std::filesystem::path &main_path) {
    const auto context = compose_timeline(TimelineAssemblyRequest{
        .intro_source_path = intro_path,
        .main_source_path = main_path,
        .subtitles_present = false,
        .subtitle_timing_mode = SubtitleTimingMode::main_segment_only
    });
    if (!context.has_value()) {
        return 1;
    }

    const auto &summary = context->output.timeline_summary;
    const auto &decoded = context->output.decoded_media_source;

    if (summary.segments.size() != 2 ||
        summary.segments[0].kind != TimelineSegmentKind::intro ||
        summary.segments[1].kind != TimelineSegmentKind::main) {
        return fail("Unexpected normalized intro/main segment order.");
    }

    if (format_rational(summary.output_frame_rate) != "24000/1001" ||
        format_rational(summary.output_video_time_base) != "1/1000") {
        return fail("The normalized intro/main timeline did not keep the main cadence authoritative.");
    }

    if (summary.segments[0].video_frame_count != 24 ||
        summary.segments[0].duration_microseconds != 1000000 ||
        summary.segments[1].start_microseconds != 1000000 ||
        summary.output_video_frame_count != 72) {
        return fail("Unexpected normalized intro/main frame counts or segment timing.");
    }

    if (decoded.video_frames[0].timestamp.start_microseconds != 0 ||
        decoded.video_frames[0].timestamp.duration_microseconds != 42000 ||
        decoded.video_frames[1].timestamp.start_microseconds != 42000 ||
        decoded.video_frames[1].timestamp.duration_microseconds != 41000 ||
        decoded.video_frames[2].timestamp.start_microseconds != 83000 ||
        decoded.video_frames[23].timestamp.start_microseconds != 959000 ||
        decoded.video_frames[23].timestamp.duration_microseconds != 41000 ||
        decoded.video_frames[24].timestamp.start_microseconds != 1000000 ||
        decoded.video_frames[25].timestamp.start_microseconds != 1042000) {
        return fail("The normalized intro cadence did not produce the expected output timestamps.");
    }

    if (audio_block_is_silent(decoded.audio_blocks[0])) {
        return fail("The normalized intro audio should not have been dropped.");
    }

    std::cout << build_summary(context->output) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 3) {
        return fail(
            "Usage: utsure_core_timeline_tests "
            "[--main-only <main>|--intro-main <intro> <main>|--main-outro <main> <outro>|"
            "--intro-main-outro <intro> <main> <outro>|--subtitle-scope <intro> <main> <outro>|"
            "--normalized-fps <bad-intro> <main>]"
        );
    }

    const std::string_view mode(argv[1]);

    if (mode == "--main-only" && argc == 3) {
        return assert_main_only(std::filesystem::path(argv[2]));
    }

    if (mode == "--intro-main" && argc == 4) {
        return assert_intro_main(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
    }

    if (mode == "--main-outro" && argc == 4) {
        return assert_main_outro(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
    }

    if (mode == "--intro-main-outro" && argc == 5) {
        return assert_intro_main_outro(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4])
        );
    }

    if (mode == "--subtitle-scope" && argc == 5) {
        return assert_subtitle_scope(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4])
        );
    }

    if (mode == "--normalized-fps" && argc == 4) {
        return assert_normalized_fps_intro(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
    }

    return fail("Unknown mode or wrong argument count for utsure_core_timeline_tests.");
}
