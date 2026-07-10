#include "windows_taskbar_progress.hpp"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <shobjidl.h>

#include <QWidget>
#endif

namespace utsure::app {

namespace {

constexpr std::uint64_t kTaskbarProgressMaximum = 10000;

#ifdef _WIN32

ITaskbarList3 *as_taskbar_list(void *taskbar_list) noexcept {
    return static_cast<ITaskbarList3 *>(taskbar_list);
}

TBPFLAG to_native_progress_state(const TaskbarProgressState state) noexcept {
    switch (state) {
    case TaskbarProgressState::none:
        return TBPF_NOPROGRESS;
    case TaskbarProgressState::indeterminate:
        return TBPF_INDETERMINATE;
    case TaskbarProgressState::normal:
        return TBPF_NORMAL;
    case TaskbarProgressState::paused:
        return TBPF_PAUSED;
    }

    return TBPF_NOPROGRESS;
}

#endif

}  // namespace

std::uint64_t taskbar_progress_value(const double fraction) noexcept {
    if (!std::isfinite(fraction)) {
        return 0;
    }

    const double clamped_fraction = std::clamp(fraction, 0.0, 1.0);
    return static_cast<std::uint64_t>(std::llround(clamped_fraction * static_cast<double>(kTaskbarProgressMaximum)));
}

WindowsTaskbarProgress::~WindowsTaskbarProgress() {
#ifdef _WIN32
    if (taskbar_list_ != nullptr) {
        as_taskbar_list(taskbar_list_)->Release();
        taskbar_list_ = nullptr;
    }
    if (com_initialized_by_adapter_) {
        CoUninitialize();
    }
#endif
}

bool WindowsTaskbarProgress::ensure_initialized() noexcept {
#ifdef _WIN32
    if (initialization_attempted_) {
        return taskbar_list_ != nullptr;
    }
    initialization_attempted_ = true;

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(com_result)) {
        com_initialized_by_adapter_ = true;
    } else if (com_result != RPC_E_CHANGED_MODE) {
        return false;
    }

    ITaskbarList3 *taskbar_list = nullptr;
    const HRESULT create_result = CoCreateInstance(
        CLSID_TaskbarList,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&taskbar_list)
    );
    if (FAILED(create_result) || taskbar_list == nullptr) {
        return false;
    }

    if (FAILED(taskbar_list->HrInit())) {
        taskbar_list->Release();
        return false;
    }

    taskbar_list_ = taskbar_list;
    return true;
#else
    return false;
#endif
}

void WindowsTaskbarProgress::update(
    QWidget *window,
    const TaskbarProgressState state,
    const std::optional<double> fraction
) noexcept {
#ifdef _WIN32
    if (window == nullptr || !ensure_initialized()) {
        return;
    }

    const HWND window_handle = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(window->winId()));
    if (window_handle == nullptr) {
        return;
    }

    auto *taskbar_list = as_taskbar_list(taskbar_list_);
    (void)taskbar_list->SetProgressState(window_handle, to_native_progress_state(state));
    if (state == TaskbarProgressState::normal || state == TaskbarProgressState::paused) {
        (void)taskbar_list->SetProgressValue(
            window_handle,
            static_cast<ULONGLONG>(taskbar_progress_value(fraction.value_or(0.0))),
            static_cast<ULONGLONG>(kTaskbarProgressMaximum)
        );
    }
#else
    (void)window;
    (void)state;
    (void)fraction;
#endif
}

}  // namespace utsure::app
