#include "preview_frame_renderer_worker.hpp"

#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/subtitles/subtitle_frame_composer.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <QImage>
#include <QFile>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::size_t kPreviewWindowFrameCount = 24;

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::filesystem::path qstring_to_path(const QString &text) {
#ifdef _WIN32
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(text).constData());
#endif
}

QImage image_from_decoded_frame(const utsure::core::media::DecodedVideoFrame &video_frame) {
    if (video_frame.pixel_format != utsure::core::media::NormalizedVideoPixelFormat::rgba8 ||
        video_frame.planes.size() != 1 ||
        video_frame.width <= 0 ||
        video_frame.height <= 0) {
        throw std::runtime_error("The preview frame is not available in the expected rgba8 layout.");
    }

    const auto &plane = video_frame.planes.front();
    if (plane.line_stride_bytes < (video_frame.width * 4)) {
        throw std::runtime_error("The preview frame uses a truncated RGBA line stride.");
    }

    QImage image(video_frame.width, video_frame.height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        throw std::runtime_error("Failed to allocate the preview image surface.");
    }

    for (int row = 0; row < video_frame.height; ++row) {
        std::memcpy(
            image.scanLine(row),
            plane.bytes.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(plane.line_stride_bytes),
            static_cast<std::size_t>(video_frame.width * 4)
        );
    }

    return image;
}

QString format_error_detail(std::string_view message, std::string_view actionable_hint = {}) {
    QString detail = to_qstring(message).trimmed();
    const QString hint = to_qstring(actionable_hint).trimmed();
    if (!hint.isEmpty()) {
        if (!detail.isEmpty()) {
            detail.append('\n');
        }
        detail.append(hint);
    }

    return detail;
}

bool rationals_match(
    const utsure::core::media::Rational &left,
    const utsure::core::media::Rational &right
) {
    return left.numerator == right.numerator && left.denominator == right.denominator;
}

qint64 frame_coverage_end_us(const utsure::core::media::DecodedVideoFrame &frame) {
    return frame.timestamp.duration_microseconds.has_value()
        ? frame.timestamp.start_microseconds + *frame.timestamp.duration_microseconds
        : frame.timestamp.start_microseconds;
}

}  // namespace

PreviewFrameRendererWorker::PreviewFrameRendererWorker(QObject *parent) : QObject(parent) {}

PreviewFrameRendererWorker::~PreviewFrameRendererWorker() = default;

void PreviewFrameRendererWorker::render_request(const PreviewFrameRenderRequest &request) {
    try {
        if (request.source_path.trimmed().isEmpty()) {
            emit preview_failed(
                request.request_token,
                request.requested_time_us,
                "PREVIEW UNAVAILABLE",
                "Select a source video before requesting preview."
            );
            return;
        }

        const QString normalized_source_path = request.source_path.trimmed();
        if (!cached_preview_window_covers(normalized_source_path, request.requested_time_us)) {
            if (cached_source_path_ != normalized_source_path) {
                invalidate_preview_frame_cache();
                invalidate_subtitle_session();
            }

            const auto preview_window_result = utsure::core::media::MediaDecoder::decode_video_frame_window_at_time(
                qstring_to_path(normalized_source_path),
                request.requested_time_us,
                kPreviewWindowFrameCount
            );
            if (!preview_window_result.succeeded()) {
                emit preview_failed(
                    request.request_token,
                    request.requested_time_us,
                    "PREVIEW UNAVAILABLE",
                    format_error_detail(
                        preview_window_result.error->message,
                        preview_window_result.error->actionable_hint
                    )
                );
                return;
            }

            cached_source_path_ = normalized_source_path;
            cached_preview_frames_ = std::move(*preview_window_result.video_frames);
        }

        const auto *cached_preview_frame = select_cached_preview_frame(request.requested_time_us);
        if (cached_preview_frame == nullptr) {
            emit preview_failed(
                request.request_token,
                request.requested_time_us,
                "PREVIEW UNAVAILABLE",
                "The preview cache did not contain a frame for the requested time."
            );
            return;
        }

        auto preview_frame = *cached_preview_frame;

        if (request.subtitle_enabled && !request.subtitle_path.trimmed().isEmpty()) {
            ensure_subtitle_session(request, preview_frame);
            if (!subtitle_session_) {
                emit preview_failed(
                    request.request_token,
                    request.requested_time_us,
                    "PREVIEW UNAVAILABLE",
                    "The subtitle preview session could not be created."
                );
                return;
            }

            const auto compose_result = utsure::core::subtitles::compose_subtitles_into_frame(
                preview_frame,
                *subtitle_session_,
                preview_frame.timestamp.start_microseconds
            );
            if (!compose_result.succeeded()) {
                emit preview_failed(
                    request.request_token,
                    request.requested_time_us,
                    "PREVIEW UNAVAILABLE",
                    format_error_detail(
                        compose_result.error->message,
                        compose_result.error->actionable_hint
                    )
                );
                return;
            }
        } else {
            invalidate_subtitle_session();
        }

        emit preview_ready(
            request.request_token,
            request.requested_time_us,
            preview_frame.timestamp.start_microseconds,
            preview_frame.timestamp.duration_microseconds.value_or(0),
            image_from_decoded_frame(preview_frame)
        );
    } catch (const std::exception &exception) {
        emit preview_failed(
            request.request_token,
            request.requested_time_us,
            "PREVIEW UNAVAILABLE",
            to_qstring(exception.what())
        );
    }
}

void PreviewFrameRendererWorker::clear_cache() {
    invalidate_preview_frame_cache();
    invalidate_subtitle_session();
}

void PreviewFrameRendererWorker::invalidate_preview_frame_cache() {
    cached_source_path_.clear();
    cached_preview_frames_.clear();
}

void PreviewFrameRendererWorker::invalidate_subtitle_session() {
    cached_subtitle_path_.clear();
    cached_subtitle_format_hint_ = "auto";
    cached_subtitle_canvas_width_ = 0;
    cached_subtitle_canvas_height_ = 0;
    cached_subtitle_sample_aspect_ratio_.reset();
    subtitle_session_.reset();
}

bool PreviewFrameRendererWorker::cached_preview_window_covers(
    const QString &source_path,
    const qint64 requested_time_us
) const {
    if (cached_source_path_ != source_path || cached_preview_frames_.empty()) {
        return false;
    }

    const auto &first_frame = cached_preview_frames_.front();
    const auto &last_frame = cached_preview_frames_.back();
    return requested_time_us >= first_frame.timestamp.start_microseconds &&
        requested_time_us <= frame_coverage_end_us(last_frame);
}

const utsure::core::media::DecodedVideoFrame *PreviewFrameRendererWorker::select_cached_preview_frame(
    const qint64 requested_time_us
) const {
    if (cached_preview_frames_.empty()) {
        return nullptr;
    }

    const auto upper_bound = std::upper_bound(
        cached_preview_frames_.begin(),
        cached_preview_frames_.end(),
        requested_time_us,
        [](const qint64 timestamp_microseconds, const utsure::core::media::DecodedVideoFrame &frame) {
            return timestamp_microseconds < frame.timestamp.start_microseconds;
        }
    );

    if (upper_bound == cached_preview_frames_.begin()) {
        return &cached_preview_frames_.front();
    }

    const auto &previous_frame = *(upper_bound - 1);
    if (requested_time_us < frame_coverage_end_us(previous_frame)) {
        return &previous_frame;
    }

    if (upper_bound == cached_preview_frames_.end()) {
        return &cached_preview_frames_.back();
    }

    return &(*upper_bound);
}

void PreviewFrameRendererWorker::ensure_subtitle_session(
    const PreviewFrameRenderRequest &request,
    const utsure::core::media::DecodedVideoFrame &preview_frame
) {
    const QString normalized_subtitle_path = request.subtitle_path.trimmed();
    const QString normalized_format_hint = request.subtitle_format_hint.trimmed().isEmpty()
        ? QString("auto")
        : request.subtitle_format_hint.trimmed().toLower();

    if (subtitle_session_ &&
        cached_subtitle_path_ == normalized_subtitle_path &&
        cached_subtitle_format_hint_ == normalized_format_hint &&
        cached_subtitle_canvas_width_ == preview_frame.width &&
        cached_subtitle_canvas_height_ == preview_frame.height &&
        cached_subtitle_sample_aspect_ratio_.has_value() &&
        rationals_match(*cached_subtitle_sample_aspect_ratio_, preview_frame.sample_aspect_ratio)) {
        return;
    }

    if (!subtitle_renderer_) {
        subtitle_renderer_ = utsure::core::subtitles::create_default_subtitle_renderer();
    }
    if (!subtitle_renderer_) {
        throw std::runtime_error("The default libassmod-backed subtitle renderer is not available.");
    }

    auto session_result = subtitle_renderer_->create_session(utsure::core::subtitles::SubtitleRenderSessionCreateRequest{
        .subtitle_path = qstring_to_path(normalized_subtitle_path),
        .format_hint = normalized_format_hint.toUtf8().toStdString(),
        .canvas_width = preview_frame.width,
        .canvas_height = preview_frame.height,
        .sample_aspect_ratio = preview_frame.sample_aspect_ratio
    });
    if (!session_result.succeeded()) {
        throw std::runtime_error(
            session_result.error->message + " " + session_result.error->actionable_hint
        );
    }

    subtitle_session_ = std::move(session_result.session);
    cached_subtitle_path_ = normalized_subtitle_path;
    cached_subtitle_format_hint_ = normalized_format_hint;
    cached_subtitle_canvas_width_ = preview_frame.width;
    cached_subtitle_canvas_height_ = preview_frame.height;
    cached_subtitle_sample_aspect_ratio_ = preview_frame.sample_aspect_ratio;
}
