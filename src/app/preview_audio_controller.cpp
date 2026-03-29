#include "preview_audio_controller.hpp"

#include <QAudio>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

Q_LOGGING_CATEGORY(previewAudioControllerLog, "utsure.preview.audio.controller")

class PreviewAudioBufferDevice final : public QIODevice {
public:
    explicit PreviewAudioBufferDevice(QObject *parent = nullptr) : QIODevice(parent) {
        open(QIODevice::ReadOnly);
    }

    [[nodiscard]] bool isSequential() const override {
        return true;
    }

    void clear_buffer() {
        QMutexLocker locker(&mutex_);
        buffer_.clear();
        read_offset_ = 0;
    }

    void append_bytes(const QByteArray &pcm_bytes) {
        if (pcm_bytes.isEmpty()) {
            return;
        }

        {
            QMutexLocker locker(&mutex_);
            compact_unlocked();
            buffer_.append(pcm_bytes);
        }
        emit readyRead();
    }

    [[nodiscard]] qint64 buffered_byte_count() const {
        QMutexLocker locker(&mutex_);
        return static_cast<qint64>(std::max<qsizetype>(buffer_.size() - read_offset_, 0));
    }

    [[nodiscard]] qint64 bytesAvailable() const override {
        return buffered_byte_count() + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 max_size) override {
        if (data == nullptr || max_size <= 0) {
            return 0;
        }

        QMutexLocker locker(&mutex_);
        const auto available_bytes = static_cast<qint64>(std::max<qsizetype>(buffer_.size() - read_offset_, 0));
        if (available_bytes <= 0) {
            return 0;
        }

        const auto bytes_to_copy = std::min(max_size, available_bytes);
        std::memcpy(
            data,
            buffer_.constData() + read_offset_,
            static_cast<std::size_t>(bytes_to_copy)
        );
        read_offset_ += static_cast<qsizetype>(bytes_to_copy);
        compact_unlocked();
        return bytes_to_copy;
    }

    qint64 writeData(const char *, qint64) override {
        return -1;
    }

private:
    void compact_unlocked() {
        if (read_offset_ <= 0) {
            return;
        }

        if (read_offset_ >= buffer_.size()) {
            buffer_.clear();
            read_offset_ = 0;
            return;
        }

        if (read_offset_ < 65536 && read_offset_ * 2 < buffer_.size()) {
            return;
        }

        buffer_.remove(0, read_offset_);
        read_offset_ = 0;
    }

    mutable QMutex mutex_{};
    QByteArray buffer_{};
    qsizetype read_offset_{0};
};

qint64 bytes_for_duration_or_default(const QAudioFormat &audio_format, const qint64 duration_us) {
    const qint64 computed_bytes = audio_format.bytesForDuration(duration_us);
    if (computed_bytes > 0) {
        return computed_bytes;
    }

    const int bytes_per_frame = audio_format.bytesPerFrame();
    const int sample_rate = audio_format.sampleRate();
    if (bytes_per_frame <= 0 || sample_rate <= 0 || duration_us <= 0) {
        return 0;
    }

    return (static_cast<qint64>(bytes_per_frame) * static_cast<qint64>(sample_rate) * duration_us) / 1000000;
}

qint64 duration_for_bytes_or_default(const QAudioFormat &audio_format, const qint64 byte_count) {
    const int bytes_per_frame = audio_format.bytesPerFrame();
    const int sample_rate = audio_format.sampleRate();
    if (bytes_per_frame <= 0 || sample_rate <= 0 || byte_count <= 0) {
        return 0;
    }

    return (byte_count * 1000000) / (static_cast<qint64>(bytes_per_frame) * static_cast<qint64>(sample_rate));
}

}  // namespace

PreviewAudioController::PreviewAudioController(QObject *parent) : QObject(parent) {
    worker_ = new PreviewAudioWorker();
    worker_->moveToThread(&worker_thread_);

    audio_buffer_device_ = new PreviewAudioBufferDevice(this);
    refill_timer_ = new QTimer(this);
    refill_timer_->setInterval(30);

    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &PreviewAudioWorker::audio_chunk_ready, this, &PreviewAudioController::handle_audio_chunk_ready);
    connect(worker_, &PreviewAudioWorker::audio_chunk_failed, this, &PreviewAudioController::handle_audio_chunk_failed);
    connect(refill_timer_, &QTimer::timeout, this, &PreviewAudioController::handle_refill_tick);

    worker_thread_.start();
}

PreviewAudioController::~PreviewAudioController() {
    reset_playback_state(false);
    worker_thread_.quit();
    worker_thread_.wait();
}

bool PreviewAudioController::start_preview(const PreviewAudioPlaybackRequest &request) {
    reset_playback_state(true);

    const QString normalized_source_path = request.source_path.trimmed();
    if (normalized_source_path.isEmpty() ||
        request.source_audio_stream_info.sample_rate <= 0 ||
        request.source_audio_stream_info.channel_count <= 0) {
        return false;
    }

    const auto resolved_output_format = resolve_output_format(request.source_audio_stream_info);
    if (!resolved_output_format.has_value()) {
        emit preview_audio_failed("Preview audio could not resolve a usable Qt output format.");
        return false;
    }

    source_path_ = normalized_source_path;
    audio_device_ = resolved_output_format->device;
    audio_format_ = resolved_output_format->format;
    sample_rate_ = audio_format_.sampleRate();
    channel_count_ = audio_format_.channelCount();
    sample_format_ = audio_format_.sampleFormat();
    block_samples_ = std::max(request.block_samples, 1);
    chunk_duration_us_ = std::max<qint64>(request.chunk_duration_us, 100000);
    playback_anchor_time_us_ = std::max<qint64>(request.requested_time_us, 0);
    next_chunk_time_us_ = playback_anchor_time_us_;
    target_buffer_bytes_ = std::max<qint64>(bytes_for_duration_or_default(audio_format_, 350000), 1);
    low_watermark_bytes_ = std::max<qint64>(bytes_for_duration_or_default(audio_format_, 125000), 1);
    playback_active_ = true;

    recreate_audio_sink();
    if (audio_sink_ == nullptr) {
        playback_active_ = false;
        emit preview_audio_failed("Preview audio could not initialize a Qt audio sink for the resolved output format.");
        return false;
    }

    request_audio_chunk(true);
    refill_timer_->start();
    return true;
}

void PreviewAudioController::stop_preview() {
    reset_playback_state(true);
}

void PreviewAudioController::clear() {
    reset_playback_state(true);
}

bool PreviewAudioController::is_audio_playing() const noexcept {
    return playback_active_ && audio_sink_ != nullptr && audio_sink_started_;
}

qint64 PreviewAudioController::current_playback_time_us() const noexcept {
    if (!playback_active_) {
        return -1;
    }

    if (audio_sink_ == nullptr || !audio_sink_started_) {
        return playback_anchor_time_us_;
    }

    const qint64 processed_audio_us = std::max<qint64>(audio_sink_->processedUSecs(), 0);
    const qint64 sink_buffer_bytes = std::max<qint64>(static_cast<qint64>(audio_sink_->bufferSize()), 0);
    const qint64 sink_bytes_free = std::clamp<qint64>(
        static_cast<qint64>(audio_sink_->bytesFree()),
        0,
        sink_buffer_bytes
    );
    const qint64 queued_sink_bytes = std::max<qint64>(sink_buffer_bytes - sink_bytes_free, 0);
    const qint64 queued_sink_duration_us = duration_for_bytes_or_default(audio_format_, queued_sink_bytes);
    const qint64 audible_audio_us = std::max<qint64>(processed_audio_us - queued_sink_duration_us, 0);
    return playback_anchor_time_us_ + audible_audio_us;
}

std::optional<PreviewAudioChunkRequest> PreviewAudioController::build_chunk_request(const bool reset_session) const {
    if (!playback_active_ ||
        source_path_.isEmpty() ||
        sample_rate_ <= 0 ||
        channel_count_ <= 0 ||
        sample_format_ == QAudioFormat::SampleFormat::Unknown) {
        return std::nullopt;
    }

    return PreviewAudioChunkRequest{
        .request_token = request_token_,
        .source_path = source_path_,
        .requested_time_us = next_chunk_time_us_,
        .reset_session = reset_session,
        .sample_rate = sample_rate_,
        .channel_count = channel_count_,
        .sample_format = sample_format_,
        .block_samples = block_samples_,
        .minimum_duration_us = chunk_duration_us_
    };
}

std::optional<PreviewAudioController::ResolvedOutputFormat> PreviewAudioController::resolve_output_format(
    const utsure::core::media::AudioStreamInfo &source_audio_stream_info
) const {
    const QAudioDevice default_output_device = QMediaDevices::defaultAudioOutput();
    if (source_audio_stream_info.sample_rate <= 0 || source_audio_stream_info.channel_count <= 0) {
        return std::nullopt;
    }

    QAudioFormat exact_float_format;
    exact_float_format.setSampleRate(source_audio_stream_info.sample_rate);
    exact_float_format.setChannelCount(source_audio_stream_info.channel_count);
    exact_float_format.setSampleFormat(QAudioFormat::SampleFormat::Float);
    if (default_output_device.isFormatSupported(exact_float_format)) {
        return ResolvedOutputFormat{
            .device = default_output_device,
            .format = exact_float_format
        };
    }

    QAudioFormat exact_int16_format = exact_float_format;
    exact_int16_format.setSampleFormat(QAudioFormat::SampleFormat::Int16);
    if (default_output_device.isFormatSupported(exact_int16_format)) {
        return ResolvedOutputFormat{
            .device = default_output_device,
            .format = exact_int16_format
        };
    }

    const QAudioFormat preferred_format = default_output_device.preferredFormat();
    if (preferred_format.sampleRate() > 0 &&
        preferred_format.channelCount() > 0 &&
        preferred_format.sampleFormat() != QAudioFormat::SampleFormat::Unknown) {
        return ResolvedOutputFormat{
            .device = default_output_device,
            .format = preferred_format
        };
    }

    return std::nullopt;
}

void PreviewAudioController::recreate_audio_sink() {
    if (audio_sink_ != nullptr) {
        audio_sink_->stop();
        delete audio_sink_;
        audio_sink_ = nullptr;
    }

    if (audio_format_.sampleRate() <= 0 ||
        audio_format_.channelCount() <= 0 ||
        audio_format_.sampleFormat() == QAudioFormat::SampleFormat::Unknown) {
        return;
    }

    audio_sink_ = new QAudioSink(audio_device_, audio_format_, this);
    audio_sink_->setBufferSize(static_cast<qsizetype>(
        std::clamp<qint64>(target_buffer_bytes_, 4096, std::numeric_limits<int>::max())
    ));
    connect(audio_sink_, &QAudioSink::stateChanged, this, [this](const QtAudio::State state) {
        if (!playback_active_ || audio_sink_ == nullptr) {
            return;
        }

        if (state == QtAudio::IdleState) {
            if (end_of_stream_ &&
                static_cast<PreviewAudioBufferDevice *>(audio_buffer_device_)->buffered_byte_count() <= 0) {
                playback_active_ = false;
                audio_sink_started_ = false;
                refill_timer_->stop();
            }
            return;
        }

        if (state == QtAudio::StoppedState && audio_sink_->error() != QtAudio::NoError) {
            const QString detail = QString("Preview audio output stopped unexpectedly (error %1).")
                .arg(static_cast<int>(audio_sink_->error()));
            emit preview_audio_failed(detail);
            playback_active_ = false;
            audio_sink_started_ = false;
            refill_timer_->stop();
            static_cast<PreviewAudioBufferDevice *>(audio_buffer_device_)->clear_buffer();
        }
    });
}

void PreviewAudioController::request_audio_chunk(const bool reset_session) {
    if (request_in_flight_) {
        return;
    }

    const auto chunk_request = build_chunk_request(reset_session);
    if (!chunk_request.has_value()) {
        return;
    }

    request_in_flight_ = true;
    qCInfo(previewAudioControllerLog).noquote()
        << QString("request_audio_chunk token=%1 reset=%2 requested=%3")
               .arg(chunk_request->request_token)
               .arg(reset_session ? "true" : "false")
               .arg(chunk_request->requested_time_us);
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, request = *chunk_request]() {
            worker->decode_chunk(request);
        },
        Qt::QueuedConnection
    );
}

void PreviewAudioController::reset_playback_state(const bool clear_worker_session) {
    ++request_token_;
    request_in_flight_ = false;
    playback_active_ = false;
    audio_sink_started_ = false;
    end_of_stream_ = false;
    playback_anchor_time_us_ = 0;
    next_chunk_time_us_ = 0;
    source_path_.clear();
    sample_rate_ = 0;
    channel_count_ = 0;
    sample_format_ = QAudioFormat::SampleFormat::Unknown;
    audio_format_ = QAudioFormat{};
    target_buffer_bytes_ = 0;
    low_watermark_bytes_ = 0;

    if (refill_timer_ != nullptr) {
        refill_timer_->stop();
    }
    if (audio_sink_ != nullptr) {
        audio_sink_->stop();
        delete audio_sink_;
        audio_sink_ = nullptr;
    }
    if (audio_buffer_device_ != nullptr) {
        static_cast<PreviewAudioBufferDevice *>(audio_buffer_device_)->clear_buffer();
    }
    if (clear_worker_session && worker_ != nullptr) {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_]() {
                worker->clear_session();
            },
            Qt::QueuedConnection
        );
    }
}

void PreviewAudioController::handle_audio_chunk_ready(
    const quint64 request_token,
    const qint64 chunk_start_us,
    const qint64 chunk_end_us,
    const QByteArray &pcm_bytes,
    const bool exhausted
) {
    request_in_flight_ = false;
    if (request_token != request_token_ || !playback_active_) {
        return;
    }

    qCInfo(previewAudioControllerLog).noquote()
        << QString("handle_audio_chunk_ready token=%1 start=%2 end=%3 bytes=%4 exhausted=%5")
               .arg(request_token)
               .arg(chunk_start_us)
               .arg(chunk_end_us)
               .arg(pcm_bytes.size())
               .arg(exhausted ? "true" : "false");

    end_of_stream_ = exhausted;
    next_chunk_time_us_ = std::max(chunk_end_us, chunk_start_us);

    if (!pcm_bytes.isEmpty() && audio_buffer_device_ != nullptr) {
        if (!audio_sink_started_) {
            playback_anchor_time_us_ = chunk_start_us;
        }
        static_cast<PreviewAudioBufferDevice *>(audio_buffer_device_)->append_bytes(pcm_bytes);
        if (audio_sink_ != nullptr && !audio_sink_started_) {
            audio_sink_->start(audio_buffer_device_);
            audio_sink_started_ = true;
        }
    }

    if (end_of_stream_ &&
        pcm_bytes.isEmpty() &&
        static_cast<PreviewAudioBufferDevice *>(audio_buffer_device_)->buffered_byte_count() <= 0) {
        playback_active_ = false;
        audio_sink_started_ = false;
        refill_timer_->stop();
        return;
    }

    if (!end_of_stream_ &&
        static_cast<PreviewAudioBufferDevice *>(audio_buffer_device_)->buffered_byte_count() < target_buffer_bytes_) {
        request_audio_chunk(false);
    }
}

void PreviewAudioController::handle_audio_chunk_failed(const quint64 request_token, const QString &detail) {
    request_in_flight_ = false;
    if (request_token != request_token_) {
        return;
    }

    qCWarning(previewAudioControllerLog).noquote()
        << QString("handle_audio_chunk_failed token=%1 detail=%2").arg(request_token).arg(detail);
    emit preview_audio_failed(detail);

    if (audio_sink_ != nullptr) {
        audio_sink_->stop();
    }
    if (audio_buffer_device_ != nullptr) {
        static_cast<PreviewAudioBufferDevice *>(audio_buffer_device_)->clear_buffer();
    }

    playback_active_ = false;
    audio_sink_started_ = false;
    end_of_stream_ = false;
    refill_timer_->stop();
}

void PreviewAudioController::handle_refill_tick() {
    if (!playback_active_ || request_in_flight_ || end_of_stream_ || audio_buffer_device_ == nullptr) {
        return;
    }

    if (static_cast<PreviewAudioBufferDevice *>(audio_buffer_device_)->buffered_byte_count() <= low_watermark_bytes_) {
        request_audio_chunk(false);
    }
}
