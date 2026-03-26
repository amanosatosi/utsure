#include "preview_frame_renderer_worker.hpp"

#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/subtitles/subtitle_frame_composer.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <QImage>
#include <QFile>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace {

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

const utsure::core::media::DecodedVideoFrame &select_preview_frame(
    const utsure::core::media::DecodedMediaSource &decoded_media_source,
    const qint64 requested_time_us
) {
    if (decoded_media_source.video_frames.empty()) {
        throw std::runtime_error("The selected source did not decode into any video frames.");
    }

    const auto upper_bound = std::upper_bound(
        decoded_media_source.video_frames.begin(),
        decoded_media_source.video_frames.end(),
        requested_time_us,
        [](const qint64 timestamp_microseconds, const utsure::core::media::DecodedVideoFrame &frame) {
            return timestamp_microseconds < frame.timestamp.start_microseconds;
        }
    );

    if (upper_bound == decoded_media_source.video_frames.begin()) {
        return decoded_media_source.video_frames.front();
    }

    if (upper_bound == decoded_media_source.video_frames.end()) {
        return decoded_media_source.video_frames.back();
    }

    const auto &next_frame = *upper_bound;
    const auto &previous_frame = *(upper_bound - 1);
    const auto previous_distance = std::llabs(requested_time_us - previous_frame.timestamp.start_microseconds);
    const auto next_distance = std::llabs(next_frame.timestamp.start_microseconds - requested_time_us);
    return previous_distance <= next_distance ? previous_frame : next_frame;
}

bool rationals_match(
    const utsure::core::media::Rational &left,
    const utsure::core::media::Rational &right
) {
    return left.numerator == right.numerator && left.denominator == right.denominator;
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

        ensure_decoded_source(request);
        if (!cached_decoded_source_.has_value()) {
            emit preview_failed(
                request.request_token,
                request.requested_time_us,
                "PREVIEW UNAVAILABLE",
                "The preview decoder did not return any media data."
            );
            return;
        }

        const auto &selected_frame = select_preview_frame(*cached_decoded_source_, request.requested_time_us);
        auto preview_frame = selected_frame;

        if (request.subtitle_enabled && !request.subtitle_path.trimmed().isEmpty()) {
            ensure_subtitle_session(request, *cached_decoded_source_);
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
                request.requested_time_us
            );
            if (!compose_result.succeeded()) {
                emit preview_failed(
                    request.request_token,
                    request.requested_time_us,
                    "PREVIEW UNAVAILABLE",
                    to_qstring(compose_result.error->message + " " + compose_result.error->actionable_hint)
                );
                return;
            }
        } else {
            invalidate_subtitle_session();
        }

        emit preview_ready(
            request.request_token,
            request.requested_time_us,
            selected_frame.timestamp.start_microseconds,
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
    cached_source_path_.clear();
    cached_decoded_source_.reset();
    invalidate_subtitle_session();
}

void PreviewFrameRendererWorker::invalidate_subtitle_session() {
    cached_subtitle_path_.clear();
    cached_subtitle_format_hint_ = "auto";
    cached_subtitle_canvas_width_ = 0;
    cached_subtitle_canvas_height_ = 0;
    cached_subtitle_sample_aspect_ratio_.reset();
    subtitle_session_.reset();
}

void PreviewFrameRendererWorker::ensure_decoded_source(const PreviewFrameRenderRequest &request) {
    const QString normalized_source_path = request.source_path.trimmed();
    if (cached_decoded_source_.has_value() && cached_source_path_ == normalized_source_path) {
        return;
    }

    invalidate_subtitle_session();
    cached_decoded_source_.reset();
    cached_source_path_.clear();

    const auto decode_result = utsure::core::media::MediaDecoder::decode(
        qstring_to_path(normalized_source_path),
        {},
        utsure::core::media::DecodeStreamSelection{
            .decode_video = true,
            .decode_audio = false
        }
    );
    if (!decode_result.succeeded()) {
        throw std::runtime_error(
            decode_result.error->message + " " + decode_result.error->actionable_hint
        );
    }

    cached_source_path_ = normalized_source_path;
    cached_decoded_source_ = std::move(*decode_result.decoded_media_source);
}

void PreviewFrameRendererWorker::ensure_subtitle_session(
    const PreviewFrameRenderRequest &request,
    const utsure::core::media::DecodedMediaSource &decoded_media_source
) {
    if (decoded_media_source.video_frames.empty()) {
        throw std::runtime_error("Create the preview decode before requesting subtitle preview.");
    }

    const auto &first_frame = decoded_media_source.video_frames.front();
    const QString normalized_subtitle_path = request.subtitle_path.trimmed();
    const QString normalized_format_hint = request.subtitle_format_hint.trimmed().isEmpty()
        ? QString("auto")
        : request.subtitle_format_hint.trimmed().toLower();

    if (subtitle_session_ &&
        cached_subtitle_path_ == normalized_subtitle_path &&
        cached_subtitle_format_hint_ == normalized_format_hint &&
        cached_subtitle_canvas_width_ == first_frame.width &&
        cached_subtitle_canvas_height_ == first_frame.height &&
        cached_subtitle_sample_aspect_ratio_.has_value() &&
        rationals_match(*cached_subtitle_sample_aspect_ratio_, first_frame.sample_aspect_ratio)) {
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
        .canvas_width = first_frame.width,
        .canvas_height = first_frame.height,
        .sample_aspect_ratio = first_frame.sample_aspect_ratio
    });
    if (!session_result.succeeded()) {
        throw std::runtime_error(
            session_result.error->message + " " + session_result.error->actionable_hint
        );
    }

    subtitle_session_ = std::move(session_result.session);
    cached_subtitle_path_ = normalized_subtitle_path;
    cached_subtitle_format_hint_ = normalized_format_hint;
    cached_subtitle_canvas_width_ = first_frame.width;
    cached_subtitle_canvas_height_ = first_frame.height;
    cached_subtitle_sample_aspect_ratio_ = first_frame.sample_aspect_ratio;
}
