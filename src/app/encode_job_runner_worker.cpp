#include "encode_job_runner_worker.hpp"

#include "utsure/core/job/encode_job_report.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

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

class ScopedEncodeProcessPriority final {
public:
    static ScopedEncodeProcessPriority apply(
        const utsure::core::job::EncodeJobProcessPriority priority
    ) noexcept {
        ScopedEncodeProcessPriority scope{};
        scope.apply_message_ = "Process priority: " + std::string(utsure::core::job::to_display_string(priority)) + '.';

#ifdef _WIN32
        const DWORD desired_priority_class = map_priority_class(priority);
        const DWORD current_priority_class = GetPriorityClass(GetCurrentProcess());
        if (current_priority_class == 0) {
            scope.apply_message_ =
                "Process priority '" + std::string(utsure::core::job::to_display_string(priority)) +
                "' could not read the current process priority; continuing without changing it.";
            return scope;
        }

        if (current_priority_class == desired_priority_class) {
            scope.apply_message_ += " Already active.";
            return scope;
        }

        if (!SetPriorityClass(GetCurrentProcess(), desired_priority_class)) {
            scope.apply_message_ =
                "Process priority '" + std::string(utsure::core::job::to_display_string(priority)) +
                "' could not be applied; continuing at the current process priority.";
            return scope;
        }

        scope.previous_priority_class_ = current_priority_class;
        scope.priority_changed_ = true;
        if (priority == utsure::core::job::EncodeJobProcessPriority::low) {
            scope.apply_message_ = "Process priority: Low (applied as Windows Idle).";
        } else {
            scope.apply_message_ += " Applied.";
        }
#else
        scope.apply_message_ += " Priority control is unavailable on this platform.";
#endif

        return scope;
    }

    ScopedEncodeProcessPriority(const ScopedEncodeProcessPriority &) = delete;
    ScopedEncodeProcessPriority &operator=(const ScopedEncodeProcessPriority &) = delete;

    ScopedEncodeProcessPriority(ScopedEncodeProcessPriority &&other) noexcept
        : previous_priority_class_(other.previous_priority_class_),
          priority_changed_(other.priority_changed_),
          apply_message_(std::move(other.apply_message_)) {
        other.previous_priority_class_.reset();
        other.priority_changed_ = false;
    }

    ScopedEncodeProcessPriority &operator=(ScopedEncodeProcessPriority &&other) noexcept {
        if (this == &other) {
            return *this;
        }

        restore();
        previous_priority_class_ = other.previous_priority_class_;
        priority_changed_ = other.priority_changed_;
        apply_message_ = std::move(other.apply_message_);
        other.previous_priority_class_.reset();
        other.priority_changed_ = false;
        return *this;
    }

    ~ScopedEncodeProcessPriority() {
        restore();
    }

    [[nodiscard]] const std::string &apply_message() const noexcept {
        return apply_message_;
    }

    [[nodiscard]] bool priority_changed() const noexcept {
        return priority_changed_;
    }

    std::optional<std::string> restore() noexcept {
#ifdef _WIN32
        if (!priority_changed_ || !previous_priority_class_.has_value()) {
            return std::nullopt;
        }

        const DWORD restore_priority_class = *previous_priority_class_;
        previous_priority_class_.reset();
        priority_changed_ = false;
        if (!SetPriorityClass(GetCurrentProcess(), restore_priority_class)) {
            return std::string(
                "The previous process priority could not be restored after encode. "
                "The current process may keep the encode priority until exit."
            );
        }
#endif

        return std::nullopt;
    }

private:
    ScopedEncodeProcessPriority() = default;

#ifdef _WIN32
    static DWORD map_priority_class(const utsure::core::job::EncodeJobProcessPriority priority) noexcept {
        switch (priority) {
        case utsure::core::job::EncodeJobProcessPriority::high:
            return HIGH_PRIORITY_CLASS;
        case utsure::core::job::EncodeJobProcessPriority::above_normal:
            return ABOVE_NORMAL_PRIORITY_CLASS;
        case utsure::core::job::EncodeJobProcessPriority::normal:
            return NORMAL_PRIORITY_CLASS;
        case utsure::core::job::EncodeJobProcessPriority::below_normal:
            return BELOW_NORMAL_PRIORITY_CLASS;
        case utsure::core::job::EncodeJobProcessPriority::low:
            return IDLE_PRIORITY_CLASS;
        default:
            return BELOW_NORMAL_PRIORITY_CLASS;
        }
    }
#endif

    std::optional<unsigned long> previous_priority_class_{};
    bool priority_changed_{false};
    std::string apply_message_{};
};

}  // namespace

EncodeJobRunnerWorker::EncodeJobRunnerWorker(QObject *parent) : QObject(parent) {}

void EncodeJobRunnerWorker::run_job(const utsure::core::job::EncodeJob &job) {
    auto priority_scope = ScopedEncodeProcessPriority::apply(job.execution.process_priority);
    emit log_message(QString("[info] %1").arg(to_qstring(priority_scope.apply_message())));

    const auto result = utsure::core::job::EncodeJobRunner::run(job, utsure::core::job::EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = this
    });
    const bool priority_was_changed = priority_scope.priority_changed();
    if (const auto restore_message = priority_scope.restore(); restore_message.has_value()) {
        emit log_message(QString("[warning] %1").arg(to_qstring(*restore_message)));
    } else if (priority_was_changed) {
        emit log_message("[info] Process priority restored.");
    }

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
