#include "subtitles/subtitle_burn_in.hpp"
#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/media/decoded_media.hpp"
#include "utsure/core/subtitles/subtitle_font_recovery.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using utsure::core::job::EncodeJobSubtitleSettings;
using utsure::core::media::DecodedMediaSource;
using utsure::core::media::DecodedVideoFrame;
using utsure::core::media::MediaTimestamp;
using utsure::core::media::NormalizedVideoPixelFormat;
using utsure::core::media::Rational;
using utsure::core::media::TimestampOrigin;
using utsure::core::media::VideoPlane;
using utsure::core::subtitles::PreparedSubtitleRenderSessionRequest;
using utsure::core::subtitles::RenderedSubtitleFrame;
using utsure::core::subtitles::SubtitleFontRecoveryOptions;
using utsure::core::subtitles::SubtitleFontRecoveryOutcome;
using utsure::core::subtitles::SubtitleRenderRequest;
using utsure::core::subtitles::SubtitleRenderResult;
using utsure::core::subtitles::SubtitleRenderSession;
using utsure::core::subtitles::SubtitleRenderSessionCreateRequest;
using utsure::core::subtitles::SubtitleRenderSessionResult;
using utsure::core::subtitles::SubtitleRenderer;
using utsure::core::subtitles::SubtitleRendererError;
using utsure::core::subtitles::burn_in::apply;
using utsure::core::subtitles::prepare_subtitle_render_session_request;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

struct TempDirectoryGuard final {
    explicit TempDirectoryGuard(std::filesystem::path value) : path(std::move(value)) {}

    ~TempDirectoryGuard() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path{};
};

std::filesystem::path make_temp_directory() {
    const auto unique_suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root =
        std::filesystem::temp_directory_path() / ("utsure-subtitle-font-recovery-tests-" + unique_suffix);
    std::filesystem::create_directories(root);
    return root;
}

void write_text_file(const std::filesystem::path &path, const std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
}

std::filesystem::path make_fontcollector_stub(const std::filesystem::path &root) {
#ifdef _WIN32
    const auto script_path = root / "fontcollector-stub.cmd";
    write_text_file(
        script_path,
        "@echo off\r\n"
        "setlocal EnableExtensions EnableDelayedExpansion\r\n"
        "set \"OUTPUT_DIR=\"\r\n"
        "set \"LOG_PATH=\"\r\n"
        ":parse\r\n"
        "if \"%~1\"==\"\" goto parsed\r\n"
        "if /I \"%~1\"==\"--output\" (\r\n"
        "  set \"OUTPUT_DIR=%~2\"\r\n"
        "  shift\r\n"
        "  shift\r\n"
        "  goto parse\r\n"
        ")\r\n"
        "if /I \"%~1\"==\"--logging\" (\r\n"
        "  set \"LOG_PATH=%~2\"\r\n"
        "  shift\r\n"
        "  shift\r\n"
        "  goto parse\r\n"
        ")\r\n"
        "shift\r\n"
        "goto parse\r\n"
        ":parsed\r\n"
        "if not \"%LOG_PATH%\"==\"\" echo fontcollector stub>\"%LOG_PATH%\"\r\n"
        "if /I \"%UTSURE_FONTCOLLECTOR_STUB_MODE%\"==\"success\" (\r\n"
        "  if not exist \"%OUTPUT_DIR%\" mkdir \"%OUTPUT_DIR%\"\r\n"
        "  >\"%OUTPUT_DIR%\\RecoveredA.ttf\" echo font-a\r\n"
        "  >\"%OUTPUT_DIR%\\RecoveredB.otf\" echo font-b\r\n"
        "  exit /b 0\r\n"
        ")\r\n"
        "if /I \"%UTSURE_FONTCOLLECTOR_STUB_MODE%\"==\"no-fonts\" exit /b 0\r\n"
        "if /I \"%UTSURE_FONTCOLLECTOR_STUB_MODE%\"==\"fail\" exit /b 7\r\n"
        "exit /b 0\r\n"
    );
    return script_path;
#else
    const auto script_path = root / "fontcollector-stub.sh";
    write_text_file(
        script_path,
        "#!/bin/sh\n"
        "OUTPUT_DIR=\"\"\n"
        "LOG_PATH=\"\"\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  case \"$1\" in\n"
        "    --output)\n"
        "      OUTPUT_DIR=\"$2\"\n"
        "      shift 2\n"
        "      ;;\n"
        "    --logging)\n"
        "      LOG_PATH=\"$2\"\n"
        "      shift 2\n"
        "      ;;\n"
        "    *)\n"
        "      shift\n"
        "      ;;\n"
        "  esac\n"
        "done\n"
        "if [ -n \"$LOG_PATH\" ]; then printf \"fontcollector stub\" > \"$LOG_PATH\"; fi\n"
        "case \"$UTSURE_FONTCOLLECTOR_STUB_MODE\" in\n"
        "  success)\n"
        "    mkdir -p \"$OUTPUT_DIR\"\n"
        "    printf \"font-a\" > \"$OUTPUT_DIR/RecoveredA.ttf\"\n"
        "    printf \"font-b\" > \"$OUTPUT_DIR/RecoveredB.otf\"\n"
        "    exit 0\n"
        "    ;;\n"
        "  no-fonts)\n"
        "    exit 0\n"
        "    ;;\n"
        "  fail)\n"
        "    exit 7\n"
        "    ;;\n"
        "esac\n"
        "exit 0\n"
    );
    std::filesystem::permissions(
        script_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add
    );
    return script_path;
#endif
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(std::string name, std::optional<std::string> value)
        : name_(std::move(name)),
          old_value_(read(name_)) {
        set(value);
    }

    ~ScopedEnvironmentVariable() {
        set(old_value_);
    }

private:
    static std::optional<std::string> read(const std::string &name) {
        const char *value = std::getenv(name.c_str());
        if (value == nullptr) {
            return std::nullopt;
        }

        return std::string(value);
    }

    void set(const std::optional<std::string> &value) const {
#ifdef _WIN32
        if (!value.has_value()) {
            _putenv_s(name_.c_str(), "");
            return;
        }

        _putenv_s(name_.c_str(), value->c_str());
#else
        if (!value.has_value()) {
            unsetenv(name_.c_str());
            return;
        }

        setenv(name_.c_str(), value->c_str(), 1);
#endif
    }

    std::string name_{};
    std::optional<std::string> old_value_{};
};

SubtitleRenderSessionCreateRequest make_session_request(const std::filesystem::path &subtitle_path) {
    return SubtitleRenderSessionCreateRequest{
        .subtitle_path = subtitle_path,
        .format_hint = "ass",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    };
}

DecodedMediaSource make_minimal_decoded_media_source() {
    DecodedMediaSource source{};
    source.normalization_policy.video_pixel_format = NormalizedVideoPixelFormat::rgba8;

    DecodedVideoFrame frame{};
    frame.frame_index = 0;
    frame.timestamp = MediaTimestamp{
        .source_time_base = Rational{1, 24},
        .source_pts = 0,
        .source_duration = 1,
        .origin = TimestampOrigin::stream_cursor,
        .start_microseconds = 0,
        .duration_microseconds = 41667
    };
    frame.width = 4;
    frame.height = 2;
    frame.sample_aspect_ratio = Rational{1, 1};
    frame.pixel_format = NormalizedVideoPixelFormat::rgba8;
    frame.planes.push_back(VideoPlane{
        .line_stride_bytes = 16,
        .visible_width = 4,
        .visible_height = 2,
        .bytes = std::vector<std::uint8_t>(32U, 0U)
    });
    source.video_frames.push_back(std::move(frame));
    return source;
}

struct RecordingState final {
    std::optional<SubtitleRenderSessionCreateRequest> create_request{};
    int render_request_count{0};
};

class RecordingSubtitleRenderSession final : public SubtitleRenderSession {
public:
    explicit RecordingSubtitleRenderSession(std::shared_ptr<RecordingState> state) : state_(std::move(state)) {}

    [[nodiscard]] SubtitleRenderResult render(const SubtitleRenderRequest &request) noexcept override {
        ++state_->render_request_count;
        return SubtitleRenderResult{
            .rendered_frame = RenderedSubtitleFrame{
                .timestamp_microseconds = request.timestamp_microseconds,
                .canvas_width = 4,
                .canvas_height = 2,
                .bitmaps = {}
            },
            .error = std::nullopt
        };
    }

private:
    std::shared_ptr<RecordingState> state_{};
};

class RecordingSubtitleRenderer final : public SubtitleRenderer {
public:
    explicit RecordingSubtitleRenderer(std::shared_ptr<RecordingState> state) : state_(std::move(state)) {}

    [[nodiscard]] SubtitleRenderSessionResult create_session(
        const SubtitleRenderSessionCreateRequest &request
    ) noexcept override {
        state_->create_request = request;
        return SubtitleRenderSessionResult{
            .session = std::make_unique<RecordingSubtitleRenderSession>(state_),
            .error = std::nullopt
        };
    }

private:
    std::shared_ptr<RecordingState> state_{};
};

int assert_successful_recovery_behavior(const std::filesystem::path &root, const std::filesystem::path &tool_path) {
    const ScopedEnvironmentVariable stub_mode("UTSURE_FONTCOLLECTOR_STUB_MODE", std::string("success"));
    const auto subtitle_path = root / "episode01.ass";
    write_text_file(subtitle_path, "[Script Info]\nTitle: episode01\n");

    const PreparedSubtitleRenderSessionRequest prepared_request = prepare_subtitle_render_session_request(
        make_session_request(subtitle_path),
        SubtitleFontRecoveryOptions{
            .fontcollector_executable_override = tool_path
        }
    );

    if (prepared_request.font_recovery_report.outcome != SubtitleFontRecoveryOutcome::recovered_fonts ||
        !prepared_request.font_recovery_report.attempted() ||
        !prepared_request.font_recovery_report.recovered_fonts_applied ||
        !prepared_request.session_request.font_search_directory.has_value() ||
        !prepared_request.font_recovery_artifacts ||
        prepared_request.font_recovery_report.recovered_font_files.size() != 2U) {
        return fail("Font recovery did not report a recovered-font success path.");
    }

    if (!std::filesystem::exists(*prepared_request.session_request.font_search_directory) ||
        prepared_request.font_recovery_report.message.find("RecoveredA.ttf") == std::string::npos ||
        prepared_request.font_recovery_report.message.find("will be applied") == std::string::npos) {
        return fail("Font recovery success reporting did not describe the applied recovered fonts.");
    }

    if (!std::filesystem::exists(prepared_request.font_recovery_report.tool_log_path)) {
        return fail("Font recovery success did not preserve the FontCollector log while the request is alive.");
    }

    std::cout << prepared_request.font_recovery_report.message << '\n';
    return 0;
}

int assert_no_fonts_behavior(const std::filesystem::path &root, const std::filesystem::path &tool_path) {
    const ScopedEnvironmentVariable stub_mode("UTSURE_FONTCOLLECTOR_STUB_MODE", std::string("no-fonts"));
    const auto subtitle_path = root / "episode02.ass";
    write_text_file(subtitle_path, "[Script Info]\nTitle: episode02\n");

    const PreparedSubtitleRenderSessionRequest prepared_request = prepare_subtitle_render_session_request(
        make_session_request(subtitle_path),
        SubtitleFontRecoveryOptions{
            .fontcollector_executable_override = tool_path
        }
    );

    if (prepared_request.font_recovery_report.outcome != SubtitleFontRecoveryOutcome::no_additional_fonts ||
        !prepared_request.font_recovery_report.attempted() ||
        prepared_request.font_recovery_report.recovered_fonts_applied ||
        prepared_request.session_request.font_search_directory.has_value() ||
        !prepared_request.font_recovery_artifacts ||
        !prepared_request.font_recovery_report.recovered_font_files.empty()) {
        return fail("Font recovery did not report the no-font-found path correctly.");
    }

    if (prepared_request.font_recovery_report.message.find("did not recover any additional font files") ==
        std::string::npos) {
        return fail("Font recovery no-font-found reporting was not explicit.");
    }

    std::cout << prepared_request.font_recovery_report.message << '\n';
    return 0;
}

int assert_tool_unavailable_behavior(const std::filesystem::path &root) {
    const auto subtitle_path = root / "episode03.ass";
    write_text_file(subtitle_path, "[Script Info]\nTitle: episode03\n");

    const PreparedSubtitleRenderSessionRequest prepared_request = prepare_subtitle_render_session_request(
        make_session_request(subtitle_path),
        SubtitleFontRecoveryOptions{
            .fontcollector_executable_override = root / "missing-fontcollector.exe"
        }
    );

    if (prepared_request.font_recovery_report.outcome != SubtitleFontRecoveryOutcome::tool_unavailable ||
        prepared_request.font_recovery_report.attempted() ||
        prepared_request.font_recovery_artifacts ||
        prepared_request.session_request.font_search_directory.has_value()) {
        return fail("Font recovery did not report the tool-unavailable path correctly.");
    }

    if (prepared_request.font_recovery_report.message.find("unavailable") == std::string::npos ||
        prepared_request.font_recovery_report.actionable_hint.find("UTSURE_FONTCOLLECTOR_PATH") == std::string::npos) {
        return fail("Font recovery tool-unavailable reporting did not explain the fallback state.");
    }

    std::cout << prepared_request.font_recovery_report.message << '\n';
    return 0;
}

int assert_burn_in_pipeline_applies_recovered_font_directory(
    const std::filesystem::path &root,
    const std::filesystem::path &tool_path
) {
    const ScopedEnvironmentVariable fontcollector_path(
        "UTSURE_FONTCOLLECTOR_PATH",
        tool_path.string()
    );
    const ScopedEnvironmentVariable stub_mode("UTSURE_FONTCOLLECTOR_STUB_MODE", std::string("success"));

    const auto subtitle_path = root / "episode04.ass";
    write_text_file(subtitle_path, "[Script Info]\nTitle: episode04\n");

    auto recording_state = std::make_shared<RecordingState>();
    RecordingSubtitleRenderer renderer(recording_state);
    const auto decoded_media_source = make_minimal_decoded_media_source();

    const auto burn_in_result = apply(
        decoded_media_source,
        renderer,
        EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        }
    );
    if (!burn_in_result.succeeded()) {
        return fail("Subtitle burn-in unexpectedly failed while testing font recovery integration.");
    }

    if (!recording_state->create_request.has_value() ||
        !recording_state->create_request->font_search_directory.has_value() ||
        !std::filesystem::exists(*recording_state->create_request->font_search_directory)) {
        return fail("Subtitle burn-in did not pass the recovered font directory into the renderer session request.");
    }

    if (recording_state->render_request_count != 1) {
        return fail("Subtitle burn-in did not render through the recording session as expected.");
    }

    std::cout << "burn_in.font_directory="
              << recording_state->create_request->font_search_directory->lexically_normal().string()
              << '\n';
    return 0;
}

}  // namespace

int main() {
    const auto root = make_temp_directory();
    const TempDirectoryGuard cleanup(root);
    const auto tool_path = make_fontcollector_stub(root);

    if (assert_successful_recovery_behavior(root, tool_path) != 0) {
        return 1;
    }

    if (assert_no_fonts_behavior(root, tool_path) != 0) {
        return 1;
    }

    if (assert_tool_unavailable_behavior(root) != 0) {
        return 1;
    }

    if (assert_burn_in_pipeline_applies_recovered_font_directory(root, tool_path) != 0) {
        return 1;
    }

    return 0;
}
