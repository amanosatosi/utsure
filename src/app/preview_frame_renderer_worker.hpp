#pragma once

#include "utsure/core/media/decoded_media.hpp"
#include "utsure/core/subtitles/subtitle_renderer.hpp"

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace utsure::core::media {
class VideoPreviewSession;
}

struct PreviewFrameRenderRequest final {
    quint64 request_token{0};
    quint64 dispatch_generation{0};
    qint64 enqueued_steady_us{0};
    QString source_path{};
    qint64 requested_time_us{0};
    qint64 trim_in_us{0};
    std::optional<qint64> trim_out_us{};
    bool playback_active{false};
    bool subtitle_enabled{false};
    QString subtitle_path{};
    QString subtitle_format_hint{"auto"};
};

class PreviewFrameRendererWorker final : public QObject {
    Q_OBJECT

public:
    explicit PreviewFrameRendererWorker(
        std::atomic<quint64> *latest_request_generation,
        QObject *parent = nullptr
    );
    ~PreviewFrameRendererWorker() override;

    void render_request(const PreviewFrameRenderRequest &request);
    void clear_cache();

signals:
    void preview_ready(
        quint64 request_token,
        qint64 requested_time_us,
        qint64 frame_time_us,
        qint64 frame_duration_us,
        QImage image
    );
    void preview_skipped(quint64 request_token, qint64 requested_time_us);
    void preview_failed(quint64 request_token, qint64 requested_time_us, QString title, QString detail);

private:
    void maybe_start_sequential_prefetch(const PreviewFrameRenderRequest &request);
    void invalidate_prefetch_state(bool advance_generation = true);
    void invalidate_subtitle_session();
    void invalidate_preview_frame_cache();
    void ensure_subtitle_session(
        const PreviewFrameRenderRequest &request,
        const utsure::core::media::DecodedVideoFrame &preview_frame
    );
    [[nodiscard]] bool cached_preview_window_covers(const QString &source_path, qint64 requested_time_us) const;
    [[nodiscard]] bool should_decode_next_preview_window(const PreviewFrameRenderRequest &request) const;
    [[nodiscard]] bool should_start_sequential_prefetch(const PreviewFrameRenderRequest &request) const;
    [[nodiscard]] bool request_is_superseded(const PreviewFrameRenderRequest &request) const;

    QString cached_source_path_{};
    std::vector<utsure::core::media::DecodedVideoFrame> cached_preview_frames_{};
    std::unique_ptr<utsure::core::media::VideoPreviewSession> preview_session_{};
    std::unique_ptr<utsure::core::media::VideoPreviewSession> prefetch_session_{};
    QString prefetch_session_source_path_{};
    qint64 cached_trim_in_us_{0};
    std::optional<qint64> cached_trim_out_us_{};
    quint64 preview_cache_generation_{0};
    bool prefetch_in_progress_{false};
    qint64 prefetch_window_start_us_{-1};
    qint64 blocked_prefetch_window_start_us_{-1};
    QString prefetch_source_path_{};
    QString cached_subtitle_path_{};
    QString cached_subtitle_format_hint_{"auto"};
    int cached_subtitle_canvas_width_{0};
    int cached_subtitle_canvas_height_{0};
    std::optional<utsure::core::media::Rational> cached_subtitle_sample_aspect_ratio_{};
    std::unique_ptr<utsure::core::subtitles::SubtitleRenderer> subtitle_renderer_{};
    std::unique_ptr<utsure::core::subtitles::SubtitleRenderSession> subtitle_session_{};
    std::atomic<quint64> *latest_request_generation_{nullptr};
};
