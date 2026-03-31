#include "utsure/core/job/batch_parallelism.hpp"

#include <algorithm>
#include <thread>

namespace utsure::core::job {

namespace {

std::uint32_t detect_usable_thread_count() noexcept {
    return std::thread::hardware_concurrency();
}

std::uint32_t effective_usable_thread_count(const std::optional<std::uint32_t> usable_thread_count_override) noexcept {
    const auto detected = usable_thread_count_override.value_or(detect_usable_thread_count());
    return detected > 0U ? detected : 1U;
}

std::vector<int> build_valid_job_counts(const std::uint32_t usable_thread_count) {
    std::vector<int> valid_job_counts{};
    valid_job_counts.reserve(static_cast<std::size_t>(usable_thread_count));

    for (std::uint32_t candidate = 1; candidate <= usable_thread_count; ++candidate) {
        if ((usable_thread_count % candidate) == 0U) {
            valid_job_counts.push_back(static_cast<int>(candidate));
        }
    }

    if (valid_job_counts.empty()) {
        valid_job_counts.push_back(1);
    }

    return valid_job_counts;
}

int select_valid_job_count(
    const std::vector<int> &valid_job_counts,
    const int requested_job_count
) noexcept {
    if (requested_job_count > 0 &&
        std::find(valid_job_counts.begin(), valid_job_counts.end(), requested_job_count) != valid_job_counts.end()) {
        return requested_job_count;
    }

    return 1;
}

std::size_t resolve_video_frame_queue_depth(
    const std::uint32_t usable_thread_count,
    const int selected_job_count
) noexcept {
    if (selected_job_count <= 1) {
        return 70U;
    }

    if (selected_job_count <= 3) {
        return 40U;
    }

    const int half_thread_count = static_cast<int>(usable_thread_count / 2U);
    if (selected_job_count <= half_thread_count) {
        return 20U;
    }

    return 10U;
}

}  // namespace

ParallelBatchSummary BatchParallelism::summarize(
    const ParallelBatchSettings &settings,
    const std::optional<std::uint32_t> usable_thread_count_override
) noexcept {
    const std::uint32_t usable_thread_count = effective_usable_thread_count(usable_thread_count_override);
    const std::vector<int> valid_job_counts = build_valid_job_counts(usable_thread_count);

    const int selected_job_count = settings.enabled
        ? select_valid_job_count(valid_job_counts, settings.requested_job_count)
        : 1;
    const int threads_per_job = std::max(1, static_cast<int>(usable_thread_count) / std::max(selected_job_count, 1));

    return ParallelBatchSummary{
        .usable_thread_count = usable_thread_count,
        .valid_job_counts = valid_job_counts,
        .enabled = settings.enabled,
        .selected_job_count = std::max(selected_job_count, 1),
        .threads_per_job = threads_per_job,
        .video_frame_queue_depth = resolve_video_frame_queue_depth(usable_thread_count, selected_job_count)
    };
}

void BatchParallelism::apply_execution_settings(EncodeJob &job, const ParallelBatchSummary &summary) noexcept {
    if (!summary.enabled) {
        job.execution.threading.decoder_thread_count_override.reset();
        job.execution.threading.encoder_thread_count_override.reset();
        job.execution.threading.logical_core_count_override.reset();
        job.execution.video_frame_queue_depth_override.reset();
        return;
    }

    const int threads_per_job = std::max(summary.threads_per_job, 1);
    job.execution.threading.decoder_thread_count_override = threads_per_job;
    job.execution.threading.encoder_thread_count_override = threads_per_job;
    job.execution.threading.logical_core_count_override = static_cast<std::uint32_t>(threads_per_job);
    job.execution.video_frame_queue_depth_override = summary.video_frame_queue_depth;
}

}  // namespace utsure::core::job
