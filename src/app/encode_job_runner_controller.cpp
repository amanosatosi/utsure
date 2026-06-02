#include "encode_job_runner_controller.hpp"

#include "encode_job_runner_worker.hpp"

#include <QMetaType>
#include <QMetaObject>

namespace {

constexpr int kShutdownWaitChunkMilliseconds = 250;
constexpr int kShutdownTotalWaitMilliseconds = 5000;

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
    initialize_worker_thread();
}

EncodeJobRunnerController::EncodeJobRunnerController(EncodeJobRunnerWorker *worker, QObject *parent)
    : QObject(parent),
      worker_(worker != nullptr ? worker : new EncodeJobRunnerWorker()) {
    qRegisterMetaType<utsure::core::job::EncodeJobProgress>("utsure::core::job::EncodeJobProgress");
    initialize_worker_thread();
}

void EncodeJobRunnerController::initialize_worker_thread() {
    worker_thread_ = new QThread();
    worker_->moveToThread(worker_thread_);

    connect(worker_, &EncodeJobRunnerWorker::progress_changed, this, &EncodeJobRunnerController::progress_changed);
    connect(worker_, &EncodeJobRunnerWorker::log_message, this, &EncodeJobRunnerController::log_message);
    connect(
        worker_,
        &EncodeJobRunnerWorker::job_finished,
        this,
        &EncodeJobRunnerController::handle_worker_finished
    );

    worker_thread_->start();
}

EncodeJobRunnerController::~EncodeJobRunnerController() {
    shutdown_worker();
}

void EncodeJobRunnerController::shutdown_worker() {
    shutting_down_ = true;
    if (worker_ != nullptr) {
        worker_->request_cancel();
        if (worker_->is_active()) {
            emit log_message("[warning] Waiting for active encode worker to observe shutdown cancellation.");
        }
        disconnect(worker_, nullptr, this, nullptr);
    }

    state_ = RunnerState::finishing;
    if (worker_thread_ == nullptr) {
        state_ = RunnerState::finished;
        return;
    }

    worker_thread_->quit();
    int waited_ms = 0;
    while (worker_thread_->isRunning() && waited_ms < kShutdownTotalWaitMilliseconds) {
        worker_thread_->wait(kShutdownWaitChunkMilliseconds);
        waited_ms += kShutdownWaitChunkMilliseconds;
        if (worker_ != nullptr && worker_->is_active()) {
            emit log_message(
                QString("[warning] Encode worker still active during shutdown after %1 ms.").arg(waited_ms)
            );
        }
    }

    if (worker_thread_->isRunning()) {
        emit log_message("[error] Encode worker did not stop after cancellation; terminating worker thread.");
        worker_thread_->terminate();
        worker_thread_->wait(kShutdownTotalWaitMilliseconds);
    }

    if (worker_ != nullptr) {
        if (worker_->is_active()) {
            emit log_message("[error] Encode worker remained active after shutdown; leaving worker object undeleted.");
        } else {
            delete worker_;
        }
        worker_ = nullptr;
    }

    if (worker_thread_->isRunning()) {
        emit log_message("[error] Worker thread is still running after bounded shutdown; leaving QThread object alive.");
        worker_thread_ = nullptr;
    } else {
        delete worker_thread_;
        worker_thread_ = nullptr;
    }
    state_ = RunnerState::finished;
}

bool EncodeJobRunnerController::is_running() const noexcept {
    return state_ == RunnerState::running ||
        state_ == RunnerState::cancel_requested ||
        state_ == RunnerState::finishing;
}

void EncodeJobRunnerController::start_job(const utsure::core::job::EncodeJob &job) {
    if (is_running() || worker_ == nullptr || worker_thread_ == nullptr || shutting_down_) {
        return;
    }

    worker_->clear_cancel_request();
    worker_thread_->setPriority(map_thread_priority(job.execution.process_priority));
    state_ = RunnerState::running;
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
    if (!is_running() || worker_ == nullptr || state_ == RunnerState::cancel_requested) {
        return;
    }

    state_ = RunnerState::cancel_requested;
    worker_->request_cancel();
    emit log_message("[warning] Cancel requested for the active encode job.");
}

void EncodeJobRunnerController::handle_worker_finished(
    const bool succeeded,
    const bool canceled,
    const QString &status_text,
    const QString &details_text,
    const QString &output_path
) {
    if (shutting_down_) {
        state_ = RunnerState::finished;
        return;
    }

    state_ = RunnerState::idle;
    if (worker_thread_ != nullptr) {
        worker_thread_->setPriority(QThread::NormalPriority);
    }
    emit running_changed(false);
    emit job_finished(succeeded, canceled, status_text, details_text, output_path);
}
