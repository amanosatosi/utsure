#include "startup_profile.hpp"

#include <QEvent>
#include <QObject>
#include <QTimer>
#include <QWidget>

#include <array>
#include <cstdio>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
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

class FirstPaintObserver final : public QObject {
public:
    explicit FirstPaintObserver(QWidget &window) : QObject(&window) {
        window.installEventFilter(this);
    }

private:
    bool eventFilter(QObject *window, QEvent *event) override {
        if (event->type() == QEvent::Paint) {
            window->removeEventFilter(this);
            checkpoint("First window paint event");
            QTimer::singleShot(0, window, [] {
                checkpoint("Event loop turn after first paint");
                report();
            });
            deleteLater();
        }
        return false;
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

void observe_first_paint(QWidget &window) {
    if (active_profile != nullptr) {
        new FirstPaintObserver(window);
    }
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
