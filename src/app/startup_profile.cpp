#include "startup_profile.hpp"

#include <QEvent>
#include <QObject>
#include <QTimer>
#include <QWidget>

#include <array>
#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace utsure::app::startup {
namespace {

struct Entry {
    const char *label{};
    double from_main_ms{};
    double stage_ms{};
};

struct Profile {
    Clock::time_point main_entry{};
    Clock::time_point previous_checkpoint{};
    std::array<Entry, 64> entries{};
    std::size_t count{0};
    double pre_main_ms{-1.0};
};

Profile storage;
Profile *active_profile{nullptr};
bool profiling{false};
Clock::time_point import_started{};

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void record(const char *label, Clock::time_point now, Clock::time_point start) {
    if (active_profile->count < active_profile->entries.size()) {
        active_profile->entries[active_profile->count++] = {
            label, milliseconds(now - active_profile->main_entry), milliseconds(now - start)
        };
    }
}

void report() {
    if (active_profile->pre_main_ms >= 0.0) {
        std::fprintf(stderr, "[startup] Process creation to main (ms): %.3f\n", active_profile->pre_main_ms);
    }
    std::fprintf(stderr, "[startup] %11s %11s  %s\n", "Main + ms", "Stage ms", "Completed stage");
    for (std::size_t index = 0; index < active_profile->count; ++index) {
        const auto &entry = active_profile->entries[index];
        std::fprintf(stderr, "[startup] %11.3f %11.3f  %s\n", entry.from_main_ms, entry.stage_ms, entry.label);
    }
    std::fflush(stderr);
    active_profile = nullptr;
}

void report_process_resources() {
#if defined(_WIN32)
    const HANDLE process = GetCurrentProcess();
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        const auto ticks = [](FILETIME time) {
            return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        std::fprintf(stderr, "[startup] Ready CPU (ms): %.3f\n",
                     static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0);
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
        std::fprintf(stderr, "[startup] Ready working set (bytes): %llu\n",
                     static_cast<unsigned long long>(memory.WorkingSetSize));
        std::fprintf(stderr, "[startup] Ready private memory (bytes): %llu\n",
                     static_cast<unsigned long long>(memory.PrivateUsage));
    }
#endif
}

class FirstPaintObserver final : public QObject {
public:
    FirstPaintObserver(QWidget &window, std::function<void()> after_ready)
        : QObject(&window), after_ready_(std::move(after_ready)) {
        window.installEventFilter(this);
    }

private:
    bool eventFilter(QObject *window, QEvent *event) override {
        if (event->type() == QEvent::Paint) {
            window->removeEventFilter(this);
            checkpoint("First window paint event");
            QTimer::singleShot(0, this, [this] {
                checkpoint("Event loop turn after first paint");
                report_process_resources();
                report();
                monitor_event_loop();
                if (after_ready_) {
                    after_ready_();
                }
            });
        }
        return false;
    }
    std::function<void()> after_ready_;

    void monitor_event_loop() {
        auto *timer = new QTimer(this);
        timer->setTimerType(Qt::PreciseTimer);
        constexpr auto interval = std::chrono::milliseconds(10);
        connect(timer, &QTimer::timeout, this,
                [this, interval, previous = Clock::now(), started = Clock::now(), maximum = 0.0]() mutable {
            const auto now = Clock::now();
            maximum = std::max(maximum, milliseconds(now - previous - interval));
            previous = now;
            if (now - started >= std::chrono::seconds(1)) {
                std::fprintf(stderr, "[startup] Maximum UI timer lateness (ms): %.3f\n", maximum);
                deleteLater();
            }
        });
        timer->start(interval);
    }
};

}  // namespace

void begin(int argc, char *argv[], Clock::time_point main_entry) {
    bool enabled = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--") {
            break;
        }
        if (argument == "--startup-profile" || argument == "-startup-profile") {
            enabled = true;
            break;
        }
    }
    if (!enabled) {
        return;
    }

    storage.main_entry = main_entry;
    profiling = true;
    storage.previous_checkpoint = main_entry;
    active_profile = &storage;
#if defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{}, now{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        GetSystemTimePreciseAsFileTime(&now);
        const auto sampled_at = Clock::now();
        const auto ticks = [](FILETIME time) {
            return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        if (ticks(now) >= ticks(creation)) {
            storage.pre_main_ms = static_cast<double>(ticks(now) - ticks(creation)) / 10000.0
                - milliseconds(sampled_at - main_entry);
        }
    }
#endif
    checkpoint("Profile setup");
}

void checkpoint(const char *label) {
    if (active_profile != nullptr) {
        const auto now = Clock::now();
        record(label, now, active_profile->previous_checkpoint);
        active_profile->previous_checkpoint = now;
    }
}

void observe_first_paint(QWidget &window, std::function<void()> after_ready) {
    if (active_profile != nullptr) {
        new FirstPaintObserver(window, std::move(after_ready));
    }
}

void begin_import() {
    if (profiling) {
        import_started = Clock::now();
    }
}

void finish_import(int job_count) {
    if (import_started == Clock::time_point{}) {
        return;
    }
    std::fprintf(stderr, "[startup] Video import (ms): %.3f (%d jobs)\n",
                 milliseconds(Clock::now() - import_started), job_count);
    import_started = {};
}

Stage::Stage(const char *label)
    : label_(active_profile != nullptr ? label : nullptr),
      started_(label_ != nullptr ? Clock::now() : Clock::time_point{}) {}

Stage::~Stage() {
    if (label_ != nullptr && active_profile != nullptr) {
        record(label_, Clock::now(), started_);
    }
}

}  // namespace utsure::app::startup
