#pragma once

#include "utsure/core/job/encode_job.hpp"

#include <string>

namespace utsure::core::job {

[[nodiscard]] std::string format_encode_job_report(const EncodeJobSummary &encode_job_summary);

}  // namespace utsure::core::job
