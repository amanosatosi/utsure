#include "preview_frame_renderer_controller.hpp"

#include <QLoggingCategory>
#include <QMetaObject>

#include <chrono>

Q_LOGGING_CATEGORY(previewControllerLog, "utsure.preview.controller")

namespace {

using SteadyClock = std::chrono::steady_clock;

qint64 steady_clock_now_microseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               SteadyClock::now().time_since_epoch()
           )
        .count();
}

QString format_elapsed_milliseconds(const qint64 elapsed_microseconds) {
    return QString::number(static_cast<double>(elapsed_microseconds) / 1000.0, 'f', 2) + "ms";
}

}  // namespace

PreviewFrameRendererController::PreviewFrameRendererController(QObject *parent) : QObject(parent) {
    worker_ = new PreviewFrameRendererWorker(&latest_request_generation_);
    worker_->moveToThread(&worker_thread_);

    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &PreviewFrameRendererWorker::preview_skipped, this, &PreviewFrameRendererController::handle_preview_skipped);
    connect(worker_, &PreviewFrameRendererWorker::preview_ready, this, &PreviewFrameRendererController::handle_preview_ready);
    connect(worker_, &PreviewFrameRendererWorker::preview_failed, this, &PreviewFrameRendererController::handle_preview_failed);

    worker_thread_.start();
}

PreviewFrameRendererController::~PreviewFrameRendererController() {
    worker_thread_.quit();
    worker_thread_.wait();
}

void PreviewFrameRendererController::request_preview(const PreviewFrameRenderRequest &request) {
    auto queued_request = request;
    queued_request.dispatch_generation = ++dispatch_generation_counter_;
    queued_request.enqueued_steady_us = steady_clock_now_microseconds();
    latest_request_generation_.store(queued_request.dispatch_generation, std::memory_order_release);

    if (request_in_flight_) {
        qCInfo(previewControllerLog).noquote()
            << QString("request_preview queued_pending token=%1 generation=%2 requested=%3")
                   .arg(queued_request.request_token)
                   .arg(queued_request.dispatch_generation)
                   .arg(queued_request.requested_time_us);
        pending_request_ = std::move(queued_request);
        return;
    }

    qCInfo(previewControllerLog).noquote()
        << QString("request_preview dispatch token=%1 generation=%2 requested=%3")
               .arg(queued_request.request_token)
               .arg(queued_request.dispatch_generation)
               .arg(queued_request.requested_time_us);
    dispatch_request(queued_request);
}

void PreviewFrameRendererController::clear_cache() {
    qCInfo(previewControllerLog) << "clear_cache requested";
    pending_request_.reset();
    latest_request_generation_.store(++dispatch_generation_counter_, std::memory_order_release);
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
    active_request_ = request;
    const qint64 queue_wait_us = std::max<qint64>(0, steady_clock_now_microseconds() - request.enqueued_steady_us);
    qCInfo(previewControllerLog).noquote()
        << QString("dispatch_request token=%1 generation=%2 requested=%3 queue_wait=%4")
               .arg(request.request_token)
               .arg(request.dispatch_generation)
               .arg(request.requested_time_us)
               .arg(format_elapsed_milliseconds(queue_wait_us));
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
    active_request_.reset();
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

void PreviewFrameRendererController::handle_preview_skipped(const quint64 request_token, const qint64 requested_time_us) {
    const qint64 total_elapsed_us = active_request_.has_value()
        ? std::max<qint64>(0, steady_clock_now_microseconds() - active_request_->enqueued_steady_us)
        : 0;
    qCInfo(previewControllerLog).noquote()
        << QString("handle_preview_skipped token=%1 requested=%2 total=%3")
               .arg(request_token)
               .arg(requested_time_us)
               .arg(format_elapsed_milliseconds(total_elapsed_us));
    finish_request();
}

void PreviewFrameRendererController::handle_preview_ready(
    const quint64 request_token,
    const qint64 requested_time_us,
    const qint64 frame_time_us,
    const qint64 frame_duration_us,
    const QImage &image
) {
    const qint64 total_elapsed_us = active_request_.has_value()
        ? std::max<qint64>(0, steady_clock_now_microseconds() - active_request_->enqueued_steady_us)
        : 0;
    qCInfo(previewControllerLog).noquote()
        << QString("handle_preview_ready token=%1 requested=%2 frame_time=%3 frame_duration=%4 total=%5")
               .arg(request_token)
               .arg(requested_time_us)
               .arg(frame_time_us)
               .arg(frame_duration_us)
               .arg(format_elapsed_milliseconds(total_elapsed_us));
    emit preview_ready(request_token, requested_time_us, frame_time_us, frame_duration_us, image);
    finish_request();
}

void PreviewFrameRendererController::handle_preview_failed(
    const quint64 request_token,
    const qint64 requested_time_us,
    const QString &title,
    const QString &detail
) {
    const qint64 total_elapsed_us = active_request_.has_value()
        ? std::max<qint64>(0, steady_clock_now_microseconds() - active_request_->enqueued_steady_us)
        : 0;
    qCInfo(previewControllerLog).noquote()
        << QString("handle_preview_failed token=%1 requested=%2 total=%3 title='%4' detail='%5'")
               .arg(request_token)
               .arg(requested_time_us)
               .arg(format_elapsed_milliseconds(total_elapsed_us))
               .arg(title, detail);
    emit preview_failed(request_token, requested_time_us, title, detail);
    finish_request();
}
