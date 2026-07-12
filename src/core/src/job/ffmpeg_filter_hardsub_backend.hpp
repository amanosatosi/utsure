#pragma once

#include "utsure/core/job/encode_job.hpp"
#include "utsure/core/timeline/timeline.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace utsure::core::job {

enum class HardsubBackend : std::uint8_t {
    internal = 0,
    ffmpeg_filter
};

struct FfmpegFilterCommandPlan final {
    std::filesystem::path ffmpeg_executable{};
    std::string ffmpeg_source{"unknown"};
    std::string subtitle_filter_name{"ass"};
    std::string subtitle_source{};
    std::optional<int> subtitle_stream_index{};
    std::string mangetsu_rgba_mode{"auto"};
    std::string mangetsu_actor_colorcoding_mode{"auto"};
    bool strict_same_thread_diagnostic_enabled{false};
    std::vector<std::string> arguments{};
};

struct FfmpegFilterHardsubSummary final {
    timeline::TimelineCompositionSummary timeline_summary{};
    media::EncodedMediaSummary encoded_media_summary{};
    std::int64_t encoded_elapsed_microseconds{0};
};

struct FfmpegFilterHardsubError final {
    std::string message{};
    std::string actionable_hint{};
    bool canceled{false};
};

struct FfmpegFilterHardsubResult final {
    std::optional<FfmpegFilterHardsubSummary> summary{};
    std::optional<FfmpegFilterHardsubError> error{};

    [[nodiscard]] bool succeeded() const noexcept;
};

struct FfmpegFilterHardsubRunRequest final {
    EncodeJob job{};
    timeline::TimelinePlan timeline_plan{};
    media::DecodeNormalizationPolicy normalization_policy{};
    std::function<void(const std::string &message)> log_callback{};
    std::function<void(const std::string &message)> warning_callback{};
    std::function<bool()> cancellation_requested{};
};

[[nodiscard]] const char *to_string(HardsubBackend backend) noexcept;
[[nodiscard]] HardsubBackend resolve_hardsub_backend_from_environment() noexcept;
[[nodiscard]] std::string EscapeFfmpegFilterValue(const std::filesystem::path &path);
[[nodiscard]] std::string format_ffmpeg_command_for_log(
    const std::filesystem::path &executable,
    const std::vector<std::string> &arguments
);
[[nodiscard]] FfmpegFilterCommandPlan build_ffmpeg_filter_hardsub_command(
    const EncodeJob &job,
    const timeline::TimelinePlan &timeline_plan
);
[[nodiscard]] FfmpegFilterHardsubResult run_ffmpeg_filter_hardsub_backend(
    const FfmpegFilterHardsubRunRequest &request
) noexcept;

}  // namespace utsure::core::job
