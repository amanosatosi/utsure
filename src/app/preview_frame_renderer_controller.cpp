#include "preview_frame_renderer_controller.hpp"

#include <QMetaObject>

PreviewFrameRendererController::PreviewFrameRendererController(QObject *parent) : QObject(parent) {
    worker_ = new PreviewFrameRendererWorker();
    worker_->moveToThread(&worker_thread_);

    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &PreviewFrameRendererWorker::preview_ready, this, &PreviewFrameRendererController::handle_preview_ready);
    connect(worker_, &PreviewFrameRendererWorker::preview_failed, this, &PreviewFrameRendererController::handle_preview_failed);

    worker_thread_.start();
}

PreviewFrameRendererController::~PreviewFrameRendererController() {
    worker_thread_.quit();
    worker_thread_.wait();
}

void PreviewFrameRendererController::request_preview(const PreviewFrameRenderRequest &request) {
    if (request_in_flight_) {
        pending_request_ = request;
        return;
    }

    dispatch_request(request);
}

void PreviewFrameRendererController::clear_cache() {
    pending_request_.reset();
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_]() {
            worker->clear_cache();
        },
        Qt::QueuedConnection
    );
}

void PreviewFrameRendererController::dispatch_request(const PreviewFrameRenderRequest &request) {
    request_in_flight_ = true;
    emit preview_loading(request.request_token, request.requested_time_us);

    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, request]() {
            worker->render_request(request);
        },
        Qt::QueuedConnection
    );
}

void PreviewFrameRendererController::finish_request() {
    request_in_flight_ = false;
    if (!pending_request_.has_value()) {
        return;
    }

    const auto next_request = *pending_request_;
    pending_request_.reset();
    dispatch_request(next_request);
}

void PreviewFrameRendererController::handle_preview_ready(
    const quint64 request_token,
    const qint64 requested_time_us,
    const qint64 frame_time_us,
    const QImage &image
) {
    emit preview_ready(request_token, requested_time_us, frame_time_us, image);
    finish_request();
}

void PreviewFrameRendererController::handle_preview_failed(
    const quint64 request_token,
    const qint64 requested_time_us,
    const QString &title,
    const QString &detail
) {
    emit preview_failed(request_token, requested_time_us, title, detail);
    finish_request();
}
