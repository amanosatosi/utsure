# utsure

`utsure` is a new desktop video encoder repository built around a reusable C++ core and a thin Qt 6 Widgets GUI.

## Current scope

The repository currently contains the initial project skeleton plus the first media inspection, source decode path, and minimal software video encode backends:

- `utsure_encoder_core`: reusable C++ target for media inspection, source decode normalization, minimal software video encode backends, and future media, timeline, subtitle, and broader encode logic.
- `utsure_encoder_app`: Qt 6 Widgets desktop shell that links the core and launches a minimal window.
- Windows GitHub Actions build validation using MSYS2 UCRT64, CMake, Ninja, Qt 6 Widgets, FFmpeg, `libx264`, `libx265`, and a pinned source build of `libassmod`.

Implemented so far:

- FFmpeg-based primary stream inspection into structured core metadata.
- Main-source decode into normalized internal video frame and audio sample objects with explicit timestamps.
- Main-source software video encode backends for `libx264` and `libx265` with minimal codec, preset, and CRF settings.

Not implemented yet:

- Audio output encode.
- Timeline assembly.
- Subtitle burn-in.
- Broader encode session orchestration.

## Layout

- `src/core/`: reusable core target and public headers.
- `src/app/`: Qt 6 Widgets desktop application.
- `cmake/`: small CMake helpers shared by repository targets.
- `docs/architecture/`: architecture notes for the current skeleton.
- `docs/setup/`: Windows-first setup notes for developers and CI.
- `scripts/ci/`: CI entry points.

## Build validation

The repository is set up to build in GitHub Actions on Windows. The workflow:

1. Installs the MSYS2 UCRT64 toolchain and binary dependencies.
2. Builds a pinned `libassmod` source dependency into an isolated prefix.
3. Audits configure-time dependency discovery.
4. Configures the project with CMake.
5. Builds `utsure_encoder_core`, `utsure_encoder_app`, and the core media inspection, decode, and encode test executables.
6. Generates a deterministic sample media file and runs the core inspection, decode, and encode tests.
7. Launches the Qt app in offscreen smoke-test mode.

See:

- `docs/architecture/dependencies.md`
- `docs/architecture/adapters.md`
- `docs/setup/windows-msys2.md`
