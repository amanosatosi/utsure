#include "queue_terminal_notification.hpp"

#include <utility>

namespace utsure::app {

void QueueTerminalNotificationTracker::begin(
    const quint64 run_id,
    const std::vector<int> &planned_job_indices
) {
    abandon();
    run_id_ = run_id;
    planned_job_indices_.insert(planned_job_indices.begin(), planned_job_indices.end());
    active_ = run_id_ != 0 && !planned_job_indices_.empty();
}

void QueueTerminalNotificationTracker::record_job_result(
    const int job_index,
    const bool succeeded,
    const bool canceled,
    const QString &output_path
) {
    if (!active_ || !planned_job_indices_.contains(job_index) || recorded_job_indices_.contains(job_index)) {
        return;
    }

    recorded_job_indices_.insert(job_index);
    if (succeeded) {
        ++successful_job_count_;
        const QString normalized_output_path = output_path.trimmed();
        if (!normalized_output_path.isEmpty() && !completed_output_paths_.contains(normalized_output_path)) {
            completed_output_paths_.push_back(normalized_output_path);
        }
        return;
    }

    if (!canceled) {
        failed_ = true;
        if (failure_summary_.isEmpty()) {
            failure_summary_ = "One or more encode jobs failed";
        }
    }
}

void QueueTerminalNotificationTracker::mark_run_failure(const QString &summary) {
    if (!active_) {
        return;
    }

    failed_ = true;
    const QString normalized_summary = summary.trimmed();
    if (!normalized_summary.isEmpty()) {
        failure_summary_ = normalized_summary;
    }
}

std::optional<QueueTerminalNotificationData> QueueTerminalNotificationTracker::finish(const bool stopped) {
    if (!active_) {
        return std::nullopt;
    }

    QueueTerminalNotificationData data{
        .run_id = run_id_,
        .outcome = failed_ ? QueueTerminalNotificationOutcome::failed
                           : QueueTerminalNotificationOutcome::succeeded,
        .total_job_count = static_cast<int>(planned_job_indices_.size()),
        .successful_job_count = successful_job_count_,
        .completed_output_paths = completed_output_paths_,
        .failure_summary = failure_summary_
    };
    const bool notify = failed_ || (!stopped && successful_job_count_ == data.total_job_count);
    abandon();
    return notify ? std::optional<QueueTerminalNotificationData>(std::move(data)) : std::nullopt;
}

void QueueTerminalNotificationTracker::abandon() noexcept {
    run_id_ = 0;
    planned_job_indices_.clear();
    recorded_job_indices_.clear();
    completed_output_paths_.clear();
    failure_summary_.clear();
    successful_job_count_ = 0;
    active_ = false;
    failed_ = false;
}

bool QueueTerminalNotificationTracker::active() const noexcept {
    return active_;
}

}  // namespace utsure::app
