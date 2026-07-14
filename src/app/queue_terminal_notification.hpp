#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace utsure::app {

enum class QueueTerminalNotificationOutcome : std::uint8_t {
    succeeded = 0,
    failed
};

struct QueueTerminalNotificationData final {
    quint64 run_id{0};
    QueueTerminalNotificationOutcome outcome{QueueTerminalNotificationOutcome::succeeded};
    int total_job_count{0};
    int successful_job_count{0};
    QStringList completed_output_paths{};
    QString failure_summary{};
};

class QueueTerminalNotificationTracker final {
public:
    void begin(quint64 run_id, const std::vector<int> &planned_job_indices);
    void record_job_result(int job_index, bool succeeded, bool canceled, const QString &output_path);
    void mark_run_failure(const QString &summary);
    [[nodiscard]] std::optional<QueueTerminalNotificationData> finish(bool stopped);
    void abandon() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    quint64 run_id_{0};
    std::unordered_set<int> planned_job_indices_{};
    std::unordered_set<int> recorded_job_indices_{};
    QStringList completed_output_paths_{};
    QString failure_summary_{};
    int successful_job_count_{0};
    bool active_{false};
    bool failed_{false};
};

}  // namespace utsure::app
