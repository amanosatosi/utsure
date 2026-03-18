#include "encode_job_runner_worker.hpp"

#include "utsure/core/job/encode_job_report.hpp"

#include <string_view>

namespace {

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString format_log_line(const utsure::core::job::EncodeJobLogMessage &message) {
    return QString("[%1] %2")
        .arg(to_qstring(utsure::core::job::to_string(message.level)))
        .arg(to_qstring(message.message));
}

QString format_error_details(const utsure::core::job::EncodeJobError &error) {
    QString details = QString("main_source=%1\noutput=%2\nmessage=%3")
        .arg(to_qstring(error.main_source_path))
        .arg(to_qstring(error.output_path))
        .arg(to_qstring(error.message));

    if (!error.actionable_hint.empty()) {
        details += QString("\nhint=%1").arg(to_qstring(error.actionable_hint));
    }

    return details;
}

}  // namespace

EncodeJobRunnerWorker::EncodeJobRunnerWorker(QObject *parent) : QObject(parent) {}

void EncodeJobRunnerWorker::run_job(const utsure::core::job::EncodeJob &job) {
    const auto result = utsure::core::job::EncodeJobRunner::run(job, utsure::core::job::EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = this
    });

    if (result.succeeded()) {
        emit job_finished(
            true,
            "Encode completed successfully.",
            to_qstring(utsure::core::job::format_encode_job_report(*result.encode_job_summary))
        );
        return;
    }

    emit job_finished(
        false,
        QString("Encode failed: %1").arg(to_qstring(result.error->message)),
        format_error_details(*result.error)
    );
}

void EncodeJobRunnerWorker::on_progress(const utsure::core::job::EncodeJobProgress &progress) {
    emit progress_changed(progress.current_step, progress.total_steps, to_qstring(progress.message));
}

void EncodeJobRunnerWorker::on_log(const utsure::core::job::EncodeJobLogMessage &message) {
    emit log_message(format_log_line(message));
}
