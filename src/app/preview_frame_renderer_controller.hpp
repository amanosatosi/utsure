#pragma once

#include "preview_frame_renderer_worker.hpp"

#include <QObject>
#include <QThread>

#include <atomic>
#include <optional>

class PreviewFrameRendererController final : public QObject {
    Q_OBJECT

public:
    explicit PreviewFrameRendererController(QObject *parent = nullptr);
    ~PreviewFrameRendererController() override;

    void request_preview(const PreviewFrameRenderRequest &request);
    void clear_cache();

signals:
    void preview_loading(quint64 request_token, qint64 requested_time_us);
    void preview_ready(
        quint64 request_token,
        qint64 requested_time_us,
        qint64 frame_time_us,
        qint64 frame_duration_us,
        QImage image
    );
    void preview_failed(quint64 request_token, qint64 requested_time_us, QString title, QString detail);

private:
    void dispatch_request(const PreviewFrameRenderRequest &request);
    void finish_request();
    void handle_preview_skipped(quint64 request_token, qint64 requested_time_us);
    void handle_preview_ready(
        quint64 request_token,
        qint64 requested_time_us,
        qint64 frame_time_us,
        qint64 frame_duration_us,
        const QImage &image
    );
    void handle_preview_failed(quint64 request_token, qint64 requested_time_us, const QString &title, const QString &detail);

    QThread worker_thread_{};
    PreviewFrameRendererWorker *worker_{nullptr};
    bool request_in_flight_{false};
    std::optional<PreviewFrameRenderRequest> pending_request_{};
    std::optional<PreviewFrameRenderRequest> active_request_{};
    quint64 dispatch_generation_counter_{0};
    std::atomic<quint64> latest_request_generation_{0};
};
