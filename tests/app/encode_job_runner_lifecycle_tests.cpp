#include "encode_job_runner_controller.hpp"
#include "encode_job_runner_worker.hpp"

#include "utsure/core/job/encode_job.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>

namespace {

using utsure::core::job::EncodeJob;
using utsure::core::job::EncodeJobError;
using utsure::core::job::EncodeJobLogLevel;
using utsure::core::job::EncodeJobLogMessage;
using utsure::core::job::EncodeJobProgress;
using utsure::core::job::EncodeJobResult;
using utsure::core::job::EncodeJobRunOptions;
using utsure::core::job::EncodeJobStage;

int fail(const char *message) {
    std::cerr << message << '\n';
    return 1;
}

EncodeJob make_lifecycle_job(const char *name) {
    return EncodeJob{
        .input = {
            .main_source_path = std::filesystem::path{name}
        },
        .output = {
            .output_path = std::filesystem::path{"lifecycle-output.mp4"}
        }
    };
}

EncodeJobResult make_canceled_result(const EncodeJob &job) {
    return EncodeJobResult{
        .error = EncodeJobError{
            .main_source_path = job.input.main_source_path.string(),
            .output_path = job.output.output_path.string(),
            .message = "Fake lifecycle job canceled.",
            .actionable_hint = "Test cancellation observed.",
            .canceled = true
        }
    };
}

bool wait_until(const std::function<bool()> &predicate, const int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) {
            return true;
        }
        QThread::msleep(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

EncodeJobRunnerWorker *make_blocking_worker(
    std::atomic_int &started_count,
    std::atomic_int &finished_count
) {
    return new EncodeJobRunnerWorker(
        [&started_count, &finished_count](const EncodeJob &job, const EncodeJobRunOptions &options) {
            ++started_count;
            if (options.observer != nullptr) {
                options.observer->on_log(EncodeJobLogMessage{
                    .level = EncodeJobLogLevel::info,
                    .message = "Fake lifecycle job entered active work."
                });
                options.observer->on_progress(EncodeJobProgress{
                    .stage = EncodeJobStage::encoding_output,
                    .message = "Fake lifecycle encode active."
                });
            }

            while (!options.cancellation_requested || !options.cancellation_requested()) {
                QThread::msleep(10);
            }

            ++finished_count;
            return make_canceled_result(job);
        }
    );
}

int run_cancel_while_active_assertion() {
    std::atomic_int started_count{0};
    std::atomic_int finished_count{0};
    int job_finished_count = 0;
    EncodeJobRunnerController controller(make_blocking_worker(started_count, finished_count));
    QObject::connect(
        &controller,
        &EncodeJobRunnerController::job_finished,
        &controller,
        [&job_finished_count](const bool, const bool canceled, const QString &, const QString &, const QString &) {
            if (canceled) {
                ++job_finished_count;
            }
        }
    );

    controller.start_job(make_lifecycle_job("cancel-active.mp4"));
    if (!wait_until([&]() { return started_count.load() == 1; }, 1000)) {
        return fail("Fake active job did not start before timeout.");
    }
    if (!controller.is_running()) {
        return fail("Controller did not report running while fake job was active.");
    }

    controller.cancel_job();
    if (!wait_until([&]() { return job_finished_count == 1; }, 2000)) {
        return fail("Canceling an active fake job did not finish before timeout.");
    }
    if (controller.is_running()) {
        return fail("Controller still reported running after active cancel finished.");
    }

    std::cout << "runner_lifecycle.cancel_active=ok\n";
    return 0;
}

int run_destroy_while_active_assertion() {
    std::atomic_int started_count{0};
    std::atomic_int finished_count{0};
    {
        auto controller = std::make_unique<EncodeJobRunnerController>(
            make_blocking_worker(started_count, finished_count)
        );
        controller->start_job(make_lifecycle_job("destroy-active.mp4"));
        if (!wait_until([&]() { return started_count.load() == 1; }, 1000)) {
            return fail("Fake active destroy job did not start before timeout.");
        }
        if (!controller->is_running()) {
            return fail("Controller did not report running before active destruction.");
        }
        controller.reset();
    }

    if (!wait_until([&]() { return finished_count.load() == 1; }, 1000)) {
        return fail("Destroying controller did not request cancellation for active work.");
    }

    std::cout << "runner_lifecycle.destroy_active=ok\n";
    return 0;
}

int run_reentrant_finished_starts_next_job_assertion() {
    std::atomic_int started_count{0};
    std::atomic_int finished_count{0};
    int job_finished_count = 0;
    EncodeJobRunnerController controller(make_blocking_worker(started_count, finished_count));
    QObject::connect(
        &controller,
        &EncodeJobRunnerController::job_finished,
        &controller,
        [&](const bool, const bool, const QString &, const QString &, const QString &) {
            ++job_finished_count;
            if (job_finished_count == 1) {
                controller.start_job(make_lifecycle_job("second-reentrant.mp4"));
            }
        }
    );

    controller.start_job(make_lifecycle_job("first-reentrant.mp4"));
    if (!wait_until([&]() { return started_count.load() == 1; }, 1000)) {
        return fail("First reentrant fake job did not start.");
    }
    controller.cancel_job();
    if (!wait_until([&]() { return started_count.load() == 2; }, 2000)) {
        return fail("job_finished handler did not start the second job.");
    }
    if (!controller.is_running()) {
        return fail("Controller state was clobbered to idle after reentrant job start.");
    }

    controller.cancel_job();
    if (!wait_until([&]() { return job_finished_count == 2; }, 2000)) {
        return fail("Second reentrant fake job did not finish after cancel.");
    }

    std::cout << "runner_lifecycle.reentrant_finished_start=ok\n";
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

    if (run_cancel_idle_assertion() != 0 ||
        run_cancel_while_active_assertion() != 0 ||
        run_destroy_while_active_assertion() != 0 ||
        run_reentrant_finished_starts_next_job_assertion() != 0) {
        return 1;
    }

    return 0;
}
