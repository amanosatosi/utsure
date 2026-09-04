#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/job/encode_job_report.hpp"
#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using utsure::core::job::EncodeJob;
using utsure::core::job::EncodeJobLogLevel;
using utsure::core::job::EncodeJobLogMessage;
using utsure::core::job::EncodeJobObserver;
using utsure::core::job::EncodeJobProgress;
using utsure::core::job::EncodeJobResult;
using utsure::core::job::EncodeJobRunner;
using utsure::core::job::EncodeJobRunOptions;
using utsure::core::job::EncodeJobStage;
using utsure::core::job::EncodeJobSummary;
using utsure::core::job::format_encode_job_report;
using utsure::core::media::DecodedMediaSource;
using utsure::core::media::MediaDecodeResult;
using utsure::core::media::MediaDecoder;
using utsure::core::media::OutputVideoCodec;
using utsure::core::media::Rational;
using utsure::core::subtitles::SubtitleCompositionDebugContext;
using utsure::core::subtitles::SubtitleRenderRequest;
using utsure::core::subtitles::SubtitleRenderResult;
using utsure::core::subtitles::SubtitleRenderSessionCreateRequest;
using utsure::core::subtitles::create_default_subtitle_renderer;
using utsure::core::timeline::SubtitleTimingMode;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

bool contains_text(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

struct CollectingObserver final : EncodeJobObserver {
    std::vector<EncodeJobProgress> progress_updates{};
    std::vector<EncodeJobLogMessage> log_messages{};

    void on_progress(const EncodeJobProgress &progress) override {
        progress_updates.push_back(progress);
    }

    void on_log(const EncodeJobLogMessage &message) override {
        log_messages.push_back(message);
    }
};

bool observer_logs_contain_text(const CollectingObserver &observer, std::string_view needle) {
    for (const auto &message : observer.log_messages) {
        if (message.message.find(needle) != std::string::npos) {
            return true;
        }
    }

    return false;
}

bool string_messages_contain_text(const std::vector<std::string> &messages, std::string_view needle) {
    for (const auto &message : messages) {
        if (message.find(needle) != std::string::npos) {
            return true;
        }
    }

    return false;
}

void set_test_environment_variable(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_test_environment_variable(const char *name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

struct ProcessMemorySnapshot final {
    std::uint64_t rss_bytes{0};
    std::uint64_t peak_rss_bytes{0};
};

std::optional<ProcessMemorySnapshot> sample_process_memory() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return std::nullopt;
    }

    return ProcessMemorySnapshot{
        .rss_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize),
        .peak_rss_bytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize)
    };
#else
    return std::nullopt;
#endif
}

std::size_t count_string_messages_containing_text(
    const std::vector<std::string> &messages,
    const std::string_view needle
) {
    std::size_t count = 0;
    for (const auto &message : messages) {
        if (message.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

void write_text_file(const std::filesystem::path &path, const std::string_view text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}

const char *test_log_level_name(const EncodeJobLogLevel level) noexcept {
    switch (level) {
    case EncodeJobLogLevel::info:
        return "info";
    case EncodeJobLogLevel::warning:
        return "warning";
    case EncodeJobLogLevel::error:
        return "error";
    default:
        return "unknown";
    }
}

struct SubtitleScheduleDiagnostic final {
    std::string segment_name{};
    std::int64_t output_pts{0};
    std::int64_t segment_relative_timestamp_microseconds{0};
    std::int64_t subtitle_source_time_microseconds{0};
    std::int64_t bitmap_count{0};
    int destination_width{0};
    int destination_height{0};
};

std::optional<std::int64_t> parse_diagnostic_int64(
    const std::string &message,
    const std::string_view key
) {
    const auto key_position = message.find(key);
    if (key_position == std::string::npos) {
        return std::nullopt;
    }

    std::size_t value_begin = key_position + key.size();
    if (value_begin >= message.size()) {
        return std::nullopt;
    }

    std::size_t value_end = value_begin;
    if (message[value_end] == '-') {
        ++value_end;
    }

    const auto digit_begin = value_end;
    while (value_end < message.size() &&
           std::isdigit(static_cast<unsigned char>(message[value_end])) != 0) {
        ++value_end;
    }

    if (digit_begin == value_end) {
        return std::nullopt;
    }

    try {
        return std::stoll(message.substr(value_begin, value_end - value_begin));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> parse_diagnostic_text(
    const std::string &message,
    const std::string_view key
) {
    const auto key_position = message.find(key);
    if (key_position == std::string::npos) {
        return std::nullopt;
    }

    const std::size_t value_begin = key_position + key.size();
    if (value_begin >= message.size()) {
        return std::nullopt;
    }

    const auto value_end = message.find(',', value_begin);
    return message.substr(
        value_begin,
        value_end == std::string::npos ? std::string::npos : value_end - value_begin
    );
}

std::optional<std::pair<int, int>> parse_diagnostic_dimensions(
    const std::string &message,
    const std::string_view key
) {
    const auto value = parse_diagnostic_text(message, key);
    if (!value.has_value()) {
        return std::nullopt;
    }

    const auto separator = value->find('x');
    if (separator == std::string::npos) {
        return std::nullopt;
    }

    try {
        return std::pair<int, int>{
            std::stoi(value->substr(0, separator)),
            std::stoi(value->substr(separator + 1))
        };
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<SubtitleScheduleDiagnostic> collect_subtitle_schedule_diagnostics(
    const CollectingObserver &observer
) {
    std::vector<SubtitleScheduleDiagnostic> diagnostics{};
    for (const auto &message : observer.log_messages) {
        if (!contains_text(message.message, "Subtitle composition diagnostics:")) {
            continue;
        }

        const auto segment_name = parse_diagnostic_text(message.message, "segment=");
        const auto output_pts = parse_diagnostic_int64(message.message, "output_pts=");
        const auto segment_relative_us = parse_diagnostic_int64(message.message, "segment_relative_us=");
        const auto subtitle_source_time_us = parse_diagnostic_int64(message.message, "subtitle_source_time_us=");
        const auto bitmap_count = parse_diagnostic_int64(message.message, "bitmap_count=");
        const auto destination = parse_diagnostic_dimensions(message.message, "destination=");
        if (!segment_name.has_value() ||
            !output_pts.has_value() ||
            !segment_relative_us.has_value() ||
            !subtitle_source_time_us.has_value() ||
            !bitmap_count.has_value() ||
            !destination.has_value()) {
            continue;
        }

        diagnostics.push_back(SubtitleScheduleDiagnostic{
            .segment_name = *segment_name,
            .output_pts = *output_pts,
            .segment_relative_timestamp_microseconds = *segment_relative_us,
            .subtitle_source_time_microseconds = *subtitle_source_time_us,
            .bitmap_count = *bitmap_count,
            .destination_width = destination->first,
            .destination_height = destination->second
        });
    }

    std::stable_sort(
        diagnostics.begin(),
        diagnostics.end(),
        [](const SubtitleScheduleDiagnostic &left, const SubtitleScheduleDiagnostic &right) {
            return left.output_pts < right.output_pts;
        }
    );
    return diagnostics;
}

std::size_t count_bitmap_positive_subtitle_diagnostics(
    const std::vector<SubtitleScheduleDiagnostic> &diagnostics
) {
    return static_cast<std::size_t>(std::count_if(
        diagnostics.begin(),
        diagnostics.end(),
        [](const SubtitleScheduleDiagnostic &diagnostic) {
            return diagnostic.bitmap_count > 0;
        }
    ));
}

void dump_subtitle_schedule_frame_count_diagnostics(
    const std::string_view context,
    const EncodeJobSummary &summary,
    const CollectingObserver &observer,
    const std::int64_t expected_total_frame_count,
    const std::int64_t expected_bitmap_positive_frame_count
) {
    const auto diagnostics = collect_subtitle_schedule_diagnostics(observer);
    const auto active_diagnostic_count = count_bitmap_positive_subtitle_diagnostics(diagnostics);

    std::cerr << context << ".\n";
    std::cerr << "diagnostic.frame_count.expected.total=" << expected_total_frame_count << '\n';
    std::cerr << "diagnostic.frame_count.actual.total=" << summary.timeline_summary.output_video_frame_count << '\n';
    std::cerr << "diagnostic.frame_count.expected.subtitle_diagnostics=";
    if (summary.timeline_summary.segments.size() > 1U) {
        std::cerr << summary.timeline_summary.segments[1].video_frame_count << '\n';
    } else {
        std::cerr << "unknown\n";
    }
    std::cerr << "diagnostic.frame_count.actual.subtitle_diagnostics=" << diagnostics.size() << '\n';
    std::cerr << "diagnostic.frame_count.expected.bitmap_positive_diagnostics="
              << expected_bitmap_positive_frame_count << '\n';
    std::cerr << "diagnostic.frame_count.actual.bitmap_positive_diagnostics=" << active_diagnostic_count << '\n';
    std::cerr << "diagnostic.frame_count.summary_subtitled_frames="
              << summary.subtitled_video_frame_count << '\n';

    if (summary.timeline_summary.segments.size() > 1U) {
        const auto main_start = summary.timeline_summary.segments[1].start_microseconds;
        const auto main_end = main_start + summary.timeline_summary.segments[1].duration_microseconds;
        std::cerr << "diagnostic.main_segment.expected_range_us=" << main_start << ".." << main_end << '\n';
        std::cerr << "diagnostic.main_segment.expected_frames="
                  << summary.timeline_summary.segments[1].video_frame_count << '\n';
    }

    if (!diagnostics.empty()) {
        const auto &first = diagnostics.front();
        const auto &last = diagnostics.back();
        std::cerr << "diagnostic.subtitle.first.output_pts=" << first.output_pts << '\n';
        std::cerr << "diagnostic.subtitle.last.output_pts=" << last.output_pts << '\n';
        std::cerr << "diagnostic.subtitle.first.subtitle_source_time_us="
                  << first.subtitle_source_time_microseconds << '\n';
        std::cerr << "diagnostic.subtitle.last.subtitle_source_time_us="
                  << last.subtitle_source_time_microseconds << '\n';
        std::cerr << "diagnostic.subtitle.first.segment_relative_us="
                  << first.segment_relative_timestamp_microseconds << '\n';
        std::cerr << "diagnostic.subtitle.last.segment_relative_us="
                  << last.segment_relative_timestamp_microseconds << '\n';
        std::cerr << "diagnostic.subtitle.first.destination="
                  << first.destination_width << 'x' << first.destination_height << '\n';
        std::cerr << "diagnostic.subtitle.last.destination="
                  << last.destination_width << 'x' << last.destination_height << '\n';
    }

    const auto dump_sample = [](const char *label, const std::size_t index, const SubtitleScheduleDiagnostic &diagnostic) {
        std::cerr << "diagnostic.subtitle." << label << '[' << index << "]="
                  << "segment=" << diagnostic.segment_name
                  << ",output_pts=" << diagnostic.output_pts
                  << ",segment_relative_us=" << diagnostic.segment_relative_timestamp_microseconds
                  << ",subtitle_source_time_us=" << diagnostic.subtitle_source_time_microseconds
                  << ",destination=" << diagnostic.destination_width << 'x' << diagnostic.destination_height
                  << ",bitmap_count=" << diagnostic.bitmap_count
                  << ",subtitle_visible=" << (diagnostic.bitmap_count > 0 ? "yes" : "no")
                  << '\n';
    };

    const std::size_t leading_count = std::min<std::size_t>(3U, diagnostics.size());
    for (std::size_t index = 0; index < leading_count; ++index) {
        dump_sample("first", index, diagnostics[index]);
    }

    const std::size_t trailing_count = std::min<std::size_t>(3U, diagnostics.size());
    const std::size_t trailing_begin = diagnostics.size() - trailing_count;
    for (std::size_t index = trailing_begin; index < diagnostics.size(); ++index) {
        dump_sample("last", index - trailing_begin, diagnostics[index]);
    }
}

void dump_encode_job_failure_diagnostics(
    const std::string_view context,
    const EncodeJobResult &result,
    const CollectingObserver &observer,
    const std::filesystem::path &output_path
) {
    std::cerr << context << ".\n";
    std::cerr << "diagnostic.output_path=" << output_path.string() << '\n';

    if (result.error.has_value()) {
        std::cerr << "diagnostic.error.message=" << result.error->message << '\n';
        std::cerr << "diagnostic.error.hint=" << result.error->actionable_hint << '\n';
        std::cerr << "diagnostic.error.main_source_path=" << result.error->main_source_path << '\n';
        std::cerr << "diagnostic.error.output_path=" << result.error->output_path << '\n';
        std::cerr << "diagnostic.error.canceled=" << (result.error->canceled ? "yes" : "no") << '\n';
    } else {
        std::cerr << "diagnostic.error=missing\n";
    }

    if (result.encode_job_summary.has_value()) {
        std::cerr << "diagnostic.encode_job_summary.begin\n"
                  << format_encode_job_report(*result.encode_job_summary)
                  << "\ndiagnostic.encode_job_summary.end\n";
    } else {
        std::cerr << "diagnostic.encode_job_summary=missing\n";
    }

    for (const auto &message : observer.log_messages) {
        if (message.level == EncodeJobLogLevel::error ||
            message.level == EncodeJobLogLevel::warning ||
            contains_text(message.message, "Subtitle composition diagnostics:") ||
            contains_text(message.message, "Timeline") ||
            contains_text(message.message, "timeline") ||
            contains_text(message.message, "subtitle") ||
            contains_text(message.message, "Subtitle")) {
            std::cerr << "diagnostic.observer." << test_log_level_name(message.level)
                      << "=" << message.message << '\n';
        }
    }
}

std::string lowercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

bool current_subtitle_strict_same_thread_mode() {
    const auto flag_enabled = [](const char *name) {
        const char *raw = std::getenv(name);
        if (raw == nullptr || raw[0] == '\0') {
            return false;
        }

        const auto normalized = lowercase_ascii(std::string(raw));
        return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes";
    };
    return flag_enabled("UTSURE_SUBTITLE_STRICT_SAME_THREAD") ||
        flag_enabled("UTSURE_SUBTITLE_SAFE_MODE");
}

std::string current_subtitle_bitmap_mode() {
    const auto forced_flag_enabled = [](const char *name) {
        const char *raw = std::getenv(name);
        if (raw == nullptr || raw[0] == '\0') {
            return false;
        }

        const auto normalized = lowercase_ascii(std::string(raw));
        return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes";
    };
    if (forced_flag_enabled("UTSURE_SUBTITLE_SAFE_MODE") ||
        forced_flag_enabled("UTSURE_SUBTITLE_SYNC") ||
        forced_flag_enabled("UTSURE_SUBTITLE_BITMAP_COPY") ||
        forced_flag_enabled("UTSURE_DISABLE_DIRECT_SUBTITLE_BITMAPS")) {
        return "copied";
    }

    const char *value = std::getenv("UTSURE_SUBTITLE_BITMAP_MODE");
    if (value == nullptr || value[0] == '\0') {
        return "copied";
    }

    const auto normalized = lowercase_ascii(std::string(value));
    return (normalized == "direct" || normalized == "raw")
        ? "direct"
        : "copied";
}

std::string current_subtitle_composition_mode() {
    const auto forced_flag_enabled = [](const char *name) {
        const char *raw = std::getenv(name);
        if (raw == nullptr || raw[0] == '\0') {
            return false;
        }

        const auto normalized = lowercase_ascii(std::string(raw));
        return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes";
    };
    if (forced_flag_enabled("UTSURE_SUBTITLE_SAFE_MODE") ||
        forced_flag_enabled("UTSURE_SUBTITLE_SYNC")) {
        return "serialized";
    }

    const char *value = std::getenv("UTSURE_SUBTITLE_COMPOSITION_MODE");
    if (value == nullptr || value[0] == '\0') {
        return "serialized";
    }

    const auto normalized = lowercase_ascii(std::string(value));
    return (normalized == "worker" || normalized == "worker_local" || normalized == "worker-local" || normalized == "parallel")
        ? "worker_local"
        : "serialized";
}

std::string format_rational(const Rational &value) {
    if (!value.is_valid()) {
        return "unknown";
    }

    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

std::string format_path_leaf(const std::filesystem::path &path) {
    if (path.empty()) {
        return {};
    }

    const auto leaf = path.filename();
    if (!leaf.empty()) {
        return leaf.string();
    }

    return path.lexically_normal().string();
}

std::int64_t decoded_video_end_microseconds(const DecodedMediaSource &decoded_output) {
    if (decoded_output.video_frames.empty()) {
        return 0;
    }

    const auto &last_frame = decoded_output.video_frames.back();
    return last_frame.timestamp.start_microseconds + last_frame.timestamp.duration_microseconds.value_or(0);
}

std::int64_t decoded_audio_end_microseconds(const DecodedMediaSource &decoded_output) {
    if (decoded_output.audio_blocks.empty()) {
        return 0;
    }

    const auto &last_block = decoded_output.audio_blocks.back();
    return last_block.timestamp.start_microseconds + last_block.timestamp.duration_microseconds.value_or(0);
}

int assert_trimmed_audio_window(
    const DecodedMediaSource &decoded_output,
    const std::int64_t expected_duration_us,
    const std::int64_t boundary_tolerance_us,
    const std::int64_t av_sync_tolerance_us,
    std::string_view context
) {
    if (decoded_output.audio_blocks.empty()) {
        return fail(std::string(context) + " unexpectedly dropped audio.");
    }

    const auto audio_start_us = decoded_output.audio_blocks.front().timestamp.start_microseconds;
    const auto audio_end_us = decoded_audio_end_microseconds(decoded_output);
    const auto video_end_us = decoded_video_end_microseconds(decoded_output);
    if (audio_start_us < 0 || audio_start_us > boundary_tolerance_us) {
        return fail(std::string(context) + " audio did not start near the requested trim boundary.");
    }

    if (audio_end_us > expected_duration_us + boundary_tolerance_us ||
        std::llabs(audio_end_us - expected_duration_us) > boundary_tolerance_us) {
        return fail(std::string(context) + " audio extended beyond the requested trim duration tolerance.");
    }

    if (std::llabs(video_end_us - audio_end_us) > av_sync_tolerance_us) {
        return fail(std::string(context) + " audio/video durations drifted too far apart.");
    }

    return 0;
}

bool frames_are_identical(
    const DecodedMediaSource &left,
    const DecodedMediaSource &right,
    const std::size_t frame_index
) {
    return left.video_frames[frame_index].planes.front().bytes == right.video_frames[frame_index].planes.front().bytes;
}

bool any_frame_changed_in_range(
    const DecodedMediaSource &plain_output,
    const DecodedMediaSource &burned_output,
    const std::size_t start_index,
    const std::size_t frame_count
) {
    for (std::size_t index = 0; index < frame_count; ++index) {
        if (!frames_are_identical(plain_output, burned_output, start_index + index)) {
            return true;
        }
    }

    return false;
}

bool frame_changed(
    const DecodedMediaSource &plain_output,
    const DecodedMediaSource &burned_output,
    const std::size_t frame_index
) {
    return !frames_are_identical(plain_output, burned_output, frame_index);
}

bool has_opaque_color_variation(const utsure::core::subtitles::RenderedSubtitleFrame &rendered_frame) {
    for (const auto &bitmap : rendered_frame.bitmaps) {
        if (bitmap.pixel_format != utsure::core::subtitles::SubtitleBitmapPixelFormat::rgba8_premultiplied ||
            bitmap.line_stride_bytes < (bitmap.width * 4)) {
            continue;
        }

        bool found_opaque_pixel = false;
        std::array<std::uint8_t, 3> first_opaque_color{};
        for (int row = 0; row < bitmap.height; ++row) {
            const auto *source_row = bitmap.bytes.data() +
                static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.line_stride_bytes);
            for (int column = 0; column < bitmap.width; ++column) {
                const auto offset = static_cast<std::size_t>(column) * 4U;
                if (source_row[offset + 3U] < 250U) {
                    continue;
                }

                const std::array<std::uint8_t, 3> color{
                    source_row[offset + 0U],
                    source_row[offset + 1U],
                    source_row[offset + 2U]
                };
                if (!found_opaque_pixel) {
                    first_opaque_color = color;
                    found_opaque_pixel = true;
                    continue;
                }

                if (color != first_opaque_color) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool has_near_frame_bitmap(const utsure::core::subtitles::RenderedSubtitleFrame &rendered_frame) {
    for (const auto &bitmap : rendered_frame.bitmaps) {
        const int clipped_left = std::max(0, bitmap.origin_x);
        const int clipped_top = std::max(0, bitmap.origin_y);
        const int clipped_right = std::min(
            rendered_frame.canvas_width,
            bitmap.origin_x + bitmap.width
        );
        const int clipped_bottom = std::min(
            rendered_frame.canvas_height,
            bitmap.origin_y + bitmap.height
        );
        if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
            continue;
        }

        const int visible_width = clipped_right - clipped_left;
        const int visible_height = clipped_bottom - clipped_top;
        if ((visible_width * 10) >= (rendered_frame.canvas_width * 9) &&
            (visible_height * 10) >= (rendered_frame.canvas_height * 9)) {
            return true;
        }
    }

    return false;
}

int assert_decoded_output(
    const DecodedMediaSource &decoded_output,
    const std::size_t expected_frame_count,
    const bool expect_audio
) {
    if (decoded_output.video_frames.size() != expected_frame_count) {
        return fail("Unexpected burned-output video frame count.");
    }

    if (expect_audio && decoded_output.audio_blocks.empty()) {
        return fail("The subtitle burn-in path unexpectedly dropped audio.");
    }

    if (!expect_audio && !decoded_output.audio_blocks.empty()) {
        return fail("The subtitle burn-in path unexpectedly contains audio.");
    }

    for (std::size_t index = 1; index < decoded_output.video_frames.size(); ++index) {
        if (decoded_output.video_frames[index].timestamp.start_microseconds <=
            decoded_output.video_frames[index - 1].timestamp.start_microseconds) {
            return fail("Unexpected burned-output timestamp sequence.");
        }
    }

    return 0;
}

int assert_decoded_video_dimensions(
    const DecodedMediaSource &decoded_output,
    const int expected_width,
    const int expected_height,
    const std::string_view context
) {
    for (const auto &frame : decoded_output.video_frames) {
        if (frame.width != expected_width || frame.height != expected_height) {
            return fail(std::string(context) + " decoded a frame with unexpected output dimensions.");
        }
    }

    return 0;
}

int assert_observer_flow(
    const CollectingObserver &observer,
    const int expected_decode_steps,
    const int expected_thumbnail_steps = 0
) {
    if (observer.progress_updates.empty()) {
        return fail("The subtitle burn-in observer did not receive any progress updates.");
    }

    const int expected_total_steps = 1 + expected_decode_steps + 1 + expected_thumbnail_steps + 1 + 1;
    const int expected_subtitle_stage_count = 1 + expected_thumbnail_steps;
    int subtitle_stage_count = 0;

    if (observer.progress_updates.front().stage != EncodeJobStage::assembling_timeline ||
        observer.progress_updates.back().stage != EncodeJobStage::completed) {
        return fail("The subtitle burn-in observer did not report the expected lifecycle stages.");
    }

    for (const auto &progress : observer.progress_updates) {
        if (progress.total_steps != expected_total_steps) {
            return fail("The subtitle burn-in observer reported an unexpected total-step count.");
        }

        if (progress.stage == EncodeJobStage::burning_in_subtitles) {
            ++subtitle_stage_count;
        }
    }

    if (subtitle_stage_count != expected_subtitle_stage_count) {
        return fail("The subtitle burn-in observer did not report the expected subtitle preparation stage count.");
    }

    for (const auto &log_message : observer.log_messages) {
        if (log_message.level == EncodeJobLogLevel::error) {
            return fail("The subtitle burn-in observer reported an unexpected error log.");
        }
    }

    if (!observer_logs_contain_text(observer, "Streaming performance: total_elapsed=")) {
        return fail("The subtitle burn-in observer did not report the expected subtitle-path runtime logs.");
    }

    return 0;
}

int assert_subtitle_runtime_visibility(
    const CollectingObserver &observer,
    const EncodeJobSummary &summary,
    const std::string_view expected_bitmap_mode,
    const std::string_view expected_composition_mode
) {
    if (summary.streaming_runtime.subtitle_bitmap_mode != expected_bitmap_mode ||
        summary.streaming_runtime.subtitle_composition_mode != expected_composition_mode) {
        return fail("The subtitle runtime summary did not record the expected isolation mode.");
    }

    const std::size_t expected_subtitle_workers = summary.streaming_runtime.subtitle_strict_same_thread
        ? 1U
        : expected_composition_mode == "worker_local"
        ? summary.streaming_runtime.video_processing_worker_count
        : 1U;
    if (summary.streaming_runtime.subtitle_processing_worker_count != expected_subtitle_workers) {
        return fail("The subtitle runtime summary reported an unexpected subtitle-worker count.");
    }

    const auto report = format_encode_job_report(summary);
    if (!observer_logs_contain_text(observer, std::string("subtitle bitmap mode ") + std::string(expected_bitmap_mode)) ||
        !observer_logs_contain_text(observer, std::string("composition mode ") + std::string(expected_composition_mode)) ||
        report.find("streaming.subtitle.bitmap_mode=" + std::string(expected_bitmap_mode)) == std::string::npos ||
        report.find("streaming.subtitle.composition_mode=" + std::string(expected_composition_mode)) == std::string::npos ||
        report.find("streaming.subtitle.strict_same_thread=" +
                    std::string(summary.streaming_runtime.subtitle_strict_same_thread ? "1" : "0")) == std::string::npos) {
        return fail("The subtitle runtime logs/report did not expose the active isolation mode.");
    }

    return 0;
}

int assert_subtitle_render_schedule_diagnostics(
    const CollectingObserver &observer,
    const EncodeJobSummary &summary,
    const DecodedMediaSource &decoded_output,
    const std::size_t output_frame_offset,
    const std::size_t expected_frame_count,
    const std::int64_t segment_start_microseconds,
    const std::string_view context,
    const std::int64_t source_trim_start_microseconds = 0,
    const std::optional<std::pair<int, int>> expected_destination_dimensions = std::nullopt
) {
    if (summary.streaming_runtime.subtitle_diagnostics_mode == "off") {
        return fail(std::string(context) + " did not run with subtitle frame diagnostics enabled.");
    }

    const auto diagnostics = collect_subtitle_schedule_diagnostics(observer);
    if (diagnostics.size() != expected_frame_count) {
        return fail(
            std::string(context) +
            " did not log exactly one subtitle render diagnostic per subtitle-enabled output frame."
        );
    }

    if (output_frame_offset + expected_frame_count > decoded_output.video_frames.size()) {
        return fail(std::string(context) + " expected more decoded output frames than the output contains.");
    }

    if (std::llabs(diagnostics.front().segment_relative_timestamp_microseconds) > 1 ||
        std::llabs(
            diagnostics.front().subtitle_source_time_microseconds - source_trim_start_microseconds
        ) > 1) {
        return fail(
            std::string(context) +
            " did not start the subtitle source/media clock at the requested trim boundary."
        );
    }

    for (std::size_t index = 0; index < expected_frame_count; ++index) {
        const auto expected_output_relative_timestamp =
            decoded_output.video_frames[output_frame_offset + index].timestamp.start_microseconds -
            segment_start_microseconds;
        const auto expected_subtitle_source_time =
            expected_output_relative_timestamp + source_trim_start_microseconds;
        if (diagnostics[index].segment_name != "main") {
            return fail(
                std::string(context) +
                " logged a subtitle render diagnostic outside the main segment."
            );
        }

        const auto actual_segment_relative_timestamp =
            diagnostics[index].segment_relative_timestamp_microseconds;
        const auto actual_subtitle_source_time = diagnostics[index].subtitle_source_time_microseconds;
        if (std::llabs(actual_segment_relative_timestamp - expected_output_relative_timestamp) > 1 ||
            std::llabs(actual_subtitle_source_time - expected_subtitle_source_time) > 1) {
            return fail(
                std::string(context) +
                " did not convert the output-relative frame time to source/media render time exactly once."
            );
        }

        if (expected_destination_dimensions.has_value() &&
            (diagnostics[index].destination_width != expected_destination_dimensions->first ||
             diagnostics[index].destination_height != expected_destination_dimensions->second)) {
            return fail(
                std::string(context) +
                " rendered subtitles on a canvas that did not match the resized output dimensions."
            );
        }
    }

    return 0;
}

std::string build_validation_report(
    const EncodeJobSummary &encode_job_summary,
    const DecodedMediaSource &plain_output,
    const DecodedMediaSource &burned_output
) {
    std::string report = format_encode_job_report(encode_job_summary);
    report += "\nverified.output.video_frames=" + std::to_string(burned_output.video_frames.size());
    report += "\nverified.output.frame0.start_us=" +
              std::to_string(burned_output.video_frames[0].timestamp.start_microseconds);
    report += "\nverified.output.frame1.start_us=" +
              std::to_string(burned_output.video_frames[1].timestamp.start_microseconds);
    report += "\nverified.output.frame0.changed=" +
              std::string(frames_are_identical(plain_output, burned_output, 0U) ? "no" : "yes");
    return report;
}

int run_render_assertion(
    const std::filesystem::path &subtitle_path,
    const bool expect_opaque_color_variation = false,
    const bool expect_near_frame_bitmap = false,
    const std::int64_t visible_timestamp_microseconds = 41667LL,
    const std::optional<std::int64_t> hidden_timestamp_microseconds = std::optional<std::int64_t>{500000LL}
) {
    auto subtitle_renderer = create_default_subtitle_renderer();
    if (!subtitle_renderer) {
        return fail("The default subtitle renderer could not be created.");
    }

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = subtitle_path,
        .format_hint = "ass",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    };

    auto session_result = subtitle_renderer->create_session(session_request);
    if (!session_result.succeeded()) {
        const std::string error_message =
            "libassmod session creation failed unexpectedly: " +
            session_result.error->message +
            " Hint: " +
            session_result.error->actionable_hint;
        return fail(error_message);
    }

    const SubtitleRenderResult visible_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = visible_timestamp_microseconds
    });
    if (!visible_result.succeeded()) {
        const std::string error_message =
            "Visible subtitle render failed unexpectedly: " +
            visible_result.error->message +
            " Hint: " +
            visible_result.error->actionable_hint;
        return fail(error_message);
    }

    if (visible_result.rendered_frame->bitmaps.empty()) {
        return fail("Expected visible subtitle content at the requested visible timestamp.");
    }

    if (hidden_timestamp_microseconds.has_value()) {
        const SubtitleRenderResult hidden_result = session_result.session->render(SubtitleRenderRequest{
            .timestamp_microseconds = *hidden_timestamp_microseconds
        });
        if (!hidden_result.succeeded()) {
            const std::string error_message =
                "Hidden subtitle render failed unexpectedly: " +
                hidden_result.error->message +
                " Hint: " +
                hidden_result.error->actionable_hint;
            return fail(error_message);
        }

        if (!hidden_result.rendered_frame->bitmaps.empty()) {
            return fail("Expected no subtitle content at the requested hidden timestamp.");
        }
    }

    if (expect_opaque_color_variation &&
        !has_opaque_color_variation(*visible_result.rendered_frame)) {
        return fail("Expected the RGBA gradient sample to contain multiple opaque colors in one rendered frame.");
    }

    if (expect_near_frame_bitmap &&
        !has_near_frame_bitmap(*visible_result.rendered_frame)) {
        return fail("Expected the BorderStyle=4 sample to preserve a large clipped background bitmap.");
    }

    std::cout << "session.subtitle_path=" << format_path_leaf(subtitle_path) << '\n';
    std::cout << "session.format_hint=ass\n";
    std::cout << "session.canvas=320x180\n";
    std::cout << "session.sample_aspect_ratio=" << format_rational(session_request.sample_aspect_ratio) << '\n';
    std::cout << "visible.timestamp_us=" << visible_timestamp_microseconds << '\n';
    std::cout << "visible.has_content=yes\n";
    if (expect_opaque_color_variation) {
        std::cout << "visible.opaque_color_variation=yes\n";
    }
    if (expect_near_frame_bitmap) {
        std::cout << "visible.near_frame_bitmap=yes\n";
    }
    if (hidden_timestamp_microseconds.has_value()) {
        std::cout << "hidden.timestamp_us=" << *hidden_timestamp_microseconds << '\n';
        std::cout << "hidden.has_content=no\n";
    }
    return 0;
}

int run_empty_bitmap_render_assertion(const std::filesystem::path &subtitle_path) {
    auto subtitle_renderer = create_default_subtitle_renderer();
    if (!subtitle_renderer) {
        return fail("The default subtitle renderer could not be created.");
    }

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = subtitle_path,
        .format_hint = "ass",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    };

    auto session_result = subtitle_renderer->create_session(session_request);
    if (!session_result.succeeded()) {
        const std::string error_message =
            "libassmod session creation failed unexpectedly: " +
            session_result.error->message +
            " Hint: " +
            session_result.error->actionable_hint;
        return fail(error_message);
    }

    const SubtitleRenderResult visible_before_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 41667
    });
    if (!visible_before_result.succeeded()) {
        return fail("The empty-bitmap sample did not render visible content before the zero-height frame.");
    }

    const SubtitleRenderResult empty_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 250000
    });
    if (!empty_result.succeeded()) {
        const std::string error_message =
            "The empty-bitmap render unexpectedly failed at the transient zero-height frame: " +
            empty_result.error->message +
            " Hint: " +
            empty_result.error->actionable_hint;
        return fail(error_message);
    }

    const SubtitleRenderResult visible_after_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 291667
    });
    if (!visible_after_result.succeeded()) {
        return fail("The empty-bitmap sample did not recover visible content after the zero-height frame.");
    }

    if (visible_before_result.rendered_frame->bitmaps.empty()) {
        return fail("Expected visible subtitle content before the transient empty bitmap.");
    }

    if (!empty_result.rendered_frame->bitmaps.empty()) {
        return fail("Expected the transient zero-height subtitle bitmap to be skipped as empty output.");
    }

    if (visible_after_result.rendered_frame->bitmaps.empty()) {
        return fail("Expected visible subtitle content after the transient empty bitmap.");
    }

    std::cout << "session.subtitle_path=" << format_path_leaf(subtitle_path) << '\n';
    std::cout << "visible_before.timestamp_us=41667\n";
    std::cout << "visible_before.has_content=yes\n";
    std::cout << "empty.timestamp_us=250000\n";
    std::cout << "empty.has_content=no\n";
    std::cout << "visible_after.timestamp_us=291667\n";
    std::cout << "visible_after.has_content=yes\n";
    return 0;
}

int run_img_asset_render_assertion(const std::filesystem::path &subtitle_path) {
    auto subtitle_renderer = create_default_subtitle_renderer();
    if (!subtitle_renderer) {
        return fail("The default subtitle renderer could not be created.");
    }

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = subtitle_path,
        .format_hint = "ass",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    };

    auto session_result = subtitle_renderer->create_session(session_request);
    if (!session_result.succeeded()) {
        return fail(
            "The libassmod img subtitle sample unexpectedly failed session creation: " +
            session_result.error->message +
            " Hint: " +
            session_result.error->actionable_hint
        );
    }

    std::vector<std::string> diagnostics{};
    const SubtitleCompositionDebugContext debug_context{
        .decoded_frame_index = 0,
        .output_pts = 0,
        .subtitle_source_time_microseconds = 0,
        .worker_id = 0,
        .session_id = 1,
        .log_frame_details = true,
        .log_bitmap_details = false,
        .log_callback = [&diagnostics](const std::string &message) {
            diagnostics.push_back(message);
        }
    };

    const auto render_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 0,
        .debug_context = &debug_context
    });
    if (!render_result.succeeded()) {
        const std::string error_message =
            "The best-effort libassmod img subtitle sample failed to render: " +
            render_result.error->message +
            " Hint: " +
            render_result.error->actionable_hint;
        return fail(error_message);
    }

    if (!string_messages_contain_text(diagnostics, "Subtitle image asset references detected: 1") ||
        !string_messages_contain_text(diagnostics, "Subtitle image asset registered: thumbnail.png")) {
        return fail("The libassmod img subtitle sample did not log the expected asset registration diagnostics.");
    }

    const auto second_render_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 41667,
        .debug_context = &debug_context
    });
    if (!second_render_result.succeeded()) {
        return fail("The libassmod img subtitle sample failed a second render after session setup.");
    }
    if (count_string_messages_containing_text(diagnostics, "Subtitle image asset registered: thumbnail.png") != 1U) {
        return fail("The libassmod img asset path should register/log assets once per session, not per render.");
    }

    std::cout << "session.subtitle_path=" << format_path_leaf(subtitle_path) << '\n';
    std::cout << "session.created=yes\n";
    std::cout << "img_asset.registered=yes\n";
    std::cout << "img_asset.registration_once=yes\n";
    std::cout << "render.succeeded=yes\n";
    std::cout << "render.bitmap_count=" << render_result.rendered_frame->bitmaps.size() << '\n';
    return 0;
}

int run_subtitle_render_lifecycle_trace_assertion(
    const std::filesystem::path &subtitle_path,
    const std::string_view trace_mode
) {
    if (trace_mode == "off") {
        unset_test_environment_variable("UTSURE_SUBTITLE_RENDER_TRACE");
        unset_test_environment_variable("UTSURE_SUBTITLE_RENDER_TRACE_FULL");
    } else if (trace_mode == "throttled") {
        set_test_environment_variable("UTSURE_SUBTITLE_RENDER_TRACE", "1");
        unset_test_environment_variable("UTSURE_SUBTITLE_RENDER_TRACE_FULL");
    } else if (trace_mode == "full") {
        set_test_environment_variable("UTSURE_SUBTITLE_RENDER_TRACE", "1");
        set_test_environment_variable("UTSURE_SUBTITLE_RENDER_TRACE_FULL", "1");
    } else {
        return fail("Unknown subtitle render lifecycle trace assertion mode.");
    }

    auto subtitle_renderer = create_default_subtitle_renderer();
    if (!subtitle_renderer) {
        return fail("The default subtitle renderer could not be created.");
    }

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = subtitle_path,
        .format_hint = "ass",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    };

    auto session_result = subtitle_renderer->create_session(session_request);
    if (!session_result.succeeded()) {
        return fail(
            "The libassmod lifecycle trace sample unexpectedly failed session creation: " +
            session_result.error->message +
            " Hint: " +
            session_result.error->actionable_hint
        );
    }

    std::vector<std::string> visible_logs{};
    std::vector<std::string> lifecycle_updates{};
    for (int frame_index = 0; frame_index < 100; ++frame_index) {
        const std::int64_t timestamp_us = static_cast<std::int64_t>(frame_index) * 41667;
        const SubtitleCompositionDebugContext debug_context{
            .decoded_frame_index = frame_index,
            .output_pts = frame_index,
            .subtitle_source_time_microseconds = timestamp_us,
            .worker_id = 0,
            .session_id = 1,
            .log_frame_details = false,
            .log_bitmap_details = false,
            .log_callback = [&visible_logs](const std::string &message) {
                visible_logs.push_back(message);
            },
            .lifecycle_callback = [&lifecycle_updates](const std::string &message) {
                lifecycle_updates.push_back(message);
            }
        };

        const auto render_result = session_result.session->render(SubtitleRenderRequest{
            .timestamp_microseconds = timestamp_us,
            .debug_context = &debug_context
        });
        if (!render_result.succeeded()) {
            return fail("The libassmod lifecycle trace sample failed to render.");
        }
    }

    const auto visible_start_count = count_string_messages_containing_text(visible_logs, "subtitle render start");
    const auto visible_end_count = count_string_messages_containing_text(visible_logs, "subtitle render end");
    const auto lifecycle_start_count = count_string_messages_containing_text(lifecycle_updates, "subtitle render start");
    const auto lifecycle_end_count = count_string_messages_containing_text(lifecycle_updates, "subtitle render end");

    if (lifecycle_start_count != 100U || lifecycle_end_count != 100U) {
        return fail("Subtitle render lifecycle crash-context updates did not record every rendered frame.");
    }

    if (!string_messages_contain_text(lifecycle_updates, "renderer=") ||
        !string_messages_contain_text(lifecycle_updates, "track=") ||
        !string_messages_contain_text(lifecycle_updates, "library=") ||
        !string_messages_contain_text(lifecycle_updates, "thread_id=") ||
        !string_messages_contain_text(lifecycle_updates, "active_subtitle_render_count=") ||
        !string_messages_contain_text(lifecycle_updates, "last_subtitle_event_count=") ||
        !string_messages_contain_text(lifecycle_updates, "registered_image_asset_count=") ||
        !string_messages_contain_text(lifecycle_updates, "subtitle_cleanup_started=")) {
        return fail("Subtitle render lifecycle crash-context updates omitted required state fields.");
    }

    if (trace_mode == "off" && (visible_start_count != 0U || visible_end_count != 0U)) {
        return fail("Subtitle render lifecycle logs should not be visible when render trace is off.");
    }

    if (trace_mode == "throttled" && (visible_start_count != 5U || visible_end_count != 5U)) {
        return fail("Throttled subtitle render trace should only log the first five frames in a 100-frame run.");
    }

    if (trace_mode == "full" && (visible_start_count != 100U || visible_end_count != 100U)) {
        return fail("Full subtitle render trace should log every rendered frame only when explicitly enabled.");
    }

    if (current_subtitle_strict_same_thread_mode()) {
        session_result.session.reset();
        if (!string_messages_contain_text(lifecycle_updates, "subtitle session destroyed: operation=teardown") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_strict_same_thread=1") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_owner_thread_id=") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_library_created_thread_id=") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_renderer_created_thread_id=") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_track_created_thread_id=") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_render_thread_id=") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_track_destroyed_thread_id=") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_renderer_destroyed_thread_id=") ||
            !string_messages_contain_text(lifecycle_updates, "subtitle_library_destroyed_thread_id=")) {
            return fail("Strict subtitle lifecycle diagnostics omitted owner or libassmod teardown thread ids.");
        }
    }

    std::cout << "render_lifecycle.trace_mode=" << trace_mode << '\n';
    std::cout << "render_lifecycle.visible_start_count=" << visible_start_count << '\n';
    std::cout << "render_lifecycle.visible_end_count=" << visible_end_count << '\n';
    std::cout << "render_lifecycle.lifecycle_start_count=" << lifecycle_start_count << '\n';
    std::cout << "render_lifecycle.lifecycle_end_count=" << lifecycle_end_count << '\n';
    return 0;
}

int run_missing_img_asset_render_assertion(const std::filesystem::path &subtitle_path) {
    const auto missing_subtitle_path = subtitle_path.parent_path() / "subtitle-burn-img-missing.generated.ass";
    write_text_file(
        missing_subtitle_path,
        "[Script Info]\n"
        "Title: missing img asset test\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 320\n"
        "PlayResY: 180\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,24,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,12,12,12,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:00.45,Default,,0,0,0,,{\\img(missing-asset.png)}IMG TAG\n"
    );

    auto subtitle_renderer = create_default_subtitle_renderer();
    if (!subtitle_renderer) {
        return fail("The default subtitle renderer could not be created.");
    }

    const auto session_result = subtitle_renderer->create_session(SubtitleRenderSessionCreateRequest{
        .subtitle_path = missing_subtitle_path,
        .format_hint = "ass",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    });
    if (session_result.succeeded() ||
        !session_result.error.has_value() ||
        session_result.error->message.find("Missing subtitle image asset: missing-asset.png") == std::string::npos) {
        return fail("Missing libassmod img asset did not fail session creation clearly.");
    }

    std::cout << "img_asset.missing=session_failed\n";
    std::cout << "img_asset.error=clear\n";
    return 0;
}

int run_burn_in_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path,
    const OutputVideoCodec codec
) {
    CollectingObserver observer{};
    const EncodeJob plain_job{
        .input = {
            .main_source_path = sample_path
        },
        .output = {
            .output_path = plain_output_path,
            .video = {
                .codec = codec,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJob burned_job{
        .input = {
            .main_source_path = sample_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = codec,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    if (!plain_job_result.succeeded()) {
        return fail("Plain encode job failed unexpectedly before subtitle comparison.");
    }

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        return fail("Subtitle burn-in job failed unexpectedly.");
    }

    if (burned_job_result.encode_job_summary->subtitled_video_frame_count != 11) {
        return fail("Unexpected count of subtitled video frames in the burn-in summary.");
    }

    if (burned_job_result.encode_job_summary->streaming_runtime.subtitle_compose_microseconds == 0U) {
        return fail("Subtitle burn-in jobs should report non-zero subtitle composition time.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("Subtitle burn-in output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, 48U, true) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, 48U, true) != 0) {
        return 1;
    }

    if (frames_are_identical(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("Subtitle burn-in did not alter the first output frame.");
    }

    const auto observer_result = assert_observer_flow(observer, 1);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        *burned_job_result.encode_job_summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    if (current_subtitle_strict_same_thread_mode()) {
        if (!burned_job_result.encode_job_summary->streaming_runtime.subtitle_strict_same_thread ||
            !observer_logs_contain_text(
                observer,
                "Subtitle strict same-thread mode: main libassmod session creation is deferred to the subtitle-owner worker."
            ) ||
            !observer_logs_contain_text(observer, "subtitle render start: operation=compose") ||
            !observer_logs_contain_text(observer, "subtitle_strict_same_thread=1") ||
            !observer_logs_contain_text(observer, "subtitle_owner_thread_id=") ||
            !observer_logs_contain_text(observer, "subtitle_library_created_thread_id=") ||
            !observer_logs_contain_text(observer, "subtitle_renderer_created_thread_id=") ||
            !observer_logs_contain_text(observer, "subtitle_track_created_thread_id=") ||
            !observer_logs_contain_text(observer, "subtitle_render_thread_id=")) {
            return fail("Strict burn-in mode did not report the subtitle-owner session lifetime diagnostics.");
        }
    }

    std::cout << build_validation_report(
        *burned_job_result.encode_job_summary,
        *plain_output_decode.decoded_media_source,
        *burned_output_decode.decoded_media_source
    ) << '\n';
    return 0;
}

int run_empty_bitmap_burn_in_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path
) {
    CollectingObserver observer{};
    const EncodeJob plain_job{
        .input = {
            .main_source_path = sample_path
        },
        .output = {
            .output_path = plain_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJob burned_job{
        .input = {
            .main_source_path = sample_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    if (!plain_job_result.succeeded()) {
        return fail("Plain encode job failed unexpectedly before the empty-bitmap regression check.");
    }

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        return fail("The empty-bitmap subtitle burn-in job failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    if (summary.subtitled_video_frame_count != 11) {
        return fail("Unexpected count of subtitled video frames for the empty-bitmap regression sample.");
    }

    if (summary.streaming_runtime.subtitle_compose_microseconds == 0U) {
        return fail("The empty-bitmap subtitle burn-in job should report non-zero subtitle composition time.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("The empty-bitmap regression output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, 48U, true) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, 48U, true) != 0) {
        return 1;
    }

    if (frames_are_identical(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("The empty-bitmap subtitle burn-in regression did not alter the first output frame.");
    }

    const auto observer_result = assert_observer_flow(observer, 1);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    std::cout << build_validation_report(
        summary,
        *plain_output_decode.decoded_media_source,
        *burned_output_decode.decoded_media_source
    ) << '\n';
    return 0;
}

int run_img_asset_burn_in_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path
) {
    CollectingObserver observer{};
    const EncodeJob plain_job{
        .input = {
            .main_source_path = sample_path
        },
        .output = {
            .output_path = plain_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJob burned_job{
        .input = {
            .main_source_path = sample_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    if (!plain_job_result.succeeded()) {
        return fail("Plain encode job failed unexpectedly before the best-effort img regression check.");
    }

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        return fail("The img asset subtitle burn-in job failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    if (summary.streaming_runtime.subtitle_compose_microseconds == 0U) {
        return fail("The img asset subtitle burn-in job should report non-zero subtitle composition time.");
    }

    if (!observer_logs_contain_text(observer, "Subtitle image asset references detected: 1") ||
        !observer_logs_contain_text(observer, "Subtitle image asset registered: thumbnail.png")) {
        return fail("The img asset subtitle burn-in job did not log the expected asset registration diagnostics.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("The best-effort img regression output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, 48U, true) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, 48U, true) != 0) {
        return 1;
    }

    const auto observer_result = assert_observer_flow(observer, 1);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    std::cout << build_validation_report(
        summary,
        *plain_output_decode.decoded_media_source,
        *burned_output_decode.decoded_media_source
    ) << '\n';
    std::cout << "img_asset.registered=yes\n";
    std::cout << "img_asset.encode_succeeded=yes\n";
    std::cout << "img_asset.frame0.changed="
              << (frames_are_identical(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)
                    ? "no"
                    : "yes")
              << '\n';
    return 0;
}

int run_render_schedule_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &burned_output_path
) {
    CollectingObserver observer{};
    const EncodeJob burned_job{
        .input = {
            .main_source_path = sample_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        return fail("Subtitle render scheduling encode failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!burned_output_decode.succeeded()) {
        return fail("Subtitle render scheduling output decode failed unexpectedly.");
    }

    const auto expected_frame_count = static_cast<std::size_t>(summary.timeline_summary.output_video_frame_count);
    if (assert_decoded_output(*burned_output_decode.decoded_media_source, expected_frame_count, true) != 0) {
        return 1;
    }

    if (summary.subtitled_video_frame_count != summary.timeline_summary.output_video_frame_count ||
        summary.streaming_runtime.subtitle_compose_microseconds == 0U) {
        return fail("Subtitle render scheduling did not apply subtitles to every output frame.");
    }

    const auto observer_result = assert_observer_flow(observer, 1);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    const auto schedule_result = assert_subtitle_render_schedule_diagnostics(
        observer,
        summary,
        *burned_output_decode.decoded_media_source,
        0U,
        expected_frame_count,
        0,
        "Subtitle render scheduling"
    );
    if (schedule_result != 0) {
        return schedule_result;
    }

    std::cout << "schedule.subtitle_path=" << format_path_leaf(subtitle_path) << '\n';
    std::cout << "schedule.output_frames=" << summary.timeline_summary.output_video_frame_count << '\n';
    std::cout << "schedule.render_calls=" << expected_frame_count << '\n';
    std::cout << "schedule.timestamps=main_relative\n";
    return 0;
}

int run_trimmed_main_burn_in_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path
) {
    CollectingObserver observer{};
    const EncodeJob plain_job{
        .input = {
            .main_source_path = sample_path,
            .main_source_trim_in_us = 250000,
            .main_source_trim_out_us = 1250000
        },
        .output = {
            .output_path = plain_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJob burned_job{
        .input = {
            .main_source_path = sample_path,
            .main_source_trim_in_us = 250000,
            .main_source_trim_out_us = 1250000
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    if (!plain_job_result.succeeded()) {
        return fail("Plain trimmed encode failed unexpectedly before subtitle comparison.");
    }

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        return fail("Trimmed subtitle burn-in job failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    if (summary.job.input.main_source_trim_in_us != std::optional<std::int64_t>(250000) ||
        summary.job.input.main_source_trim_out_us != std::optional<std::int64_t>(1250000) ||
        summary.timeline_summary.output_duration_microseconds != 1000000 ||
        summary.timeline_summary.output_video_frame_count != 24 ||
        summary.subtitled_video_frame_count != 5 ||
        summary.streaming_runtime.subtitle_compose_microseconds == 0U) {
        return fail("Unexpected trimmed subtitle burn-in summary state.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("Trimmed subtitle burn-in output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, 24U, true) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, 24U, true) != 0) {
        return 1;
    }

    if (assert_trimmed_audio_window(
            *plain_output_decode.decoded_media_source,
            1000000,
            100000,
            100000,
            "The plain trimmed subtitle-baseline output"
        ) != 0 ||
        assert_trimmed_audio_window(
            *burned_output_decode.decoded_media_source,
            1000000,
            100000,
            100000,
            "The trimmed subtitle burn-in output"
        ) != 0) {
        return 1;
    }

    if (!frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("A subtitle overlapping trim start was not visible on the first encoded frame.");
    }

    const auto report = format_encode_job_report(summary);
    if (!contains_text(report, "job.input.main_trim.in_us=250000") ||
        !contains_text(report, "job.input.main_trim.out_us=1250000")) {
        return fail("The trimmed subtitle burn-in report did not include the trim range.");
    }

    if (summary.streaming_runtime.subtitle_diagnostics_mode != "off") {
        const auto schedule_result = assert_subtitle_render_schedule_diagnostics(
            observer,
            summary,
            *burned_output_decode.decoded_media_source,
            0U,
            static_cast<std::size_t>(summary.timeline_summary.output_video_frame_count),
            0,
            "Trimmed main subtitle render scheduling",
            250000
        );
        if (schedule_result != 0) {
            return schedule_result;
        }

        const auto diagnostics = collect_subtitle_schedule_diagnostics(observer);
        if (diagnostics.size() <= 5U ||
            diagnostics.front().bitmap_count <= 0 ||
            diagnostics[4].bitmap_count <= 0 ||
            diagnostics[5].bitmap_count != 0) {
            return fail(
                "Trimmed subtitle diagnostics did not preserve the overlap at output zero "
                "or stop rendering at the original source end time."
            );
        }
    }

    const auto observer_result = assert_observer_flow(observer, 1);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    std::cout << build_validation_report(
        summary,
        *plain_output_decode.decoded_media_source,
        *burned_output_decode.decoded_media_source
    ) << '\n';
    return 0;
}

int run_timeline_burn_in_assertion(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path
) {
    CollectingObserver observer{};
    const EncodeJob plain_job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .output = {
            .output_path = plain_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJob burned_job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!plain_job_result.succeeded() || !burned_job_result.succeeded()) {
        return fail("Timeline subtitle burn-in jobs failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    if (summary.timeline_summary.segments.size() != 3 ||
        summary.timeline_summary.segments[0].subtitles_enabled ||
        !summary.timeline_summary.segments[1].subtitles_enabled ||
        summary.timeline_summary.segments[2].subtitles_enabled) {
        return fail("Timeline subtitle scope did not stay on the main segment.");
    }

    if (summary.subtitled_video_frame_count != 11) {
        return fail("Unexpected count of subtitled video frames for the timeline burn-in path.");
    }

    if (summary.streaming_runtime.subtitle_compose_microseconds == 0U) {
        return fail("Timeline subtitle burn-in jobs should report non-zero subtitle composition time.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("Timeline subtitle output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, 96U, true) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, 96U, true) != 0) {
        return 1;
    }

    const auto main_frame_offset =
        static_cast<std::size_t>(summary.timeline_summary.segments[0].video_frame_count);
    const auto main_frame_count =
        static_cast<std::size_t>(summary.timeline_summary.segments[1].video_frame_count);
    if (summary.streaming_runtime.subtitle_diagnostics_mode != "off") {
        const auto schedule_result = assert_subtitle_render_schedule_diagnostics(
            observer,
            summary,
            *burned_output_decode.decoded_media_source,
            main_frame_offset,
            main_frame_count,
            summary.timeline_summary.segments[1].start_microseconds,
            "Timeline main-segment subtitle render scheduling"
        );
        if (schedule_result != 0) {
            return schedule_result;
        }
    }

    // Encoder prediction can let changed main-segment frames influence later compressed frames.
    // Keep this assertion anchored to the first intro frame, which should remain outside subtitle scope.
    if (frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("Timeline subtitle burn-in altered the first intro frame unexpectedly.");
    }

    if (!any_frame_changed_in_range(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 24U, 48U)) {
        return fail("Timeline subtitle burn-in did not alter any main-segment frames.");
    }

    if (!frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, main_frame_offset + 24U)) {
        return fail("Timeline subtitle burn-in did not change the main-segment frame at the ASS event time.");
    }

    const auto observer_result = assert_observer_flow(observer, 3);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    std::cout << "timeline.intro.frame0.changed=no\n";
    std::cout << "timeline.main.changed=yes\n";
    std::cout << "timeline.outro.scope=diagnostics_only\n";
    std::cout << "timeline.subtitled_frames=" << summary.subtitled_video_frame_count << '\n';
    if (summary.streaming_runtime.subtitle_diagnostics_mode != "off") {
        std::cout << "timeline.render_schedule=main_relative\n";
    }
    return 0;
}

int run_timeline_full_output_rejection_assertion(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path
) {
    (void)plain_output_path;
    CollectingObserver observer{};
    const EncodeJob rejected_job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass",
            .timing_mode = SubtitleTimingMode::full_output_timeline
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult result = EncodeJobRunner::run(rejected_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (result.succeeded()) {
        return fail("Full-output timeline subtitle timing succeeded even though it violates main-only subtitle scope.");
    }

    if (!result.error.has_value() || !contains_text(result.error->message, "Full-output subtitle timing")) {
        return fail("Full-output timeline subtitle timing did not report the expected rejection reason.");
    }

    std::cout << "timeline.full_output.rejected=yes\n";
    std::cout << "timeline.full_output.reason=main_subtitle_scope\n";
    return 0;
}

int run_timeline_thumbnail_preroll_burn_in_assertion(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &burned_output_path
) {
    CollectingObserver observer{};
    const EncodeJob burned_job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .thumbnail_preroll = utsure::core::job::EncodeJobThumbnailPrerollSettings{
            .enabled = true
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        return fail("Thumbnail pre-roll timeline subtitle burn-in job failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    if (summary.timeline_summary.segments.size() != 3 ||
        summary.timeline_summary.segments[0].subtitles_enabled ||
        !summary.timeline_summary.segments[1].subtitles_enabled ||
        summary.timeline_summary.segments[2].subtitles_enabled) {
        return fail("Thumbnail pre-roll timeline subtitle scope did not stay on the main segment.");
    }

    if (summary.timeline_summary.output_video_frame_count != 314 ||
        summary.subtitled_video_frame_count != 13) {
        return fail("Unexpected thumbnail pre-roll timeline subtitle frame counts.");
    }

    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!burned_output_decode.succeeded()) {
        return fail("Thumbnail pre-roll timeline subtitle output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*burned_output_decode.decoded_media_source, 314U, true) != 0) {
        return 1;
    }

    const auto main_frame_offset =
        2U + static_cast<std::size_t>(summary.timeline_summary.segments[0].video_frame_count);
    const auto main_frame_count =
        static_cast<std::size_t>(summary.timeline_summary.segments[1].video_frame_count);
    if (summary.timeline_summary.segments[1].start_microseconds <= 10000000) {
        return fail("Thumbnail pre-roll plus the 10 second intro did not move the main segment start after the intro boundary.");
    }
    const auto first_main_subtitle_frame_output_us =
        burned_output_decode.decoded_media_source->video_frames[main_frame_offset + 24U].timestamp.start_microseconds;
    if (first_main_subtitle_frame_output_us < 11000000) {
        return fail("The 1 second ASS event was scheduled before the 10 second intro had elapsed.");
    }

    if (summary.streaming_runtime.subtitle_diagnostics_mode != "off") {
        const auto schedule_result = assert_subtitle_render_schedule_diagnostics(
            observer,
            summary,
            *burned_output_decode.decoded_media_source,
            main_frame_offset,
            main_frame_count,
            summary.timeline_summary.segments[1].start_microseconds,
            "Thumbnail pre-roll timeline subtitle render scheduling"
        );
        if (schedule_result != 0) {
            return schedule_result;
        }
    }

    const auto observer_result = assert_observer_flow(observer, 3, 1);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    std::cout << "timeline.thumbnail_preroll.output_frames=" << summary.timeline_summary.output_video_frame_count << '\n';
    std::cout << "timeline.thumbnail_preroll.subtitled_frames=" << summary.subtitled_video_frame_count << '\n';
    std::cout << "timeline.thumbnail_preroll.intro_seconds=10\n";
    std::cout << "timeline.thumbnail_preroll.first_main_subtitle_output_us="
              << first_main_subtitle_frame_output_us << '\n';
    std::cout << "timeline.thumbnail_preroll.render_schedule=main_relative\n";
    return 0;
}

int run_timeline_resize_burn_in_assertion(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &burned_output_path
) {
    CollectingObserver observer{};
    const EncodeJob burned_job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            },
            .resize = {
                .mode = utsure::core::job::EncodeResizeMode::target_height,
                .target_height = 90,
                .allow_upscale = false
            }
        }
    };

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        dump_encode_job_failure_diagnostics(
            "Main-relative resized timeline subtitle burn-in job failed unexpectedly",
            burned_job_result,
            observer,
            burned_output_path
        );
        return fail("Main-relative resized timeline subtitle burn-in job failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    constexpr std::int64_t kExpectedTotalFrameCount = 96;
    // subtitle-burn-sample.ass is active on the main timeline for [0 us, 450000 us).
    // At the sample's 24 fps cadence this covers the first 11 main frames:
    // 0, 41666, ..., 416666 us. The 458333 us frame is outside the event.
    constexpr std::size_t kExpectedBitmapPositiveDiagnosticCount = 11U;
    if (summary.timeline_summary.segments.size() != 3 ||
        summary.timeline_summary.segments[0].subtitles_enabled ||
        !summary.timeline_summary.segments[1].subtitles_enabled ||
        summary.timeline_summary.segments[2].subtitles_enabled) {
        return fail("Resized timeline subtitle scope did not stay on the main segment.");
    }

    const auto schedule_diagnostics = collect_subtitle_schedule_diagnostics(observer);
    const auto bitmap_positive_diagnostics = count_bitmap_positive_subtitle_diagnostics(schedule_diagnostics);
    // The resized timeline path validates visible subtitle activity through renderer
    // diagnostics. The final streaming summary can report a separate handoff counter,
    // but bitmap-positive diagnostics prove the resized 160x90 main frames received
    // rendered subtitle images without relying on source-size pixel-region checks.
    if (summary.timeline_summary.output_video_frame_count != kExpectedTotalFrameCount ||
        schedule_diagnostics.size() != static_cast<std::size_t>(summary.timeline_summary.segments[1].video_frame_count) ||
        bitmap_positive_diagnostics != kExpectedBitmapPositiveDiagnosticCount) {
        dump_subtitle_schedule_frame_count_diagnostics(
            "Unexpected main-relative resized timeline subtitle diagnostic frame counts",
            summary,
            observer,
            kExpectedTotalFrameCount,
            static_cast<std::int64_t>(kExpectedBitmapPositiveDiagnosticCount)
        );
        return fail("Unexpected main-relative resized timeline subtitle diagnostic frame counts.");
    }

    if (!summary.inspected_input_info.primary_video_stream.has_value() ||
        summary.inspected_input_info.primary_video_stream->width != 320 ||
        summary.inspected_input_info.primary_video_stream->height != 180) {
        return fail("Resized timeline subtitle job rewrote the native main source dimensions.");
    }

    if (!summary.encoded_media_summary.output_info.primary_video_stream.has_value() ||
        summary.encoded_media_summary.output_info.primary_video_stream->width != 160 ||
        summary.encoded_media_summary.output_info.primary_video_stream->height != 90) {
        return fail("Resized timeline subtitle job did not encode the requested 160x90 output dimensions.");
    }

    if (!observer_logs_contain_text(observer, "libassmod renderer setup: frame_size=160x90")) {
        return fail("Resized timeline subtitle job did not create the libassmod renderer on the resized output canvas.");
    }

    if (!observer_logs_contain_text(observer, "intro segment normalized toward the main output: raster 320x180 -> 160x90") ||
        !observer_logs_contain_text(observer, "outro segment normalized toward the main output: raster 320x180 -> 160x90")) {
        return fail("Resized timeline subtitle job did not normalize intro/outro toward the resized output dimensions.");
    }

    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!burned_output_decode.succeeded()) {
        return fail("Resized timeline subtitle output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*burned_output_decode.decoded_media_source, 96U, true) != 0 ||
        assert_decoded_video_dimensions(*burned_output_decode.decoded_media_source, 160, 90, "Resized timeline subtitle output") != 0) {
        return 1;
    }

    const auto main_frame_offset =
        static_cast<std::size_t>(summary.timeline_summary.segments[0].video_frame_count);
    const auto main_frame_count =
        static_cast<std::size_t>(summary.timeline_summary.segments[1].video_frame_count);
    if (summary.streaming_runtime.subtitle_diagnostics_mode != "off") {
        const auto schedule_result = assert_subtitle_render_schedule_diagnostics(
            observer,
            summary,
            *burned_output_decode.decoded_media_source,
            main_frame_offset,
            main_frame_count,
            summary.timeline_summary.segments[1].start_microseconds,
            "Main-relative resized timeline subtitle render scheduling",
            0,
            std::pair<int, int>{160, 90}
        );
        if (schedule_result != 0) {
            return schedule_result;
        }
    }

    const auto observer_result = assert_observer_flow(observer, 3);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    std::cout << "timeline.resize.output_dimensions=160x90\n";
    std::cout << "timeline.resize.render_schedule=main_relative\n";
    return 0;
}

int run_timeline_thumbnail_resize_burn_in_assertion(
    const std::filesystem::path &intro_path,
    const std::filesystem::path &main_path,
    const std::filesystem::path &outro_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &burned_output_path
) {
    CollectingObserver observer{};
    const EncodeJob burned_job{
        .input = {
            .intro_source_path = intro_path,
            .main_source_path = main_path,
            .outro_source_path = outro_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .thumbnail_preroll = utsure::core::job::EncodeJobThumbnailPrerollSettings{
            .enabled = true
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            },
            .resize = {
                .mode = utsure::core::job::EncodeResizeMode::target_height,
                .target_height = 90,
                .allow_upscale = false
            }
        }
    };

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        dump_encode_job_failure_diagnostics(
            "Resized thumbnail pre-roll timeline subtitle burn-in job failed unexpectedly",
            burned_job_result,
            observer,
            burned_output_path
        );
        return fail("Resized thumbnail pre-roll timeline subtitle burn-in job failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    constexpr std::int64_t kExpectedTotalFrameCount = 98;
    constexpr std::size_t kExpectedBitmapPositiveDiagnosticCount = 11U;
    if (summary.timeline_summary.segments.size() != 3 ||
        summary.timeline_summary.segments[0].subtitles_enabled ||
        !summary.timeline_summary.segments[1].subtitles_enabled ||
        summary.timeline_summary.segments[2].subtitles_enabled) {
        return fail("Resized thumbnail pre-roll timeline subtitle scope did not stay on the main segment.");
    }

    const auto schedule_diagnostics = collect_subtitle_schedule_diagnostics(observer);
    const auto bitmap_positive_diagnostics = count_bitmap_positive_subtitle_diagnostics(schedule_diagnostics);
    if (summary.timeline_summary.output_video_frame_count != kExpectedTotalFrameCount ||
        schedule_diagnostics.size() != static_cast<std::size_t>(summary.timeline_summary.segments[1].video_frame_count) ||
        bitmap_positive_diagnostics != kExpectedBitmapPositiveDiagnosticCount) {
        dump_subtitle_schedule_frame_count_diagnostics(
            "Unexpected resized thumbnail pre-roll timeline subtitle diagnostic frame counts",
            summary,
            observer,
            kExpectedTotalFrameCount,
            static_cast<std::int64_t>(kExpectedBitmapPositiveDiagnosticCount)
        );
        return fail("Unexpected resized thumbnail pre-roll timeline subtitle diagnostic frame counts.");
    }

    if (!observer_logs_contain_text(observer, "normalized thumbnail source from 320x180 to 160x90")) {
        return fail("Resized thumbnail pre-roll did not log thumbnail normalization to the final output size.");
    }

    if (!observer_logs_contain_text(observer, "libassmod renderer setup: frame_size=160x90")) {
        return fail("Resized thumbnail pre-roll timeline job did not create libassmod renderers on the resized output canvas.");
    }

    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!burned_output_decode.succeeded()) {
        return fail("Resized thumbnail pre-roll timeline subtitle output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*burned_output_decode.decoded_media_source, static_cast<std::size_t>(kExpectedTotalFrameCount), true) != 0 ||
        assert_decoded_video_dimensions(*burned_output_decode.decoded_media_source, 160, 90, "Resized thumbnail pre-roll timeline subtitle output") != 0) {
        return 1;
    }

    const auto main_frame_offset =
        2U + static_cast<std::size_t>(summary.timeline_summary.segments[0].video_frame_count);
    const auto main_frame_count =
        static_cast<std::size_t>(summary.timeline_summary.segments[1].video_frame_count);
    if (summary.streaming_runtime.subtitle_diagnostics_mode != "off") {
        const auto schedule_result = assert_subtitle_render_schedule_diagnostics(
            observer,
            summary,
            *burned_output_decode.decoded_media_source,
            main_frame_offset,
            main_frame_count,
            summary.timeline_summary.segments[1].start_microseconds,
            "Resized thumbnail pre-roll main-segment subtitle render scheduling",
            0,
            std::pair<int, int>{160, 90}
        );
        if (schedule_result != 0) {
            return schedule_result;
        }
    }

    const auto observer_result = assert_observer_flow(observer, 3, 1);
    if (observer_result != 0) {
        return observer_result;
    }

    std::cout << "timeline.thumbnail_resize.output_dimensions=160x90\n";
    std::cout << "timeline.thumbnail_resize.normalized_thumbnail=yes\n";
    std::cout << "timeline.thumbnail_resize.render_schedule=main_relative\n";
    return 0;
}

int run_stress_burn_in_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &plain_output_path,
    const std::filesystem::path &burned_output_path
) {
    constexpr std::size_t kExpectedFrameCount = 480U;

    CollectingObserver observer{};
    const EncodeJob plain_job{
        .input = {
            .main_source_path = sample_path
        },
        .output = {
            .output_path = plain_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJob burned_job{
        .input = {
            .main_source_path = sample_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = burned_output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 23
            }
        }
    };

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    if (!plain_job_result.succeeded()) {
        return fail("Plain stress encode failed unexpectedly before subtitle comparison.");
    }

    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!burned_job_result.succeeded()) {
        return fail("Subtitle stress encode failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    if (summary.timeline_summary.output_video_frame_count != static_cast<std::int64_t>(kExpectedFrameCount) ||
        summary.subtitled_video_frame_count != static_cast<std::int64_t>(kExpectedFrameCount) ||
        summary.streaming_runtime.subtitle_compose_microseconds == 0U) {
        return fail("Unexpected summary counts for the subtitle stress encode.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("Subtitle stress output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, kExpectedFrameCount, true) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, kExpectedFrameCount, true) != 0) {
        return 1;
    }

    if (!frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U) ||
        !frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 240U) ||
        !frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 479U)) {
        return fail("Subtitle stress burn-in did not visibly alter the expected sampled frames.");
    }

    const auto observer_result = assert_observer_flow(observer, 1);
    if (observer_result != 0) {
        return observer_result;
    }

    const auto runtime_result = assert_subtitle_runtime_visibility(
        observer,
        summary,
        current_subtitle_bitmap_mode(),
        current_subtitle_composition_mode()
    );
    if (runtime_result != 0) {
        return runtime_result;
    }

    std::cout << "stress.bitmap_mode=" << current_subtitle_bitmap_mode() << '\n';
    std::cout << "stress.composition_mode=" << current_subtitle_composition_mode() << '\n';
    std::cout << "stress.subtitle_workers=" << summary.streaming_runtime.subtitle_processing_worker_count << '\n';
    std::cout << "stress.subtitled_frames=" << summary.subtitled_video_frame_count << '\n';
    std::cout << "stress.sample_frame0.changed=yes\n";
    std::cout << "stress.sample_frame240.changed=yes\n";
    std::cout << "stress.sample_frame479.changed=yes\n";
    return 0;
}

int run_sequential_stability_assertion(
    const std::filesystem::path &sample_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &first_output_path,
    const std::filesystem::path &second_output_path,
    const std::filesystem::path &third_output_path
) {
    const std::array<std::filesystem::path, 3> outputs{
        first_output_path,
        second_output_path,
        third_output_path
    };

    std::int64_t previous_frame_count = -1;
    const auto initial_memory = sample_process_memory();
    std::vector<ProcessMemorySnapshot> after_job_memory{};
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const auto before_job_memory = sample_process_memory();
        if (before_job_memory.has_value()) {
            std::cout << "sequential_stability.job" << (index + 1)
                      << ".rss_before=" << before_job_memory->rss_bytes
                      << " peak_before=" << before_job_memory->peak_rss_bytes << '\n';
        }

        CollectingObserver observer{};
        const EncodeJob job{
            .input = {
                .main_source_path = sample_path
            },
            .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
                .subtitle_path = subtitle_path,
                .format_hint = "ass"
            },
            .output = {
                .output_path = outputs[index],
                .video = {
                    .codec = OutputVideoCodec::h264,
                    .preset = "ultrafast",
                    .crf = 28
                }
            },
            .execution = {
                .threading = {
                    .cpu_usage_mode = utsure::core::media::CpuUsageMode::auto_select
                },
                .video_frame_queue_depth_override = 12U
            }
        };

        const EncodeJobResult result = EncodeJobRunner::run(job, EncodeJobRunOptions{
            .decode_normalization_policy = {},
            .observer = &observer
        });
        if (!result.succeeded()) {
            return fail("A sequential subtitle stability encode failed unexpectedly.");
        }

        const auto &summary = *result.encode_job_summary;
        if (summary.streaming_runtime.selected_video_decoder_thread_count == 0 ||
            summary.streaming_runtime.selected_video_encoder_thread_count == 0 ||
            summary.streaming_runtime.subtitle_processing_worker_count != 1U ||
            summary.streaming_runtime.video_frame_queue_depth > 12U ||
            summary.streaming_runtime.subtitle_compose_microseconds == 0U) {
            return fail("Sequential stability encode did not use the expected bounded runtime.");
        }

        if (!observer_logs_contain_text(observer, "Segment start: name=main") ||
            !observer_logs_contain_text(observer, "Streaming frame checkpoint: segment=main") ||
            !observer_logs_contain_text(observer, "decoder_threads=") ||
            !observer_logs_contain_text(observer, "encoder_threads=") ||
            !observer_logs_contain_text(observer, "subtitle_workers=") ||
            !observer_logs_contain_text(observer, "video_queue_depth=") ||
            !observer_logs_contain_text(observer, "Mux stage end: output finalized")) {
            return fail("Sequential stability encode did not emit the expected long-run diagnostics.");
        }

        const MediaDecodeResult output_decode = MediaDecoder::decode(outputs[index]);
        if (!output_decode.succeeded() || output_decode.decoded_media_source->video_frames.empty()) {
            return fail("Sequential stability output did not decode after encode.");
        }

        const auto frame_count = summary.encoded_media_summary.encoded_video_frame_count;
        if (previous_frame_count >= 0 && frame_count != previous_frame_count) {
            return fail("Sequential stability encodes produced inconsistent frame counts.");
        }
        previous_frame_count = frame_count;

        const auto after_memory = sample_process_memory();
        if (after_memory.has_value()) {
            after_job_memory.push_back(*after_memory);
            std::cout << "sequential_stability.job" << (index + 1)
                      << ".rss_after=" << after_memory->rss_bytes
                      << " peak_after=" << after_memory->peak_rss_bytes << '\n';
        }
    }

    if (initial_memory.has_value() && after_job_memory.size() == outputs.size()) {
        constexpr std::uint64_t kAllowedSequentialGrowthBytes = 256ULL * 1024ULL * 1024ULL;
        const auto final_rss = after_job_memory.back().rss_bytes;
        if (final_rss > initial_memory->rss_bytes + kAllowedSequentialGrowthBytes) {
            return fail("Sequential stability RSS grew beyond the allowed tolerance.");
        }
        std::cout << "sequential_stability.rss_initial=" << initial_memory->rss_bytes << '\n';
        std::cout << "sequential_stability.rss_final=" << final_rss << '\n';
        std::cout << "sequential_stability.peak_final=" << after_job_memory.back().peak_rss_bytes << '\n';
    }

    std::cout << "sequential_stability.jobs=3\n";
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 3) {
        return fail(
            "Usage: utsure_core_subtitle_burn_in_tests "
            "[--render <subtitle>|--render-gradient <subtitle>|--render-empty-effect <subtitle>|"
            "--render-img-asset <subtitle>|--render-bs4 <subtitle>|--render-bs4-gradient <subtitle>|"
            "--render-bs4-forced-rgba <subtitle>|--render-lifecycle-trace-off <subtitle>|"
            "--render-lifecycle-trace-throttled <subtitle>|--render-lifecycle-trace-full <subtitle>|"
            "--h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--empty-bitmap-h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--img-asset-h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--schedule-h264 <input> <subtitle> <burned-output>|"
            "--h265 <input> <subtitle> <plain-output> <burned-output>|"
            "--trimmed-h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--stress-h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--sequential-stability-h264 <input> <subtitle> <out1> <out2> <out3>|"
            "--timeline-h264 <intro> <main> <outro> <subtitle> <plain-output> <burned-output>|"
            "--timeline-thumbnail-h264 <intro> <main> <outro> <subtitle> <burned-output>|"
            "--timeline-resize-h264 <intro> <main> <outro> <subtitle> <burned-output>|"
            "--timeline-thumbnail-resize-h264 <intro> <main> <outro> <subtitle> <burned-output>|"
            "--timeline-full-rejected <intro> <main> <outro> <subtitle> <plain-output> <burned-output>]"
        );
    }

    const std::string_view mode(argv[1]);

    if (mode == "--render" && argc == 3) {
        return run_render_assertion(std::filesystem::path(argv[2]));
    }

    if (mode == "--render-gradient" && argc == 3) {
        return run_render_assertion(std::filesystem::path(argv[2]), true);
    }

    if (mode == "--render-bs4" && argc == 3) {
        return run_render_assertion(std::filesystem::path(argv[2]), false, true);
    }

    if (mode == "--render-bs4-gradient" && argc == 3) {
        return run_render_assertion(std::filesystem::path(argv[2]), true, true);
    }

    if (mode == "--render-bs4-forced-rgba" && argc == 3) {
        return run_render_assertion(
            std::filesystem::path(argv[2]),
            true,
            true,
            1204800000LL,
            std::nullopt
        );
    }

    if (mode == "--render-empty-effect" && argc == 3) {
        return run_empty_bitmap_render_assertion(std::filesystem::path(argv[2]));
    }

    if (mode == "--render-img-asset" && argc == 3) {
        return run_img_asset_render_assertion(std::filesystem::path(argv[2]));
    }

    if (mode == "--render-img-missing" && argc == 3) {
        return run_missing_img_asset_render_assertion(std::filesystem::path(argv[2]));
    }

    if (mode == "--render-lifecycle-trace-off" && argc == 3) {
        return run_subtitle_render_lifecycle_trace_assertion(std::filesystem::path(argv[2]), "off");
    }

    if (mode == "--render-lifecycle-trace-throttled" && argc == 3) {
        return run_subtitle_render_lifecycle_trace_assertion(std::filesystem::path(argv[2]), "throttled");
    }

    if (mode == "--render-lifecycle-trace-full" && argc == 3) {
        return run_subtitle_render_lifecycle_trace_assertion(std::filesystem::path(argv[2]), "full");
    }

    if (mode == "--h264" && argc == 6) {
        return run_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            OutputVideoCodec::h264
        );
    }

    if (mode == "--empty-bitmap-h264" && argc == 6) {
        return run_empty_bitmap_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5])
        );
    }

    if (mode == "--img-asset-h264" && argc == 6) {
        return run_img_asset_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5])
        );
    }

    if (mode == "--schedule-h264" && argc == 5) {
        return run_render_schedule_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4])
        );
    }

    if (mode == "--h265" && argc == 6) {
        return run_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            OutputVideoCodec::h265
        );
    }

    if (mode == "--trimmed-h264" && argc == 6) {
        return run_trimmed_main_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5])
        );
    }

    if (mode == "--stress-h264" && argc == 6) {
        return run_stress_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5])
        );
    }

    if (mode == "--sequential-stability-h264" && argc == 7) {
        return run_sequential_stability_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6])
        );
    }

    if (mode == "--timeline-h264" && argc == 8) {
        return run_timeline_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6]),
            std::filesystem::path(argv[7])
        );
    }

    if (mode == "--timeline-full-rejected" && argc == 8) {
        return run_timeline_full_output_rejection_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6]),
            std::filesystem::path(argv[7])
        );
    }

    if (mode == "--timeline-thumbnail-h264" && argc == 7) {
        return run_timeline_thumbnail_preroll_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6])
        );
    }

    if (mode == "--timeline-resize-h264" && argc == 7) {
        return run_timeline_resize_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6])
        );
    }

    if (mode == "--timeline-thumbnail-resize-h264" && argc == 7) {
        return run_timeline_thumbnail_resize_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6])
        );
    }

    return fail("Unknown mode or wrong argument count for utsure_core_subtitle_burn_in_tests.");
}
