#include "queue_terminal_notification.hpp"

#include <QByteArray>
#include <QFile>

#include <cassert>

namespace {

quint16 read_little_endian_u16(const QByteArray &bytes, const qsizetype offset) {
    assert(offset >= 0 && offset + 2 <= bytes.size());
    return static_cast<quint16>(static_cast<unsigned char>(bytes[offset])) |
        static_cast<quint16>(static_cast<unsigned char>(bytes[offset + 1]) << 8U);
}

quint32 read_little_endian_u32(const QByteArray &bytes, const qsizetype offset) {
    assert(offset >= 0 && offset + 4 <= bytes.size());
    return static_cast<quint32>(static_cast<unsigned char>(bytes[offset])) |
        (static_cast<quint32>(static_cast<unsigned char>(bytes[offset + 1])) << 8U) |
        (static_cast<quint32>(static_cast<unsigned char>(bytes[offset + 2])) << 16U) |
        (static_cast<quint32>(static_cast<unsigned char>(bytes[offset + 3])) << 24U);
}

void assert_notification_wav(const QString &resource_path) {
    QFile wav_file(resource_path);
    assert(wav_file.open(QIODevice::ReadOnly));
    const QByteArray header = wav_file.read(44);
    assert(header.size() == 44);
    assert(header.mid(0, 4) == "RIFF");
    assert(header.mid(8, 4) == "WAVE");
    assert(header.mid(12, 4) == "fmt ");
    assert(read_little_endian_u16(header, 20) == 1);      // PCM
    assert(read_little_endian_u16(header, 22) == 1);      // mono
    assert(read_little_endian_u32(header, 24) == 48000);  // Hz
    assert(read_little_endian_u16(header, 34) == 16);     // bits per sample
}

}  // namespace

int main() {
    Q_INIT_RESOURCE(app_resources);

    using utsure::app::JobTerminalNotificationData;
    using utsure::app::JobTerminalNotificationOutcome;
    using utsure::app::JobTerminalNotificationTracker;

    JobTerminalNotificationTracker tracker{};
    const quint64 first_run = tracker.begin_job_run();
    const quint64 second_run = tracker.begin_job_run();
    assert(first_run != 0);
    assert(second_run != 0);
    assert(first_run != second_run);
    assert(tracker.claim_terminal(first_run));
    assert(!tracker.claim_terminal(first_run));
    assert(tracker.claim_terminal(second_run));
    assert(!tracker.claim_terminal(0));

    // A later run receives a fresh identity even when it represents the same source file.
    const quint64 rerun = tracker.begin_job_run();
    assert(rerun != first_run);
    assert(tracker.claim_terminal(rerun));
    for (int index = 0; index < 300; ++index) {
        assert(tracker.claim_terminal(tracker.begin_job_run()));
    }
    assert(!tracker.claim_terminal(first_run));

    const JobTerminalNotificationData completed_job{
        .run_id = first_run,
        .outcome = JobTerminalNotificationOutcome::succeeded,
        .job_display_name = "Episode 01.mkv",
        .output_path = "C:/encodes/Episode 01.mp4"
    };
    assert(completed_job.job_display_name == "Episode 01.mkv");
    assert(completed_job.output_path == "C:/encodes/Episode 01.mp4");

    assert(QFile::exists(QStringLiteral(":/audio/\u6C7A\u5B9A\u30DC\u30BF\u30F3\u3092\u62BC\u305941.wav")));
    assert(QFile::exists(QStringLiteral(":/audio/\u30D3\u30FC\u30D7\u97F34.wav")));
    assert_notification_wav(QStringLiteral(":/audio/\u6C7A\u5B9A\u30DC\u30BF\u30F3\u3092\u62BC\u305941.wav"));
    assert_notification_wav(QStringLiteral(":/audio/\u30D3\u30FC\u30D7\u97F34.wav"));
    assert(QFile::exists(QStringLiteral(":/images/\u305A\u3093\u3060\u3082\u3093\u30FC\u732B\u3060\u3063\u30531.png")));
    assert(QFile::exists(QStringLiteral(":/images/\u305A\u3093\u3060\u3082\u3093\u30FC\u843D\u3061\u8FBC\u308012.png")));
    assert(QFile::exists(":/icons/logo.svg"));

    return 0;
}
