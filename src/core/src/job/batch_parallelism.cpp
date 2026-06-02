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

std::size_t resolve_video_worker_count(const int threads_per_job) noexcept {
    return threads_per_job >= 8 ? 2U : 1U;
}

std::size_t resolve_subtitle_worker_count(const int /*threads_per_job*/) noexcept {
    return 1U;
}

int resolve_decoder_thread_count() noexcept {
    return 1;
}

int resolve_encoder_thread_count(
    const bool parallel_enabled,
    const int threads_per_job,
    const std::size_t video_worker_count,
    const std::size_t subtitle_worker_count
) noexcept {
    if (!parallel_enabled) {
        return std::max(threads_per_job, 1);
    }

    constexpr int kCoordinatorAndMuxOverheadThreads = 1;
    const int remaining_budget =
        threads_per_job -
        resolve_decoder_thread_count() -
        static_cast<int>(video_worker_count) -
        static_cast<int>(subtitle_worker_count) -
        kCoordinatorAndMuxOverheadThreads;
    return std::max(remaining_budget, 1);
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
    if (!settings.enabled) {
        return ParallelBatchSummary{
            .usable_thread_count = usable_thread_count,
            .valid_job_counts = valid_job_counts,
            .enabled = false,
            .selected_job_count = 1,
            .threads_per_job = threads_per_job,
            .decoder_threads_per_job = 0,
            .encoder_threads_per_job = 0,
            .video_workers_per_job = 0U,
            .subtitle_workers_per_job = 0U,
            .estimated_threads_per_job = threads_per_job,
            .estimated_total_threads = threads_per_job,
            .estimated_threads_exceed_usable_cores = false,
            .video_frame_queue_depth = resolve_video_frame_queue_depth(usable_thread_count, selected_job_count)
        };
    }

    const auto video_worker_count = resolve_video_worker_count(threads_per_job);
    const auto subtitle_worker_count = resolve_subtitle_worker_count(threads_per_job);
    const int decoder_thread_count = resolve_decoder_thread_count();
    const int encoder_thread_count = resolve_encoder_thread_count(
        settings.enabled,
        threads_per_job,
        video_worker_count,
        subtitle_worker_count
    );
    const int estimated_threads_per_job =
        decoder_thread_count +
        encoder_thread_count +
        static_cast<int>(video_worker_count) +
        static_cast<int>(subtitle_worker_count) +
        1;
    const int estimated_total_threads = estimated_threads_per_job * std::max(selected_job_count, 1);

    return ParallelBatchSummary{
        .usable_thread_count = usable_thread_count,
        .valid_job_counts = valid_job_counts,
        .enabled = settings.enabled,
        .selected_job_count = std::max(selected_job_count, 1),
        .threads_per_job = threads_per_job,
        .decoder_threads_per_job = decoder_thread_count,
        .encoder_threads_per_job = encoder_thread_count,
        .video_workers_per_job = video_worker_count,
        .subtitle_workers_per_job = subtitle_worker_count,
        .estimated_threads_per_job = estimated_threads_per_job,
        .estimated_total_threads = estimated_total_threads,
        .estimated_threads_exceed_usable_cores =
            estimated_total_threads > static_cast<int>(usable_thread_count),
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

    job.execution.threading.decoder_thread_count_override = std::max(summary.decoder_threads_per_job, 1);
    job.execution.threading.encoder_thread_count_override = std::max(summary.encoder_threads_per_job, 1);
    job.execution.threading.logical_core_count_override =
        static_cast<std::uint32_t>(std::max(summary.threads_per_job, 1));
    job.execution.video_frame_queue_depth_override = summary.video_frame_queue_depth;
}

}  // namespace utsure::core::job
