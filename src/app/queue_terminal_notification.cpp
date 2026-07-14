#include "queue_terminal_notification.hpp"

#include <limits>

namespace utsure::app {

quint64 JobTerminalNotificationTracker::begin_job_run() noexcept {
    const quint64 run_id = next_run_id_;
    if (next_run_id_ == std::numeric_limits<quint64>::max()) {
        next_run_id_ = 1;
    } else {
        ++next_run_id_;
    }
    return run_id;
}

bool JobTerminalNotificationTracker::claim_terminal(const quint64 run_id) {
    if (run_id == 0 || claimed_terminal_run_ids_.contains(run_id)) {
        return false;
    }

    claimed_terminal_run_ids_.insert(run_id);
    return true;
}

}  // namespace utsure::app
