#include "utsure/core/job/encode_benchmark.hpp"
#include "utsure/core/job/encode_job.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using utsure::core::job::EncodeBenchmarkWriteResult;
using utsure::core::job::EncodeJob;
using utsure::core::job::EncodeJobLogLevel;
using utsure::core::job::EncodeJobLogMessage;
using utsure::core::job::EncodeJobObserver;
using utsure::core::job::EncodeJobProgress;
using utsure::core::job::EncodeJobResult;
using utsure::core::job::EncodeJobRunOptions;
using utsure::core::job::EncodeJobRunner;
using utsure::core::job::format_encode_benchmark_summary;
using utsure::core::job::kEncodeBenchmarkOutputDirEnv;
using utsure::core::job::write_encode_benchmark_artifacts;
using utsure::core::media::OutputVideoCodec;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

std::string lowercase_ascii(std::string value) {
    for (char &character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    return value;
}

std::string format_path(const std::filesystem::path &path) {
    return path.lexically_normal().string();
}

std::string sanitize_token(std::string value) {
    for (char &character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) == 0 && character != '-' && character != '_') {
            character = '-';
        }
    }

    while (!value.empty() && value.front() == '-') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == '-') {
        value.pop_back();
    }

    return value.empty() ? std::string("default") : value;
}

std::optional<OutputVideoCodec> parse_codec(const std::string_view value) {
    const auto normalized = lowercase_ascii(std::string(value));
    if (normalized == "x264" || normalized == "h264") {
        return OutputVideoCodec::h264;
    }

    if (normalized == "x265" || normalized == "h265" || normalized == "hevc") {
        return OutputVideoCodec::h265;
    }

    return std::nullopt;
}

std::optional<int> parse_int(const std::string_view value) {
    try {
        std::size_t parsed_length = 0;
        const int parsed_value = std::stoi(std::string(value), &parsed_length, 10);
        if (parsed_length != value.size()) {
            return std::nullopt;
        }

        return parsed_value;
    } catch (...) {
        return std::nullopt;
    }
}

std::string benchmark_codec_name(const OutputVideoCodec codec) {
    switch (codec) {
    case OutputVideoCodec::h264:
        return "x264";
    case OutputVideoCodec::h265:
        return "x265";
    default:
        return "unknown";
    }
}

std::string default_subtitle_format_hint(const std::filesystem::path &subtitle_path) {
    const auto extension = lowercase_ascii(subtitle_path.extension().string());
    if (!extension.empty() && extension.front() == '.') {
        return extension.substr(1);
    }

    return extension.empty() ? std::string("ass") : extension;
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

struct BenchmarkOptions final {
    std::filesystem::path input_path{};
    std::optional<std::filesystem::path> subtitle_path{};
    std::string subtitle_format_hint{};
    OutputVideoCodec codec{OutputVideoCodec::h264};
    std::string preset{"medium"};
    int crf{23};
    std::filesystem::path output_path{};
    std::filesystem::path benchmark_output_root{};
};

std::string usage_text() {
    std::ostringstream usage;
    usage
        << "Usage: utsure_core_encode_benchmark "
        << "--input <path> "
        << "--codec <x264|x265> "
        << "--preset <name> "
        << "--crf <value> "
        << "--output <path> "
        << "[--subtitle <path>] "
        << "[--subtitle-format <hint>] "
        << "[--benchmark-output-dir <path>]\n"
        << "If --benchmark-output-dir is omitted, set " << kEncodeBenchmarkOutputDirEnv << ".";
    return usage.str();
}

std::optional<BenchmarkOptions> parse_arguments(int argc, char *argv[], std::string &error_message) {
    BenchmarkOptions options{};
    std::optional<std::filesystem::path> benchmark_output_dir{};

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto require_value = [&](const std::string_view name) -> std::optional<std::string_view> {
            if ((index + 1) >= argc) {
                error_message = "Missing value for " + std::string(name) + '.';
                return std::nullopt;
            }

            ++index;
            return std::string_view(argv[index]);
        };

        if (argument == "--help") {
            error_message.clear();
            return std::nullopt;
        }

        if (argument == "--input") {
            if (const auto value = require_value(argument); value.has_value()) {
                options.input_path = std::filesystem::path(*value);
            } else {
                return std::nullopt;
            }
            continue;
        }

        if (argument == "--subtitle") {
            if (const auto value = require_value(argument); value.has_value()) {
                options.subtitle_path = std::filesystem::path(*value);
            } else {
                return std::nullopt;
            }
            continue;
        }

        if (argument == "--subtitle-format") {
            if (const auto value = require_value(argument); value.has_value()) {
                options.subtitle_format_hint = std::string(*value);
            } else {
                return std::nullopt;
            }
            continue;
        }

        if (argument == "--codec") {
            if (const auto value = require_value(argument); value.has_value()) {
                const auto codec = parse_codec(*value);
                if (!codec.has_value()) {
                    error_message = "Unsupported codec '" + std::string(*value) + "'. Use x264 or x265.";
                    return std::nullopt;
                }

                options.codec = *codec;
            } else {
                return std::nullopt;
            }
            continue;
        }

        if (argument == "--preset") {
            if (const auto value = require_value(argument); value.has_value()) {
                options.preset = std::string(*value);
            } else {
                return std::nullopt;
            }
            continue;
        }

        if (argument == "--crf") {
            if (const auto value = require_value(argument); value.has_value()) {
                const auto parsed_crf = parse_int(*value);
                if (!parsed_crf.has_value()) {
                    error_message = "Invalid CRF value '" + std::string(*value) + "'.";
                    return std::nullopt;
                }

                options.crf = *parsed_crf;
            } else {
                return std::nullopt;
            }
            continue;
        }

        if (argument == "--output") {
            if (const auto value = require_value(argument); value.has_value()) {
                options.output_path = std::filesystem::path(*value);
            } else {
                return std::nullopt;
            }
            continue;
        }

        if (argument == "--benchmark-output-dir") {
            if (const auto value = require_value(argument); value.has_value()) {
                benchmark_output_dir = std::filesystem::path(*value);
            } else {
                return std::nullopt;
            }
            continue;
        }

        error_message = "Unknown argument '" + std::string(argument) + "'.";
        return std::nullopt;
    }

    if (options.input_path.empty()) {
        error_message = "The benchmark input path is required.";
        return std::nullopt;
    }

    if (options.output_path.empty()) {
        error_message = "The benchmark output path is required.";
        return std::nullopt;
    }

    if (options.preset.empty()) {
        error_message = "The benchmark preset must not be empty.";
        return std::nullopt;
    }

    if (!benchmark_output_dir.has_value()) {
        const std::string env_name(kEncodeBenchmarkOutputDirEnv);
        const char *env_value = std::getenv(env_name.c_str());
        if (env_value != nullptr && env_value[0] != '\0') {
            benchmark_output_dir = std::filesystem::path(env_value);
        }
    }

    if (!benchmark_output_dir.has_value() || benchmark_output_dir->empty()) {
        error_message =
            "The benchmark output directory is required via --benchmark-output-dir or " +
            std::string(kEncodeBenchmarkOutputDirEnv) + '.';
        return std::nullopt;
    }

    if (options.subtitle_path.has_value() && options.subtitle_format_hint.empty()) {
        options.subtitle_format_hint = default_subtitle_format_hint(*options.subtitle_path);
    }

    options.benchmark_output_root = benchmark_output_dir->lexically_normal();
    return options;
}

std::filesystem::path case_directory_for(const BenchmarkOptions &options) {
    std::string directory_name =
        benchmark_codec_name(options.codec) + "-preset-" + sanitize_token(options.preset) + "-crf-" +
        std::to_string(options.crf);
    if (options.subtitle_path.has_value()) {
        directory_name += "-subtitles";
    }

    return options.benchmark_output_root / directory_name;
}

struct BenchmarkObserver final : EncodeJobObserver {
    std::vector<std::string> entries{};

    void record(std::string line) {
        std::cout << line << '\n';
        entries.push_back(std::move(line));
    }

    void on_progress(const EncodeJobProgress &progress) override {
        std::ostringstream line;
        line << "[progress] stage=" << utsure::core::job::to_string(progress.stage)
             << " step=" << progress.current_step << '/' << progress.total_steps
             << " message=" << progress.message;
        if (progress.stage_fraction.has_value()) {
            line << " stage_fraction=" << *progress.stage_fraction;
        }
        if (progress.overall_fraction.has_value()) {
            line << " overall_fraction=" << *progress.overall_fraction;
        }
        if (progress.encoded_video_frames.has_value()) {
            line << " encoded_video_frames=" << *progress.encoded_video_frames;
        }
        if (progress.total_video_frames.has_value()) {
            line << " total_video_frames=" << *progress.total_video_frames;
        }
        if (progress.encoded_fps.has_value()) {
            line << " encoded_fps=" << *progress.encoded_fps;
        }

        record(line.str());
    }

    void on_log(const EncodeJobLogMessage &message) override {
        std::ostringstream line;
        line << '[' << utsure::core::job::to_string(message.level) << "] " << message.message;
        record(line.str());
    }

    std::string joined_log() const {
        std::ostringstream log;
        for (const auto &entry : entries) {
            log << entry << '\n';
        }

        return log.str();
    }
};

}  // namespace

int main(int argc, char *argv[]) {
    std::string parse_error{};
    const auto options = parse_arguments(argc, argv, parse_error);
    if (!options.has_value()) {
        if (parse_error.empty()) {
            std::cout << usage_text() << '\n';
            return 0;
        }

        return fail(parse_error + "\n" + usage_text());
    }

    const auto benchmark_case_directory = case_directory_for(*options);
    std::error_code filesystem_error;
    std::filesystem::create_directories(benchmark_case_directory, filesystem_error);
    if (filesystem_error) {
        return fail(
            "Failed to create the benchmark case directory '" + format_path(benchmark_case_directory) +
            "': " + filesystem_error.message()
        );
    }

    const auto raw_log_path = benchmark_case_directory / "benchmark-run.log";
    BenchmarkObserver observer{};
    observer.record("Benchmark case directory: " + format_path(benchmark_case_directory));
    observer.record("Benchmark input: " + format_path(options->input_path));
    if (options->subtitle_path.has_value()) {
        observer.record("Benchmark subtitle: " + format_path(*options->subtitle_path));
    } else {
        observer.record("Benchmark subtitle: none");
    }
    observer.record("Benchmark output: " + format_path(options->output_path));
    observer.record("Benchmark codec: " + benchmark_codec_name(options->codec));
    observer.record("Benchmark preset: " + options->preset);
    observer.record("Benchmark CRF: " + std::to_string(options->crf));

    EncodeJob job{
        .input = {
            .main_source_path = options->input_path
        },
        .output = {
            .output_path = options->output_path,
            .video = {
                .codec = options->codec,
                .preset = options->preset,
                .crf = options->crf
            }
        }
    };
    if (options->subtitle_path.has_value()) {
        job.subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = *options->subtitle_path,
            .format_hint = options->subtitle_format_hint
        };
    }

    const EncodeJobResult result = EncodeJobRunner::run(job, EncodeJobRunOptions{
        .decode_normalization_policy = {},
        .observer = &observer
    });

    if (!result.succeeded()) {
        observer.record("[error] Benchmark failed.");
        observer.record("[error] " + result.error->message);
        if (!result.error->actionable_hint.empty()) {
            observer.record("[error] Hint: " + result.error->actionable_hint);
        }

        try {
            write_text_file(raw_log_path, observer.joined_log());
        } catch (const std::exception &exception) {
            std::cerr << exception.what() << '\n';
        }

        return 1;
    }

    const EncodeBenchmarkWriteResult benchmark_result =
        write_encode_benchmark_artifacts(*result.encode_job_summary, benchmark_case_directory);
    if (!benchmark_result.succeeded()) {
        observer.record("[error] " + benchmark_result.error->message);
        if (!benchmark_result.error->actionable_hint.empty()) {
            observer.record("[error] Hint: " + benchmark_result.error->actionable_hint);
        }

        try {
            write_text_file(raw_log_path, observer.joined_log());
        } catch (const std::exception &exception) {
            std::cerr << exception.what() << '\n';
        }

        return 1;
    }

    observer.record("Benchmark JSON: " + format_path(benchmark_result.artifact_set->json_path));
    observer.record("Benchmark CSV: " + format_path(benchmark_result.artifact_set->csv_path));
    observer.record("Benchmark summary: " + format_path(benchmark_result.artifact_set->summary_path));

    const auto formatted_summary = format_encode_benchmark_summary(*benchmark_result.benchmark_record);
    std::cout << '\n' << formatted_summary;

    try {
        write_text_file(raw_log_path, observer.joined_log() + '\n' + formatted_summary);
    } catch (const std::exception &exception) {
        return fail(exception.what());
    }

    return 0;
}
