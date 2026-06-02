#include "encode_job_runner_controller.hpp"

#include "encode_job_runner_worker.hpp"

#include <QMetaType>
#include <QMetaObject>

#include <mutex>
#include <vector>

namespace {

constexpr int kShutdownWaitChunkMilliseconds = 250;
constexpr int kShutdownTotalWaitMilliseconds = 5000;

struct QuarantinedEncodeWorker final {
    QThread *thread{nullptr};
    EncodeJobRunnerWorker *worker{nullptr};
};

std::vector<QuarantinedEncodeWorker> &quarantined_encode_workers() {
    static auto *workers = new std::vector<QuarantinedEncodeWorker>();
    return *workers;
}

std::mutex &quarantined_encode_workers_mutex() {
    static auto *mutex = new std::mutex();
    return *mutex;
}

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
        emit log_message(
            "[error] Encode worker did not stop after cooperative cancellation before the shutdown timeout. "
            "Keeping the worker and QThread in a process-lifetime quarantine instead of terminating the thread."
        );
        {
            const std::lock_guard lock(quarantined_encode_workers_mutex());
            quarantined_encode_workers().push_back(QuarantinedEncodeWorker{
                .thread = worker_thread_,
                .worker = worker_
            });
        }
        worker_ = nullptr;
        worker_thread_ = nullptr;
        state_ = RunnerState::finished;
        return;
    }

    if (worker_ != nullptr) {
        if (worker_->is_active()) {
            emit log_message("[error] Encode worker thread stopped while worker still reported active; quarantining worker object.");
            const std::lock_guard lock(quarantined_encode_workers_mutex());
            quarantined_encode_workers().push_back(QuarantinedEncodeWorker{
                .thread = nullptr,
                .worker = worker_
            });
        } else {
            delete worker_;
        }
        worker_ = nullptr;
    }

    if (worker_thread_ != nullptr) {
        delete worker_thread_;
        worker_thread_ = nullptr;
    } else {
        emit log_message("[error] Worker thread ownership was already released during shutdown.");
    }
    state_ = RunnerState::finished;
}

bool EncodeJobRunnerController::is_running() const noexcept {
    return state_ == RunnerState::running ||
        state_ == RunnerState::cancel_requested ||
        state_ == RunnerState::finishing;
}

std::size_t EncodeJobRunnerController::quarantined_worker_count() noexcept {
    const std::lock_guard lock(quarantined_encode_workers_mutex());
    return quarantined_encode_workers().size();
}

std::size_t EncodeJobRunnerController::quarantined_worker_count_for_tests() noexcept {
    return quarantined_worker_count();
}

bool EncodeJobRunnerController::start_job(const utsure::core::job::EncodeJob &job) {
    if (is_running() ||
        worker_ == nullptr ||
        worker_thread_ == nullptr ||
        !worker_thread_->isRunning() ||
        shutting_down_) {
        return false;
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

    const bool queued = QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, job]() {
            worker->run_job(job);
        },
        Qt::QueuedConnection
    );
    if (!queued) {
        state_ = RunnerState::idle;
        worker_thread_->setPriority(QThread::NormalPriority);
        emit log_message("[error] Failed to queue encode work on the worker thread.");
        emit running_changed(false);
        return false;
    }
    return true;
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
