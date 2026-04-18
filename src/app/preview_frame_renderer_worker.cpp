#include "preview_frame_renderer_worker.hpp"

#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/media/preview_trim.hpp"
#include "utsure/core/subtitles/subtitle_frame_composer.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <QImage>
#include <QFile>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t kInteractivePreviewWindowFrameCount = 8;
constexpr std::size_t kInteractiveTrimmedPreviewWindowFrameCount = 8;
constexpr std::size_t kPlaybackStartupPreviewWindowFrameCount = 24;
constexpr std::size_t kPlaybackPreviewWindowFrameCount = 48;
constexpr std::size_t kRetainedPreviewFrameCount = 192;
constexpr int kPreviewDecodeMaxWidth = 960;
constexpr int kPreviewDecodeMaxHeight = 540;
constexpr qint64 kSequentialPreviewRefillToleranceUs = 1000000;
constexpr std::size_t kSequentialPreviewPrefetchLeadFrames = 24;

Q_LOGGING_CATEGORY(previewWorkerLog, "utsure.preview.worker")

struct CachedPreviewWindowSummary final {
    bool has_frames{false};
    std::size_t frame_count{0};
    qint64 start_us{0};
    qint64 end_us{0};
};

struct PrefetchTaskResult final {
    quint64 generation{0};
    QString source_path{};
    qint64 window_start_us{-1};
    std::vector<utsure::core::media::DecodedVideoFrame> frames{};
    std::unique_ptr<utsure::core::media::VideoPreviewSession> session{};
    bool reused_existing_session{false};
    QString session_diagnostics{};
    QString decode_diagnostics{};
    QString error_message{};
};

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

qint64 steady_clock_now_microseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               SteadyClock::now().time_since_epoch()
           )
        .count();
}

QString format_elapsed_milliseconds(const qint64 elapsed_microseconds) {
    return QString::number(static_cast<double>(elapsed_microseconds) / 1000.0, 'f', 2) + "ms";
}

utsure::core::media::DecodeNormalizationPolicy preview_decode_normalization_policy() {
    utsure::core::media::DecodeNormalizationPolicy normalization_policy{};
    normalization_policy.video_max_width = kPreviewDecodeMaxWidth;
    normalization_policy.video_max_height = kPreviewDecodeMaxHeight;
    return normalization_policy;
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

QString format_time_us(const qint64 microseconds) {
    const auto total_milliseconds = std::max<qint64>(0, microseconds / 1000);
    const auto milliseconds = total_milliseconds % 1000;
    const auto total_seconds = total_milliseconds / 1000;
    const auto seconds = total_seconds % 60;
    const auto minutes = (total_seconds / 60) % 60;
    const auto hours = total_seconds / 3600;

    return QString("%1:%2:%3.%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(milliseconds, 3, 10, QChar('0'));
}

QString bool_text(const bool value) {
    return value ? "true" : "false";
}

CachedPreviewWindowSummary summarize_cached_preview_window(
    const std::vector<utsure::core::media::DecodedVideoFrame> &frames
) {
    if (frames.empty()) {
        return {};
    }

    return CachedPreviewWindowSummary{
        .has_frames = true,
        .frame_count = frames.size(),
        .start_us = frames.front().timestamp.start_microseconds,
        .end_us = frame_coverage_end_us(frames.back())
    };
}

QString format_cached_preview_window_summary(const CachedPreviewWindowSummary &summary) {
    if (!summary.has_frames) {
        return "empty";
    }

    return QString("frames=%1 start=%2 (%3) end=%4 (%5)")
        .arg(static_cast<qulonglong>(summary.frame_count))
        .arg(summary.start_us)
        .arg(format_time_us(summary.start_us))
        .arg(summary.end_us)
        .arg(format_time_us(summary.end_us));
}

void append_preview_frames_with_retention(
    std::vector<utsure::core::media::DecodedVideoFrame> &cached_frames,
    std::vector<utsure::core::media::DecodedVideoFrame> new_frames
) {
    cached_frames.insert(
        cached_frames.end(),
        std::make_move_iterator(new_frames.begin()),
        std::make_move_iterator(new_frames.end())
    );

    if (cached_frames.size() <= kRetainedPreviewFrameCount) {
        return;
    }

    const auto frames_to_discard = cached_frames.size() - kRetainedPreviewFrameCount;
    cached_frames.erase(
        cached_frames.begin(),
        cached_frames.begin() + static_cast<std::ptrdiff_t>(frames_to_discard)
    );
}

}  // namespace

PreviewFrameRendererWorker::PreviewFrameRendererWorker(
    std::atomic<quint64> *latest_request_generation,
    QObject *parent
) : QObject(parent),
    latest_request_generation_(latest_request_generation) {}

PreviewFrameRendererWorker::~PreviewFrameRendererWorker() = default;

void PreviewFrameRendererWorker::render_request(const PreviewFrameRenderRequest &request) {
    try {
        const qint64 render_started_us = steady_clock_now_microseconds();
        const qint64 queue_wait_us = std::max<qint64>(0, render_started_us - request.enqueued_steady_us);
        const auto trim_range = utsure::core::media::normalize_preview_trim_range(
            request.trim_in_us,
            request.trim_out_us
        );
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
        const auto cache_summary_before = summarize_cached_preview_window(cached_preview_frames_);
        const bool cache_covers_request =
            cached_trim_in_us_ == trim_range.trim_in_microseconds &&
            cached_trim_out_us_ == trim_range.trim_out_microseconds &&
            cached_preview_window_covers(normalized_source_path, request.requested_time_us);
        qCInfo(previewWorkerLog).noquote()
            << QString(
                   "render_request token=%1 generation=%2 requested=%3 (%4) trim_in=%5 trim_out=%6 playback_active=%7 queue_wait=%8 cache=%9 covers=%10"
               )
                   .arg(request.request_token)
                   .arg(request.dispatch_generation)
                   .arg(request.requested_time_us)
                   .arg(format_time_us(request.requested_time_us))
                   .arg(trim_range.trim_in_microseconds)
                   .arg(trim_range.trim_out_microseconds.has_value() ? QString::number(*trim_range.trim_out_microseconds) : QString("none"))
                   .arg(bool_text(request.playback_active))
                   .arg(format_elapsed_milliseconds(queue_wait_us))
                   .arg(format_cached_preview_window_summary(cache_summary_before))
                   .arg(bool_text(cache_covers_request));

        if (request_is_superseded(request)) {
            qCInfo(previewWorkerLog).noquote()
                << QString("render_request skipped_superseded_before_work token=%1 generation=%2")
                       .arg(request.request_token)
                       .arg(request.dispatch_generation);
            emit preview_skipped(request.request_token, request.requested_time_us);
            return;
        }

        if (!cache_covers_request) {
            const bool source_context_changed = cached_source_path_ != normalized_source_path;
            const bool trim_context_changed =
                cached_trim_in_us_ != trim_range.trim_in_microseconds ||
                cached_trim_out_us_ != trim_range.trim_out_microseconds;
            if (source_context_changed || trim_context_changed) {
                qCInfo(previewWorkerLog).noquote()
                    << QString(
                           "render_request context_changed old_source='%1' new_source='%2' old_trim_in=%3 new_trim_in=%4 old_trim_out=%5 new_trim_out=%6 resetting cache"
                       )
                           .arg(cached_source_path_, normalized_source_path)
                           .arg(cached_trim_in_us_)
                           .arg(trim_range.trim_in_microseconds)
                           .arg(cached_trim_out_us_.has_value() ? QString::number(*cached_trim_out_us_) : QString("none"))
                           .arg(trim_range.trim_out_microseconds.has_value()
                                    ? QString::number(*trim_range.trim_out_microseconds)
                                    : QString("none"));
                invalidate_preview_frame_cache();
                if (source_context_changed) {
                    invalidate_subtitle_session();
                }
            }

            if (!preview_session_) {
                const qint64 session_started_us = steady_clock_now_microseconds();
                auto session_result = utsure::core::media::MediaDecoder::create_video_preview_session(
                    qstring_to_path(normalized_source_path),
                    preview_decode_normalization_policy()
                );
                const qint64 session_elapsed_us = steady_clock_now_microseconds() - session_started_us;
                if (!session_result.diagnostics.empty()) {
                    qCInfo(previewWorkerLog).noquote()
                        << QString("render_request session_create diagnostics=%1")
                               .arg(to_qstring(session_result.diagnostics));
                }
                if (!session_result.succeeded()) {
                    emit preview_failed(
                        request.request_token,
                        request.requested_time_us,
                        "PREVIEW UNAVAILABLE",
                        format_error_detail(
                            session_result.error->message,
                            session_result.error->actionable_hint
                        )
                    );
                    return;
                }

                preview_session_ = std::move(session_result.session);
                qCInfo(previewWorkerLog).noquote()
                    << QString("render_request created preview session for '%1' elapsed=%2")
                           .arg(normalized_source_path)
                           .arg(format_elapsed_milliseconds(session_elapsed_us));
            }

            if (request_is_superseded(request)) {
                qCInfo(previewWorkerLog).noquote()
                    << QString("render_request skipped_after_session token=%1 generation=%2")
                           .arg(request.request_token)
                           .arg(request.dispatch_generation);
                emit preview_skipped(request.request_token, request.requested_time_us);
                return;
            }

            const bool use_sequential_refill = should_decode_next_preview_window(request);
            const std::size_t refill_frame_count = request.playback_active
                ? (use_sequential_refill ? kPlaybackPreviewWindowFrameCount : kPlaybackStartupPreviewWindowFrameCount)
                : (trim_range.has_trim() ? kInteractiveTrimmedPreviewWindowFrameCount : kInteractivePreviewWindowFrameCount);
            const qint64 decode_request_time_us =
                utsure::core::media::effective_trimmed_preview_frame_time(request.requested_time_us, trim_range);
            invalidate_prefetch_state();
            qCInfo(previewWorkerLog).noquote()
                << QString("render_request refill_strategy requested=%1 (%2) decode_request=%3 (%4) sequential=%5 frames=%6 method=%7")
                       .arg(request.requested_time_us)
                       .arg(format_time_us(request.requested_time_us))
                       .arg(decode_request_time_us)
                       .arg(format_time_us(decode_request_time_us))
                       .arg(bool_text(use_sequential_refill))
                       .arg(static_cast<qulonglong>(refill_frame_count))
                       .arg(use_sequential_refill ? "decode_next_window" : "seek_and_decode_window_at_time");

            const qint64 decode_started_us = steady_clock_now_microseconds();
            auto preview_window_result = use_sequential_refill
                ? preview_session_->decode_next_window(refill_frame_count)
                : preview_session_->seek_and_decode_window_at_time(
                    decode_request_time_us,
                    refill_frame_count
                );
            const qint64 decode_elapsed_us = steady_clock_now_microseconds() - decode_started_us;
            if (!preview_window_result.diagnostics.empty()) {
                qCInfo(previewWorkerLog).noquote()
                    << QString("render_request decode diagnostics=%1")
                           .arg(to_qstring(preview_window_result.diagnostics));
            }
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

            if (request_is_superseded(request)) {
                qCInfo(previewWorkerLog).noquote()
                    << QString("render_request skipped_after_decode token=%1 generation=%2 elapsed=%3")
                           .arg(request.request_token)
                           .arg(request.dispatch_generation)
                           .arg(format_elapsed_milliseconds(decode_elapsed_us));
                emit preview_skipped(request.request_token, request.requested_time_us);
                return;
            }

            cached_source_path_ = normalized_source_path;
            cached_trim_in_us_ = trim_range.trim_in_microseconds;
            cached_trim_out_us_ = trim_range.trim_out_microseconds;
            auto new_preview_frames = utsure::core::media::trim_preview_video_frames(
                std::move(*preview_window_result.video_frames),
                trim_range
            );
            const auto new_window_summary = summarize_cached_preview_window(new_preview_frames);
            qCInfo(previewWorkerLog).noquote()
                << QString("render_request refill_result method=%1 returned=%2 elapsed=%3 window=%4")
                       .arg(use_sequential_refill ? "decode_next_window" : "seek_and_decode_window_at_time")
                       .arg(static_cast<qulonglong>(new_preview_frames.size()))
                       .arg(format_elapsed_milliseconds(decode_elapsed_us))
                       .arg(format_cached_preview_window_summary(new_window_summary));

            if (use_sequential_refill && !cached_preview_frames_.empty()) {
                append_preview_frames_with_retention(cached_preview_frames_, std::move(new_preview_frames));
            } else {
                cached_preview_frames_ = std::move(new_preview_frames);
            }

            qCInfo(previewWorkerLog).noquote()
                << QString("render_request cache_updated mode=%1 cache=%2")
                       .arg(use_sequential_refill ? "append" : "replace")
                       .arg(format_cached_preview_window_summary(summarize_cached_preview_window(cached_preview_frames_)));
        }

        const auto *cached_preview_frame = utsure::core::media::select_trimmed_preview_frame(
            cached_preview_frames_,
            request.requested_time_us,
            trim_range
        );
        if (cached_preview_frame == nullptr) {
            qCInfo(previewWorkerLog).noquote()
                << QString("render_request selection_failed requested=%1 (%2) cache=%3")
                       .arg(request.requested_time_us)
                       .arg(format_time_us(request.requested_time_us))
                       .arg(format_cached_preview_window_summary(summarize_cached_preview_window(cached_preview_frames_)));
            emit preview_failed(
                request.request_token,
                request.requested_time_us,
                "PREVIEW UNAVAILABLE",
                "The preview cache did not contain a frame for the requested time."
            );
            return;
        }

        qCInfo(previewWorkerLog).noquote()
            << QString("render_request selection_ready requested=%1 (%2) frame_time=%3 (%4) frame_duration=%5")
                   .arg(request.requested_time_us)
                   .arg(format_time_us(request.requested_time_us))
                   .arg(cached_preview_frame->timestamp.start_microseconds)
                   .arg(format_time_us(cached_preview_frame->timestamp.start_microseconds))
                   .arg(cached_preview_frame->timestamp.duration_microseconds.value_or(0));

        if (request_is_superseded(request)) {
            qCInfo(previewWorkerLog).noquote()
                << QString("render_request skipped_before_subtitles token=%1 generation=%2")
                       .arg(request.request_token)
                       .arg(request.dispatch_generation);
            emit preview_skipped(request.request_token, request.requested_time_us);
            return;
        }

        const utsure::core::media::DecodedVideoFrame *selected_preview_frame = cached_preview_frame;
        std::optional<utsure::core::media::DecodedVideoFrame> composed_preview_frame{};
        qint64 subtitle_session_elapsed_us = 0;
        qint64 subtitle_compose_elapsed_us = 0;

        if (request.subtitle_enabled && !request.subtitle_path.trimmed().isEmpty()) {
            composed_preview_frame = *cached_preview_frame;
            const qint64 subtitle_session_started_us = steady_clock_now_microseconds();
            ensure_subtitle_session(request, *composed_preview_frame);
            subtitle_session_elapsed_us = steady_clock_now_microseconds() - subtitle_session_started_us;
            if (!subtitle_session_) {
                emit preview_failed(
                    request.request_token,
                    request.requested_time_us,
                    "PREVIEW UNAVAILABLE",
                    "The subtitle preview session could not be created."
                );
                return;
            }

            if (request_is_superseded(request)) {
                qCInfo(previewWorkerLog).noquote()
                    << QString("render_request skipped_before_subtitle_compose token=%1 generation=%2")
                           .arg(request.request_token)
                           .arg(request.dispatch_generation);
                emit preview_skipped(request.request_token, request.requested_time_us);
                return;
            }

            const qint64 subtitle_compose_started_us = steady_clock_now_microseconds();
            const auto compose_result = utsure::core::subtitles::compose_subtitles_into_frame(
                *composed_preview_frame,
                *subtitle_session_,
                composed_preview_frame->timestamp.start_microseconds
            );
            subtitle_compose_elapsed_us = steady_clock_now_microseconds() - subtitle_compose_started_us;
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

            selected_preview_frame = &*composed_preview_frame;
        } else {
            invalidate_subtitle_session();
        }

        if (request_is_superseded(request)) {
            qCInfo(previewWorkerLog).noquote()
                << QString("render_request skipped_before_image token=%1 generation=%2")
                       .arg(request.request_token)
                       .arg(request.dispatch_generation);
            emit preview_skipped(request.request_token, request.requested_time_us);
            return;
        }

        const qint64 image_conversion_started_us = steady_clock_now_microseconds();
        auto preview_image = image_from_decoded_frame(*selected_preview_frame);
        const qint64 image_conversion_elapsed_us = steady_clock_now_microseconds() - image_conversion_started_us;
        const qint64 total_elapsed_us = steady_clock_now_microseconds() - render_started_us;
        qCInfo(previewWorkerLog).noquote()
            << QString(
                   "render_request ready token=%1 generation=%2 subtitle_session=%3 subtitle_compose=%4 image_copy=%5 total=%6"
               )
                   .arg(request.request_token)
                   .arg(request.dispatch_generation)
                   .arg(format_elapsed_milliseconds(subtitle_session_elapsed_us))
                   .arg(format_elapsed_milliseconds(subtitle_compose_elapsed_us))
                   .arg(format_elapsed_milliseconds(image_conversion_elapsed_us))
                   .arg(format_elapsed_milliseconds(total_elapsed_us));

        emit preview_ready(
            request.request_token,
            request.requested_time_us,
            selected_preview_frame->timestamp.start_microseconds,
            selected_preview_frame->timestamp.duration_microseconds.value_or(0),
            std::move(preview_image)
        );

        maybe_start_sequential_prefetch(request);
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
    qCInfo(previewWorkerLog) << "clear_cache requested";
    invalidate_preview_frame_cache();
    invalidate_subtitle_session();
}

void PreviewFrameRendererWorker::invalidate_prefetch_state(const bool advance_generation) {
    if (advance_generation) {
        ++preview_cache_generation_;
    }

    qCInfo(previewWorkerLog).noquote()
        << QString("invalidate_prefetch_state generation=%1 in_progress=%2 window_start=%3 source='%4'")
               .arg(preview_cache_generation_)
               .arg(bool_text(prefetch_in_progress_))
               .arg(prefetch_window_start_us_)
               .arg(prefetch_source_path_);
    prefetch_in_progress_ = false;
    prefetch_window_start_us_ = -1;
    blocked_prefetch_window_start_us_ = -1;
    prefetch_source_path_.clear();
}

void PreviewFrameRendererWorker::invalidate_preview_frame_cache() {
    invalidate_prefetch_state();
    qCInfo(previewWorkerLog).noquote()
        << QString("invalidate_preview_frame_cache cache=%1")
               .arg(format_cached_preview_window_summary(summarize_cached_preview_window(cached_preview_frames_)));
    cached_source_path_.clear();
    cached_preview_frames_.clear();
    cached_trim_in_us_ = 0;
    cached_trim_out_us_.reset();
    preview_session_.reset();
    prefetch_session_.reset();
    prefetch_session_source_path_.clear();
}

void PreviewFrameRendererWorker::invalidate_subtitle_session() {
    cached_subtitle_path_.clear();
    cached_subtitle_format_hint_ = "auto";
    cached_subtitle_canvas_width_ = 0;
    cached_subtitle_canvas_height_ = 0;
    cached_subtitle_sample_aspect_ratio_.reset();
    subtitle_session_.reset();
}

bool PreviewFrameRendererWorker::should_start_sequential_prefetch(const PreviewFrameRenderRequest &request) const {
    if (!request.playback_active || !preview_session_ || cached_preview_frames_.empty()) {
        return false;
    }

    const QString normalized_source_path = request.source_path.trimmed();
    if (cached_source_path_ != normalized_source_path) {
        return false;
    }

    const auto trim_range = utsure::core::media::normalize_preview_trim_range(
        request.trim_in_us,
        request.trim_out_us
    );
    const qint64 current_cache_end_us = frame_coverage_end_us(cached_preview_frames_.back());
    if (trim_range.trim_out_microseconds.has_value() &&
        current_cache_end_us >= *trim_range.trim_out_microseconds) {
        return false;
    }
    if (prefetch_in_progress_ ||
        prefetch_window_start_us_ == current_cache_end_us ||
        blocked_prefetch_window_start_us_ == current_cache_end_us) {
        return false;
    }

    if (cached_preview_frames_.size() < kRetainedPreviewFrameCount) {
        return true;
    }

    const std::size_t lead_index = cached_preview_frames_.size() > kSequentialPreviewPrefetchLeadFrames
        ? cached_preview_frames_.size() - kSequentialPreviewPrefetchLeadFrames
        : 0;
    return request.requested_time_us >= cached_preview_frames_[lead_index].timestamp.start_microseconds;
}

void PreviewFrameRendererWorker::maybe_start_sequential_prefetch(const PreviewFrameRenderRequest &request) {
    if (request_is_superseded(request) || !should_start_sequential_prefetch(request)) {
        return;
    }

    const QString normalized_source_path = request.source_path.trimmed();
    const auto trim_range = utsure::core::media::normalize_preview_trim_range(
        request.trim_in_us,
        request.trim_out_us
    );
    const qint64 prefetch_window_start_us = frame_coverage_end_us(cached_preview_frames_.back());
    const quint64 generation = preview_cache_generation_;

    prefetch_in_progress_ = true;
    prefetch_window_start_us_ = prefetch_window_start_us;
    prefetch_source_path_ = normalized_source_path;

    qCInfo(previewWorkerLog).noquote()
        << QString("prefetch_start generation=%1 requested=%2 (%3) window_start=%4 (%5) cache=%6")
               .arg(generation)
               .arg(request.requested_time_us)
               .arg(format_time_us(request.requested_time_us))
               .arg(prefetch_window_start_us)
               .arg(format_time_us(prefetch_window_start_us))
               .arg(format_cached_preview_window_summary(summarize_cached_preview_window(cached_preview_frames_)));

    std::unique_ptr<utsure::core::media::VideoPreviewSession> task_session{};
    bool reused_existing_session = false;
    if (prefetch_session_ && prefetch_session_source_path_ == normalized_source_path) {
        task_session = std::move(prefetch_session_);
        reused_existing_session = true;
    } else {
        prefetch_session_.reset();
    }
    prefetch_session_source_path_.clear();

    const QPointer<PreviewFrameRendererWorker> worker(this);
    QThreadPool::globalInstance()->start(QRunnable::create(
        [
            worker,
            generation,
            normalized_source_path,
            trim_range,
            prefetch_window_start_us,
            reused_existing_session,
            task_session = std::move(task_session)
        ]() mutable {
            auto result = std::make_shared<PrefetchTaskResult>();
            result->generation = generation;
            result->source_path = normalized_source_path;
            result->window_start_us = prefetch_window_start_us;
            result->reused_existing_session = reused_existing_session;

            if (task_session == nullptr) {
                auto session_result = utsure::core::media::MediaDecoder::create_video_preview_session(
                    qstring_to_path(normalized_source_path),
                    preview_decode_normalization_policy()
                );
                result->session_diagnostics = to_qstring(session_result.diagnostics);
                if (!session_result.succeeded()) {
                    result->error_message = format_error_detail(
                        session_result.error->message,
                        session_result.error->actionable_hint
                    );
                } else {
                    result->session = std::move(session_result.session);
                }
            } else {
                result->session = std::move(task_session);
            }

            if (result->session != nullptr) {
                auto window_result = result->session->seek_and_decode_window_at_time(
                    prefetch_window_start_us,
                    kPlaybackPreviewWindowFrameCount
                );
                result->decode_diagnostics = to_qstring(window_result.diagnostics);
                if (!window_result.succeeded()) {
                    result->error_message = format_error_detail(
                        window_result.error->message,
                        window_result.error->actionable_hint
                    );
                    result->session.reset();
                } else {
                    result->frames = utsure::core::media::trim_preview_video_frames(
                        std::move(*window_result.video_frames),
                        trim_range
                    );
                }
            }

            if (worker == nullptr) {
                return;
            }

            QMetaObject::invokeMethod(
                worker.data(),
                [worker, result]() mutable {
                    if (worker == nullptr) {
                        return;
                    }

                    if (result->generation != worker->preview_cache_generation_ ||
                        worker->cached_source_path_ != result->source_path ||
                        worker->prefetch_window_start_us_ != result->window_start_us) {
                        qCInfo(previewWorkerLog).noquote()
                            << QString("prefetch_ignored_stale generation=%1 active_generation=%2 result_window_start=%3 active_window_start=%4")
                                   .arg(result->generation)
                                   .arg(worker->preview_cache_generation_)
                                   .arg(result->window_start_us)
                                   .arg(worker->prefetch_window_start_us_);
                        return;
                    }

                    worker->prefetch_in_progress_ = false;
                    worker->prefetch_window_start_us_ = -1;
                    worker->prefetch_source_path_.clear();

                    if (!result->error_message.isEmpty()) {
                        worker->blocked_prefetch_window_start_us_ = result->window_start_us;
                        qCInfo(previewWorkerLog).noquote()
                            << QString(
                                   "prefetch_failed window_start=%1 (%2) reused_session=%3 session_diagnostics=%4 decode_diagnostics=%5 detail=%6"
                               )
                                   .arg(result->window_start_us)
                                   .arg(format_time_us(result->window_start_us))
                                   .arg(bool_text(result->reused_existing_session))
                                   .arg(result->session_diagnostics, result->decode_diagnostics, result->error_message);
                        return;
                    }

                    if (result->frames.empty()) {
                        worker->blocked_prefetch_window_start_us_ = result->window_start_us;
                        qCInfo(previewWorkerLog).noquote()
                            << QString("prefetch_ignored_empty window_start=%1 active_cache_end=%2")
                                   .arg(result->window_start_us)
                                   .arg(
                                       worker->cached_preview_frames_.empty()
                                           ? -1
                                           : frame_coverage_end_us(worker->cached_preview_frames_.back())
                                   );
                        return;
                    }

                    const qint64 active_cache_end_us = worker->cached_preview_frames_.empty()
                        ? -1
                        : frame_coverage_end_us(worker->cached_preview_frames_.back());
                    if (active_cache_end_us != result->window_start_us) {
                        qCInfo(previewWorkerLog).noquote()
                            << QString("prefetch_ignored_misaligned window_start=%1 active_cache_end=%2")
                                   .arg(result->window_start_us)
                                   .arg(active_cache_end_us);
                        return;
                    }

                    worker->blocked_prefetch_window_start_us_ = -1;
                    append_preview_frames_with_retention(worker->cached_preview_frames_, std::move(result->frames));
                    auto previous_primary_session = std::move(worker->preview_session_);
                    worker->preview_session_ = std::move(result->session);
                    worker->prefetch_session_ = std::move(previous_primary_session);
                    worker->prefetch_session_source_path_ = worker->cached_source_path_;
                    qCInfo(previewWorkerLog).noquote()
                        << QString(
                               "prefetch_ready_applied window_start=%1 (%2) reused_session=%3 session_diagnostics=%4 decode_diagnostics=%5 cache=%6"
                           )
                               .arg(result->window_start_us)
                               .arg(format_time_us(result->window_start_us))
                               .arg(bool_text(result->reused_existing_session))
                               .arg(
                                   result->session_diagnostics,
                                   result->decode_diagnostics,
                                   format_cached_preview_window_summary(
                                       summarize_cached_preview_window(worker->cached_preview_frames_)
                                   )
                               );
                },
                Qt::QueuedConnection
            );
        }
    ));
}

bool PreviewFrameRendererWorker::cached_preview_window_covers(
    const QString &source_path,
    const qint64 requested_time_us
) const {
    if (cached_source_path_ != source_path || cached_preview_frames_.empty()) {
        return false;
    }

    return utsure::core::media::trimmed_preview_frames_cover_time(
        cached_preview_frames_,
        requested_time_us,
        utsure::core::media::PreviewTrimRange{
            .trim_in_microseconds = cached_trim_in_us_,
            .trim_out_microseconds = cached_trim_out_us_
        }
    );
}

bool PreviewFrameRendererWorker::should_decode_next_preview_window(const PreviewFrameRenderRequest &request) const {
    if (!preview_session_ || cached_preview_frames_.empty()) {
        return false;
    }

    if (cached_source_path_ != request.source_path.trimmed()) {
        return false;
    }

    const qint64 cached_window_end_us = frame_coverage_end_us(cached_preview_frames_.back());
    return request.requested_time_us >= cached_window_end_us &&
        request.requested_time_us <= (cached_window_end_us + kSequentialPreviewRefillToleranceUs);
}

bool PreviewFrameRendererWorker::request_is_superseded(const PreviewFrameRenderRequest &request) const {
    if (latest_request_generation_ == nullptr) {
        return false;
    }

    return latest_request_generation_->load(std::memory_order_acquire) != request.dispatch_generation;
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
