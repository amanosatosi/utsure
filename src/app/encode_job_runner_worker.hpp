#pragma once

#include "encode_job_progress_qt_types.hpp"

#include <QObject>
#include <QString>
#include <atomic>
#include <optional>

class EncodeJobRunnerWorker final
    : public QObject,
      public utsure::core::job::EncodeJobObserver {
    Q_OBJECT

public:
    explicit EncodeJobRunnerWorker(QObject *parent = nullptr);

    void run_job(const utsure::core::job::EncodeJob &job);
    void request_cancel() noexcept;
    void clear_cancel_request() noexcept;
    [[nodiscard]] bool is_active() const noexcept;

    void on_progress(const utsure::core::job::EncodeJobProgress &progress) override;
    void on_log(const utsure::core::job::EncodeJobLogMessage &message) override;

signals:
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
    [[nodiscard]] bool cancel_requested() const noexcept;
    [[nodiscard]] QString format_last_progress_context() const;

    std::atomic_bool cancel_requested_{false};
    std::atomic_bool active_{false};
    std::optional<utsure::core::job::EncodeJobProgress> last_progress_{};
};
