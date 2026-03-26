#pragma once

#include "encode_job_progress_qt_types.hpp"

#include <QObject>
#include <QString>
#include <atomic>

class EncodeJobRunnerWorker final
    : public QObject,
      public utsure::core::job::EncodeJobObserver {
    Q_OBJECT

public:
    explicit EncodeJobRunnerWorker(QObject *parent = nullptr);

    void run_job(const utsure::core::job::EncodeJob &job);
    void request_cancel() noexcept;
    void clear_cancel_request() noexcept;

    void on_progress(const utsure::core::job::EncodeJobProgress &progress) override;
    void on_log(const utsure::core::job::EncodeJobLogMessage &message) override;

signals:
    void progress_changed(utsure::core::job::EncodeJobProgress progress);
    void log_message(const QString &line);
    void job_finished(bool succeeded, bool canceled, const QString &status_text, const QString &details_text);

private:
    [[nodiscard]] bool cancel_requested() const noexcept;

    std::atomic_bool cancel_requested_{false};
};
