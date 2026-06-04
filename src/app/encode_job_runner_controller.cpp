#include "encode_job_runner_controller.hpp"

#include "crash_dump_writer.hpp"
#include "encode_job_runner_worker.hpp"
#include "utsure/core/filesystem/path_format.hpp"

#include <QMetaType>
#include <QMetaObject>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
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

std::atomic_int &next_runner_slot_index() {
    static auto *slot = new std::atomic_int{0};
    return *slot;
}

std::atomic_int &active_encode_job_count() {
    static auto *count = new std::atomic_int{0};
    return *count;
}

std::string path_to_utf8_string(const std::filesystem::path &path) {
    return utsure::core::filesystem::path_to_utf8_string(path);
}

std::string qstring_to_utf8_string(const QString &text) {
    const auto bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

void update_crash_context_safely(const utsure::app::crash::CrashContextUpdate &update) noexcept {
    try {
        utsure::app::crash::update_crash_context(update);
    } catch (...) {
    }
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

EncodeJobRunnerController::EncodeJobRunnerController(QObject *parent)
    : QObject(parent),
      runner_slot_index_(next_runner_slot_index().fetch_add(1)) {
    qRegisterMetaType<utsure::core::job::EncodeJobProgress>("utsure::core::job::EncodeJobProgress");

    worker_ = new EncodeJobRunnerWorker();
    initialize_worker_thread();
}

EncodeJobRunnerController::EncodeJobRunnerController(EncodeJobRunnerWorker *worker, QObject *parent)
    : QObject(parent),
      worker_(worker != nullptr ? worker : new EncodeJobRunnerWorker()),
      runner_slot_index_(next_runner_slot_index().fetch_add(1)) {
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
    const int active_count = active_encode_job_count().fetch_add(1) + 1;
    update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
        .runner_slot_index = runner_slot_index_,
        .active_job_count = active_count,
        .input_path = path_to_utf8_string(job.input.main_source_path),
        .output_path = path_to_utf8_string(job.output.output_path),
        .video_output_codec = utsure::core::media::to_string(job.output.video.codec),
        .current_stage = "controller_start_job",
        .subtitle_enabled = job.subtitles.has_value(),
        .cancellation_requested = false
    });
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
        const int remaining_active_count = std::max(active_encode_job_count().fetch_sub(1) - 1, 0);
        state_ = RunnerState::idle;
        worker_thread_->setPriority(QThread::NormalPriority);
        update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
            .runner_slot_index = runner_slot_index_,
            .active_job_count = remaining_active_count,
            .current_stage = "controller_queue_failed"
        });
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
    update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
        .runner_slot_index = runner_slot_index_,
        .active_job_count = active_encode_job_count().load(),
        .current_stage = "controller_cancel_requested",
        .cancellation_requested = true
    });
    emit log_message("[warning] Cancel requested for the active encode job.");
}

void EncodeJobRunnerController::handle_worker_finished(
    const bool succeeded,
    const bool canceled,
    const QString &status_text,
    const QString &details_text,
    const QString &output_path
) {
    const int remaining_active_count = std::max(active_encode_job_count().fetch_sub(1) - 1, 0);
    if (shutting_down_) {
        state_ = RunnerState::finished;
        update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
            .runner_slot_index = runner_slot_index_,
            .active_job_count = remaining_active_count,
            .output_path = qstring_to_utf8_string(output_path),
            .current_stage = canceled ? std::string("controller_shutdown_canceled") : succeeded
                ? std::string("controller_shutdown_completed")
                : std::string("controller_shutdown_failed"),
            .cancellation_requested = canceled
        });
        return;
    }

    state_ = RunnerState::idle;
    update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
        .runner_slot_index = runner_slot_index_,
        .active_job_count = remaining_active_count,
        .output_path = qstring_to_utf8_string(output_path),
        .current_stage = canceled ? std::string("controller_canceled") : succeeded
            ? std::string("controller_completed")
            : std::string("controller_failed"),
        .cancellation_requested = canceled
    });
    if (worker_thread_ != nullptr) {
        worker_thread_->setPriority(QThread::NormalPriority);
    }
    emit running_changed(false);
    emit job_finished(succeeded, canceled, status_text, details_text, output_path);
}
