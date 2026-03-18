#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <QObject>
#include <QString>

class EncodeJobRunnerWorker final
    : public QObject,
      public utsure::core::job::EncodeJobObserver {
    Q_OBJECT

public:
    explicit EncodeJobRunnerWorker(QObject *parent = nullptr);

    void run_job(const utsure::core::job::EncodeJob &job);

    void on_progress(const utsure::core::job::EncodeJobProgress &progress) override;
    void on_log(const utsure::core::job::EncodeJobLogMessage &message) override;

signals:
    void progress_changed(int current_step, int total_steps, const QString &status_text);
    void log_message(const QString &line);
    void job_finished(bool succeeded, const QString &status_text, const QString &details_text);
};
