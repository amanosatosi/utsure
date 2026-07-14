# Utsure v1.0

Utsure is a Windows-first desktop video encoder for batch encoding and ASS/SSA hardsub workflows. Version 1.0 is the first release-ready milestone of the Qt 6 application and its reusable C++ media core.

## Highlights

- Drag files or folders into a naturally ordered encode queue and edit multiple queued jobs.
- Run jobs sequentially or with configurable parallel encoding.
- Encode H.264 with `libx264` or H.265 with `libx265`, with CRF, preset, resize, and reusable profile controls.
- Change the active encode process priority while work is running.
- Use a bounded-memory FFmpeg streaming pipeline for decode, conversion, audio processing, video encode, and muxing.
- Encode AAC audio, copy compatible source audio, disable audio, or select a specific source track.
- Burn ASS/SSA subtitles through `libassmod`, including bundled-font preparation and supported `\img` sidecar assets.
- Automatically match subtitles or select them manually per job.
- Preview video and audio, step through frames, jump to a timestamp, and set trim boundaries.
- Compose intro, main, and end-card/outro media while retaining the main video's output frame rate.
- Generate output names from configurable tokens and avoid collisions across a batch.
- Follow each active job through the Windows taskbar progress indicator and receive compact per-job success/failure notifications.
- Inspect per-job and session logs; Windows crash dumps include build and runtime diagnostics.

## Windows release

The supported v1.0 release format is a portable Windows x64 `.zip` bundle. It includes `utsure.exe`, the Qt runtime, required media/subtitle libraries, the ASS font-collection helper, licenses, and dependency manifests.

To use it:

1. Extract the complete portable archive to a writable folder.
2. Keep the bundled files and directories together.
3. Run `utsure.exe`.
4. Add source videos or folders, configure the selected jobs, choose output paths, and start the queue.

Utsure v1.0 is portable rather than installed. It does not currently include an installer, code signing, automatic updates, or file associations.

## Version identity

The application identifies itself as version `1.0` in its window title, Qt application metadata, information dialog, startup/build diagnostics, and crash reports. CMake's top-level project version is the authoritative source for normal builds.

## Supported pipeline

Utsure v1.0 uses:

- C++20 and Qt 6 Widgets for the desktop application.
- A pinned Mangetsu FFmpeg 7.1-based build for the media pipeline.
- FFMS2 for preview indexing and frame access.
- `libx264` and `libx265` for software video encoding.
- Mangetsu `libassmod` for subtitle rendering.
- FontCollector for preparing fonts used by ASS subtitle scripts.

The application is Windows-first. Linux and macOS builds are not release-validated yet, and hardware-accelerated encode/decode is not part of v1.0.

## Building and validation

GitHub Actions is the authoritative build and test environment. The Windows workflow builds the pinned dependencies, configures with CMake and Ninja under MSYS2 UCRT64, builds the app and test targets, runs the automated validation set, packages the portable bundle, extracts it, and smoke-tests the packaged application outside the build tree.

Developer and release documentation:

- [`docs/setup/windows-msys2.md`](docs/setup/windows-msys2.md) — supported development setup and commands.
- [`docs/release/windows-portable.md`](docs/release/windows-portable.md) — portable packaging process and release checklist.
- [`docs/architecture/streaming-transcode-pipeline.md`](docs/architecture/streaming-transcode-pipeline.md) — streaming media-pipeline design.
- [`docs/architecture/dependencies.md`](docs/architecture/dependencies.md) — dependency discovery and pinning.
- [`docs/dev/crash-dumps.md`](docs/dev/crash-dumps.md) — Windows crash-dump behavior.
- [`docs/roadmap.md`](docs/roadmap.md) — deferred and future work.

## Repository layout

- `src/core/` — reusable media, job, timeline, subtitle, and encode logic.
- `src/app/` — Qt 6 Widgets desktop application and Windows integration.
- `tests/` — core and application tests, including real-media CI smoke coverage.
- `cmake/` — dependency and compiler configuration.
- `scripts/ci/` — Windows dependency builds, validation, and portable packaging.
- `docs/` — architecture, setup, release, and diagnostic documentation.

## Credits and license

Utsure's source code is available under the [MIT License](LICENSE). Bundled third-party libraries and media assets retain their own licenses and usage terms. See [CREDITS.md](CREDITS.md) and the manifests included in the portable bundle for attribution and dependency details.
