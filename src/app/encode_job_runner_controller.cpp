#include "encode_job_runner_controller.hpp"

#include "encode_job_runner_worker.hpp"

#include <QMetaType>
#include <QMetaObject>

namespace {

QThread::Priority map_thread_priority(const utsure::core::job::EncodeJobProcessPriority priority) {
    switch (priority) {
    case utsure::core::job::EncodeJobProcessPriority::high:
        return QThread::HighestPriority;
    case utsure::core::job::EncodeJobProcessPriority::above_normal:
        return QThread::HighPriority;
    case utsure::core::job::EncodeJobProcessPriority::normal:
        return QThread::NormalPriority;
    case utsure::core::job::EncodeJobProcessPriority::below_normal:
        return QThread::LowPriority;
    case utsure::core::job::EncodeJobProcessPriority::low:
    default:
        return QThread::LowestPriority;
    }
}

}  // namespace

EncodeJobRunnerController::EncodeJobRunnerController(QObject *parent) : QObject(parent) {
    qRegisterMetaType<utsure::core::job::EncodeJobProgress>("utsure::core::job::EncodeJobProgress");

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

    worker_->clear_cancel_request();
    worker_thread_.setPriority(map_thread_priority(job.execution.process_priority));
    running_ = true;
    emit running_changed(true);
    emit progress_changed(utsure::core::job::EncodeJobProgress{
        .stage = utsure::core::job::EncodeJobStage::assembling_timeline,
        .current_step = 0,
        .total_steps = 0,
        .message = "Starting encode job."
    });
    emit log_message("[info] Starting encode job.");
    emit log_message(
        QString("[info] Worker thread priority: %1.")
            .arg(QString::fromUtf8(utsure::core::job::to_display_string(job.execution.process_priority)))
    );

    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, job]() {
            worker->run_job(job);
        },
        Qt::QueuedConnection
    );
}

void EncodeJobRunnerController::cancel_job() noexcept {
    if (!running_ || worker_ == nullptr) {
        return;
    }

    worker_->request_cancel();
    emit log_message("[warning] Cancel requested for the active encode job.");
}

void EncodeJobRunnerController::handle_worker_finished(
    const bool succeeded,
    const bool canceled,
    const QString &status_text,
    const QString &details_text
) {
    running_ = false;
    worker_thread_.setPriority(QThread::NormalPriority);
    emit running_changed(false);
    emit job_finished(succeeded, canceled, status_text, details_text);
}
