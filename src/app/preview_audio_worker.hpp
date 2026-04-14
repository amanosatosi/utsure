#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace utsure::core::media {
class AudioPreviewSession;
}

struct PreviewAudioChunkRequest final {
    quint64 request_token{0};
    QString source_path{};
    qint64 requested_time_us{0};
    qint64 trim_in_us{0};
    std::optional<qint64> trim_out_us{};
    bool reset_session{true};
    int sample_rate{0};
    int channel_count{0};
    QAudioFormat::SampleFormat sample_format{QAudioFormat::SampleFormat::Unknown};
    int block_samples{1024};
    qint64 minimum_duration_us{250000};
};

class PreviewAudioWorker final : public QObject {
    Q_OBJECT

public:
    explicit PreviewAudioWorker(QObject *parent = nullptr);
    ~PreviewAudioWorker() override;

    void decode_chunk(const PreviewAudioChunkRequest &request);
    void clear_session();

signals:
    void audio_chunk_ready(
        quint64 request_token,
        qint64 chunk_start_us,
        qint64 chunk_end_us,
        QByteArray pcm_bytes,
        bool exhausted
    );
    void audio_chunk_failed(quint64 request_token, QString detail);

private:
    void invalidate_cached_session();

    std::unique_ptr<utsure::core::media::AudioPreviewSession> audio_session_{};
    QString cached_source_path_{};
    int cached_sample_rate_{0};
    int cached_channel_count_{0};
    int cached_block_samples_{0};
};
