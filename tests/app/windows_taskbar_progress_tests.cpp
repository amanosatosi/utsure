#include "windows_taskbar_progress.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

int main() {
    using utsure::app::taskbar_progress_value;

    assert(taskbar_progress_value(-0.25) == 0);
    assert(taskbar_progress_value(0.0) == 0);
    assert(taskbar_progress_value(0.1234) == 1234);
    assert(taskbar_progress_value(0.12345) == 1235);
    assert(taskbar_progress_value(1.0) == 10000);
    assert(taskbar_progress_value(1.25) == 10000);
    assert(taskbar_progress_value(std::numeric_limits<double>::quiet_NaN()) == 0);
    assert(taskbar_progress_value(std::numeric_limits<double>::infinity()) == 0);

    return 0;
}
