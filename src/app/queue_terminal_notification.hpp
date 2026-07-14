#pragma once

#include <QString>

#include <cstdint>
#include <unordered_set>

namespace utsure::app {

enum class JobTerminalNotificationOutcome : std::uint8_t {
    succeeded = 0,
    failed
};

struct JobTerminalNotificationData final {
    quint64 run_id{0};
    JobTerminalNotificationOutcome outcome{JobTerminalNotificationOutcome::succeeded};
    QString job_display_name{};
    QString output_path{};
    QString failure_summary{};
};

class JobTerminalNotificationTracker final {
public:
    [[nodiscard]] quint64 begin_job_run() noexcept;
    [[nodiscard]] bool claim_terminal(quint64 run_id);

private:
    quint64 next_run_id_{1};
    std::unordered_set<quint64> claimed_terminal_run_ids_{};
};

}  // namespace utsure::app
