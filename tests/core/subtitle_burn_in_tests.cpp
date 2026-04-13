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
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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
using utsure::core::subtitles::SubtitleRenderRequest;
using utsure::core::subtitles::SubtitleRenderResult;
using utsure::core::subtitles::SubtitleRenderSessionCreateRequest;
using utsure::core::subtitles::create_default_subtitle_renderer;
using utsure::core::timeline::SubtitleTimingMode;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
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

std::string current_subtitle_bitmap_mode() {
    const char *value = std::getenv("UTSURE_SUBTITLE_BITMAP_MODE");
    if (value == nullptr || value[0] == '\0') {
        return "copied";
    }

    const auto normalized = lowercase_ascii(std::string(value));
    return (normalized == "direct" || normalized == "raw") ? "direct" : "copied";
}

std::string current_subtitle_composition_mode() {
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

int assert_observer_flow(
    const CollectingObserver &observer,
    const int expected_decode_steps
) {
    if (observer.progress_updates.empty()) {
        return fail("The subtitle burn-in observer did not receive any progress updates.");
    }

    const int expected_total_steps = 1 + expected_decode_steps + 1 + 1 + 1;
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

    if (subtitle_stage_count != 1) {
        return fail("The subtitle burn-in observer did not report the subtitle stage exactly once.");
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

    const std::size_t expected_subtitle_workers = expected_composition_mode == "worker_local"
        ? summary.streaming_runtime.video_processing_worker_count
        : 1U;
    if (summary.streaming_runtime.subtitle_processing_worker_count != expected_subtitle_workers) {
        return fail("The subtitle runtime summary reported an unexpected subtitle-worker count.");
    }

    const auto report = format_encode_job_report(summary);
    if (!observer_logs_contain_text(observer, std::string("subtitle bitmap mode ") + std::string(expected_bitmap_mode)) ||
        !observer_logs_contain_text(observer, std::string("composition mode ") + std::string(expected_composition_mode)) ||
        report.find("streaming.subtitle.bitmap_mode=" + std::string(expected_bitmap_mode)) == std::string::npos ||
        report.find("streaming.subtitle.composition_mode=" + std::string(expected_composition_mode)) == std::string::npos) {
        return fail("The subtitle runtime logs/report did not expose the active isolation mode.");
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
    const bool expect_opaque_color_variation = false
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
        .timestamp_microseconds = 41667
    });
    if (!visible_result.succeeded()) {
        const std::string error_message =
            "Visible subtitle render failed unexpectedly: " +
            visible_result.error->message +
            " Hint: " +
            visible_result.error->actionable_hint;
        return fail(error_message);
    }

    const SubtitleRenderResult hidden_result = session_result.session->render(SubtitleRenderRequest{
        .timestamp_microseconds = 500000
    });
    if (!hidden_result.succeeded()) {
        const std::string error_message =
            "Hidden subtitle render failed unexpectedly: " +
            hidden_result.error->message +
            " Hint: " +
            hidden_result.error->actionable_hint;
        return fail(error_message);
    }

    if (visible_result.rendered_frame->bitmaps.empty()) {
        return fail("Expected visible subtitle content at 41667 us.");
    }

    if (!hidden_result.rendered_frame->bitmaps.empty()) {
        return fail("Expected no subtitle content at 500000 us.");
    }

    if (expect_opaque_color_variation &&
        !has_opaque_color_variation(*visible_result.rendered_frame)) {
        return fail("Expected the RGBA gradient sample to contain multiple opaque colors in one rendered frame.");
    }

    std::cout << "session.subtitle_path=" << format_path_leaf(subtitle_path) << '\n';
    std::cout << "session.format_hint=ass\n";
    std::cout << "session.canvas=320x180\n";
    std::cout << "session.sample_aspect_ratio=" << format_rational(session_request.sample_aspect_ratio) << '\n';
    std::cout << "visible.timestamp_us=41667\n";
    std::cout << "visible.has_content=yes\n";
    if (expect_opaque_color_variation) {
        std::cout << "visible.opaque_color_variation=yes\n";
    }
    std::cout << "hidden.timestamp_us=500000\n";
    std::cout << "hidden.has_content=no\n";
    return 0;
}

int run_unsupported_img_render_assertion(const std::filesystem::path &subtitle_path) {
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
    if (session_result.succeeded() || !session_result.error.has_value()) {
        return fail("The unsupported libassmod img subtitle sample unexpectedly created a render session.");
    }

    if (session_result.error->message.find("\\img") == std::string::npos) {
        return fail("The unsupported libassmod img subtitle sample did not report an img-specific error.");
    }

    std::cout << session_result.error->message << '\n';
    std::cout << session_result.error->actionable_hint << '\n';
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

    std::cout << build_validation_report(
        *burned_job_result.encode_job_summary,
        *plain_output_decode.decoded_media_source,
        *burned_output_decode.decoded_media_source
    ) << '\n';
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

    if (!frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("Trimmed subtitle burn-in did not alter the first kept output frame.");
    }

    const auto report = format_encode_job_report(summary);
    if (!contains_text(report, "job.input.main_trim.in_us=250000") ||
        !contains_text(report, "job.input.main_trim.out_us=1250000")) {
        return fail("The trimmed subtitle burn-in report did not include the trim range.");
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

    // Encoder prediction can let changed main-segment frames influence later compressed frames.
    // Keep this assertion anchored to the first intro frame, which should remain outside subtitle scope.
    if (frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("Timeline subtitle burn-in altered the first intro frame unexpectedly.");
    }

    if (!any_frame_changed_in_range(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 24U, 48U)) {
        return fail("Timeline subtitle burn-in did not alter any main-segment frames.");
    }

    if (!frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 24U)) {
        return fail("Timeline subtitle burn-in did not change the first main-segment frame.");
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
    std::cout << "timeline.outro.scope=not_asserted_after_encode\n";
    std::cout << "timeline.subtitled_frames=" << summary.subtitled_video_frame_count << '\n';
    return 0;
}

int run_timeline_full_output_burn_in_assertion(
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

    const EncodeJobResult plain_job_result = EncodeJobRunner::run(plain_job);
    const EncodeJobResult burned_job_result = EncodeJobRunner::run(burned_job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });
    if (!plain_job_result.succeeded() || !burned_job_result.succeeded()) {
        return fail("Full-output timeline subtitle burn-in jobs failed unexpectedly.");
    }

    const auto &summary = *burned_job_result.encode_job_summary;
    if (summary.timeline_summary.segments.size() != 3 ||
        !summary.timeline_summary.segments[0].subtitles_enabled ||
        !summary.timeline_summary.segments[1].subtitles_enabled ||
        !summary.timeline_summary.segments[2].subtitles_enabled) {
        return fail("Full-output timeline subtitle scope did not enable every segment.");
    }

    if (summary.subtitled_video_frame_count != 11) {
        return fail("Unexpected count of subtitled video frames for the full-output timeline burn-in path.");
    }

    if (summary.streaming_runtime.subtitle_compose_microseconds == 0U) {
        return fail("Full-output subtitle burn-in jobs should report non-zero subtitle composition time.");
    }

    const MediaDecodeResult plain_output_decode = MediaDecoder::decode(plain_output_path);
    const MediaDecodeResult burned_output_decode = MediaDecoder::decode(burned_output_path);
    if (!plain_output_decode.succeeded() || !burned_output_decode.succeeded()) {
        return fail("Full-output timeline subtitle output decode failed unexpectedly.");
    }

    if (assert_decoded_output(*plain_output_decode.decoded_media_source, 96U, true) != 0 ||
        assert_decoded_output(*burned_output_decode.decoded_media_source, 96U, true) != 0) {
        return 1;
    }

    if (!frame_changed(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U)) {
        return fail("Full-output timeline subtitle burn-in did not change the first intro frame.");
    }

    if (!any_frame_changed_in_range(*plain_output_decode.decoded_media_source, *burned_output_decode.decoded_media_source, 0U, 11U)) {
        return fail("Full-output timeline subtitle burn-in did not alter the expected opening frame range.");
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

    std::cout << "timeline.full_output.frame0.changed=yes\n";
    std::cout << "timeline.full_output.subtitled_frames=" << summary.subtitled_video_frame_count << '\n';
    std::cout << "timeline.full_output.segment_scope=intro:yes,main:yes,outro:yes\n";
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

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 3) {
        return fail(
            "Usage: utsure_core_subtitle_burn_in_tests "
            "[--render <subtitle>|--render-gradient <subtitle>|--render-unsupported-img <subtitle>|"
            "--h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--h265 <input> <subtitle> <plain-output> <burned-output>|"
            "--trimmed-h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--stress-h264 <input> <subtitle> <plain-output> <burned-output>|"
            "--timeline-h264 <intro> <main> <outro> <subtitle> <plain-output> <burned-output>|"
            "--timeline-full-h264 <intro> <main> <outro> <subtitle> <plain-output> <burned-output>]"
        );
    }

    const std::string_view mode(argv[1]);

    if (mode == "--render" && argc == 3) {
        return run_render_assertion(std::filesystem::path(argv[2]));
    }

    if (mode == "--render-gradient" && argc == 3) {
        return run_render_assertion(std::filesystem::path(argv[2]), true);
    }

    if (mode == "--render-unsupported-img" && argc == 3) {
        return run_unsupported_img_render_assertion(std::filesystem::path(argv[2]));
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

    if (mode == "--timeline-full-h264" && argc == 8) {
        return run_timeline_full_output_burn_in_assertion(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
            std::filesystem::path(argv[4]),
            std::filesystem::path(argv[5]),
            std::filesystem::path(argv[6]),
            std::filesystem::path(argv[7])
        );
    }

    return fail("Unknown mode or wrong argument count for utsure_core_subtitle_burn_in_tests.");
}
