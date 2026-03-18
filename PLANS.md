# PLANS.md

This file is the living execution plan for the repository. Update it when a milestone starts, changes shape, or completes.

## Current status

- [x] M0 Repository inspection completed.
- [x] M1 Repo scaffolding docs completed.
- [ ] M2 Project skeleton is implemented and awaiting CI validation.
- [ ] M3 Core-only build foundation is partially implemented; the first test target is still pending.
- [ ] M4 Dependency strategy and adapter seams are implemented and awaiting CI validation.

## Active assumptions

- The project uses CMake.
- The architecture stays split between a reusable `encoder-core` and a thin Qt 6 Widgets desktop shell.
- Subtitle burn-in is hidden behind an adapter boundary so `libassmod` is not coupled directly to the rest of the core.
- Intro and outro clips are modeled as timeline segments, not one-off pipeline branches.
- Output frame rate always follows the main source video.
- Correctness comes before performance work.
- Windows build validation is currently anchored in GitHub Actions because the local machine does not expose a Qt-capable compiler toolchain.
- `libassmod` is currently treated as a pinned source dependency built into an isolated prefix because it installs as a `libass`-compatible package name rather than a uniquely named `libassmod` package.

## Architecture direction

- `encoder-core`
  - Owns project state, media probing, timeline assembly, subtitle composition requests, encode orchestration, and validation rules.
- `desktop-app`
  - Owns Qt Widgets windows, user actions, progress display, and error presentation.
- `media adapter`
  - Wraps FFmpeg access for probing, decode, filter, and encode operations.
- `subtitle renderer adapter`
  - Wraps `libassmod` so subtitle rendering details stay isolated.
- `timeline model`
  - Represents ordered segments: intro, main source, outro, and subtitle overlays.

## Highest-risk design choices

- Dependency strategy on Windows: how Qt 6, FFmpeg, `libassmod`, `libx264`, and `libx265` are acquired, versioned, and reproduced later on CI and other platforms.
- Timeline and timebase rules: keeping the main source frame rate authoritative while still accepting intro/outro clips with mismatched properties.
- Burn-in placement: deciding whether subtitle rendering is inserted through FFmpeg filtergraph composition, custom frame processing, or a hybrid approach.
- FFmpeg pipeline ownership: choosing how much logic lives in filtergraphs versus explicit C++ orchestration.
- Packaging and codec availability: binary distribution constraints for H.264/H.265-capable builds.

## Milestones

### M2 Bootstrap repository layout and root build entry

Status: Implemented, pending CI validation

Scope:
- Create the top-level repository layout needed for source, tests, docs, scripts, and CMake support.
- Add the root `CMakeLists.txt` and only the minimum non-product scaffolding needed to configure the project.
- Add a `.gitignore` and a short root `README.md`.

Likely files/modules:
- `CMakeLists.txt`
- `.gitignore`
- `README.md`
- `cmake/`
- `docs/architecture/`
- `src/core/`
- `src/app/`
- `tests/`
- `scripts/`

Risks:
- Locking in a folder layout that fights the future split between core and GUI.
- Adding too much build logic before the dependency strategy is settled.

Validation:
- Repository tree exists in the expected shape.
- `cmake -S . -B build` succeeds with placeholder targets or an intentionally minimal root configuration.

Done criteria:
- The repository has the initial top-level layout.
- The root CMake entry point configures successfully.
- No product features are implemented yet.

### M3 Establish core-only build and test foundation

Status: In progress

Scope:
- Create an empty `encoder-core` target and a minimal test target.
- Set compiler defaults, warning policy, and a small test harness.
- Keep the build usable before Qt or FFmpeg are fully wired.

Likely files/modules:
- `src/core/CMakeLists.txt`
- `tests/core/CMakeLists.txt`
- `tests/core/`
- `cmake/modules/`

Risks:
- Overcommitting to compiler flags or C++ standard settings too early.
- Letting test infrastructure pull GUI or media dependencies into the core build.

Validation:
- Configure and build the empty core target.
- Run the minimal test target successfully.

Done criteria:
- `encoder-core` builds.
- At least one minimal test target builds and runs.
- The core remains free of Qt Widgets dependencies.

### M4 Define dependency strategy and adapter seams

Status: Implemented, pending CI validation

Scope:
- Decide how the project locates and versions Qt 6, FFmpeg, `libassmod`, `libx264`, and `libx265`.
- Add the initial CMake integration points for third-party dependencies.
- Define the adapter boundaries that keep third-party APIs out of the core domain model.

Likely files/modules:
- `docs/architecture/dependencies.md`
- `docs/architecture/adapters.md`
- `cmake/modules/Find*.cmake` or equivalent config glue
- `src/core/include/`

Risks:
- Choosing a setup that works only on one Windows machine.
- Leaking FFmpeg or `libassmod` types into domain headers.
- Underestimating codec licensing and redistribution constraints.

Validation:
- Documented dependency path exists for local development.
- CMake can locate or intentionally gate the external dependencies with clear errors.

Done criteria:
- One reproducible dependency approach is selected.
- Adapter boundaries are documented.
- The build does not hard-wire GUI and core together.

### M5 Define core project and timeline contracts

Scope:
- Introduce the first stable core types for project settings, source assets, timeline segments, and encode requests.
- Encode the rule that the main source owns output frame rate.
- Capture intro and outro as ordinary timeline segment types.

Likely files/modules:
- `src/core/include/utsure/project/`
- `src/core/include/utsure/timeline/`
- `src/core/include/utsure/encode/`
- `src/core/src/`
- `tests/core/`

Risks:
- Hard-coding assumptions about assets before probing rules exist.
- Mixing UI concerns into project or timeline types.

Validation:
- Core unit tests cover basic project and timeline construction.
- Timeline contracts compile without FFmpeg or Qt runtime dependencies.

Done criteria:
- Core public headers define the initial domain contracts.
- Timeline segment representation includes intro, main, and outro.
- Frame-rate ownership by the main source is explicit in the model or validation rules.

### M6 Implement FFmpeg-based media probing and normalization rules

Scope:
- Add a media probe service that reads stream properties from candidate inputs.
- Define the normalization rules the rest of the pipeline will rely on.
- Surface mismatch cases clearly for later GUI and validation use.

Likely files/modules:
- `src/core/media/`
- `src/core/adapters/ffmpeg/`
- `tests/integration/media/`

Risks:
- Ambiguity around VFR sources, time bases, and stream selection.
- Defining normalization rules too loosely and pushing errors downstream.

Validation:
- Probe known sample files and capture width, height, pixel format, frame rate, duration, and stream presence.
- Tests cover obvious mismatch scenarios for intro/outro inputs.

Done criteria:
- Media probe service returns structured metadata.
- The project can determine whether assets are compatible or require normalization.
- Output frame-rate derivation from the main source is enforced in the probe-to-project path.

### M7 Assemble timeline segments for intro, main, and outro

Scope:
- Build the timeline assembler that converts probed assets into an ordered output plan.
- Normalize or reject unsupported segment combinations according to the rules from M6.
- Keep intro/outro handling generic rather than branching around the main source path.

Likely files/modules:
- `src/core/timeline/`
- `src/core/project/`
- `tests/core/timeline/`

Risks:
- Hidden assumptions about resolution, audio layout, or segment boundaries.
- Accidentally creating special-case logic that makes later features harder.

Validation:
- Unit tests cover main-only, intro+main, main+outro, and intro+main+outro cases.
- Tests verify that output frame rate remains tied to the main source.

Done criteria:
- Timeline assembly works for the supported segment combinations.
- Unsupported combinations fail with explicit diagnostics.

### M8 Add encode session orchestration and first H.264 path

Scope:
- Introduce the encode session object that drives decode, transform, and encode stages.
- Add the first working H.264 output path.
- Keep the orchestration interfaces ready for subtitle burn and H.265 extension.

Likely files/modules:
- `src/core/encode/`
- `src/core/adapters/ffmpeg/`
- `tests/integration/encode/`

Risks:
- Poor cancellation/progress/error boundaries.
- Overcoupling the encode path to H.264-specific options.

Validation:
- End-to-end encode test for a simple main-source-only H.264 output.
- Verify that the output frame rate matches the main source.

Done criteria:
- A minimal H.264 encode completes successfully through the core.
- Progress and failure states are observable from the orchestration layer.

### M9 Add subtitle renderer adapter and burn-in path

Scope:
- Introduce the subtitle renderer abstraction and `libassmod` implementation.
- Burn subtitles into the video path without leaking renderer details into the domain model.
- Define subtitle timing expectations against the assembled timeline.

Likely files/modules:
- `src/core/subtitles/`
- `src/core/adapters/libassmod/`
- `tests/integration/subtitles/`

Risks:
- Subtitle timing alignment across intro/main/outro boundaries.
- Pixel format conversions and color handling around burn-in.
- Tight coupling between the renderer and FFmpeg frame handling.

Validation:
- Integration test with a known subtitle sample and visual or metadata-based verification.
- Tests ensure subtitle timing is correct relative to the assembled timeline.

Done criteria:
- Subtitle burn-in works through the adapter boundary.
- Core logic can request subtitle rendering without knowing `libassmod` details.

### M10 Extend encode support to H.265

Scope:
- Add H.265 output as a first-class encode option.
- Reuse the encode session architecture from M8 rather than forking it.
- Keep output configuration and validation shared where possible.

Likely files/modules:
- `src/core/encode/`
- `tests/integration/encode/`

Risks:
- Capability drift between H.264 and H.265 settings.
- Different codec availability across FFmpeg builds.

Validation:
- End-to-end H.265 encode test on a simple sample.
- Verify that H.264 and H.265 share the same core timeline and subtitle paths.

Done criteria:
- The core supports both H.264 and H.265 output.
- Codec selection does not duplicate large parts of the pipeline.

### M11 Add thin Qt 6 Widgets desktop shell

Scope:
- Add the first real desktop window and wire it to the core boundaries.
- Keep state management, validation, and encode logic out of the GUI layer.
- Show asset selection, settings entry, progress, and error reporting.

Likely files/modules:
- `src/app/`
- `src/app/ui/`
- `src/app/presenters/` or equivalent thin coordination layer
- `tests/integration/app/`

Risks:
- Letting the GUI accumulate business rules.
- Weak cancellation and progress plumbing between GUI and core.

Validation:
- Application builds and launches on Windows.
- A basic encode flow can be started from the GUI and delegated to the core.

Done criteria:
- Qt Widgets shell launches and drives the core successfully.
- GUI remains thin and does not own timeline or encode policy.

### M12 Harden correctness, packaging, and portability

Scope:
- Expand test coverage around probe, timeline, subtitle, and encode correctness.
- Improve diagnostics, cancellation, and recoverability.
- Add Windows packaging and document the portability work needed for Linux/macOS.

Likely files/modules:
- `tests/`
- `docs/architecture/`
- `scripts/packaging/`

Risks:
- Packaging complexity for FFmpeg and codec-enabled builds.
- Platform assumptions hidden in path handling, process setup, or toolchain logic.

Validation:
- Run the milestone test matrix.
- Produce a Windows build artifact with documented runtime dependencies.

Done criteria:
- The repository has a usable Windows packaging path.
- Major correctness risks are covered by tests.
- The remaining Linux/macOS gaps are documented explicitly.

## Immediate next milestone

Complete M3 by adding the first core test target, then re-run CI so M2 and M4 can be closed with real build validation.
