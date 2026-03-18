#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using utsure::core::media::Rational;
using utsure::core::subtitles::RenderedSubtitleFrame;
using utsure::core::subtitles::SubtitleBitmap;
using utsure::core::subtitles::SubtitleBitmapPixelFormat;
using utsure::core::subtitles::SubtitleRenderRequest;
using utsure::core::subtitles::SubtitleRenderResult;
using utsure::core::subtitles::SubtitleRenderSession;
using utsure::core::subtitles::SubtitleRenderSessionCreateRequest;
using utsure::core::subtitles::SubtitleRenderSessionResult;
using utsure::core::subtitles::SubtitleRenderer;
using utsure::core::subtitles::SubtitleRendererError;
using utsure::core::subtitles::to_string;

constexpr std::string_view kExpectedFlowReport =
    "session.subtitle_path=sample.ass\n"
    "session.format_hint=auto\n"
    "session.canvas=320x180\n"
    "session.sample_aspect_ratio=1/1\n"
    "request.timestamp_us=41667\n"
    "request.count=1\n"
    "output.timestamp_us=41667\n"
    "output.canvas=320x180\n"
    "output.bitmap_count=1\n"
    "output.bitmap[0].origin=24,132\n"
    "output.bitmap[0].size=128x32\n"
    "output.bitmap[0].pixel_format=rgba8_premultiplied\n"
    "output.bitmap[0].stride=512\n"
    "output.bitmap[0].bytes=16384";

constexpr std::string_view kExpectedErrorMessage =
    "Cannot create a subtitle render session because the subtitle source path is empty.";

constexpr std::string_view kExpectedErrorHint =
    "Provide a subtitle file path before requesting subtitle rendering.";

struct RecordingState final {
    std::optional<SubtitleRenderSessionCreateRequest> create_request{};
    std::vector<SubtitleRenderRequest> render_requests{};
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

class RecordingSubtitleRenderSession final : public SubtitleRenderSession {
public:
    RecordingSubtitleRenderSession(
        std::shared_ptr<RecordingState> state,
        SubtitleRenderSessionCreateRequest create_request
    )
        : state_(std::move(state)),
          create_request_(std::move(create_request)) {
    }

    [[nodiscard]] SubtitleRenderResult render(const SubtitleRenderRequest &request) noexcept override {
        state_->render_requests.push_back(request);

        const int bitmap_width = 128;
        const int bitmap_height = 32;
        const int bitmap_stride = bitmap_width * 4;
        SubtitleBitmap bitmap{
            .origin_x = 24,
            .origin_y = 132,
            .width = bitmap_width,
            .height = bitmap_height,
            .pixel_format = SubtitleBitmapPixelFormat::rgba8_premultiplied,
            .line_stride_bytes = bitmap_stride,
            .bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(bitmap_stride * bitmap_height), 0x80)
        };

        return SubtitleRenderResult{
            .rendered_frame = RenderedSubtitleFrame{
                .timestamp_microseconds = request.timestamp_microseconds,
                .canvas_width = create_request_.canvas_width,
                .canvas_height = create_request_.canvas_height,
                .bitmaps = {std::move(bitmap)}
            },
            .error = std::nullopt
        };
    }

private:
    std::shared_ptr<RecordingState> state_{};
    SubtitleRenderSessionCreateRequest create_request_{};
};

class RecordingSubtitleRenderer final : public SubtitleRenderer {
public:
    explicit RecordingSubtitleRenderer(std::shared_ptr<RecordingState> state)
        : state_(std::move(state)) {
    }

    [[nodiscard]] SubtitleRenderSessionResult create_session(
        const SubtitleRenderSessionCreateRequest &request
    ) noexcept override {
        if (request.subtitle_path.empty()) {
            return SubtitleRenderSessionResult{
                .session = nullptr,
                .error = SubtitleRendererError{
                    .subtitle_path = {},
                    .message = std::string(kExpectedErrorMessage),
                    .actionable_hint = std::string(kExpectedErrorHint)
                }
            };
        }

        state_->create_request = request;
        return SubtitleRenderSessionResult{
            .session = std::make_unique<RecordingSubtitleRenderSession>(state_, request),
            .error = std::nullopt
        };
    }

private:
    std::shared_ptr<RecordingState> state_{};
};

SubtitleRenderResult render_through_pipeline_boundary(
    SubtitleRenderer &renderer,
    const SubtitleRenderSessionCreateRequest &session_request,
    const SubtitleRenderRequest &render_request
) {
    SubtitleRenderSessionResult session_result = renderer.create_session(session_request);
    if (!session_result.succeeded()) {
        return SubtitleRenderResult{
            .rendered_frame = std::nullopt,
            .error = session_result.error
        };
    }

    return session_result.session->render(render_request);
}

int assert_rendered_frame_shape(const RenderedSubtitleFrame &rendered_frame) {
    if (rendered_frame.bitmaps.size() != 1) {
        return fail("Unexpected subtitle bitmap count.");
    }

    const auto &bitmap = rendered_frame.bitmaps.front();
    if (bitmap.pixel_format != SubtitleBitmapPixelFormat::rgba8_premultiplied) {
        return fail("Unexpected subtitle bitmap pixel format.");
    }

    if (bitmap.line_stride_bytes != (bitmap.width * 4)) {
        return fail("Unexpected subtitle bitmap stride.");
    }

    if (bitmap.bytes.size() != static_cast<std::size_t>(bitmap.line_stride_bytes * bitmap.height)) {
        return fail("Unexpected subtitle bitmap byte count.");
    }

    return 0;
}

std::string build_flow_report(
    const SubtitleRenderSessionCreateRequest &session_request,
    const RecordingState &recording_state,
    const RenderedSubtitleFrame &rendered_frame
) {
    const auto &bitmap = rendered_frame.bitmaps.front();

    std::string report;
    report += "session.subtitle_path=" + format_path_leaf(session_request.subtitle_path);
    report += "\nsession.format_hint=" + session_request.format_hint;
    report += "\nsession.canvas=" + std::to_string(session_request.canvas_width) + "x" +
              std::to_string(session_request.canvas_height);
    report += "\nsession.sample_aspect_ratio=" + format_rational(session_request.sample_aspect_ratio);
    report += "\nrequest.timestamp_us=" + std::to_string(recording_state.render_requests.front().timestamp_microseconds);
    report += "\nrequest.count=" + std::to_string(recording_state.render_requests.size());
    report += "\noutput.timestamp_us=" + std::to_string(rendered_frame.timestamp_microseconds);
    report += "\noutput.canvas=" + std::to_string(rendered_frame.canvas_width) + "x" +
              std::to_string(rendered_frame.canvas_height);
    report += "\noutput.bitmap_count=" + std::to_string(rendered_frame.bitmaps.size());
    report += "\noutput.bitmap[0].origin=" + std::to_string(bitmap.origin_x) + "," + std::to_string(bitmap.origin_y);
    report += "\noutput.bitmap[0].size=" + std::to_string(bitmap.width) + "x" + std::to_string(bitmap.height);
    report += "\noutput.bitmap[0].pixel_format=" + std::string(to_string(bitmap.pixel_format));
    report += "\noutput.bitmap[0].stride=" + std::to_string(bitmap.line_stride_bytes);
    report += "\noutput.bitmap[0].bytes=" + std::to_string(bitmap.bytes.size());
    return report;
}

int run_flow_assertion() {
    auto recording_state = std::make_shared<RecordingState>();
    RecordingSubtitleRenderer renderer(recording_state);

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = std::filesystem::path("sample.ass"),
        .format_hint = "auto",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    };

    const SubtitleRenderRequest render_request{
        .timestamp_microseconds = 41667
    };

    const SubtitleRenderResult render_result =
        render_through_pipeline_boundary(renderer, session_request, render_request);
    if (!render_result.succeeded()) {
        const std::string error_message =
            "Subtitle render flow failed unexpectedly: " +
            render_result.error->message +
            " Hint: " +
            render_result.error->actionable_hint;
        return fail(error_message);
    }

    if (!recording_state->create_request.has_value()) {
        return fail("Subtitle session creation was not recorded.");
    }

    if (recording_state->render_requests.size() != 1 ||
        recording_state->render_requests.front().timestamp_microseconds != render_request.timestamp_microseconds) {
        return fail("Subtitle render request timing was not forwarded through the abstraction.");
    }

    const auto shape_result = assert_rendered_frame_shape(*render_result.rendered_frame);
    if (shape_result != 0) {
        return shape_result;
    }

    const std::string actual_report = build_flow_report(
        *recording_state->create_request,
        *recording_state,
        *render_result.rendered_frame
    );
    std::cout << actual_report << '\n';

    if (actual_report != kExpectedFlowReport) {
        std::cerr << "Expected subtitle flow report:\n" << kExpectedFlowReport << "\n";
        std::cerr << "Actual subtitle flow report:\n" << actual_report << "\n";
        return 1;
    }

    return 0;
}

int run_error_assertion() {
    auto recording_state = std::make_shared<RecordingState>();
    RecordingSubtitleRenderer renderer(recording_state);

    const SubtitleRenderSessionCreateRequest session_request{
        .subtitle_path = {},
        .format_hint = "auto",
        .canvas_width = 320,
        .canvas_height = 180,
        .sample_aspect_ratio = Rational{1, 1}
    };

    const SubtitleRenderResult render_result = render_through_pipeline_boundary(
        renderer,
        session_request,
        SubtitleRenderRequest{.timestamp_microseconds = 0}
    );

    if (render_result.succeeded() || !render_result.error.has_value()) {
        return fail("Empty-path subtitle session creation unexpectedly succeeded.");
    }

    if (render_result.error->message != kExpectedErrorMessage) {
        std::cerr << "Expected error message:\n" << kExpectedErrorMessage << "\n";
        std::cerr << "Actual error message:\n" << render_result.error->message << "\n";
        return 1;
    }

    if (render_result.error->actionable_hint != kExpectedErrorHint) {
        std::cerr << "Expected actionable hint:\n" << kExpectedErrorHint << "\n";
        std::cerr << "Actual actionable hint:\n" << render_result.error->actionable_hint << "\n";
        return 1;
    }

    std::cout << render_result.error->message << '\n';
    std::cout << render_result.error->actionable_hint << '\n';
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return fail("Usage: utsure_core_subtitle_renderer_tests [--flow|--error]");
    }

    const std::string_view mode(argv[1]);
    if (mode == "--flow") {
        return run_flow_assertion();
    }

    if (mode == "--error") {
        return run_error_assertion();
    }

    return fail("Unknown mode. Use --flow or --error.");
}
