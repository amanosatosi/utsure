#include "encode_job_runner_controller.hpp"
#include "encode_job_runner_worker.hpp"

#include "utsure/core/job/encode_job.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using utsure::core::job::EncodeJob;
using utsure::core::job::EncodeJobError;
using utsure::core::job::EncodeJobLogLevel;
using utsure::core::job::EncodeJobLogMessage;
using utsure::core::job::EncodeJobProgress;
using utsure::core::job::EncodeJobResult;
using utsure::core::job::EncodeJobRunOptions;
using utsure::core::job::EncodeJobSummary;
using utsure::core::job::EncodeJobStage;
using utsure::core::media::OutputVideoCodec;

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

EncodeJobResult make_success_result(const EncodeJob &job) {
    return EncodeJobResult{
        .encode_job_summary = EncodeJobSummary{
            .job = job,
            .encoded_media_summary = {
                .output_path = job.output.output_path
            }
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

EncodeJobRunnerWorker *make_queue_worker(
    std::atomic_int &started_count,
    std::atomic_int &active_count,
    std::atomic_int &max_active_count
) {
    return new EncodeJobRunnerWorker(
        [&started_count, &active_count, &max_active_count](const EncodeJob &job, const EncodeJobRunOptions &options) {
            ++started_count;
            const int active = ++active_count;
            int observed_max = max_active_count.load();
            while (active > observed_max && !max_active_count.compare_exchange_weak(observed_max, active)) {
            }
            struct ActiveGuard final {
                std::atomic_int &count;
                ~ActiveGuard() {
                    --count;
                }
            } active_guard{active_count};

            for (int tick = 0; tick < 50; ++tick) {
                if (options.cancellation_requested && options.cancellation_requested()) {
                    return make_canceled_result(job);
                }
                QThread::msleep(5);
            }

            return make_success_result(job);
        }
    );
}

EncodeJob make_real_subtitle_job(
    const std::filesystem::path &input_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &output_path
) {
    return EncodeJob{
        .input = {
            .main_source_path = input_path
        },
        .subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        },
        .output = {
            .output_path = output_path,
            .video = {
                .codec = OutputVideoCodec::h264,
                .preset = "medium",
                .crf = 30
            }
        },
        .execution = {
            .threading = {
                .decoder_thread_count_override = 1,
                .encoder_thread_count_override = 1,
                .logical_core_count_override = 2U
            },
            .video_frame_queue_depth_override = 4U
        }
    };
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

int run_deterministic_queue_assertion() {
    enum class QueueItemState {
        queued,
        starting,
        running,
        cancel_requested,
        finishing,
        completed,
        failed,
        canceled
    };

    constexpr int kJobCount = 12;
    constexpr int kQueuedCancelIndex = 5;
    std::atomic_int started_count{0};
    std::atomic_int active_count{0};
    std::atomic_int max_active_count{0};
    int terminal_count = 0;
    int next_index = 0;
    int active_index = -1;
    std::vector<QueueItemState> states(kJobCount, QueueItemState::queued);
    std::vector<int> started_order{};
    started_order.reserve(kJobCount);

    EncodeJobRunnerController controller(make_queue_worker(started_count, active_count, max_active_count));
    std::function<void()> start_next;
    start_next = [&]() {
        if (controller.is_running() || active_index >= 0) {
            return;
        }

        while (next_index < kJobCount && states[static_cast<std::size_t>(next_index)] == QueueItemState::canceled) {
            ++next_index;
        }
        if (next_index >= kJobCount) {
            return;
        }

        active_index = next_index;
        states[static_cast<std::size_t>(active_index)] = QueueItemState::starting;
        started_order.push_back(active_index);
        controller.start_job(make_lifecycle_job(("queue-job-" + std::to_string(active_index) + ".mp4").c_str()));
        states[static_cast<std::size_t>(active_index)] = QueueItemState::running;
        ++next_index;
    };

    QObject::connect(
        &controller,
        &EncodeJobRunnerController::job_finished,
        &controller,
        [&](const bool succeeded, const bool canceled, const QString &, const QString &, const QString &) {
            if (active_index < 0) {
                return;
            }

            states[static_cast<std::size_t>(active_index)] = QueueItemState::finishing;
            states[static_cast<std::size_t>(active_index)] = canceled
                ? QueueItemState::canceled
                : succeeded ? QueueItemState::completed : QueueItemState::failed;
            active_index = -1;
            ++terminal_count;
            QTimer::singleShot(0, &controller, [&start_next]() {
                start_next();
            });
        }
    );

    start_next();
    if (!wait_until([&]() { return started_count.load() == 1; }, 1000)) {
        return fail("Deterministic queue did not start its first active job.");
    }

    states[static_cast<std::size_t>(kQueuedCancelIndex)] = QueueItemState::canceled;
    ++terminal_count;
    states[0] = QueueItemState::cancel_requested;
    controller.cancel_job();

    if (!wait_until([&]() { return terminal_count == kJobCount; }, 5000)) {
        return fail("Deterministic fake queue did not put every job into a terminal state.");
    }

    if (max_active_count.load() != 1 || active_count.load() != 0) {
        return fail("Deterministic fake queue allowed overlapping active jobs.");
    }
    if (states[0] != QueueItemState::canceled || states[static_cast<std::size_t>(kQueuedCancelIndex)] != QueueItemState::canceled) {
        return fail("Deterministic fake queue did not preserve active and queued cancellation states.");
    }
    for (int index = 1; index < kJobCount; ++index) {
        if (index == kQueuedCancelIndex) {
            continue;
        }
        if (states[static_cast<std::size_t>(index)] != QueueItemState::completed) {
            return fail("Deterministic fake queue left a non-canceled job outside completed state.");
        }
    }
    if (std::find(started_order.begin(), started_order.end(), kQueuedCancelIndex) != started_order.end()) {
        return fail("Deterministic fake queue started a job that was canceled while queued.");
    }

    std::cout << "runner_lifecycle.queue_jobs=12\n";
    std::cout << "runner_lifecycle.queue_max_active=" << max_active_count.load() << '\n';
    return 0;
}

int run_real_cancel_and_destroy_assertion(
    const std::filesystem::path &input_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &cancel_output_path,
    const std::filesystem::path &destroy_output_path
) {
    {
        bool streaming_stage_seen = false;
        bool canceled_finished = false;
        EncodeJobRunnerController controller;
        QObject::connect(
            &controller,
            &EncodeJobRunnerController::progress_changed,
            &controller,
            [&streaming_stage_seen](const EncodeJobProgress &progress) {
                if (progress.stage == EncodeJobStage::encoding_output) {
                    streaming_stage_seen = true;
                }
            }
        );
        QObject::connect(
            &controller,
            &EncodeJobRunnerController::job_finished,
            &controller,
            [&canceled_finished](const bool, const bool canceled, const QString &, const QString &, const QString &) {
                canceled_finished = canceled;
            }
        );

        controller.start_job(make_real_subtitle_job(input_path, subtitle_path, cancel_output_path));
        if (!wait_until([&]() { return streaming_stage_seen || canceled_finished; }, 10000)) {
            return fail("Real encode did not reach streaming stage before cancel timeout.");
        }
        if (canceled_finished) {
            return fail("Real encode finished before active cancellation could be exercised.");
        }
        controller.cancel_job();
        if (!wait_until([&]() { return canceled_finished; }, 10000)) {
            return fail("Real active encode cancellation did not finish before timeout.");
        }
    }

    {
        bool streaming_stage_seen = false;
        QElapsedTimer destruction_timer;
        auto controller = std::make_unique<EncodeJobRunnerController>();
        QObject::connect(
            controller.get(),
            &EncodeJobRunnerController::progress_changed,
            controller.get(),
            [&streaming_stage_seen](const EncodeJobProgress &progress) {
                if (progress.stage == EncodeJobStage::encoding_output) {
                    streaming_stage_seen = true;
                }
            }
        );

        controller->start_job(make_real_subtitle_job(input_path, subtitle_path, destroy_output_path));
        if (!wait_until([&]() { return streaming_stage_seen; }, 10000)) {
            return fail("Real encode did not reach streaming stage before destroy timeout.");
        }
        destruction_timer.start();
        controller.reset();
        if (destruction_timer.elapsed() > 7000) {
            return fail("Destroying a controller during a real encode exceeded the bounded shutdown timeout.");
        }
    }

    std::cout << "runner_lifecycle.real_cancel_destroy=ok\n";
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
        run_reentrant_finished_starts_next_job_assertion() != 0 ||
        run_deterministic_queue_assertion() != 0) {
        return 1;
    }

    if (argc == 6 && std::string_view(argv[1]) == "--real-cancel-destroy") {
        return run_real_cancel_and_destroy_assertion(argv[2], argv[3], argv[4], argv[5]);
    }

    return 0;
}
