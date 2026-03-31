#include "utsure/core/job/batch_parallelism.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

using utsure::core::job::BatchParallelism;
using utsure::core::job::EncodeJob;
using utsure::core::job::ParallelBatchSettings;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int assert_divisor_selection_and_buffer_tiers() {
    const auto disabled = BatchParallelism::summarize(
        ParallelBatchSettings{
            .enabled = false,
            .requested_job_count = 6
        },
        12U
    );
    if (disabled.enabled || disabled.selected_job_count != 1 || disabled.threads_per_job != 12 ||
        disabled.video_frame_queue_depth != 70U) {
        return fail("Disabled parallel mode did not preserve the single-job default behavior.");
    }

    const std::vector<int> expected_job_counts{1, 2, 3, 4, 6, 12};
    if (disabled.valid_job_counts != expected_job_counts) {
        return fail("The valid parallel job counts were not limited to exact divisors.");
    }

    const auto two_jobs = BatchParallelism::summarize(
        ParallelBatchSettings{
            .enabled = true,
            .requested_job_count = 2
        },
        12U
    );
    if (!two_jobs.enabled || two_jobs.selected_job_count != 2 || two_jobs.threads_per_job != 6 ||
        two_jobs.video_frame_queue_depth != 40U) {
        return fail("Two-job parallel planning did not resolve the expected threads/job or buffer/job values.");
    }

    const auto four_jobs = BatchParallelism::summarize(
        ParallelBatchSettings{
            .enabled = true,
            .requested_job_count = 4
        },
        12U
    );
    if (four_jobs.selected_job_count != 4 || four_jobs.threads_per_job != 3 ||
        four_jobs.video_frame_queue_depth != 20U) {
        return fail("Four-job parallel planning did not resolve the expected threads/job or buffer/job values.");
    }

    const auto twelve_jobs = BatchParallelism::summarize(
        ParallelBatchSettings{
            .enabled = true,
            .requested_job_count = 12
        },
        12U
    );
    if (twelve_jobs.selected_job_count != 12 || twelve_jobs.threads_per_job != 1 ||
        twelve_jobs.video_frame_queue_depth != 10U) {
        return fail("Max-job parallel planning did not resolve the expected threads/job or buffer/job values.");
    }

    std::cout << "parallel.valid_count=" << disabled.valid_job_counts.size() << '\n';
    std::cout << "parallel.jobs_2.threads=" << two_jobs.threads_per_job << '\n';
    std::cout << "parallel.jobs_4.buffer=" << four_jobs.video_frame_queue_depth << '\n';
    std::cout << "parallel.jobs_12.buffer=" << twelve_jobs.video_frame_queue_depth << '\n';
    return 0;
}

int assert_invalid_counts_and_zero_threads_fallback() {
    const auto summary = BatchParallelism::summarize(
        ParallelBatchSettings{
            .enabled = true,
            .requested_job_count = 5
        },
        0U
    );
    if (summary.usable_thread_count != 1U || summary.selected_job_count != 1 || summary.threads_per_job != 1 ||
        summary.valid_job_counts != std::vector<int>{1}) {
        return fail("Parallel planning did not fall back predictably when thread detection or requested counts were invalid.");
    }

    std::cout << "parallel.fallback_threads=" << summary.usable_thread_count << '\n';
    return 0;
}

int assert_execution_settings_application() {
    EncodeJob job{};
    job.execution.threading.cpu_usage_mode = utsure::core::media::CpuUsageMode::auto_select;

    const auto disabled = BatchParallelism::summarize(
        ParallelBatchSettings{
            .enabled = false,
            .requested_job_count = 3
        },
        12U
    );
    BatchParallelism::apply_execution_settings(job, disabled);
    if (job.execution.threading.decoder_thread_count_override.has_value() ||
        job.execution.threading.encoder_thread_count_override.has_value() ||
        job.execution.threading.logical_core_count_override.has_value() ||
        job.execution.video_frame_queue_depth_override.has_value()) {
        return fail("Disabled parallel mode should not inject execution overrides into the job.");
    }

    const auto enabled = BatchParallelism::summarize(
        ParallelBatchSettings{
            .enabled = true,
            .requested_job_count = 3
        },
        12U
    );
    BatchParallelism::apply_execution_settings(job, enabled);
    if (job.execution.threading.decoder_thread_count_override != 4 ||
        job.execution.threading.encoder_thread_count_override != 4 ||
        job.execution.threading.logical_core_count_override != 4U ||
        job.execution.video_frame_queue_depth_override != 40U) {
        return fail("Enabled parallel mode did not inject the planned thread/buffer settings into the job.");
    }

    std::cout << "parallel.apply_threads=" << *job.execution.threading.encoder_thread_count_override << '\n';
    std::cout << "parallel.apply_buffer=" << *job.execution.video_frame_queue_depth_override << '\n';
    return 0;
}

}  // namespace

int main() {
    if (assert_divisor_selection_and_buffer_tiers() != 0) {
        return 1;
    }

    if (assert_invalid_counts_and_zero_threads_fallback() != 0) {
        return 1;
    }

    if (assert_execution_settings_application() != 0) {
        return 1;
    }

    return 0;
}
