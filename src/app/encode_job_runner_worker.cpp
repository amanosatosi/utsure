#include "encode_job_runner_worker.hpp"

#include "utsure/core/job/encode_job_report.hpp"

#include <sstream>
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
    QString details = QString("Main source: %1\nOutput: %2\nProblem: %3")
        .arg(to_qstring(error.main_source_path))
        .arg(to_qstring(error.output_path))
        .arg(to_qstring(error.message));

    if (!error.actionable_hint.empty()) {
        details += QString("\nWhat to do next: %1").arg(to_qstring(error.actionable_hint));
    }

    return details;
}

QString format_success_details(const utsure::core::job::EncodeJobSummary &summary) {
    std::ostringstream readable_summary;
    readable_summary
        << "Output file: " << summary.encoded_media_summary.output_path.lexically_normal().string() << '\n'
        << "Codec: " << utsure::core::media::to_string(summary.job.output.video.codec) << '\n'
        << "Preset / CRF: " << summary.job.output.video.preset << " / " << summary.job.output.video.crf << '\n'
        << "Timeline segments: " << summary.timeline_summary.segments.size() << '\n'
        << "Output frame rate: " << summary.timeline_summary.output_frame_rate.numerator
        << '/' << summary.timeline_summary.output_frame_rate.denominator << '\n'
        << "Decoded video frames: " << summary.decoded_video_frame_count << '\n'
        << "Decoded audio blocks: " << summary.decoded_audio_block_count << '\n'
        << "Subtitled frames: " << summary.subtitled_video_frame_count << "\n\n"
        << "Detailed report:\n"
        << utsure::core::job::format_encode_job_report(summary);

    return to_qstring(readable_summary.str());
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
            format_success_details(*result.encode_job_summary)
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
    emit progress_changed(progress);
}

void EncodeJobRunnerWorker::on_log(const utsure::core::job::EncodeJobLogMessage &message) {
    emit log_message(format_log_line(message));
}
