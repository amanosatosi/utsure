#pragma once

#include "preview_audio_worker.hpp"
#include "utsure/core/media/media_info.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QObject>
#include <QThread>

#include <optional>

class QAudioSink;
class QIODevice;
class QTimer;

struct PreviewAudioPlaybackRequest final {
    QString source_path{};
    qint64 requested_time_us{0};
    utsure::core::media::AudioStreamInfo source_audio_stream_info{};
    qint64 chunk_duration_us{250000};
    int block_samples{1024};
};

class PreviewAudioController final : public QObject {
    Q_OBJECT

public:
    explicit PreviewAudioController(QObject *parent = nullptr);
    ~PreviewAudioController() override;

    [[nodiscard]] bool start_preview(const PreviewAudioPlaybackRequest &request);
    void stop_preview();
    void clear();

    [[nodiscard]] bool is_audio_playing() const noexcept;
    [[nodiscard]] qint64 current_playback_time_us() const noexcept;

signals:
    void preview_audio_failed(QString detail);

private:
    struct ResolvedOutputFormat final {
        QAudioDevice device{};
        QAudioFormat format{};
    };

    [[nodiscard]] std::optional<PreviewAudioChunkRequest> build_chunk_request(bool reset_session) const;
    [[nodiscard]] std::optional<ResolvedOutputFormat> resolve_output_format(
        const utsure::core::media::AudioStreamInfo &source_audio_stream_info
    ) const;
    void destroy_audio_output_path();
    void recreate_audio_sink();
    void request_audio_chunk(bool reset_session);
    void reset_playback_state(bool clear_worker_session);
    void handle_audio_chunk_ready(
        quint64 request_token,
        qint64 chunk_start_us,
        qint64 chunk_end_us,
        const QByteArray &pcm_bytes,
        bool exhausted
    );
    void handle_audio_chunk_failed(quint64 request_token, const QString &detail);
    void handle_refill_tick();

    QThread worker_thread_{};
    PreviewAudioWorker *worker_{nullptr};
    QIODevice *audio_buffer_device_{nullptr};
    QAudioSink *audio_sink_{nullptr};
    QTimer *refill_timer_{nullptr};
    quint64 request_token_{0};
    bool playback_active_{false};
    bool request_in_flight_{false};
    bool audio_sink_started_{false};
    bool end_of_stream_{false};
    qint64 playback_anchor_time_us_{0};
    qint64 next_chunk_time_us_{0};
    qint64 chunk_duration_us_{250000};
    qint64 low_watermark_bytes_{0};
    qint64 target_buffer_bytes_{0};
    int block_samples_{1024};
    QString source_path_{};
    QAudioDevice audio_device_{};
    QAudioFormat audio_format_{};
    int sample_rate_{0};
    int channel_count_{0};
    QAudioFormat::SampleFormat sample_format_{QAudioFormat::SampleFormat::Unknown};
};
