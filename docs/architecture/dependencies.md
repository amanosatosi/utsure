# Dependency Strategy

This repository treats GitHub Actions as the source of truth for build verification.

## Selected strategy

- Windows-first dependency provider: MSYS2 UCRT64.
- Build system: CMake.
- Qt discovery: `find_package(Qt6 ...)` through the active prefix path.
- FFmpeg, `libx264`, and `libx265` discovery: `pkg-config`.
- `libassmod` discovery: `pkg-config`, but only from an explicit isolated install prefix.

## Why `libassmod` is handled separately

The `libassmod` repository builds and installs a `libass`-compatible library and pkg-config file rather than a uniquely named `libassmod` package. That means the project must not silently resolve whichever `libass` happens to be first on the machine.

To keep this explicit:

- `libassmod` is built from source in CI.
- The CI build installs it into `.deps/libassmod/prefix`.
- CMake requires `UTSURE_LIBASSMOD_ROOT` when subtitle dependency validation is enabled.
- The dependency audit fails if `pkg-config libass` resolves outside that prefix.

## CMake contract

The root configure step now understands these dependency inputs:

- `UTSURE_ENABLE_DEPENDENCY_AUDIT`
  - When `ON`, configure-time dependency discovery verifies the planned external stack.
- `UTSURE_REQUIRE_LIBASSMOD`
  - When `ON`, configure fails unless `libassmod` is available from the expected isolated prefix.
- `UTSURE_LIBASSMOD_ROOT`
  - Prefix where the source-built `libassmod` install lives.

Resolved targets exposed for future adapter code:

- `utsure::ffmpeg`
- `utsure::x264`
- `utsure::x265`
- `utsure::subtitle_renderer_dependency`

## Windows CI path

- Install Qt 6 Widgets, FFmpeg, `libx264`, `libx265`, and the `libassmod` build prerequisites from MSYS2 UCRT64.
- Clone and build `libassmod` from the upstream repository, pinned to tag `1.0`.
- Put the `libassmod` prefix ahead of the default pkg-config search path.
- Run configure-time dependency audit.
- Build the repository targets and smoke-launch the Qt app.

## Unresolved assumptions

- The current CI validates library presence and discovery, not end-to-end codec feature flags inside FFmpeg.
- Linux/macOS dependency provider details are not pinned yet.
- The exact FFmpeg component set may expand when the media pipeline and subtitle burn path are implemented.
- Packaging and redistribution rules for codec-enabled builds still need explicit policy before release work starts.
