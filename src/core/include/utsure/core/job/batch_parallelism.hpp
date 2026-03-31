#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace utsure::core::job {

struct ParallelBatchSettings final {
    bool enabled{false};
    int requested_job_count{1};
};

struct ParallelBatchSummary final {
    std::uint32_t usable_thread_count{1};
    std::vector<int> valid_job_counts{};
    bool enabled{false};
    int selected_job_count{1};
    int threads_per_job{1};
    std::size_t video_frame_queue_depth{70};
};

class BatchParallelism final {
public:
    [[nodiscard]] static ParallelBatchSummary summarize(
        const ParallelBatchSettings &settings,
        std::optional<std::uint32_t> usable_thread_count_override = std::nullopt
    ) noexcept;
    static void apply_execution_settings(EncodeJob &job, const ParallelBatchSummary &summary) noexcept;
};

}  // namespace utsure::core::job
