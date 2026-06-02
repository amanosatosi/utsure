#pragma once

#include "encode_job_progress_qt_types.hpp"

#include <QObject>
#include <QString>
#include <QThread>

#include <cstddef>

class EncodeJobRunnerWorker;

class EncodeJobRunnerController final : public QObject {
    Q_OBJECT

public:
    explicit EncodeJobRunnerController(QObject *parent = nullptr);
    explicit EncodeJobRunnerController(EncodeJobRunnerWorker *worker, QObject *parent = nullptr);
    ~EncodeJobRunnerController() override;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool start_job(const utsure::core::job::EncodeJob &job);
    void cancel_job() noexcept;
    [[nodiscard]] static std::size_t quarantined_worker_count() noexcept;
    [[nodiscard]] static std::size_t quarantined_worker_count_for_tests() noexcept;

signals:
    void running_changed(bool running);
    void progress_changed(utsure::core::job::EncodeJobProgress progress);
    void log_message(const QString &line);
    void job_finished(
        bool succeeded,
        bool canceled,
        const QString &status_text,
        const QString &details_text,
        const QString &output_path
    );

private:
    enum class RunnerState {
        idle,
        running,
        cancel_requested,
        finishing,
        finished
    };

    void handle_worker_finished(
        bool succeeded,
        bool canceled,
        const QString &status_text,
        const QString &details_text,
        const QString &output_path
    );
    void initialize_worker_thread();
    void shutdown_worker();

    QThread *worker_thread_{nullptr};
    EncodeJobRunnerWorker *worker_{nullptr};
    RunnerState state_{RunnerState::idle};
    bool shutting_down_{false};
};
