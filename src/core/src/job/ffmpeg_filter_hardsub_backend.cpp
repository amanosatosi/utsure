#include "ffmpeg_filter_hardsub_backend.hpp"

#include "../process/external_tool_runner.hpp"
#include "../subtitles/subtitle_runtime_options.hpp"
#include "utsure/core/filesystem/path_format.hpp"
#include "utsure/core/media/media_inspector.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace utsure::core::job {

namespace {

constexpr std::string_view kBackendEnv = "UTSURE_HARDSUB_BACKEND";
constexpr std::string_view kFfmpegPathEnv = "UTSURE_FFMPEG_PATH";
constexpr std::string_view kMangetsuRgbaOption = "mangetsu_rgba";
constexpr std::string_view kMangetsuActorColorcodingOption = "mangetsu_actor_colorcoding";

struct ResolvedFfmpegExecutable final {
    std::filesystem::path path{};
    std::string source{"unknown"};
};

// Full-FFmpeg mode has no app-side libassmod session. This guard instead keeps
// the synchronous child-process lifecycle owned by the thread that started it.
class FfmpegFilterStrictSameThreadOwner final {
public:
    FfmpegFilterStrictSameThreadOwner(
        const bool enabled,
        const std::function<void(const std::string &)> &log_callback
    )
        : enabled_(enabled),
          owner_thread_id_(std::this_thread::get_id()),
          log_callback_(log_callback) {
        log_lifecycle("owner-created");
    }

    ~FfmpegFilterStrictSameThreadOwner() {
        if (!enabled_) {
            return;
        }

        const auto destroyed_thread_id = std::this_thread::get_id();
        assert(destroyed_thread_id == owner_thread_id_);
        if (destroyed_thread_id == owner_thread_id_) {
            log_lifecycle("owner-destroyed");
        }
    }

    FfmpegFilterStrictSameThreadOwner(const FfmpegFilterStrictSameThreadOwner &) = delete;
    FfmpegFilterStrictSameThreadOwner &operator=(const FfmpegFilterStrictSameThreadOwner &) = delete;

    void enforce_owner_thread(const std::string_view operation) const {
        if (!enabled_ || std::this_thread::get_id() == owner_thread_id_) {
            return;
        }

        throw std::runtime_error(
            "Strict FFmpeg filter same-thread mode blocked " + std::string(operation) +
            " on a different thread than backend creation."
        );
    }

private:
    void log_lifecycle(const std::string_view phase) const noexcept {
        if (!enabled_ || !log_callback_) {
            return;
        }

        try {
            std::ostringstream message;
            message << "FFmpeg filter strict same-thread diagnostic: phase=" << phase
                    << ", backend_owner_thread_id=" << owner_thread_id_
                    << ", current_thread_id=" << std::this_thread::get_id()
                    << ", strict_same_thread=1"
                    << ", subprocess_lifecycle=created_waited_collected_on_owner_thread"
                    << ", app_side_libass_session=none"
                    << ", child_environment=inherited";
            log_callback_(message.str());
        } catch (...) {
        }
    }

    bool enabled_{false};
    std::thread::id owner_thread_id_{};
    const std::function<void(const std::string &)> &log_callback_;
};

[[nodiscard]] std::string lower_ascii(std::string value) {
    for (auto &character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

[[nodiscard]] std::string path_to_argument(const std::filesystem::path &path) {
    return filesystem::path_to_utf8_string(path.lexically_normal());
}

[[nodiscard]] bool executable_exists(const std::filesystem::path &path) {
    std::error_code error{};
    return !path.empty() &&
        std::filesystem::exists(path, error) &&
        !error &&
        std::filesystem::is_regular_file(path, error) &&
        !error;
}

#if defined(_WIN32)
[[nodiscard]] std::optional<std::filesystem::path> current_executable_directory() {
    std::wstring buffer(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length == 0 || length >= buffer.size()) {
        return std::nullopt;
    }

    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}
#endif

[[nodiscard]] std::optional<ResolvedFfmpegExecutable> resolve_ffmpeg_executable() {
    if (const char *env_path = std::getenv(std::string(kFfmpegPathEnv).c_str());
        env_path != nullptr && env_path[0] != '\0') {
        const std::filesystem::path configured_path(env_path);
        if (executable_exists(configured_path)) {
            return ResolvedFfmpegExecutable{
                .path = configured_path.lexically_normal(),
                .source = "UTSURE_FFMPEG_PATH"
            };
        }
    }

#if defined(_WIN32)
    if (const auto executable_dir = current_executable_directory(); executable_dir.has_value()) {
        const auto bundled_candidate = *executable_dir / "ffmpeg.exe";
        if (executable_exists(bundled_candidate)) {
            return ResolvedFfmpegExecutable{
                .path = bundled_candidate.lexically_normal(),
                .source = "bundled"
            };
        }
    }
#endif

    if (const auto path_candidate = process::find_executable_on_path({"ffmpeg.exe", "ffmpeg"});
        path_candidate.has_value()) {
        return ResolvedFfmpegExecutable{
            .path = path_candidate->lexically_normal(),
            .source = "PATH"
        };
    }

    return std::nullopt;
}

[[nodiscard]] std::string format_seconds(const std::int64_t microseconds) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
           << (static_cast<double>(microseconds) / 1000000.0);
    return stream.str();
}

[[nodiscard]] std::string format_rate(const media::Rational &rate) {
    if (!rate.is_valid() || rate.numerator <= 0 || rate.denominator <= 0) {
        throw std::runtime_error("FFmpeg filter backend requires a valid output frame rate.");
    }

    if (rate.denominator == 1) {
        return std::to_string(rate.numerator);
    }

    return std::to_string(rate.numerator) + "/" + std::to_string(rate.denominator);
}

[[nodiscard]] std::string format_sar(const media::Rational &sar) {
    if (!sar.is_valid() || sar.numerator <= 0 || sar.denominator <= 0) {
        return "1/1";
    }

    return std::to_string(sar.numerator) + "/" + std::to_string(sar.denominator);
}

[[nodiscard]] std::string video_encoder_name(const media::OutputVideoCodec codec) {
    switch (codec) {
    case media::OutputVideoCodec::h264:
        return "libx264";
    case media::OutputVideoCodec::h265:
        return "libx265";
    default:
        throw std::runtime_error("Unsupported output video codec selection for FFmpeg filter backend.");
    }
}

[[nodiscard]] std::string channel_layout_for_count(const int channel_count) {
    switch (channel_count) {
    case 1:
        return "mono";
    case 2:
        return "stereo";
    default:
        return std::to_string(std::max(channel_count, 1)) + "c";
    }
}

[[nodiscard]] std::int64_t rescale_to_microseconds(const std::int64_t value, const media::Rational &time_base) {
    if (!time_base.is_valid() || time_base.numerator <= 0 || time_base.denominator <= 0) {
        return 0;
    }

    return static_cast<std::int64_t>(
        (static_cast<long double>(value) * static_cast<long double>(time_base.numerator) * 1000000.0L) /
        static_cast<long double>(time_base.denominator)
    );
}

[[nodiscard]] std::int64_t estimated_segment_duration_us(const timeline::TimelineSegmentPlan &segment) {
    std::int64_t source_duration_us = segment.inspected_source_info.container_duration_microseconds.value_or(0);
    if (source_duration_us <= 0 && segment.inspected_source_info.primary_video_stream.has_value()) {
        const auto &video_stream = *segment.inspected_source_info.primary_video_stream;
        if (video_stream.timestamps.duration_pts.has_value()) {
            source_duration_us = rescale_to_microseconds(*video_stream.timestamps.duration_pts, video_stream.timestamps.time_base);
        }
    }

    if (source_duration_us <= 0) {
        throw std::runtime_error(
            "FFmpeg filter backend could not estimate the " + std::string(timeline::to_string(segment.kind)) +
            " segment duration."
        );
    }

    const std::int64_t trim_in = std::max<std::int64_t>(0, segment.source_trim_in_microseconds);
    const std::int64_t trim_out = segment.source_trim_out_microseconds.value_or(source_duration_us);
    return std::max<std::int64_t>(0, trim_out - trim_in);
}

[[nodiscard]] std::string trim_filter_for_segment(const timeline::TimelineSegmentPlan &segment) {
    std::string filter{"trim"};
    if (segment.source_trim_in_microseconds > 0) {
        filter += "=start=" + format_seconds(segment.source_trim_in_microseconds);
    }
    if (segment.source_trim_out_microseconds.has_value()) {
        filter += filter == "trim" ? "=end=" : ":end=";
        filter += format_seconds(*segment.source_trim_out_microseconds);
    }
    if (filter == "trim") {
        return {};
    }
    return filter;
}

[[nodiscard]] std::string atrim_filter_for_segment(const timeline::TimelineSegmentPlan &segment) {
    std::string filter{"atrim"};
    if (segment.source_trim_in_microseconds > 0) {
        filter += "=start=" + format_seconds(segment.source_trim_in_microseconds);
    }
    if (segment.source_trim_out_microseconds.has_value()) {
        filter += filter == "atrim" ? "=end=" : ":end=";
        filter += format_seconds(*segment.source_trim_out_microseconds);
    }
    if (filter == "atrim") {
        return {};
    }
    return filter;
}

[[nodiscard]] std::string make_subtitle_filter(const EncodeJobSubtitleSettings &subtitle_settings) {
    return "ass=filename='" + EscapeFfmpegFilterValue(subtitle_settings.subtitle_path) +
        "':mangetsu_rgba=auto:mangetsu_actor_colorcoding=auto";
}

void append_filter(std::string &chain, std::string filter) {
    if (filter.empty()) {
        return;
    }
    if (!chain.empty() && chain.back() != ']') {
        chain += ',';
    }
    chain += std::move(filter);
}

[[nodiscard]] std::string build_filter_complex(
    const EncodeJob &job,
    const timeline::TimelinePlan &timeline_plan,
    const media::ResolvedAudioOutputPlan &audio_output_plan,
    bool &maps_filtered_audio
) {
    if (!timeline_plan.output_video_shape.has_value()) {
        throw std::runtime_error("FFmpeg filter backend requires resolved output video dimensions.");
    }
    if (!job.subtitles.has_value()) {
        throw std::runtime_error("FFmpeg filter backend requires selected subtitles.");
    }

    const auto &shape = *timeline_plan.output_video_shape;
    const std::size_t segment_count = timeline_plan.segments.size();
    const std::string fps = format_rate(timeline_plan.output_frame_rate);
    const std::string sar = format_sar(shape.sample_aspect_ratio);

    std::vector<std::string> filter_lines{};
    filter_lines.reserve(segment_count * 2U + 2U);

    for (std::size_t index = 0; index < segment_count; ++index) {
        const auto &segment = timeline_plan.segments[index];
        std::string chain = "[" + std::to_string(index) + ":v:0]";
        append_filter(chain, trim_filter_for_segment(segment));
        append_filter(chain, "setpts=PTS-STARTPTS");
        append_filter(chain, "fps=" + fps);
        append_filter(chain, "scale=" + std::to_string(shape.width) + ":" + std::to_string(shape.height));
        append_filter(chain, "setsar=" + sar);
        if (segment.subtitles_enabled) {
            append_filter(chain, make_subtitle_filter(*job.subtitles));
        }
        append_filter(chain, "format=yuv420p");
        chain += "[v" + std::to_string(index) + "]";
        filter_lines.push_back(std::move(chain));
    }

    maps_filtered_audio = audio_output_plan.resolved_mode == media::ResolvedAudioOutputMode::encode_aac;
    if (maps_filtered_audio) {
        const int sample_rate = audio_output_plan.sample_rate_hz > 0 ? audio_output_plan.sample_rate_hz : 48000;
        const int channel_count = audio_output_plan.channel_count > 0 ? audio_output_plan.channel_count : 2;
        const std::string channel_layout = channel_layout_for_count(channel_count);

        for (std::size_t index = 0; index < segment_count; ++index) {
            const auto &segment = timeline_plan.segments[index];
            std::string chain{};
            if (segment.inspected_source_info.primary_audio_stream.has_value()) {
                const auto &audio_stream = *segment.inspected_source_info.primary_audio_stream;
                chain = "[" + std::to_string(index) + ":" + std::to_string(audio_stream.stream_index) + "]";
                append_filter(chain, atrim_filter_for_segment(segment));
                append_filter(chain, "asetpts=PTS-STARTPTS");
                append_filter(chain, "aresample=" + std::to_string(sample_rate));
                append_filter(chain, "aformat=sample_rates=" + std::to_string(sample_rate) +
                    ":channel_layouts=" + channel_layout);
            } else {
                chain = "anullsrc=r=" + std::to_string(sample_rate) + ":cl=" + channel_layout +
                    ":d=" + format_seconds(estimated_segment_duration_us(segment));
            }
            chain += "[a" + std::to_string(index) + "]";
            filter_lines.push_back(std::move(chain));
        }
    }

    if (segment_count == 1U) {
        filter_lines.push_back("[v0]null[vout]");
        if (maps_filtered_audio) {
            filter_lines.push_back("[a0]anull[aout]");
        }
    } else {
        std::string concat{};
        for (std::size_t index = 0; index < segment_count; ++index) {
            concat += "[v" + std::to_string(index) + "]";
            if (maps_filtered_audio) {
                concat += "[a" + std::to_string(index) + "]";
            }
        }
        concat += "concat=n=" + std::to_string(segment_count) + ":v=1:a=" +
            std::string(maps_filtered_audio ? "1" : "0") + "[vout]";
        if (maps_filtered_audio) {
            concat += "[aout]";
        }
        filter_lines.push_back(std::move(concat));
    }

    std::string filter_complex{};
    for (std::size_t index = 0; index < filter_lines.size(); ++index) {
        if (index > 0) {
            filter_complex += ';';
        }
        filter_complex += filter_lines[index];
    }
    return filter_complex;
}

[[nodiscard]] media::ResolvedAudioOutputPlan resolve_audio_plan(
    const EncodeJob &job,
    const timeline::TimelinePlan &timeline_plan
) {
    const auto &main_segment = timeline_plan.segments[timeline_plan.main_segment_index];
    return media::resolve_audio_output_plan(media::AudioOutputResolveRequest{
        .output_path = job.output.output_path,
        .settings = job.output.audio,
        .segment_count = timeline_plan.segments.size(),
        .main_source_trimmed = main_segment.has_source_trim(),
        .main_source_audio_stream = main_segment.inspected_source_info.primary_audio_stream.has_value()
            ? &*main_segment.inspected_source_info.primary_audio_stream
            : nullptr
    });
}

[[nodiscard]] timeline::TimelineCompositionSummary estimate_timeline_summary(
    const timeline::TimelinePlan &timeline_plan,
    const media::MediaSourceInfo &output_info
) {
    timeline::TimelineCompositionSummary summary{
        .segments = {},
        .output_video_time_base = timeline_plan.output_video_time_base,
        .output_frame_rate = timeline_plan.output_frame_rate,
        .output_audio_time_base = timeline_plan.output_audio_stream.has_value()
            ? std::optional<media::Rational>(timeline_plan.output_audio_stream->timestamps.time_base)
            : std::nullopt
    };
    summary.segments.reserve(timeline_plan.segments.size());

    std::int64_t next_start_us = 0;
    for (const auto &segment : timeline_plan.segments) {
        const std::int64_t duration_us = estimated_segment_duration_us(segment);
        summary.segments.push_back(timeline::TimelineSegmentSummary{
            .kind = segment.kind,
            .source_path = segment.source_path,
            .start_microseconds = next_start_us,
            .duration_microseconds = duration_us,
            .video_frame_count = 0,
            .audio_block_count = 0,
            .subtitles_enabled = segment.subtitles_enabled,
            .inserted_silence = !segment.inspected_source_info.primary_audio_stream.has_value()
        });
        next_start_us += duration_us;
    }

    summary.output_duration_microseconds = output_info.container_duration_microseconds.value_or(next_start_us);
    if (output_info.primary_video_stream.has_value() &&
        output_info.primary_video_stream->frame_count.has_value()) {
        summary.output_video_frame_count = *output_info.primary_video_stream->frame_count;
    }
    if (output_info.primary_audio_stream.has_value() &&
        output_info.primary_audio_stream->frame_count.has_value()) {
        summary.output_audio_block_count = *output_info.primary_audio_stream->frame_count;
    }
    return summary;
}

[[nodiscard]] bool contains_all_mangetsu_options(const std::string &output) {
    return output.find(kMangetsuRgbaOption) != std::string::npos &&
        output.find(kMangetsuActorColorcodingOption) != std::string::npos;
}

[[nodiscard]] std::optional<FfmpegFilterHardsubError> validate_mangetsu_filter_support(
    const std::filesystem::path &ffmpeg_executable,
    const std::function<void(const std::string &message)> &log_callback
) {
    const auto version_result = process::run_external_tool(process::ExternalToolRunRequest{
        .executable = ffmpeg_executable,
        .arguments = {"-version"}
    });
    if (!version_result.succeeded()) {
        return FfmpegFilterHardsubError{
            .message = "FFmpeg filter backend requested, but the FFmpeg binary could not be queried.",
            .actionable_hint = version_result.failure_message.empty()
                ? version_result.combined_output
                : version_result.failure_message
        };
    }
    if (log_callback) {
        std::istringstream stream(version_result.combined_output);
        std::string first_line{};
        std::getline(stream, first_line);
        log_callback("FFmpeg version output: " + first_line);
    }

    const auto ass_help_result = process::run_external_tool(process::ExternalToolRunRequest{
        .executable = ffmpeg_executable,
        .arguments = {"-hide_banner", "-h", "filter=ass"}
    });
    if (!ass_help_result.succeeded() || !contains_all_mangetsu_options(ass_help_result.combined_output)) {
        return FfmpegFilterHardsubError{
            .message = "FFmpeg filter backend requested, but bundled FFmpeg does not support mangetsu_rgba/mangetsu_actor_colorcoding options.",
            .actionable_hint = "Use the amanosatosi/FFmpeg 7.1 build at commit 6282c1941e3611ce43a4dcbe83a679c0323b8b13."
        };
    }

    return std::nullopt;
}

[[nodiscard]] FfmpegFilterHardsubResult make_error(
    std::string message,
    std::string actionable_hint,
    const bool canceled = false
) {
    return FfmpegFilterHardsubResult{
        .summary = std::nullopt,
        .error = FfmpegFilterHardsubError{
            .message = std::move(message),
            .actionable_hint = std::move(actionable_hint),
            .canceled = canceled
        }
    };
}

void remove_partial_output(const std::filesystem::path &output_path) noexcept {
    if (output_path.empty()) {
        return;
    }

    std::error_code remove_error{};
    std::filesystem::remove(output_path, remove_error);
}

}  // namespace

bool FfmpegFilterHardsubResult::succeeded() const noexcept {
    return summary.has_value() && !error.has_value();
}

const char *to_string(const HardsubBackend backend) noexcept {
    switch (backend) {
    case HardsubBackend::internal:
        return "internal";
    case HardsubBackend::ffmpeg_filter:
        return "ffmpeg_filter";
    default:
        return "unknown";
    }
}

HardsubBackend resolve_hardsub_backend_from_environment() noexcept {
    const char *raw_backend = std::getenv(std::string(kBackendEnv).c_str());
    if (raw_backend == nullptr || raw_backend[0] == '\0') {
        return HardsubBackend::internal;
    }

    const std::string backend = lower_ascii(raw_backend);
    if (backend == "ffmpeg_filter" || backend == "ffmpeg-filter" || backend == "ffmpeg") {
        return HardsubBackend::ffmpeg_filter;
    }

    return HardsubBackend::internal;
}

std::string EscapeFfmpegFilterValue(const std::filesystem::path &path) {
    const std::string value = filesystem::path_to_utf8_string(path.lexically_normal());
    std::string escaped{};
    escaped.reserve(value.size() * 2U);

    for (const char character : value) {
        switch (character) {
        case '\\':
        case '\'':
        case ':':
        case ',':
        case '[':
        case ']':
            escaped.push_back('\\');
            escaped.push_back(character);
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }

    return escaped;
}

std::string format_ffmpeg_command_for_log(
    const std::filesystem::path &executable,
    const std::vector<std::string> &arguments
) {
    const auto quote = [](const std::string &value) {
        if (value.empty()) {
            return std::string("\"\"");
        }

        const bool needs_quotes = value.find_first_of(" \t\n\"") != std::string::npos;
        if (!needs_quotes) {
            return value;
        }

        std::string quoted{"\""};
        for (const char character : value) {
            if (character == '"' || character == '\\') {
                quoted.push_back('\\');
            }
            quoted.push_back(character);
        }
        quoted.push_back('"');
        return quoted;
    };

    std::string command = quote(path_to_argument(executable));
    for (const auto &argument : arguments) {
        command.push_back(' ');
        command += quote(argument);
    }
    return command;
}

FfmpegFilterCommandPlan build_ffmpeg_filter_hardsub_command(
    const EncodeJob &job,
    const timeline::TimelinePlan &timeline_plan
) {
    if (!job.subtitles.has_value()) {
        throw std::runtime_error("FFmpeg filter backend requires selected subtitles.");
    }

    const auto resolved_ffmpeg = resolve_ffmpeg_executable();
    if (!resolved_ffmpeg.has_value()) {
        throw std::runtime_error(
            "FFmpeg filter backend requested, but no ffmpeg executable was found. Set UTSURE_FFMPEG_PATH or use the portable bundle."
        );
    }

    const auto audio_output_plan = resolve_audio_plan(job, timeline_plan);
    bool maps_filtered_audio = false;
    const std::string filter_complex = build_filter_complex(
        job,
        timeline_plan,
        audio_output_plan,
        maps_filtered_audio
    );

    std::vector<std::string> arguments{"-hide_banner", "-y"};
    for (const auto &segment : timeline_plan.segments) {
        arguments.push_back("-i");
        arguments.push_back(path_to_argument(segment.source_path));
    }
    arguments.push_back("-filter_complex");
    arguments.push_back(filter_complex);
    arguments.push_back("-map");
    arguments.push_back("[vout]");

    const bool audio_copy =
        audio_output_plan.resolved_mode == media::ResolvedAudioOutputMode::copy_source &&
        timeline_plan.segments.size() == 1U &&
        !timeline_plan.segments.front().has_source_trim() &&
        timeline_plan.segments.front().inspected_source_info.primary_audio_stream.has_value();

    if (maps_filtered_audio) {
        arguments.push_back("-map");
        arguments.push_back("[aout]");
    } else if (audio_copy) {
        const auto &audio_stream = *timeline_plan.segments.front().inspected_source_info.primary_audio_stream;
        arguments.push_back("-map");
        arguments.push_back("0:" + std::to_string(audio_stream.stream_index));
    } else {
        arguments.push_back("-an");
    }

    arguments.push_back("-c:v");
    arguments.push_back(video_encoder_name(job.output.video.codec));
    arguments.push_back("-preset");
    arguments.push_back(job.output.video.preset);
    arguments.push_back("-crf");
    arguments.push_back(std::to_string(job.output.video.crf));
    arguments.push_back("-pix_fmt");
    arguments.push_back("yuv420p");

    if (maps_filtered_audio) {
        arguments.push_back("-c:a");
        arguments.push_back("aac");
        arguments.push_back("-b:a");
        arguments.push_back(std::to_string(audio_output_plan.bitrate_kbps) + "k");
        if (audio_output_plan.sample_rate_hz > 0) {
            arguments.push_back("-ar");
            arguments.push_back(std::to_string(audio_output_plan.sample_rate_hz));
        }
        if (audio_output_plan.channel_count > 0) {
            arguments.push_back("-ac");
            arguments.push_back(std::to_string(audio_output_plan.channel_count));
        }
    } else if (audio_copy) {
        arguments.push_back("-c:a");
        arguments.push_back("copy");
    }

    arguments.push_back(path_to_argument(job.output.output_path));

    return FfmpegFilterCommandPlan{
        .ffmpeg_executable = resolved_ffmpeg->path,
        .ffmpeg_source = resolved_ffmpeg->source,
        .subtitle_filter_name = "ass",
        .subtitle_source = path_to_argument(job.subtitles->subtitle_path),
        .subtitle_stream_index = std::nullopt,
        .mangetsu_rgba_mode = "auto",
        .mangetsu_actor_colorcoding_mode = "auto",
        .strict_same_thread_diagnostic_enabled = subtitles::runtime::strict_same_thread_lifetime_enabled(),
        .arguments = std::move(arguments)
    };
}

FfmpegFilterHardsubResult run_ffmpeg_filter_hardsub_backend(
    const FfmpegFilterHardsubRunRequest &request
) noexcept {
    try {
        if (request.cancellation_requested && request.cancellation_requested()) {
            return make_error(std::string(kEncodeJobCanceledMessage), "The active encode was canceled by the user.", true);
        }
        if (request.job.thumbnail_preroll.has_value() && request.job.thumbnail_preroll->enabled) {
            return make_error(
                "FFmpeg filter backend does not support thumbnail pre-roll yet.",
                "Disable thumbnail pre-roll or use the internal subtitle compositor for this job."
            );
        }

        const FfmpegFilterStrictSameThreadOwner strict_same_thread_owner(
            subtitles::runtime::strict_same_thread_lifetime_enabled(),
            request.log_callback
        );
        strict_same_thread_owner.enforce_owner_thread("FFmpeg filter backend setup");
        const auto command_plan = build_ffmpeg_filter_hardsub_command(request.job, request.timeline_plan);
        strict_same_thread_owner.enforce_owner_thread("FFmpeg capability validation");
        if (request.log_callback) {
            request.log_callback("Selected hardsub backend: ffmpeg_filter");
            request.log_callback("FFmpeg binary path: " + path_to_argument(command_plan.ffmpeg_executable));
            request.log_callback("FFmpeg source: " + command_plan.ffmpeg_source);
            request.log_callback(
                "FFmpeg filter strict same-thread diagnostic enabled: " +
                std::string(command_plan.strict_same_thread_diagnostic_enabled ? "yes" : "no")
            );
        }
        if (request.warning_callback) {
            const auto audio_output_plan = resolve_audio_plan(request.job, request.timeline_plan);
            if (audio_output_plan.requested_mode_adjustment.has_value()) {
                request.warning_callback(*audio_output_plan.requested_mode_adjustment);
            }
        }

        if (const auto validation_error =
                validate_mangetsu_filter_support(command_plan.ffmpeg_executable, request.log_callback);
            validation_error.has_value()) {
            return make_error(validation_error->message, validation_error->actionable_hint);
        }

        if (request.log_callback) {
            request.log_callback("Bundled/custom FFmpeg detected: yes");
            request.log_callback("Subtitle filter used: " + command_plan.subtitle_filter_name);
            request.log_callback("Subtitle source path: " + command_plan.subtitle_source);
            request.log_callback("mangetsu_rgba mode: " + command_plan.mangetsu_rgba_mode);
            request.log_callback("mangetsu_actor_colorcoding mode: " + command_plan.mangetsu_actor_colorcoding_mode);
            request.log_callback(
                "Generated FFmpeg command: " +
                format_ffmpeg_command_for_log(command_plan.ffmpeg_executable, command_plan.arguments)
            );
        }

        if (const auto output_parent = request.job.output.output_path.parent_path(); !output_parent.empty()) {
            std::error_code create_error{};
            std::filesystem::create_directories(output_parent, create_error);
            if (create_error) {
                return make_error(
                    "FFmpeg filter backend could not create the output directory.",
                    "The operating system reported: " + create_error.message()
                );
            }
        }

        strict_same_thread_owner.enforce_owner_thread("FFmpeg subprocess launch and wait");
        const auto encode_started = std::chrono::steady_clock::now();
        const auto ffmpeg_result = process::run_external_tool(process::ExternalToolRunRequest{
            .executable = command_plan.ffmpeg_executable,
            .arguments = command_plan.arguments,
            .cancellation_requested = request.cancellation_requested
        });
        const auto elapsed_us = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - encode_started
            ).count()
        );

        strict_same_thread_owner.enforce_owner_thread("FFmpeg subprocess result handling");
        if (!ffmpeg_result.succeeded()) {
            remove_partial_output(request.job.output.output_path);
            if (ffmpeg_result.failure_message == "External tool invocation canceled.") {
                return make_error(
                    std::string(kEncodeJobCanceledMessage),
                    "The active FFmpeg filter backend encode was canceled by the user.",
                    true
                );
            }
            return make_error(
                "FFmpeg filter backend encode failed.",
                "Exit code " + std::to_string(ffmpeg_result.exit_code) + ". " +
                    (ffmpeg_result.failure_message.empty()
                        ? ffmpeg_result.combined_output
                        : ffmpeg_result.failure_message + "\n" + ffmpeg_result.combined_output)
            );
        }
        if (request.log_callback && !ffmpeg_result.combined_output.empty()) {
            request.log_callback("FFmpeg output:\n" + ffmpeg_result.combined_output);
        }

        strict_same_thread_owner.enforce_owner_thread("FFmpeg output inspection");
        const auto inspection_result = media::MediaInspector::inspect(request.job.output.output_path);
        if (!inspection_result.succeeded()) {
            remove_partial_output(request.job.output.output_path);
            return make_error(
                "The FFmpeg filter backend output could not be inspected after encode.",
                inspection_result.error->message + " Hint: " + inspection_result.error->actionable_hint
            );
        }

        auto encoded_frame_count = std::int64_t{0};
        if (inspection_result.media_source_info->primary_video_stream.has_value() &&
            inspection_result.media_source_info->primary_video_stream->frame_count.has_value()) {
            encoded_frame_count = *inspection_result.media_source_info->primary_video_stream->frame_count;
        }

        const auto audio_output_plan = resolve_audio_plan(request.job, request.timeline_plan);
        return FfmpegFilterHardsubResult{
            .summary = FfmpegFilterHardsubSummary{
                .timeline_summary = estimate_timeline_summary(request.timeline_plan, *inspection_result.media_source_info),
                .encoded_media_summary = media::EncodedMediaSummary{
                    .output_path = request.job.output.output_path.lexically_normal(),
                    .video_settings = {
                        .codec = request.job.output.video.codec,
                        .preset = request.job.output.video.preset,
                        .crf = request.job.output.video.crf
                    },
                    .resolved_audio_output = audio_output_plan,
                    .output_info = *inspection_result.media_source_info,
                    .encoded_video_frame_count = encoded_frame_count
                },
                .encoded_elapsed_microseconds = elapsed_us
            },
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return make_error(
            "FFmpeg filter backend raised an unclassified runtime failure.",
            exception.what()
        );
    } catch (...) {
        return make_error(
            "FFmpeg filter backend raised a non-standard runtime failure.",
            "An unknown exception escaped the FFmpeg filter backend."
        );
    }
}

}  // namespace utsure::core::job
