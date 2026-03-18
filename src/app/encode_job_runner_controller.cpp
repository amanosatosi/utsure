#include "encode_job_runner_controller.hpp"

#include "encode_job_runner_worker.hpp"

#include <QMetaObject>

EncodeJobRunnerController::EncodeJobRunnerController(QObject *parent) : QObject(parent) {
    worker_ = new EncodeJobRunnerWorker();
    worker_->moveToThread(&worker_thread_);

    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &EncodeJobRunnerWorker::progress_changed, this, &EncodeJobRunnerController::progress_changed);
    connect(worker_, &EncodeJobRunnerWorker::log_message, this, &EncodeJobRunnerController::log_message);
    connect(
        worker_,
        &EncodeJobRunnerWorker::job_finished,
        this,
        &EncodeJobRunnerController::handle_worker_finished
    );

    worker_thread_.start();
}

EncodeJobRunnerController::~EncodeJobRunnerController() {
    worker_thread_.quit();
    worker_thread_.wait();
}

bool EncodeJobRunnerController::is_running() const noexcept {
    return running_;
}

void EncodeJobRunnerController::start_job(const utsure::core::job::EncodeJob &job) {
    if (running_ || worker_ == nullptr) {
        return;
    }

    running_ = true;
    emit running_changed(true);
    emit progress_changed(0, 0, "Starting encode job.");
    emit log_message("[info] Starting encode job.");

    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, job]() {
            worker->run_job(job);
        },
        Qt::QueuedConnection
    );
}

void EncodeJobRunnerController::handle_worker_finished(
    const bool succeeded,
    const QString &status_text,
    const QString &details_text
) {
    running_ = false;
    emit running_changed(false);
    emit job_finished(succeeded, status_text, details_text);
}
