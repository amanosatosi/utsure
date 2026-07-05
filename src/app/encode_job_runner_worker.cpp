#include "encode_job_runner_worker.hpp"

#include "crash_dump_writer.hpp"
#include "utsure/core/filesystem/path_format.hpp"
#include "utsure/core/job/encode_job_report.hpp"

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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
    return utsure::core::filesystem::path_to_utf8_string(path);
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
        << "Output file: "
        << utsure::core::filesystem::path_to_utf8_string(summary.encoded_media_summary.output_path)
        << '\n'
        << "Codec: " << utsure::core::media::to_string(summary.job.output.video.codec) << '\n'
        << "Preset / CRF: " << summary.job.output.video.preset << " / " << summary.job.output.video.crf << '\n'
        << "Timeline segments: " << summary.timeline_summary.segments.size() << '\n'
        << "Output frame rate: " << summary.timeline_summary.output_frame_rate.numerator
        << '/' << summary.timeline_summary.output_frame_rate.denominator << '\n'
        << "Decoded video frames: " << summary.decoded_video_frame_count << '\n'
        << "Decoded audio blocks: " << summary.decoded_audio_block_count << '\n'
        << "Subtitled frames: " << summary.subtitled_video_frame_count << '\n';
    if (!summary.warnings.empty()) {
        readable_summary << '\n' << "Warnings:\n";
        for (const auto &warning : summary.warnings) {
            readable_summary << "- " << warning << '\n';
        }
    }
    readable_summary << '\n';
    readable_summary << "Detailed report:\n"
        << utsure::core::job::format_encode_job_report(summary);

    return to_qstring(readable_summary.str());
}

void throw_if_canceled(const bool cancel_requested) {
    if (cancel_requested) {
        throw std::runtime_error(std::string(utsure::core::job::kEncodeJobCanceledException));
    }
}

void update_crash_context_safely(const utsure::app::crash::CrashContextUpdate &update) noexcept {
    try {
        utsure::app::crash::update_crash_context(update);
    } catch (...) {
    }
}

void update_crash_progress_safely(const utsure::core::job::EncodeJobProgress &progress) noexcept {
    try {
        utsure::app::crash::update_crash_context_from_progress(progress);
    } catch (...) {
    }
}

void update_crash_log_safely(const std::string &message) noexcept {
    try {
        utsure::app::crash::update_crash_context_from_runtime_log(message);
    } catch (...) {
    }
}

}  // namespace

EncodeJobRunnerWorker::EncodeJobRunnerWorker(QObject *parent)
    : QObject(parent),
      run_function_([](const utsure::core::job::EncodeJob &job,
                       const utsure::core::job::EncodeJobRunOptions &options) {
          return utsure::core::job::EncodeJobRunner::run(job, options);
      }) {}

EncodeJobRunnerWorker::EncodeJobRunnerWorker(RunFunction run_function, QObject *parent)
    : QObject(parent),
      run_function_(std::move(run_function)) {
    if (!run_function_) {
        run_function_ = [](const utsure::core::job::EncodeJob &job,
                           const utsure::core::job::EncodeJobRunOptions &options) {
            return utsure::core::job::EncodeJobRunner::run(job, options);
        };
    }
}

void EncodeJobRunnerWorker::run_job(const utsure::core::job::EncodeJob &job, const int runner_slot_index) {
    active_.store(true);
    runner_slot_index_.store(runner_slot_index);
    last_progress_.reset();
    const int active_count = utsure::app::crash::begin_active_encode_job(runner_slot_index);
    update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
        .runner_slot_index = runner_slot_index,
        .active_job_count = active_count,
        .input_path = path_to_utf8_string(job.input.main_source_path),
        .output_path = path_to_utf8_string(job.output.output_path),
        .video_output_codec = utsure::core::media::to_string(job.output.video.codec),
        .current_stage = "worker_run_job",
        .subtitle_enabled = job.subtitles.has_value(),
        .cancellation_requested = false
    });
    struct ActiveGuard final {
        std::atomic_bool &active;
        std::atomic_int &runner_slot;
        ~ActiveGuard() {
            active.store(false);
            (void)utsure::app::crash::end_active_encode_job(runner_slot.load());
            runner_slot.store(-1);
        }
    } active_guard{active_, runner_slot_index_};
    try {
        const auto result = run_function_(job, utsure::core::job::EncodeJobRunOptions{
            .decode_normalization_policy = {},
            .observer = this,
            .cancellation_requested = [this]() {
                return cancel_requested();
            },
            .crash_context_callback = [](const std::string &message) {
                update_crash_log_safely(message);
            }
        });

        if (result.succeeded()) {
            update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
                .output_path = path_to_utf8_string(result.encode_job_summary->encoded_media_summary.output_path),
                .current_stage = "completed",
                .cancellation_requested = false
            });
            emit job_finished(
                true,
                false,
                "Encode completed successfully.",
                format_success_details(*result.encode_job_summary),
                path_to_qstring(result.encode_job_summary->encoded_media_summary.output_path)
            );
            std::fflush(stderr);
            std::fflush(stdout);
            return;
        }

        const bool canceled = result.error->canceled;
        update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
            .current_stage = canceled ? std::string("canceled") : std::string("handled_failure"),
            .cancellation_requested = canceled
        });
        emit job_finished(
            false,
            canceled,
            canceled ? "Encode canceled." : QString("Encode failed: %1").arg(to_qstring(result.error->message)),
            format_error_details(*result.error),
            to_qstring(result.error->output_path)
        );
        std::fflush(stderr);
        std::fflush(stdout);
    } catch (const std::exception &exception) {
        update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
            .current_stage = "worker_exception",
            .last_log_message = exception.what()
        });
        const auto main_source_path = path_to_qstring(job.input.main_source_path);
        const auto output_path = path_to_qstring(job.output.output_path);
        const QString problem = QString("Encode failed: The encode worker caught an unexpected runtime failure.");
        const QString details = QString("Main source: %1\nOutput: %2\nProblem: %3")
            .arg(main_source_path)
            .arg(output_path)
            .arg(to_qstring(exception.what())) +
            format_last_progress_context();
        emit job_finished(false, false, problem, details, output_path);
        std::fflush(stderr);
        std::fflush(stdout);
    } catch (...) {
        update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
            .current_stage = "worker_unknown_exception"
        });
        const auto main_source_path = path_to_qstring(job.input.main_source_path);
        const auto output_path = path_to_qstring(job.output.output_path);
        const QString problem = QString("Encode failed: The encode worker caught an unknown runtime failure.");
        const QString details = QString("Main source: %1\nOutput: %2\nProblem: Unknown non-standard exception.")
            .arg(main_source_path)
            .arg(output_path) +
            format_last_progress_context();
        emit job_finished(false, false, problem, details, output_path);
        std::fflush(stderr);
        std::fflush(stdout);
    }
}

void EncodeJobRunnerWorker::request_cancel() noexcept {
    cancel_requested_.store(true);
    update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
        .runner_slot_index = runner_slot_index_.load(),
        .cancellation_requested = true
    });
}

void EncodeJobRunnerWorker::clear_cancel_request() noexcept {
    cancel_requested_.store(false);
    update_crash_context_safely(utsure::app::crash::CrashContextUpdate{
        .runner_slot_index = runner_slot_index_.load(),
        .cancellation_requested = false
    });
}

bool EncodeJobRunnerWorker::cancel_requested() const noexcept {
    return cancel_requested_.load();
}

bool EncodeJobRunnerWorker::is_active() const noexcept {
    return active_.load();
}

QString EncodeJobRunnerWorker::format_last_progress_context() const {
    if (!last_progress_.has_value()) {
        return {};
    }

    QString details = QString("\nLast progress: stage=%1")
        .arg(to_qstring(utsure::core::job::to_string(last_progress_->stage)));
    if (last_progress_->encoded_video_frames.has_value()) {
        details += QString(", encoded_frames=%1").arg(QString::number(*last_progress_->encoded_video_frames));
    }
    if (last_progress_->encoded_video_duration_us.has_value()) {
        details += QString(", encoded_duration_us=%1").arg(QString::number(*last_progress_->encoded_video_duration_us));
    }
    return details;
}

void EncodeJobRunnerWorker::on_progress(const utsure::core::job::EncodeJobProgress &progress) {
    throw_if_canceled(cancel_requested());
    last_progress_ = progress;
    update_crash_progress_safely(progress);
    emit progress_changed(progress);
}

void EncodeJobRunnerWorker::on_log(const utsure::core::job::EncodeJobLogMessage &message) {
    throw_if_canceled(cancel_requested());
    update_crash_log_safely(message.message);
    emit log_message(format_log_line(message));
}
