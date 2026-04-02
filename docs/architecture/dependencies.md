# Dependency Strategy

This repository treats GitHub Actions as the source of truth for build verification.

## Selected strategy

- Windows-first base dependency provider: MSYS2 UCRT64.
- Build system: CMake.
- Qt discovery: `find_package(Qt6 ...)` through the active prefix path.
- FFmpeg 7.1.x `libavcodec`, `libavformat`, `libavutil`, `libswresample`, and `libswscale` discovery: `pkg-config`, but only from an explicit isolated install prefix.
- FFMS2 discovery: `pkg-config`, but only from an explicit isolated install prefix that is built against the pinned FFmpeg install.
- `libx264` and `libx265` discovery: `pkg-config`.
- `libassmod` discovery: `pkg-config`, but only from an explicit isolated install prefix.

## Why FFmpeg is handled separately

MSYS2 ships the latest FFmpeg package line, which can move ahead of the supported `7.1.x` series. The project environment is pinned to FFmpeg `7.1.x`, so configure must not silently resolve a newer system package.

To keep this explicit:

- FFmpeg is built from source in CI and the documented MSYS2 workflow.
- The current pin is `7.1.2`.
- The build installs FFmpeg into `.deps/ffmpeg/prefix`.
- CMake requires `UTSURE_FFMPEG_ROOT` when FFmpeg dependency validation is enabled.
- The dependency audit fails if any required `libav*` or `libsw*` pkg-config module resolves outside that prefix.

## Why `libassmod` is handled separately

The `libassmod` repository builds and installs a `libass`-compatible library and pkg-config file rather than a uniquely named `libassmod` package. That means the project must not silently resolve whichever `libass` happens to be first on the machine.

To keep this explicit:

- `libassmod` is built from source in CI.
- The CI build installs it into `.deps/libassmod/prefix`.
- CMake requires `UTSURE_LIBASSMOD_ROOT` when subtitle dependency validation is enabled.
- The dependency audit fails if `pkg-config libass` resolves outside that prefix.

## Why FFMS2 is handled separately

Preview now uses FFMS2 for indexed source access, but the main transcode and final-encode path stays on the existing FFmpeg/core pipeline. That means the build must make the preview dependency explicit without letting it bleed into unrelated media code.

To keep this explicit:

- FFMS2 is built from source in CI and the documented MSYS2 workflow.
- The current pin is upstream FFMS2 commit `25cef14386fcaaa58ee547065deee8f6e82c56a2`.
- The build installs FFMS2 into `.deps/ffms2/prefix`.
- CMake requires `UTSURE_FFMS2_ROOT` when preview dependency validation is enabled.
- The dependency audit fails if `pkg-config ffms2` resolves outside that prefix.
- FFMS2 is treated as a preview-only backend dependency; final encode still depends on the pinned FFmpeg stack and not on FFMS2 APIs.

## CMake contract

The root configure step now understands these dependency inputs:

- `UTSURE_ENABLE_DEPENDENCY_AUDIT`
  - When `ON`, configure-time dependency discovery verifies the planned external stack.
- `UTSURE_REQUIRE_FFMPEG`
  - When `ON`, configure fails unless FFmpeg is available from the expected isolated prefix.
- `UTSURE_FFMPEG_ROOT`
  - Prefix where the source-built FFmpeg install lives.
- `UTSURE_REQUIRE_FFMS2`
  - When `ON`, configure fails unless FFMS2 is available from the expected isolated prefix.
- `UTSURE_FFMS2_ROOT`
  - Prefix where the source-built FFMS2 install lives.
- `UTSURE_REQUIRE_LIBASSMOD`
  - When `ON`, configure fails unless `libassmod` is available from the expected isolated prefix.
- `UTSURE_LIBASSMOD_ROOT`
  - Prefix where the source-built `libassmod` install lives.

Resolved targets exposed for future adapter code:

- `utsure::ffmpeg`
- `utsure::ffms2`
- `utsure::x264`
- `utsure::x265`
- `utsure::subtitle_renderer_dependency`

The active transcode path does not depend on `libavfilter`.

## Windows CI path

- Install Qt 6 Widgets, the FFmpeg build prerequisites, `libx264`, `libx265`, and the `libassmod` build prerequisites from MSYS2 UCRT64.
- Download and build FFmpeg `7.1.2` from the official release tarball into `.deps/ffmpeg/prefix`.
- Clone and build FFMS2 from the upstream repository, pinned to commit `25cef14386fcaaa58ee547065deee8f6e82c56a2`, against that FFmpeg prefix.
- Clone and build `libassmod` from the upstream repository, pinned to commit `88a338192faf50505eb4cedfe7d1320265f1081f`.
- Put the FFmpeg, FFMS2, and `libassmod` prefixes ahead of the default pkg-config search path.
- Run configure-time dependency audit.
- Fail configure if any required FFmpeg or FFMS2 pkg-config module resolves outside the pinned prefix, or if the pinned `ffmpeg` executable reports a release outside the `7.1.x` series.
- Build the repository targets and smoke-launch the Qt app.

## Unresolved assumptions

- The current CI validates library presence and discovery, not end-to-end codec feature flags inside FFmpeg.
- FFMS2 remains preview-only; any future attempt to route final encode through FFMS2 would require a separate design pass.
- Linux/macOS dependency provider details are not pinned yet.
- The project is currently pinned to FFmpeg `7.1.x`; newer FFmpeg series are intentionally treated as unvalidated until the dependency gate is updated.
- Packaging and redistribution rules for codec-enabled builds still need explicit policy before release work starts.
