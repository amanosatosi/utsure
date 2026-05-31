#include "encode_job_runner_worker.hpp"

#include "utsure/core/job/encode_job_report.hpp"

#include <optional>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString path_to_qstring(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto text = path.lexically_normal().u8string();
    return QString::fromUtf8(reinterpret_cast<const char *>(text.c_str()), static_cast<qsizetype>(text.size()));
#else
    return QString::fromStdString(path.lexically_normal().string());
#endif
}

std::string path_to_utf8_string(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto text = path.lexically_normal().u8string();
    return std::string(reinterpret_cast<const char *>(text.c_str()), text.size());
#else
    return path.lexically_normal().string();
#endif
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
        << "Output file: " << path_to_utf8_string(summary.encoded_media_summary.output_path) << '\n'
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

void throw_if_canceled(const bool cancel_requested) {
    if (cancel_requested) {
        throw std::runtime_error(std::string(utsure::core::job::kEncodeJobCanceledException));
    }
}

}  // namespace

EncodeJobRunnerWorker::EncodeJobRunnerWorker(QObject *parent) : QObject(parent) {}

void EncodeJobRunnerWorker::run_job(const utsure::core::job::EncodeJob &job) {
    try {
        const auto result = utsure::core::job::EncodeJobRunner::run(job, utsure::core::job::EncodeJobRunOptions{
            .decode_normalization_policy = {},
            .observer = this
        });

        if (result.succeeded()) {
            emit job_finished(
                true,
                false,
                "Encode completed successfully.",
                format_success_details(*result.encode_job_summary),
                path_to_qstring(result.encode_job_summary->encoded_media_summary.output_path)
            );
            return;
        }

        const bool canceled = result.error->canceled;
        emit job_finished(
            false,
            canceled,
            canceled ? "Encode canceled." : QString("Encode failed: %1").arg(to_qstring(result.error->message)),
            format_error_details(*result.error),
            to_qstring(result.error->output_path)
        );
    } catch (const std::exception &exception) {
        const auto main_source_path = job.input.main_source_path.lexically_normal().string();
        const auto output_path = job.output.output_path.lexically_normal().string();
        const QString problem = QString("Encode failed: The encode worker caught an unexpected runtime failure.");
        const QString details = QString("Main source: %1\nOutput: %2\nProblem: %3")
            .arg(to_qstring(main_source_path))
            .arg(to_qstring(output_path))
            .arg(to_qstring(exception.what()));
        emit job_finished(false, false, problem, details, to_qstring(output_path));
    } catch (...) {
        const auto main_source_path = job.input.main_source_path.lexically_normal().string();
        const auto output_path = job.output.output_path.lexically_normal().string();
        const QString problem = QString("Encode failed: The encode worker caught an unknown runtime failure.");
        const QString details = QString("Main source: %1\nOutput: %2\nProblem: Unknown non-standard exception.")
            .arg(to_qstring(main_source_path))
            .arg(to_qstring(output_path));
        emit job_finished(false, false, problem, details, to_qstring(output_path));
    }
}

void EncodeJobRunnerWorker::request_cancel() noexcept {
    cancel_requested_.store(true);
}

void EncodeJobRunnerWorker::clear_cancel_request() noexcept {
    cancel_requested_.store(false);
}

bool EncodeJobRunnerWorker::cancel_requested() const noexcept {
    return cancel_requested_.load();
}

void EncodeJobRunnerWorker::on_progress(const utsure::core::job::EncodeJobProgress &progress) {
    throw_if_canceled(cancel_requested());
    emit progress_changed(progress);
}

void EncodeJobRunnerWorker::on_log(const utsure::core::job::EncodeJobLogMessage &message) {
    throw_if_canceled(cancel_requested());
    emit log_message(format_log_line(message));
}
