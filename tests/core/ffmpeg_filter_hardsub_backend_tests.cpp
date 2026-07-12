#include "../../src/core/src/job/ffmpeg_filter_hardsub_backend.hpp"

#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int fail(const std::string &message) {
    std::cerr << message << '\n';
    return 1;
}

utsure::core::timeline::TimelinePlan make_plan() {
    using namespace utsure::core;

    const media::VideoStreamInfo video_stream{
        .stream_index = 0,
        .codec_name = "h264",
        .width = 1920,
        .height = 1080,
        .sample_aspect_ratio = media::Rational{1, 1},
        .pixel_format_name = "yuv420p",
        .average_frame_rate = media::Rational{24000, 1001},
        .timestamps = {
            .time_base = media::Rational{1, 24000},
            .start_pts = 0,
            .duration_pts = 24000
        },
        .frame_count = 24
    };
    const media::AudioStreamInfo audio_stream{
        .stream_index = 1,
        .codec_name = "aac",
        .sample_format_name = "fltp",
        .sample_rate = 48000,
        .channel_count = 2,
        .channel_layout_name = "stereo",
        .decoder_available = true,
        .timestamps = {
            .time_base = media::Rational{1, 48000},
            .start_pts = 0,
            .duration_pts = 48000
        },
        .frame_count = 48000
    };
    const media::MediaSourceInfo source_info{
        .input_name = "input.mkv",
        .container_format_name = "matroska",
        .container_duration_microseconds = 1000000,
        .primary_video_stream = video_stream,
        .audio_streams = {audio_stream},
        .selected_audio_stream_index = 1,
        .primary_audio_stream = audio_stream
    };

    return timeline::TimelinePlan{
        .segments = {
            timeline::TimelineSegmentPlan{
                .kind = timeline::TimelineSegmentKind::main,
                .source_path = std::filesystem::path("C:\\anime\\input.mkv"),
                .inspected_source_info = source_info,
                .subtitles_enabled = true
            }
        },
        .main_segment_index = 0,
        .output_video_time_base = media::Rational{1, 24000},
        .output_frame_rate = media::Rational{24000, 1001},
        .output_video_shape = timeline::TimelineOutputVideoShape{
            .width = 1920,
            .height = 1080,
            .sample_aspect_ratio = media::Rational{1, 1}
        },
        .output_audio_stream = audio_stream
    };
}

utsure::core::job::EncodeJob make_job() {
    using namespace utsure::core;

    return job::EncodeJob{
        .input = {
            .main_source_path = std::filesystem::path("C:\\anime\\input.mkv")
        },
        .subtitles = job::EncodeJobSubtitleSettings{
            .subtitle_path = std::filesystem::path("C:\\anime\\youjo senki 2\\[SubsPlease] Youjo Senki S2 - 01.ass"),
            .format_hint = "ass"
        },
        .output = {
            .output_path = std::filesystem::path("C:\\anime\\output.mp4"),
            .video = {
                .codec = media::OutputVideoCodec::h265,
                .preset = "slow",
                .crf = 18
            },
            .audio = {
                .mode = media::AudioOutputMode::encode_aac,
                .bitrate_kbps = 192
            }
        }
    };
}

int assert_filter_value_escaping() {
    using utsure::core::job::EscapeFfmpegFilterValue;

    const std::string brackets = EscapeFfmpegFilterValue(
        std::filesystem::path("C:\\anime\\youjo senki 2\\[SubsPlease] Youjo Senki S2 - 01.ass")
    );
    if (brackets != R"(C\:\\anime\\youjo senki 2\\\[SubsPlease\] Youjo Senki S2 - 01.ass)") {
        return fail("Windows path escaping did not protect drive colon, backslashes, or brackets: " + brackets);
    }

    const std::string comma = EscapeFfmpegFilterValue(
        std::filesystem::path("C:\\anime\\Toumei na Yoru ni Kakeru Kimi to, Me ni Mienai Koi wo Shita\\subs.ass")
    );
    if (comma.find("to\\, Me") == std::string::npos) {
        return fail("Windows path escaping did not protect commas: " + comma);
    }

    const std::string apostrophe = EscapeFfmpegFilterValue(
        std::filesystem::path("C:\\anime\\O'Hara\\subs.ass")
    );
    if (apostrophe.find("O\\'Hara") == std::string::npos) {
        return fail("Windows path escaping did not protect apostrophes: " + apostrophe);
    }

    const std::string unicode = EscapeFfmpegFilterValue(
        std::filesystem::path("C:\\anime\\透明\\subs.ass")
    );
    if (unicode.find("透明") == std::string::npos) {
        return fail("Windows path escaping did not preserve Unicode path text: " + unicode);
    }

    return 0;
}

int assert_command_generation() {
    const auto plan = utsure::core::job::build_ffmpeg_filter_hardsub_command(make_job(), make_plan());

    std::string joined{};
    for (const auto &argument : plan.arguments) {
        joined += argument;
        joined += '\n';
    }

    if (plan.subtitle_filter_name != "ass") {
        return fail("External ASS files must use the existing FFmpeg ass filter.");
    }
    if (joined.find("ass=filename='") == std::string::npos ||
        joined.find("mangetsu_rgba=auto:mangetsu_actor_colorcoding=auto") == std::string::npos) {
        return fail("Generated filtergraph did not include the Mangetsu ass filter options.");
    }
    if (joined.find("scale=1920:1080") == std::string::npos ||
        joined.find("ass=filename='") < joined.find("scale=1920:1080")) {
        return fail("Generated filtergraph should scale before applying subtitles.");
    }
    if (joined.find("-c:v\nlibx265\n") == std::string::npos ||
        joined.find("-crf\n18\n") == std::string::npos ||
        joined.find("-preset\nslow\n") == std::string::npos) {
        return fail("Generated command did not preserve video encode settings.");
    }
    if (joined.find("-c:a\naac\n") == std::string::npos ||
        joined.find("-b:a\n192k\n") == std::string::npos) {
        return fail("Generated command did not preserve AAC audio encode settings.");
    }

    return 0;
}

int assert_strict_same_thread_diagnostic() {
    const auto plan = utsure::core::job::build_ffmpeg_filter_hardsub_command(make_job(), make_plan());
    if (!plan.strict_same_thread_diagnostic_enabled) {
        return fail("UTSURE_SUBTITLE_STRICT_SAME_THREAD did not enable the FFmpeg filter diagnostic plan.");
    }

    return 0;
}

}  // namespace

int main(const int argc, char **argv) {
    if (argc != 2) {
        return fail("Usage: utsure_core_ffmpeg_filter_hardsub_backend_tests [--escape|--command|--strict-same-thread]");
    }

    const std::string mode = argv[1];
    if (mode == "--escape") {
        return assert_filter_value_escaping();
    }
    if (mode == "--command") {
        return assert_command_generation();
    }
    if (mode == "--strict-same-thread") {
        return assert_strict_same_thread_diagnostic();
    }

    return fail("Unknown test mode: " + mode);
}
