#pragma once

#include <chrono>

class QWidget;

namespace utsure::app::startup {

using Clock = std::chrono::steady_clock;

void begin(int argc, char *argv[], Clock::time_point main_entry);
void checkpoint(const char *label);
void observe_first_paint(QWidget &window);

class Stage final {
public:
    explicit Stage(const char *label);
    ~Stage();

    Stage(const Stage &) = delete;
    Stage &operator=(const Stage &) = delete;

private:
    const char *label_;
    Clock::time_point started_;
};

}  // namespace utsure::app::startup
