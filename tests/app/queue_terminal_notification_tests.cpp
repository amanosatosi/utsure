#include "queue_terminal_notification.hpp"

#include <QFile>

#include <cassert>
#include <vector>

int main() {
    Q_INIT_RESOURCE(app_resources);

    using utsure::app::QueueTerminalNotificationOutcome;
    using utsure::app::QueueTerminalNotificationTracker;

    QueueTerminalNotificationTracker tracker{};
    tracker.begin(1, std::vector<int>{2});
    tracker.record_job_result(2, true, false, "C:/encodes/episode-01.mp4");
    tracker.record_job_result(2, true, false, "C:/encodes/duplicate.mp4");
    const auto single_success = tracker.finish(false);
    assert(single_success.has_value());
    assert(single_success->outcome == QueueTerminalNotificationOutcome::succeeded);
    assert(single_success->total_job_count == 1);
    assert(single_success->successful_job_count == 1);
    assert(single_success->completed_output_paths == QStringList{"C:/encodes/episode-01.mp4"});
    assert(!tracker.finish(false).has_value());

    tracker.begin(2, std::vector<int>{4, 7, 9});
    tracker.record_job_result(7, true, false, "C:/encodes/episode-02.mp4");
    tracker.record_job_result(4, true, false, "C:/encodes/episode-01.mp4");
    tracker.record_job_result(9, true, false, "D:/other/episode-03.mp4");
    const auto multi_success = tracker.finish(false);
    assert(multi_success.has_value());
    assert(multi_success->total_job_count == 3);
    assert(multi_success->successful_job_count == 3);
    assert(multi_success->completed_output_paths.size() == 3);

    tracker.begin(3, std::vector<int>{1, 3});
    tracker.record_job_result(1, false, false, "C:/encodes/broken.mp4");
    tracker.record_job_result(3, true, false, "C:/encodes/later.mp4");
    const auto failed_after_continuing = tracker.finish(false);
    assert(failed_after_continuing.has_value());
    assert(failed_after_continuing->outcome == QueueTerminalNotificationOutcome::failed);
    assert(failed_after_continuing->successful_job_count == 1);
    assert(failed_after_continuing->failure_summary == "One or more encode jobs failed");

    tracker.begin(4, std::vector<int>{5, 6});
    tracker.record_job_result(5, false, true, QString{});
    assert(!tracker.finish(true).has_value());

    tracker.begin(5, std::vector<int>{8});
    tracker.mark_run_failure("Encode worker failure stopped the queue");
    const auto forced_failure = tracker.finish(true);
    assert(forced_failure.has_value());
    assert(forced_failure->outcome == QueueTerminalNotificationOutcome::failed);
    assert(forced_failure->failure_summary == "Encode worker failure stopped the queue");

    assert(QFile::exists(QStringLiteral(":/audio/\u6C7A\u5B9A\u30DC\u30BF\u30F3\u3092\u62BC\u305941.mp3")));
    assert(QFile::exists(QStringLiteral(":/audio/\u30D3\u30FC\u30D7\u97F34.mp3")));
    assert(QFile::exists(QStringLiteral(":/images/\u305A\u3093\u3060\u3082\u3093\u30FC\u732B\u3060\u3063\u30531.png")));
    assert(QFile::exists(QStringLiteral(":/images/\u305A\u3093\u3060\u3082\u3093\u30FC\u843D\u3061\u8FBC\u308012.png")));
    assert(QFile::exists(":/icons/logo.svg"));

    return 0;
}
