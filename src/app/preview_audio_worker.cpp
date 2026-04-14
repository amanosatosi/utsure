#include "preview_audio_worker.hpp"

#include "utsure/core/media/media_decoder.hpp"
#include "utsure/core/media/preview_trim.hpp"

#include <QFile>
#include <QLoggingCategory>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

Q_LOGGING_CATEGORY(previewAudioWorkerLog, "utsure.preview.audio.worker")

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::filesystem::path qstring_to_path(const QString &text) {
#ifdef _WIN32
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(text).constData());
#endif
}

QString format_audio_worker_error(std::string_view message, std::string_view actionable_hint = {}) {
    QString detail = to_qstring(message).trimmed();
    const QString hint = to_qstring(actionable_hint).trimmed();
    if (!hint.isEmpty()) {
        if (!detail.isEmpty()) {
            detail.append('\n');
        }
        detail.append(hint);
    }
    return detail;
}

int bytes_per_sample(const QAudioFormat::SampleFormat sample_format) {
    switch (sample_format) {
    case QAudioFormat::SampleFormat::UInt8:
        return 1;
    case QAudioFormat::SampleFormat::Int16:
        return 2;
    case QAudioFormat::SampleFormat::Int32:
    case QAudioFormat::SampleFormat::Float:
        return 4;
    case QAudioFormat::SampleFormat::Unknown:
    default:
        return 0;
    }
}

template <typename SampleType>
void write_sample_bytes(QByteArray &pcm_bytes, qsizetype &write_offset, const SampleType value) {
    std::memcpy(pcm_bytes.data() + write_offset, &value, sizeof(SampleType));
    write_offset += static_cast<qsizetype>(sizeof(SampleType));
}

QByteArray interleave_audio_blocks(
    const std::vector<utsure::core::media::DecodedAudioSamples> &audio_blocks,
    const QAudioFormat::SampleFormat sample_format
) {
    if (audio_blocks.empty()) {
        return {};
    }

    const int output_bytes_per_sample = bytes_per_sample(sample_format);
    if (output_bytes_per_sample <= 0) {
        throw std::runtime_error("The preview audio worker was asked to output an unsupported sample format.");
    }

    qsizetype total_byte_count = 0;
    for (const auto &audio_block : audio_blocks) {
        if (audio_block.channel_count <= 0 ||
            audio_block.samples_per_channel < 0 ||
            static_cast<int>(audio_block.channel_samples.size()) != audio_block.channel_count) {
            throw std::runtime_error("The preview audio worker received an invalid decoded audio block shape.");
        }

        total_byte_count += static_cast<qsizetype>(audio_block.samples_per_channel) *
            static_cast<qsizetype>(audio_block.channel_count) *
            static_cast<qsizetype>(output_bytes_per_sample);
    }

    QByteArray pcm_bytes(total_byte_count, '\0');
    qsizetype write_offset = 0;

    for (const auto &audio_block : audio_blocks) {
        for (int sample_index = 0; sample_index < audio_block.samples_per_channel; ++sample_index) {
            for (int channel_index = 0; channel_index < audio_block.channel_count; ++channel_index) {
                const auto &channel_samples = audio_block.channel_samples[static_cast<std::size_t>(channel_index)];
                if (sample_index >= static_cast<int>(channel_samples.size())) {
                    throw std::runtime_error("The preview audio worker received a truncated decoded audio channel.");
                }

                const float sample = std::clamp(channel_samples[static_cast<std::size_t>(sample_index)], -1.0f, 1.0f);
                switch (sample_format) {
                case QAudioFormat::SampleFormat::UInt8: {
                    const auto output = static_cast<std::uint8_t>(
                        std::clamp(
                            static_cast<int>(std::lround((sample + 1.0f) * 127.5f)),
                            0,
                            255
                        )
                    );
                    write_sample_bytes(pcm_bytes, write_offset, output);
                    break;
                }
                case QAudioFormat::SampleFormat::Int16: {
                    const auto output = static_cast<std::int16_t>(
                        std::clamp(
                            static_cast<int>(std::lround(sample * 32767.0f)),
                            -32768,
                            32767
                        )
                    );
                    write_sample_bytes(pcm_bytes, write_offset, output);
                    break;
                }
                case QAudioFormat::SampleFormat::Int32: {
                    const auto output = static_cast<std::int32_t>(
                        std::clamp(
                            static_cast<long long>(std::llround(sample * 2147483647.0f)),
                            static_cast<long long>(std::numeric_limits<std::int32_t>::min()),
                            static_cast<long long>(std::numeric_limits<std::int32_t>::max())
                        )
                    );
                    write_sample_bytes(pcm_bytes, write_offset, output);
                    break;
                }
                case QAudioFormat::SampleFormat::Float:
                    write_sample_bytes(pcm_bytes, write_offset, sample);
                    break;
                case QAudioFormat::SampleFormat::Unknown:
                default:
                    throw std::runtime_error("The preview audio worker was asked to output an unknown sample format.");
                }
            }
        }
    }

    return pcm_bytes;
}

}  // namespace

PreviewAudioWorker::PreviewAudioWorker(QObject *parent) : QObject(parent) {}

PreviewAudioWorker::~PreviewAudioWorker() = default;

void PreviewAudioWorker::decode_chunk(const PreviewAudioChunkRequest &request) {
    try {
        const QString normalized_source_path = request.source_path.trimmed();
        const auto trim_range = utsure::core::media::normalize_preview_trim_range(
            request.trim_in_us,
            request.trim_out_us
        );
        if (normalized_source_path.isEmpty()) {
            emit audio_chunk_failed(
                request.request_token,
                "Select a source video before requesting preview audio."
            );
            return;
        }
        if (request.sample_rate <= 0 || request.channel_count <= 0 || request.block_samples <= 0) {
            emit audio_chunk_failed(
                request.request_token,
                "Preview audio requires a positive sample rate, channel count, and block size."
            );
            return;
        }
        if (request.minimum_duration_us <= 0) {
            emit audio_chunk_failed(
                request.request_token,
                "Preview audio requires a positive chunk duration."
            );
            return;
        }
        if (request.sample_format == QAudioFormat::SampleFormat::Unknown) {
            emit audio_chunk_failed(
                request.request_token,
                "Preview audio could not resolve a supported Qt output sample format."
            );
            return;
        }

        const bool session_context_changed =
            !audio_session_ ||
            cached_source_path_ != normalized_source_path ||
            cached_sample_rate_ != request.sample_rate ||
            cached_channel_count_ != request.channel_count ||
            cached_block_samples_ != request.block_samples;

        if (session_context_changed) {
            utsure::core::media::DecodeNormalizationPolicy normalization_policy{};
            normalization_policy.audio_block_samples = request.block_samples;
            auto session_result = utsure::core::media::MediaDecoder::create_audio_preview_session(
                qstring_to_path(normalized_source_path),
                utsure::core::media::AudioPreviewOutputConfig{
                    .sample_rate_hz = request.sample_rate,
                    .channel_count = request.channel_count
                },
                normalization_policy
            );
            if (!session_result.succeeded()) {
                invalidate_cached_session();
                emit audio_chunk_failed(
                    request.request_token,
                    format_audio_worker_error(
                        session_result.error->message,
                        session_result.error->actionable_hint
                    )
                );
                return;
            }

            audio_session_ = std::move(session_result.session);
            cached_source_path_ = normalized_source_path;
            cached_sample_rate_ = request.sample_rate;
            cached_channel_count_ = request.channel_count;
            cached_block_samples_ = request.block_samples;
        }

        const auto decode_result =
            ([&]() {
                qint64 decode_request_time_us = std::max(request.requested_time_us, trim_range.trim_in_microseconds);
                if (trim_range.trim_out_microseconds.has_value()) {
                    decode_request_time_us = std::min(decode_request_time_us, *trim_range.trim_out_microseconds);
                }

                return (request.reset_session || session_context_changed)
                    ? audio_session_->seek_and_decode_window_at_time(
                        decode_request_time_us,
                        request.minimum_duration_us
                    )
                    : audio_session_->decode_next_window(request.minimum_duration_us);
            })();
        if (!decode_result.succeeded()) {
            emit audio_chunk_failed(
                request.request_token,
                format_audio_worker_error(
                    decode_result.error->message,
                    decode_result.error->actionable_hint
                )
            );
            return;
        }

        auto audio_blocks = std::move(*decode_result.audio_blocks);
        const qint64 decoded_chunk_start_us =
            !audio_blocks.empty()
                ? audio_blocks.front().timestamp.start_microseconds
                : request.requested_time_us;
        const qint64 decoded_chunk_end_us =
            !audio_blocks.empty()
                ? audio_blocks.back().timestamp.start_microseconds +
                    audio_blocks.back().timestamp.duration_microseconds.value_or(0)
                : decoded_chunk_start_us;
        auto trimmed_audio_blocks = utsure::core::media::trim_preview_audio_blocks(
            std::move(audio_blocks),
            trim_range
        );
        const qint64 chunk_start_us =
            !trimmed_audio_blocks.empty()
                ? trimmed_audio_blocks.front().timestamp.start_microseconds
                : decoded_chunk_start_us;
        qint64 chunk_end_us =
            !trimmed_audio_blocks.empty()
                ? trimmed_audio_blocks.back().timestamp.start_microseconds +
                    trimmed_audio_blocks.back().timestamp.duration_microseconds.value_or(0)
                : decoded_chunk_end_us;
        bool exhausted = decode_result.exhausted;
        if (trim_range.trim_out_microseconds.has_value() &&
            (request.requested_time_us >= *trim_range.trim_out_microseconds ||
             decoded_chunk_end_us >= *trim_range.trim_out_microseconds)) {
            chunk_end_us = std::min(chunk_end_us, *trim_range.trim_out_microseconds);
            exhausted = true;
        }
        const QByteArray pcm_bytes = interleave_audio_blocks(trimmed_audio_blocks, request.sample_format);

        qCInfo(previewAudioWorkerLog).noquote()
            << QString("decode_chunk token=%1 requested=%2 trim_in=%3 trim_out=%4 start=%5 end=%6 bytes=%7 exhausted=%8")
                   .arg(request.request_token)
                   .arg(request.requested_time_us)
                   .arg(trim_range.trim_in_microseconds)
                   .arg(trim_range.trim_out_microseconds.has_value() ? QString::number(*trim_range.trim_out_microseconds) : QString("none"))
                   .arg(chunk_start_us)
                   .arg(chunk_end_us)
                   .arg(pcm_bytes.size())
                   .arg(exhausted ? "true" : "false");

        emit audio_chunk_ready(
            request.request_token,
            chunk_start_us,
            chunk_end_us,
            pcm_bytes,
            exhausted
        );
    } catch (const std::exception &exception) {
        emit audio_chunk_failed(request.request_token, to_qstring(exception.what()));
    }
}

void PreviewAudioWorker::clear_session() {
    invalidate_cached_session();
}

void PreviewAudioWorker::invalidate_cached_session() {
    audio_session_.reset();
    cached_source_path_.clear();
    cached_sample_rate_ = 0;
    cached_channel_count_ = 0;
    cached_block_samples_ = 0;
}
