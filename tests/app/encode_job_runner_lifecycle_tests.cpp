#include "encode_job_runner_controller.hpp"

#include "utsure/core/job/encode_job.hpp"

#include <QCoreApplication>

#include <filesystem>
#include <iostream>
#include <memory>

namespace {

int fail(const char *message) {
    std::cerr << message << '\n';
    return 1;
}

utsure::core::job::EncodeJob make_invalid_long_run_lifecycle_job() {
    return utsure::core::job::EncodeJob{
        .input = {
            .main_source_path = std::filesystem::path{"missing-lifecycle-source.mp4"}
        },
        .output = {
            .output_path = std::filesystem::path{"missing-lifecycle-output.mp4"}
        }
    };
}

int run_destroy_while_job_queued_assertion() {
    auto controller = std::make_unique<EncodeJobRunnerController>();
    controller->start_job(make_invalid_long_run_lifecycle_job());
    if (!controller->is_running()) {
        return fail("Controller did not enter running state after start_job.");
    }

    controller.reset();
    std::cout << "runner_lifecycle.destroy_while_queued=ok\n";
    return 0;
}

int run_cancel_idle_assertion() {
    EncodeJobRunnerController controller;
    controller.cancel_job();
    if (controller.is_running()) {
        return fail("Canceling an idle controller should not enter running state.");
    }

    std::cout << "runner_lifecycle.cancel_idle=ok\n";
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    if (run_cancel_idle_assertion() != 0) {
        return 1;
    }

    return run_destroy_while_job_queued_assertion();
}
