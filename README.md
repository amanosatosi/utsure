# utsure

`utsure` is a new desktop video encoder repository built around a reusable C++ core and a thin Qt 6 Widgets GUI.

## Current scope

The repository currently contains the initial project skeleton:

- `utsure_encoder_core`: reusable C++ target for future media, timeline, subtitle, and encode logic.
- `utsure_encoder_app`: Qt 6 Widgets desktop shell that links the core and launches a minimal window.
- Windows GitHub Actions build validation using MSYS2 UCRT64, CMake, Ninja, and Qt 6 Widgets.

No real media probing, timeline assembly, subtitle burn-in, or encoding logic is implemented yet.

## Layout

- `src/core/`: reusable core target and public headers.
- `src/app/`: Qt 6 Widgets desktop application.
- `cmake/`: small CMake helpers shared by repository targets.
- `docs/architecture/`: architecture notes for the current skeleton.
- `scripts/ci/`: CI entry points.

## Build validation

The repository is set up to build in GitHub Actions on Windows. The workflow installs MSYS2 UCRT64 packages for GCC, CMake, Ninja, and Qt 6, then:

1. Configures the project with CMake.
2. Builds `utsure_encoder_core`.
3. Builds `utsure_encoder_app`.
4. Launches the Qt app in offscreen smoke-test mode.
