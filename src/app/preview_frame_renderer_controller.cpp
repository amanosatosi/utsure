#include "preview_frame_renderer_controller.hpp"

#include <QLoggingCategory>
#include <QMetaObject>

Q_LOGGING_CATEGORY(previewControllerLog, "utsure.preview.controller")

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
        qCInfo(previewControllerLog).noquote()
            << QString("request_preview queued_pending token=%1 requested=%2")
                   .arg(request.request_token)
                   .arg(request.requested_time_us);
        pending_request_ = request;
        return;
    }

    qCInfo(previewControllerLog).noquote()
        << QString("request_preview dispatch token=%1 requested=%2")
               .arg(request.request_token)
               .arg(request.requested_time_us);
    dispatch_request(request);
}

void PreviewFrameRendererController::clear_cache() {
    qCInfo(previewControllerLog) << "clear_cache requested";
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
    qCInfo(previewControllerLog).noquote()
        << QString("dispatch_request token=%1 requested=%2")
               .arg(request.request_token)
               .arg(request.requested_time_us);
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
    qCInfo(previewControllerLog).noquote()
        << QString("finish_request request_in_flight=false pending=%1")
               .arg(pending_request_.has_value() ? "true" : "false");
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
    const qint64 frame_duration_us,
    const QImage &image
) {
    qCInfo(previewControllerLog).noquote()
        << QString("handle_preview_ready token=%1 requested=%2 frame_time=%3 frame_duration=%4")
               .arg(request_token)
               .arg(requested_time_us)
               .arg(frame_time_us)
               .arg(frame_duration_us);
    emit preview_ready(request_token, requested_time_us, frame_time_us, frame_duration_us, image);
    finish_request();
}

void PreviewFrameRendererController::handle_preview_failed(
    const quint64 request_token,
    const qint64 requested_time_us,
    const QString &title,
    const QString &detail
) {
    qCInfo(previewControllerLog).noquote()
        << QString("handle_preview_failed token=%1 requested=%2 title='%3' detail='%4'")
               .arg(request_token)
               .arg(requested_time_us)
               .arg(title, detail);
    emit preview_failed(request_token, requested_time_us, title, detail);
    finish_request();
}
