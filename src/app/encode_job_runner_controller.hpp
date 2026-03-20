#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <QObject>
#include <QString>
#include <QThread>

class EncodeJobRunnerWorker;

class EncodeJobRunnerController final : public QObject {
    Q_OBJECT

public:
    explicit EncodeJobRunnerController(QObject *parent = nullptr);
    ~EncodeJobRunnerController() override;

    [[nodiscard]] bool is_running() const noexcept;
    void start_job(const utsure::core::job::EncodeJob &job);

signals:
    void running_changed(bool running);
    void progress_changed(utsure::core::job::EncodeJobProgress progress);
    void log_message(const QString &line);
    void job_finished(bool succeeded, const QString &status_text, const QString &details_text);

private:
    void handle_worker_finished(bool succeeded, const QString &status_text, const QString &details_text);

    QThread worker_thread_{};
    EncodeJobRunnerWorker *worker_{nullptr};
    bool running_{false};
};
