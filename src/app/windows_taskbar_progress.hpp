#pragma once

#include <cstdint>
#include <optional>

class QWidget;

namespace utsure::app {

enum class TaskbarProgressState : std::uint8_t {
    none,
    indeterminate,
    normal,
    paused
};

[[nodiscard]] std::uint64_t taskbar_progress_value(double fraction) noexcept;

class WindowsTaskbarProgress final {
public:
    WindowsTaskbarProgress() = default;
    ~WindowsTaskbarProgress();

    WindowsTaskbarProgress(const WindowsTaskbarProgress &) = delete;
    WindowsTaskbarProgress &operator=(const WindowsTaskbarProgress &) = delete;

    void update(
        QWidget *window,
        TaskbarProgressState state,
        std::optional<double> fraction = std::nullopt
    ) noexcept;

private:
    [[nodiscard]] bool ensure_initialized() noexcept;

#ifdef _WIN32
    void *taskbar_list_{nullptr};
    bool initialization_attempted_{false};
    bool com_initialized_by_adapter_{false};
#endif
};

}  // namespace utsure::app
