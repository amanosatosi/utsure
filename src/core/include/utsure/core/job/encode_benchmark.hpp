#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace utsure::core::job {

inline constexpr std::string_view kEncodeBenchmarkOutputDirEnv{"UTSURE_BENCHMARK_OUTPUT_DIR"};

struct EncodeBenchmarkStageMetrics final {
    double decode_elapsed_ms{0.0};
    double subtitle_compose_elapsed_ms{0.0};
    double pixel_conversion_elapsed_ms{0.0};
    double encoder_only_elapsed_ms{0.0};
    double mux_output_write_elapsed_ms{0.0};
    double other_elapsed_ms{0.0};
    double total_elapsed_ms{0.0};
};

struct EncodeBenchmarkRecord final {
    std::string codec{};
    std::string preset{};
    int crf{0};
    std::filesystem::path input_path{};
    std::optional<std::filesystem::path> subtitle_path{};
    std::filesystem::path output_path{};
    std::optional<double> source_duration_ms{};
    std::uintmax_t output_file_size_bytes{0};
    bool subtitles_present{false};
    EncodeBenchmarkStageMetrics stage_metrics{};
};

struct EncodeBenchmarkArtifactSet final {
    std::filesystem::path artifact_directory{};
    std::filesystem::path json_path{};
    std::filesystem::path csv_path{};
    std::filesystem::path summary_path{};
};

struct EncodeBenchmarkError final {
    std::filesystem::path benchmark_output_directory{};
    std::string message{};
    std::string actionable_hint{};
};

struct EncodeBenchmarkWriteResult final {
    std::optional<EncodeBenchmarkRecord> benchmark_record{};
    std::optional<EncodeBenchmarkArtifactSet> artifact_set{};
    std::optional<EncodeBenchmarkError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

[[nodiscard]] EncodeBenchmarkRecord build_encode_benchmark_record(const EncodeJobSummary &summary);
[[nodiscard]] std::string format_encode_benchmark_summary(const EncodeBenchmarkRecord &record);
[[nodiscard]] EncodeBenchmarkWriteResult write_encode_benchmark_artifacts(
    const EncodeJobSummary &summary,
    const std::filesystem::path &benchmark_output_directory
) noexcept;

}  // namespace utsure::core::job
