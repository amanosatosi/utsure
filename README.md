# utsure

`utsure` is a new desktop video encoder repository built around a reusable C++ core and a thin Qt 6 Widgets GUI.

## Current scope

The repository currently contains the initial project skeleton plus the first usable desktop GUI, the media inspection and decode path, minimal software video encode backends, a core encode-job model, a subtitle renderer abstraction, and `libassmod` subtitle burn-in:

- `utsure_encoder_core`: reusable C++ target for media inspection, source decode normalization, minimal software video encode backends, and future media, timeline, subtitle, and broader encode logic.
- `utsure_encoder_app`: Qt 6 Widgets desktop app that builds and runs the supported encode job flow against the core.
- Windows GitHub Actions build validation and portable artifact packaging using MSYS2 UCRT64, CMake, Ninja, Qt 6 Widgets, FFmpeg, `libx264`, `libx265`, and a pinned source build of `libassmod`.

Implemented so far:

- FFmpeg-based primary stream inspection into structured core metadata.
- Main-source decode into normalized internal video frame and audio sample objects with explicit timestamps.
- Main-source software video encode backends for `libx264` and `libx265` with minimal codec, preset, and CRF settings.
- A core job/config layer that drives inspect -> decode -> muxed video encode without introducing GUI-bound settings types.
- A technology-agnostic subtitle renderer/session boundary with timestamped RGBA-oriented overlay contracts.
- A `libassmod`-backed subtitle burn-in path that renders ASS subtitles onto decoded RGBA frames before final encode.
- A core timeline layer that assembles intro/main/outro segments, preserves the main clip cadence, stitches aligned normalized audio, and feeds the existing encode-job path.
- A first usable Qt 6 Widgets window for selecting source/subtitle/intro/outro assets, choosing H.264 or H.265 with preset and CRF, setting an output path, starting the encode, and viewing progress, logs, and errors.

These milestones are currently covered by the Windows GitHub Actions build and test workflow.

Not implemented yet:

- Audio output encode.
- Broader timeline, subtitle, and audio-capable encode session orchestration.

## Developer handoff

Start here when picking up the repository:

- `docs/setup/windows-msys2.md`: supported local setup, build, test, and packaging commands
- `docs/release/windows-portable.md`: current packaging and release expectations for the Windows bundle
- `docs/roadmap.md`: near-term versus later work

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
5. Builds `utsure_encoder_core`, `utsure_encoder_app`, and the core media inspection, decode, encode, timeline, job-level encode, subtitle-renderer abstraction, and subtitle burn-in test executables.
6. Generates deterministic sample media and subtitle files and runs the core inspection, decode, encode, timeline, job-level encode, subtitle-renderer abstraction, and subtitle burn-in tests.
7. Launches the Qt app in offscreen smoke-test mode and prints the window structure summary.
8. Packages a portable Windows bundle with `windeployqt`, bundled non-Qt DLLs, a downloadable `.zip` artifact, and a post-package extract-and-launch validation pass.

See:

- `docs/architecture/dependencies.md`
- `docs/architecture/adapters.md`
- `docs/setup/windows-msys2.md`
- `docs/release/windows-portable.md`
- `docs/roadmap.md`
