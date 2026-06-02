#include "encode_job_runner_controller.hpp"
#include "encode_job_runner_worker.hpp"

#include "utsure/core/job/batch_parallelism.hpp"
#include "utsure/core/job/encode_job.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

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
using utsure::core::job::ParallelBatchSettings;
using utsure::core::job::ParallelBatchSummary;
using utsure::core::media::OutputVideoCodec;

struct ProcessMemorySnapshot final {
    std::uint64_t rss_bytes{0};
    std::uint64_t peak_rss_bytes{0};
};

std::optional<ProcessMemorySnapshot> sample_process_memory() noexcept {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return std::nullopt;
    }
    return ProcessMemorySnapshot{
        .rss_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize),
        .peak_rss_bytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize)
    };
#else
    return std::nullopt;
#endif
}

void log_parallel_resource_context(
    const char *label,
    const int active_job_count,
    const int runner_slot_index,
    const int job_index,
    const ParallelBatchSummary &summary
) {
    std::cout << label
              << ".active_jobs=" << active_job_count
              << " runner_slot=" << runner_slot_index
              << " job_index=" << job_index
              << " planned_decoder_threads=" << summary.decoder_threads_per_job
              << " planned_encoder_threads=" << summary.encoder_threads_per_job
              << " estimated_video_workers=" << summary.video_workers_per_job
              << " estimated_subtitle_workers=" << summary.subtitle_workers_per_job
              << " estimated_total_threads=" << summary.estimated_total_threads
              << " overcommit=" << (summary.estimated_threads_exceed_usable_cores ? 1 : 0);
    if (const auto memory = sample_process_memory(); memory.has_value()) {
        std::cout << " current_rss=" << memory->rss_bytes
                  << " peak_rss=" << memory->peak_rss_bytes;
    } else {
        std::cout << " current_rss=unavailable peak_rss=unavailable";
    }
    std::cout << '\n';
}

void log_real_parallel_state(
    const char *label,
    const char *reason,
    const bool first_streaming,
    const bool second_streaming,
    const bool first_finished,
    const bool second_finished,
    const bool first_succeeded,
    const bool second_succeeded,
    const bool first_canceled,
    const bool second_canceled,
    const std::size_t initial_quarantine_count
) {
    const auto current_quarantine_count = EncodeJobRunnerController::quarantined_worker_count_for_tests();
    std::cerr << label
              << "." << reason
              << ": first_streaming=" << first_streaming
              << " second_streaming=" << second_streaming
              << " first_finished=" << first_finished
              << " second_finished=" << second_finished
              << " first_succeeded=" << first_succeeded
              << " second_succeeded=" << second_succeeded
              << " first_canceled=" << first_canceled
              << " second_canceled=" << second_canceled
              << " quarantine_current=" << current_quarantine_count
              << " quarantine_initial=" << initial_quarantine_count;
    if (const auto memory = sample_process_memory(); memory.has_value()) {
        std::cerr << " current_rss=" << memory->rss_bytes
                  << " peak_rss=" << memory->peak_rss_bytes;
    } else {
        std::cerr << " current_rss=unavailable peak_rss=unavailable";
    }
    std::cerr << '\n';
}

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
    std::atomic_int &max_active_count,
    const int fake_job_ticks = 20
) {
    return new EncodeJobRunnerWorker(
        [&started_count, &active_count, &max_active_count, fake_job_ticks](
            const EncodeJob &job,
            const EncodeJobRunOptions &options
        ) {
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

            for (int tick = 0; tick < fake_job_ticks; ++tick) {
                if (options.cancellation_requested && options.cancellation_requested()) {
                    return make_canceled_result(job);
                }
                QThread::msleep(5);
            }

            return make_success_result(job);
        }
    );
}

EncodeJob make_real_encode_job(
    const std::filesystem::path &input_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &output_path,
    const bool burn_subtitles
) {
    EncodeJob job{
        .input = {
            .main_source_path = input_path
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
    if (burn_subtitles) {
        job.subtitles = utsure::core::job::EncodeJobSubtitleSettings{
            .subtitle_path = subtitle_path,
            .format_hint = "ass"
        };
    }
    return job;
}

EncodeJob make_real_subtitle_job(
    const std::filesystem::path &input_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &output_path
) {
    return make_real_encode_job(input_path, subtitle_path, output_path, true);
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

    if (!controller.start_job(make_lifecycle_job("cancel-active.mp4"))) {
        return fail("Controller refused to start the fake active cancel job.");
    }
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
        if (!controller->start_job(make_lifecycle_job("destroy-active.mp4"))) {
            return fail("Controller refused to start the fake active destroy job.");
        }
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
    bool reentrant_start_accepted = false;
    EncodeJobRunnerController controller(make_blocking_worker(started_count, finished_count));
    QObject::connect(
        &controller,
        &EncodeJobRunnerController::job_finished,
        &controller,
        [&](const bool, const bool, const QString &, const QString &, const QString &) {
            ++job_finished_count;
            if (job_finished_count == 1) {
                reentrant_start_accepted = controller.start_job(make_lifecycle_job("second-reentrant.mp4"));
            }
        }
    );

    if (!controller.start_job(make_lifecycle_job("first-reentrant.mp4"))) {
        return fail("Controller refused to start the first reentrant fake job.");
    }
    if (!wait_until([&]() { return started_count.load() == 1; }, 1000)) {
        return fail("First reentrant fake job did not start.");
    }
    controller.cancel_job();
    if (!wait_until([&]() { return job_finished_count >= 1; }, 2000)) {
        return fail("First reentrant fake job did not finish after cancel.");
    }
    if (!reentrant_start_accepted) {
        return fail("job_finished handler could not start the second reentrant job.");
    }
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
        if (!controller.start_job(make_lifecycle_job(("queue-job-" + std::to_string(active_index) + ".mp4").c_str()))) {
            states[static_cast<std::size_t>(active_index)] = QueueItemState::failed;
            active_index = -1;
            ++terminal_count;
            return;
        }
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

    if (!wait_until([&]() { return terminal_count == kJobCount; }, 15000)) {
        std::cerr
            << "Deterministic fake queue timeout: terminal_count=" << terminal_count
            << " expected=" << kJobCount
            << " started_count=" << started_count.load()
            << " active_count=" << active_count.load()
            << " max_active_count=" << max_active_count.load()
            << " next_index=" << next_index
            << " active_index=" << active_index
            << '\n';
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

int run_failed_start_acceptance_assertion() {
    std::atomic_int started_count{0};
    std::atomic_int finished_count{0};
    EncodeJobRunnerController controller(make_blocking_worker(started_count, finished_count));
    if (!controller.start_job(make_lifecycle_job("accepted-first.mp4"))) {
        return fail("Controller refused the first fake job unexpectedly.");
    }
    if (!wait_until([&]() { return started_count.load() == 1; }, 1000)) {
        return fail("First fake job did not become active before failed-start assertion.");
    }
    if (controller.start_job(make_lifecycle_job("must-not-start-while-running.mp4"))) {
        return fail("Controller accepted a second job while already running.");
    }
    if (started_count.load() != 1) {
        return fail("Rejected start unexpectedly reached the worker.");
    }

    controller.cancel_job();
    if (!wait_until([&]() { return finished_count.load() == 1; }, 2000)) {
        return fail("Failed-start assertion cleanup did not cancel the active job.");
    }

    std::cout << "runner_lifecycle.failed_start_acceptance=ok\n";
    return 0;
}

int run_queue_layer_dispatch_assertion() {
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

    struct PlannedQueueJob final {
        int job_index{0};
        EncodeJob job{};
    };

    constexpr int kJobCount = 12;
    constexpr int kQueuedCancelIndex = 5;
    constexpr int kActiveCancelIndex = 1;
    std::atomic_int started_count{0};
    std::atomic_int active_count{0};
    std::atomic_int max_active_count{0};
    std::vector<QueueItemState> states(kJobCount, QueueItemState::queued);
    std::vector<PlannedQueueJob> planned_jobs{};
    std::vector<int> dispatch_order{};
    planned_jobs.reserve(kJobCount);
    dispatch_order.reserve(kJobCount);
    for (int index = 0; index < kJobCount; ++index) {
        planned_jobs.push_back(PlannedQueueJob{
            .job_index = index,
            .job = make_lifecycle_job(("queue-layer-job-" + std::to_string(index) + ".mp4").c_str())
        });
    }

    int queue_cursor = 0;
    int active_job_index = -1;
    int terminal_count = 0;
    EncodeJobRunnerController controller(make_queue_worker(started_count, active_count, max_active_count));
    std::function<void()> start_available_jobs;
    start_available_jobs = [&]() {
        if (controller.is_running() || active_job_index >= 0) {
            return;
        }

        while (queue_cursor < static_cast<int>(planned_jobs.size())) {
            const auto &planned_job = planned_jobs[static_cast<std::size_t>(queue_cursor++)];
            const int job_index = planned_job.job_index;
            if (states[static_cast<std::size_t>(job_index)] == QueueItemState::canceled) {
                continue;
            }

            states[static_cast<std::size_t>(job_index)] = QueueItemState::starting;
            active_job_index = job_index;
            dispatch_order.push_back(job_index);
            if (!controller.start_job(planned_job.job)) {
                states[static_cast<std::size_t>(job_index)] = QueueItemState::failed;
                active_job_index = -1;
                ++terminal_count;
                continue;
            }

            states[static_cast<std::size_t>(job_index)] = QueueItemState::running;
            return;
        }
    };

    QObject::connect(
        &controller,
        &EncodeJobRunnerController::job_finished,
        &controller,
        [&](const bool succeeded, const bool canceled, const QString &, const QString &, const QString &) {
            if (active_job_index < 0) {
                return;
            }

            states[static_cast<std::size_t>(active_job_index)] = QueueItemState::finishing;
            states[static_cast<std::size_t>(active_job_index)] = canceled
                ? QueueItemState::canceled
                : succeeded ? QueueItemState::completed : QueueItemState::failed;
            active_job_index = -1;
            ++terminal_count;
            QTimer::singleShot(0, &controller, [&start_available_jobs]() {
                start_available_jobs();
            });
        }
    );

    start_available_jobs();
    if (!wait_until([&]() { return started_count.load() == 1; }, 1000)) {
        return fail("Queue-layer dispatch did not start the first job.");
    }

    states[static_cast<std::size_t>(kQueuedCancelIndex)] = QueueItemState::canceled;
    ++terminal_count;
    if (!wait_until([&]() {
            return states[0] == QueueItemState::completed && started_count.load() >= 2;
        }, 3000)) {
        return fail("Canceling a queued job interfered with the active queue-layer job.");
    }

    if (states[0] != QueueItemState::completed || states[static_cast<std::size_t>(kQueuedCancelIndex)] != QueueItemState::canceled) {
        return fail("Queue-layer queued cancellation did not preserve active/queued state.");
    }
    if (active_job_index != kActiveCancelIndex) {
        return fail("Queue-layer dispatch order was not deterministic before active cancel.");
    }

    states[static_cast<std::size_t>(kActiveCancelIndex)] = QueueItemState::cancel_requested;
    controller.cancel_job();
    if (!wait_until([&]() { return terminal_count == kJobCount; }, 15000)) {
        std::cerr
            << "Queue-layer dispatch timeout: terminal_count=" << terminal_count
            << " expected=" << kJobCount
            << " started_count=" << started_count.load()
            << " active_count=" << active_count.load()
            << " max_active_count=" << max_active_count.load()
            << " queue_cursor=" << queue_cursor
            << " active_job_index=" << active_job_index
            << '\n';
        return fail("Queue-layer dispatch did not put every job into a terminal state.");
    }

    if (max_active_count.load() != 1 || active_count.load() != 0) {
        return fail("Queue-layer dispatch allowed more than one active job.");
    }
    if (states[static_cast<std::size_t>(kActiveCancelIndex)] != QueueItemState::canceled) {
        return fail("Queue-layer active cancel did not transition to canceled.");
    }
    for (int index = 0; index < kJobCount; ++index) {
        if (index == kQueuedCancelIndex || index == kActiveCancelIndex) {
            continue;
        }
        if (states[static_cast<std::size_t>(index)] != QueueItemState::completed) {
            return fail("Queue-layer dispatch left a non-canceled job outside completed state.");
        }
    }
    if (std::find(dispatch_order.begin(), dispatch_order.end(), kQueuedCancelIndex) != dispatch_order.end()) {
        return fail("Queue-layer dispatch started a queued-canceled job.");
    }

    std::cout << "runner_lifecycle.queue_layer_jobs=12\n";
    std::cout << "runner_lifecycle.queue_layer_max_active=" << max_active_count.load() << '\n';
    return 0;
}

int run_parallel_controller_smoke_assertion() {
    constexpr int kLongFakeJobTicks = 200;
    const auto initial_quarantine_count = EncodeJobRunnerController::quarantined_worker_count_for_tests();
    std::atomic_int started_count{0};
    std::atomic_int active_count{0};
    std::atomic_int max_active_count{0};
    bool first_finished = false;
    bool first_canceled = false;
    bool second_finished = false;
    bool second_succeeded = false;

    EncodeJobRunnerController first_controller(
        make_queue_worker(started_count, active_count, max_active_count, kLongFakeJobTicks)
    );
    EncodeJobRunnerController second_controller(
        make_queue_worker(started_count, active_count, max_active_count, kLongFakeJobTicks)
    );

    QObject::connect(
        &first_controller,
        &EncodeJobRunnerController::job_finished,
        &first_controller,
        [&](const bool, const bool canceled, const QString &, const QString &, const QString &) {
            first_finished = true;
            first_canceled = canceled;
        }
    );
    QObject::connect(
        &second_controller,
        &EncodeJobRunnerController::job_finished,
        &second_controller,
        [&](const bool succeeded, const bool, const QString &, const QString &, const QString &) {
            second_finished = true;
            second_succeeded = succeeded;
        }
    );

    if (!first_controller.start_job(make_lifecycle_job("parallel-first.mp4")) ||
        !second_controller.start_job(make_lifecycle_job("parallel-second.mp4"))) {
        return fail("Parallel controller smoke could not start both fake jobs.");
    }

    if (!wait_until([&]() { return started_count.load() == 2 && max_active_count.load() >= 2; }, 2000)) {
        return fail("Parallel controller smoke did not observe two concurrently active jobs.");
    }

    first_controller.cancel_job();
    if (!wait_until([&]() { return first_finished && second_finished; }, 5000)) {
        return fail("Parallel controller smoke did not finish/cancel both jobs before timeout.");
    }

    if (!first_canceled || !second_succeeded || active_count.load() != 0) {
        return fail("Parallel controller smoke did not preserve per-job cancel/success outcomes.");
    }

    if (EncodeJobRunnerController::quarantined_worker_count_for_tests() != initial_quarantine_count) {
        return fail("Parallel controller smoke used the encode-worker quarantine fallback.");
    }

    std::cout << "runner_lifecycle.parallel_fake_jobs=2\n";
    std::cout << "runner_lifecycle.parallel_fake_max_active=" << max_active_count.load() << '\n';
    return 0;
}

int run_real_parallel_pair_assertion(
    const char *label,
    const std::filesystem::path &input_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &first_output_path,
    const std::filesystem::path &second_output_path,
    const bool burn_subtitles,
    const bool cancel_first_job
) {
    constexpr std::uint64_t kAllowedCurrentRssGrowthBytes = 512ULL * 1024ULL * 1024ULL;
    const auto initial_memory = sample_process_memory();
    const auto initial_quarantine_count = EncodeJobRunnerController::quarantined_worker_count_for_tests();
    const auto resource_summary = utsure::core::job::BatchParallelism::summarize(
        ParallelBatchSettings{
            .enabled = true,
            .requested_job_count = 2
        },
        4U
    );

    bool first_streaming = false;
    bool second_streaming = false;
    bool first_finished = false;
    bool second_finished = false;
    bool first_succeeded = false;
    bool second_succeeded = false;
    bool first_canceled = false;
    bool second_canceled = false;

    EncodeJobRunnerController first_controller;
    EncodeJobRunnerController second_controller;
    QObject::connect(
        &first_controller,
        &EncodeJobRunnerController::progress_changed,
        &first_controller,
        [&first_streaming](const EncodeJobProgress &progress) {
            if (progress.stage == EncodeJobStage::encoding_output) {
                first_streaming = true;
            }
        }
    );
    QObject::connect(
        &second_controller,
        &EncodeJobRunnerController::progress_changed,
        &second_controller,
        [&second_streaming](const EncodeJobProgress &progress) {
            if (progress.stage == EncodeJobStage::encoding_output) {
                second_streaming = true;
            }
        }
    );
    QObject::connect(
        &first_controller,
        &EncodeJobRunnerController::job_finished,
        &first_controller,
        [&](const bool succeeded, const bool canceled, const QString &, const QString &, const QString &) {
            first_finished = true;
            first_succeeded = succeeded;
            first_canceled = canceled;
        }
    );
    QObject::connect(
        &second_controller,
        &EncodeJobRunnerController::job_finished,
        &second_controller,
        [&](const bool succeeded, const bool canceled, const QString &, const QString &, const QString &) {
            second_finished = true;
            second_succeeded = succeeded;
            second_canceled = canceled;
        }
    );

    log_parallel_resource_context(label, 2, 0, 0, resource_summary);
    log_parallel_resource_context(label, 2, 1, 1, resource_summary);
    if (!first_controller.start_job(make_real_encode_job(input_path, subtitle_path, first_output_path, burn_subtitles)) ||
        !second_controller.start_job(make_real_encode_job(input_path, subtitle_path, second_output_path, burn_subtitles))) {
        return fail("Real parallel smoke could not start both encode jobs.");
    }

    if (cancel_first_job) {
        if (!wait_until([&]() { return first_streaming || first_finished; }, 15000)) {
            log_real_parallel_state(
                label,
                "cancel_start_timeout",
                first_streaming,
                second_streaming,
                first_finished,
                second_finished,
                first_succeeded,
                second_succeeded,
                first_canceled,
                second_canceled,
                initial_quarantine_count
            );
            return fail("Real parallel cancel smoke did not observe the first job entering active encode work.");
        }
        if (first_finished) {
            log_real_parallel_state(
                label,
                "cancel_start_finished_early",
                first_streaming,
                second_streaming,
                first_finished,
                second_finished,
                first_succeeded,
                second_succeeded,
                first_canceled,
                second_canceled,
                initial_quarantine_count
            );
            return fail("Real parallel cancel smoke finished the first job before cancellation could be exercised.");
        }
        first_controller.cancel_job();
    } else if (!wait_until([&]() {
            return (first_streaming || first_finished) && (second_streaming || second_finished);
        }, 15000)) {
        log_real_parallel_state(
            label,
            "active_work_timeout",
            first_streaming,
            second_streaming,
            first_finished,
            second_finished,
            first_succeeded,
            second_succeeded,
            first_canceled,
            second_canceled,
            initial_quarantine_count
        );
        return fail("Real parallel smoke did not observe both jobs entering active encode work.");
    }

    if (!wait_until([&]() { return first_finished && second_finished; }, 30000)) {
        log_real_parallel_state(
            label,
            "finish_timeout",
            first_streaming,
            second_streaming,
            first_finished,
            second_finished,
            first_succeeded,
            second_succeeded,
            first_canceled,
            second_canceled,
            initial_quarantine_count
        );
        return fail("Real parallel smoke did not finish before timeout.");
    }

    if (cancel_first_job) {
        if (!first_canceled || !second_succeeded || second_canceled) {
            log_real_parallel_state(
                label,
                "unexpected_cancel_outcome",
                first_streaming,
                second_streaming,
                first_finished,
                second_finished,
                first_succeeded,
                second_succeeded,
                first_canceled,
                second_canceled,
                initial_quarantine_count
            );
            return fail("Real parallel cancel smoke did not preserve cancel-one/success-other outcomes.");
        }
    } else if (!first_succeeded || !second_succeeded || first_canceled || second_canceled) {
        log_real_parallel_state(
            label,
            "unexpected_success_outcome",
            first_streaming,
            second_streaming,
            first_finished,
            second_finished,
            first_succeeded,
            second_succeeded,
            first_canceled,
            second_canceled,
            initial_quarantine_count
        );
        return fail("Real parallel smoke did not complete both jobs successfully.");
    }

    log_parallel_resource_context(label, 0, 0, 0, resource_summary);
    if (const auto final_memory = sample_process_memory(); initial_memory.has_value() && final_memory.has_value()) {
        std::cout << label
                  << ".current_rss_initial=" << initial_memory->rss_bytes
                  << " current_rss_final=" << final_memory->rss_bytes
                  << " peak_rss_final=" << final_memory->peak_rss_bytes << '\n';
        if (final_memory->rss_bytes > initial_memory->rss_bytes + kAllowedCurrentRssGrowthBytes) {
            return fail("Real parallel smoke current RSS grew beyond the allowed diagnostic tolerance.");
        }
    }

    if (EncodeJobRunnerController::quarantined_worker_count_for_tests() != initial_quarantine_count) {
        return fail("Real parallel smoke used the encode-worker quarantine fallback.");
    }

    std::cout << label << "=ok\n";
    return 0;
}

int run_real_parallel_smoke_assertions(
    const std::filesystem::path &input_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &no_subtitle_first_output_path,
    const std::filesystem::path &no_subtitle_second_output_path,
    const std::filesystem::path &subtitle_first_output_path,
    const std::filesystem::path &subtitle_second_output_path,
    const std::filesystem::path &cancel_first_output_path,
    const std::filesystem::path &cancel_second_output_path
) {
    std::cout << "parallel_debug_matrix=no_subtitles,subtitles_normal,subtitles_serialized_with_UTSURE_SERIALIZE_SUBTITLE_SETUP=1\n";
    if (run_real_parallel_pair_assertion(
            "runner_lifecycle.real_parallel_no_subtitle",
            input_path,
            subtitle_path,
            no_subtitle_first_output_path,
            no_subtitle_second_output_path,
            false,
            false
        ) != 0) {
        return 1;
    }

    if (run_real_parallel_pair_assertion(
            "runner_lifecycle.real_parallel_subtitle",
            input_path,
            subtitle_path,
            subtitle_first_output_path,
            subtitle_second_output_path,
            true,
            false
        ) != 0) {
        return 1;
    }

    if (run_real_parallel_pair_assertion(
            "runner_lifecycle.real_parallel_cancel_one",
            input_path,
            subtitle_path,
            cancel_first_output_path,
            cancel_second_output_path,
            true,
            true
        ) != 0) {
        return 1;
    }

    return 0;
}

int run_real_cancel_and_destroy_assertion(
    const std::filesystem::path &input_path,
    const std::filesystem::path &subtitle_path,
    const std::filesystem::path &cancel_output_path,
    const std::filesystem::path &destroy_output_path
) {
    const auto initial_quarantine_count = EncodeJobRunnerController::quarantined_worker_count_for_tests();
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

        if (!controller.start_job(make_real_subtitle_job(input_path, subtitle_path, cancel_output_path))) {
            return fail("Controller refused to start the real cancel encode job.");
        }
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
    if (EncodeJobRunnerController::quarantined_worker_count_for_tests() != initial_quarantine_count) {
        return fail("Real active cancellation used the encode-worker quarantine fallback.");
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

        if (!controller->start_job(make_real_subtitle_job(input_path, subtitle_path, destroy_output_path))) {
            return fail("Controller refused to start the real destroy encode job.");
        }
        if (!wait_until([&]() { return streaming_stage_seen; }, 10000)) {
            return fail("Real encode did not reach streaming stage before destroy timeout.");
        }
        destruction_timer.start();
        controller.reset();
        if (destruction_timer.elapsed() > 7000) {
            return fail("Destroying a controller during a real encode exceeded the bounded shutdown timeout.");
        }
    }
    if (EncodeJobRunnerController::quarantined_worker_count_for_tests() != initial_quarantine_count) {
        return fail("Real destroy during active encode used the encode-worker quarantine fallback.");
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

    if (argc >= 2 && std::string_view(argv[1]) == "--real-cancel-destroy") {
        if (argc != 6) {
            return fail("Usage: utsure_app_encode_job_runner_lifecycle_tests --real-cancel-destroy <input> <subtitle> <cancel-output> <destroy-output>");
        }
        return run_real_cancel_and_destroy_assertion(argv[2], argv[3], argv[4], argv[5]);
    }

    if (argc >= 2 && std::string_view(argv[1]) == "--real-parallel-smokes") {
        if (argc != 10) {
            return fail("Usage: utsure_app_encode_job_runner_lifecycle_tests --real-parallel-smokes <input> <subtitle> <no-sub-a> <no-sub-b> <sub-a> <sub-b> <cancel-a> <cancel-b>");
        }
        return run_real_parallel_smoke_assertions(
            argv[2],
            argv[3],
            argv[4],
            argv[5],
            argv[6],
            argv[7],
            argv[8],
            argv[9]
        );
    }

    if (argc != 1) {
        return fail("Unknown mode for utsure_app_encode_job_runner_lifecycle_tests.");
    }

    if (run_cancel_idle_assertion() != 0 ||
        run_cancel_while_active_assertion() != 0 ||
        run_destroy_while_active_assertion() != 0 ||
        run_reentrant_finished_starts_next_job_assertion() != 0 ||
        run_deterministic_queue_assertion() != 0 ||
        run_failed_start_acceptance_assertion() != 0 ||
        run_queue_layer_dispatch_assertion() != 0 ||
        run_parallel_controller_smoke_assertion() != 0) {
        return 1;
    }

    return 0;
}
