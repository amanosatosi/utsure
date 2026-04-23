#include "utsure/core/job/encode_benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace utsure::core::job {

namespace {

double microseconds_to_milliseconds(const std::uint64_t microseconds) noexcept {
    return static_cast<double>(microseconds) / 1000.0;
}

double microseconds_to_milliseconds(const std::int64_t microseconds) noexcept {
    return static_cast<double>(microseconds) / 1000.0;
}

std::string format_decimal(const double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

std::string format_path(const std::filesystem::path &path) {
    return path.lexically_normal().string();
}

bool rational_is_positive(const media::Rational &value) noexcept {
    return value.is_valid() && value.numerator > 0 && value.denominator > 0;
}

std::optional<std::int64_t> rescale_to_microseconds(
    const std::int64_t value,
    const media::Rational &time_base
) noexcept {
    if (value <= 0 || !rational_is_positive(time_base)) {
        return std::nullopt;
    }

    const long double scaled = (static_cast<long double>(value) * static_cast<long double>(time_base.numerator) *
                                1000000.0L) /
        static_cast<long double>(time_base.denominator);
    if (scaled <= 0.0L ||
        scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }

    return static_cast<std::int64_t>(std::llround(scaled));
}

std::optional<std::int64_t> estimate_source_duration_microseconds(const media::MediaSourceInfo &source_info) noexcept {
    if (source_info.container_duration_microseconds.has_value() &&
        *source_info.container_duration_microseconds > 0) {
        return source_info.container_duration_microseconds;
    }

    if (source_info.primary_video_stream.has_value()) {
        const auto &video_stream = *source_info.primary_video_stream;
        if (video_stream.timestamps.duration_pts.has_value()) {
            if (const auto duration_us =
                    rescale_to_microseconds(*video_stream.timestamps.duration_pts, video_stream.timestamps.time_base);
                duration_us.has_value()) {
                return duration_us;
            }
        }

        if (video_stream.frame_count.has_value() &&
            *video_stream.frame_count > 0 &&
            rational_is_positive(video_stream.average_frame_rate)) {
            const media::Rational inverse_frame_rate{
                .numerator = video_stream.average_frame_rate.denominator,
                .denominator = video_stream.average_frame_rate.numerator
            };
            return rescale_to_microseconds(*video_stream.frame_count, inverse_frame_rate);
        }
    }

    if (source_info.primary_audio_stream.has_value()) {
        const auto &audio_stream = *source_info.primary_audio_stream;
        if (audio_stream.timestamps.duration_pts.has_value()) {
            return rescale_to_microseconds(*audio_stream.timestamps.duration_pts, audio_stream.timestamps.time_base);
        }
    }

    return std::nullopt;
}

std::string benchmark_codec_name(const media::OutputVideoCodec codec) {
    switch (codec) {
    case media::OutputVideoCodec::h264:
        return "x264";
    case media::OutputVideoCodec::h265:
        return "x265";
    default:
        throw std::runtime_error("Unsupported benchmark codec.");
    }
}

std::uintmax_t query_output_file_size_or_throw(const std::filesystem::path &output_path) {
    std::error_code filesystem_error;
    const auto file_size = std::filesystem::file_size(output_path, filesystem_error);
    if (filesystem_error) {
        throw std::runtime_error(
            "Failed to query the output file size for '" + format_path(output_path) + "': " +
            filesystem_error.message()
        );
    }

    return file_size;
}

std::string json_escape(std::string_view value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            escaped << "\\\"";
            break;
        case '\\':
            escaped << "\\\\";
            break;
        case '\b':
            escaped << "\\b";
            break;
        case '\f':
            escaped << "\\f";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (character < 0x20U) {
                escaped << "\\u"
                        << std::hex
                        << std::setw(4)
                        << std::setfill('0')
                        << static_cast<int>(character)
                        << std::dec
                        << std::setfill(' ');
            } else {
                escaped << static_cast<char>(character);
            }
            break;
        }
    }

    return escaped.str();
}

std::string csv_escape(std::string_view value) {
    std::string escaped(value);
    std::size_t position = 0;
    while ((position = escaped.find('"', position)) != std::string::npos) {
        escaped.insert(position, 1, '"');
        position += 2;
    }

    return '"' + escaped + '"';
}

void write_text_file(const std::filesystem::path &path, const std::string &content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("Failed to open '" + format_path(path) + "' for writing.");
    }

    output << content;
    if (!output.good()) {
        throw std::runtime_error("Failed to write '" + format_path(path) + "'.");
    }
}

std::string build_json(const EncodeBenchmarkRecord &record) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"codec\": \"" << json_escape(record.codec) << "\",\n";
    json << "  \"preset\": \"" << json_escape(record.preset) << "\",\n";
    json << "  \"crf\": " << record.crf << ",\n";
    json << "  \"input_path\": \"" << json_escape(format_path(record.input_path)) << "\",\n";
    if (record.subtitle_path.has_value()) {
        json << "  \"subtitle_path\": \"" << json_escape(format_path(*record.subtitle_path)) << "\",\n";
    } else {
        json << "  \"subtitle_path\": null,\n";
    }
    json << "  \"output_path\": \"" << json_escape(format_path(record.output_path)) << "\",\n";
    if (record.source_duration_ms.has_value()) {
        json << "  \"source_duration_ms\": " << format_decimal(*record.source_duration_ms) << ",\n";
    } else {
        json << "  \"source_duration_ms\": null,\n";
    }
    json << "  \"output_file_size_bytes\": " << record.output_file_size_bytes << ",\n";
    json << "  \"subtitles_present\": " << (record.subtitles_present ? "true" : "false") << ",\n";
    json << "  \"timings_ms\": {\n";
    json << "    \"decode\": " << format_decimal(record.stage_metrics.decode_elapsed_ms) << ",\n";
    json << "    \"subtitle_compose\": " << format_decimal(record.stage_metrics.subtitle_compose_elapsed_ms) << ",\n";
    json << "    \"pixel_conversion\": " << format_decimal(record.stage_metrics.pixel_conversion_elapsed_ms) << ",\n";
    json << "    \"encoder_only\": " << format_decimal(record.stage_metrics.encoder_only_elapsed_ms) << ",\n";
    json << "    \"mux_output_write\": " << format_decimal(record.stage_metrics.mux_output_write_elapsed_ms) << ",\n";
    json << "    \"other\": " << format_decimal(record.stage_metrics.other_elapsed_ms) << ",\n";
    json << "    \"total_elapsed\": " << format_decimal(record.stage_metrics.total_elapsed_ms) << '\n';
    json << "  }\n";
    json << "}\n";
    return json.str();
}

std::string build_csv(const EncodeBenchmarkRecord &record) {
    std::ostringstream csv;
    csv << "codec,preset,crf,input_path,subtitle_path,output_path,source_duration_ms,output_file_size_bytes,"
           "subtitles_present,decode_elapsed_ms,subtitle_compose_elapsed_ms,pixel_conversion_elapsed_ms,"
           "encoder_only_elapsed_ms,mux_output_write_elapsed_ms,other_elapsed_ms,total_elapsed_ms\n";
    csv << csv_escape(record.codec) << ','
        << csv_escape(record.preset) << ','
        << record.crf << ','
        << csv_escape(format_path(record.input_path)) << ','
        << csv_escape(record.subtitle_path.has_value() ? format_path(*record.subtitle_path) : std::string()) << ','
        << csv_escape(format_path(record.output_path)) << ',';
    if (record.source_duration_ms.has_value()) {
        csv << format_decimal(*record.source_duration_ms);
    }
    csv << ','
        << record.output_file_size_bytes << ','
        << (record.subtitles_present ? "yes" : "no") << ','
        << format_decimal(record.stage_metrics.decode_elapsed_ms) << ','
        << format_decimal(record.stage_metrics.subtitle_compose_elapsed_ms) << ','
        << format_decimal(record.stage_metrics.pixel_conversion_elapsed_ms) << ','
        << format_decimal(record.stage_metrics.encoder_only_elapsed_ms) << ','
        << format_decimal(record.stage_metrics.mux_output_write_elapsed_ms) << ','
        << format_decimal(record.stage_metrics.other_elapsed_ms) << ','
        << format_decimal(record.stage_metrics.total_elapsed_ms)
        << '\n';
    return csv.str();
}

}  // namespace

bool EncodeBenchmarkWriteResult::succeeded() const noexcept {
    return benchmark_record.has_value() && artifact_set.has_value() && !error.has_value();
}

EncodeBenchmarkRecord build_encode_benchmark_record(const EncodeJobSummary &summary) {
    const auto tracked_microseconds =
        summary.streaming_runtime.video_decode_microseconds +
        summary.streaming_runtime.subtitle_compose_microseconds +
        summary.streaming_runtime.pixel_conversion_microseconds +
        summary.streaming_runtime.video_encode_microseconds +
        summary.streaming_runtime.mux_write_microseconds;
    const auto other_microseconds = summary.streaming_runtime.total_elapsed_microseconds >
            static_cast<std::int64_t>(tracked_microseconds)
        ? static_cast<std::uint64_t>(
              summary.streaming_runtime.total_elapsed_microseconds - static_cast<std::int64_t>(tracked_microseconds)
          )
        : 0U;

    return EncodeBenchmarkRecord{
        .codec = benchmark_codec_name(summary.job.output.video.codec),
        .preset = summary.job.output.video.preset,
        .crf = summary.job.output.video.crf,
        .input_path = summary.job.input.main_source_path.lexically_normal(),
        .subtitle_path = summary.job.subtitles.has_value()
            ? std::optional<std::filesystem::path>(summary.job.subtitles->subtitle_path.lexically_normal())
            : std::nullopt,
        .output_path = summary.job.output.output_path.lexically_normal(),
        .source_duration_ms = [&]() -> std::optional<double> {
            if (const auto duration_us = estimate_source_duration_microseconds(summary.inspected_input_info);
                duration_us.has_value()) {
                return microseconds_to_milliseconds(*duration_us);
            }

            return std::nullopt;
        }(),
        .output_file_size_bytes = query_output_file_size_or_throw(summary.encoded_media_summary.output_path),
        .subtitles_present = summary.job.subtitles.has_value(),
        .stage_metrics = EncodeBenchmarkStageMetrics{
            .decode_elapsed_ms = microseconds_to_milliseconds(summary.streaming_runtime.video_decode_microseconds),
            .subtitle_compose_elapsed_ms =
                microseconds_to_milliseconds(summary.streaming_runtime.subtitle_compose_microseconds),
            .pixel_conversion_elapsed_ms =
                microseconds_to_milliseconds(summary.streaming_runtime.pixel_conversion_microseconds),
            .encoder_only_elapsed_ms = microseconds_to_milliseconds(summary.streaming_runtime.video_encode_microseconds),
            .mux_output_write_elapsed_ms =
                microseconds_to_milliseconds(summary.streaming_runtime.mux_write_microseconds),
            .other_elapsed_ms = microseconds_to_milliseconds(other_microseconds),
            .total_elapsed_ms = microseconds_to_milliseconds(summary.streaming_runtime.total_elapsed_microseconds)
        }
    };
}

std::string format_encode_benchmark_summary(const EncodeBenchmarkRecord &record) {
    std::ostringstream summary;
    summary << "Encode Benchmark Summary\n";
    summary << "Codec: " << record.codec << '\n';
    summary << "Preset: " << record.preset << '\n';
    summary << "CRF: " << record.crf << '\n';
    summary << "Input: " << format_path(record.input_path) << '\n';
    summary << "Subtitle: "
            << (record.subtitle_path.has_value() ? format_path(*record.subtitle_path) : std::string("none")) << '\n';
    summary << "Output: " << format_path(record.output_path) << '\n';
    summary << "Source duration (ms): "
            << (record.source_duration_ms.has_value() ? format_decimal(*record.source_duration_ms) : std::string("n/a"))
            << '\n';
    summary << "Output file size (bytes): " << record.output_file_size_bytes << '\n';
    summary << "Subtitles present: " << (record.subtitles_present ? "yes" : "no") << '\n';
    summary << "Decode elapsed (ms): " << format_decimal(record.stage_metrics.decode_elapsed_ms) << '\n';
    summary << "Subtitle/composite elapsed (ms): "
            << format_decimal(record.stage_metrics.subtitle_compose_elapsed_ms) << '\n';
    summary << "Pixel conversion elapsed (ms): "
            << format_decimal(record.stage_metrics.pixel_conversion_elapsed_ms) << '\n';
    summary << "Encoder-only elapsed (ms): " << format_decimal(record.stage_metrics.encoder_only_elapsed_ms) << '\n';
    summary << "Mux/output write elapsed (ms): "
            << format_decimal(record.stage_metrics.mux_output_write_elapsed_ms) << '\n';
    summary << "Other elapsed (ms): " << format_decimal(record.stage_metrics.other_elapsed_ms) << '\n';
    summary << "Total elapsed (ms): " << format_decimal(record.stage_metrics.total_elapsed_ms) << '\n';
    return summary.str();
}

EncodeBenchmarkWriteResult write_encode_benchmark_artifacts(
    const EncodeJobSummary &summary,
    const std::filesystem::path &benchmark_output_directory
) noexcept {
    try {
        const auto normalized_output_directory = benchmark_output_directory.lexically_normal();
        if (normalized_output_directory.empty()) {
            throw std::runtime_error("The benchmark output directory must not be empty.");
        }

        std::error_code filesystem_error;
        std::filesystem::create_directories(normalized_output_directory, filesystem_error);
        if (filesystem_error) {
            throw std::runtime_error(
                "Failed to create the benchmark output directory '" + format_path(normalized_output_directory) +
                "': " + filesystem_error.message()
            );
        }

        const auto record = build_encode_benchmark_record(summary);
        const EncodeBenchmarkArtifactSet artifact_set{
            .artifact_directory = normalized_output_directory,
            .json_path = normalized_output_directory / "benchmark-results.json",
            .csv_path = normalized_output_directory / "benchmark-results.csv",
            .summary_path = normalized_output_directory / "benchmark-summary.txt"
        };

        write_text_file(artifact_set.json_path, build_json(record));
        write_text_file(artifact_set.csv_path, build_csv(record));
        write_text_file(artifact_set.summary_path, format_encode_benchmark_summary(record));

        return EncodeBenchmarkWriteResult{
            .benchmark_record = record,
            .artifact_set = artifact_set,
            .error = std::nullopt
        };
    } catch (const std::exception &exception) {
        return EncodeBenchmarkWriteResult{
            .benchmark_record = std::nullopt,
            .artifact_set = std::nullopt,
            .error = EncodeBenchmarkError{
                .benchmark_output_directory = benchmark_output_directory.lexically_normal(),
                .message = "Failed to write encode benchmark artifacts.",
                .actionable_hint = exception.what()
            }
        };
    }
}

}  // namespace utsure::core::job
