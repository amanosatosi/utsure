# PLANS.md

This file is the living execution plan for the repository. Update it when a milestone starts, changes shape, or completes.

## Current status

- [x] M0 Repository inspection completed.
- [x] M1 Repo scaffolding docs completed.
- [x] M2 Project skeleton completed.
- [x] M3 Core-only build foundation completed.
- [x] M4 Dependency strategy and adapter seams completed.
- [x] M5 Core job and configuration contracts completed.
- [x] M6 Media input inspection completed.
- [x] M7 Decode and normalized processing flow completed.
- [x] M9 Software encoding backends completed.
- [x] M10 Subtitle renderer abstraction completed.
- [x] M14 libassmod subtitle burn-in completed.
- [x] M8 Intro/outro timeline composition completed.
- [x] M12 First usable desktop GUI completed.
- [x] M13 Windows portable packaging slice completed.
- [x] M13 Usability and safety slice completed.
- [x] M15 Hardening and handoff completed.
- [x] M16 Streaming transcoding pipeline completed.
- [x] M17 Desktop GUI quality and usability completed.
  * Remaining FFMS2 preview-backend follow-up and CI validation are deferred and no longer block M17 completion.
  * M18 Automatic output naming completed.
  * M19 Automatic subtitle selection completed.
  * M20 FontCollector-based subtitle font recovery and fallback completed.
  * M21 Global parallel batch encoding with bounded job counts and pre-reserved output naming completed.
  * M22 FFMS2 preview latency investigation and responsiveness hardening completed.
- [x] M23 Encode-throughput investigation and subtitle-free fast path completed.
- [ ] M24 Subtitle-enabled encode-throughput investigation and optimization in progress.

## Active assumptions

- The project uses CMake.
- The architecture stays split between a reusable `encoder-core` and a thin Qt 6 Widgets desktop shell.
- Subtitle burn-in is hidden behind an adapter boundary so `libassmod` is not coupled directly to the rest of the core.
- Intro and outro clips are modeled as timeline segments, not one-off pipeline branches.
- Output frame rate always follows the main source video.
- Correctness comes before performance work.
- Windows build validation is currently anchored in GitHub Actions because the local machine does not expose a Qt-capable compiler toolchain.
- `libassmod` is currently treated as a pinned source dependency built into an isolated prefix, currently pointed at commit `88a338192faf50505eb4cedfe7d1320265f1081f`, because it installs as a `libass`-compatible package name rather than a uniquely named `libassmod` package.
- Media inspection was intentionally pulled ahead of broader project/timeline contracts because those contracts need real stream metadata and explicit cadence fields instead of placeholder assumptions.
- The decode and normalized processing milestone is also being pulled ahead of the broader project/timeline contracts by explicit user request, but it remains limited to the main source path only.
- The software encoding backend milestone is also being pulled ahead of the broader project/timeline and session-orchestration milestones by explicit user request, and it remains limited to the main-source decoded video path with minimal codec configuration.
- The muxed-output job/config milestone is also being pulled ahead of broader timeline and session modeling by explicit user request, and it remains limited to main-source video-only output settings.
- The subtitle renderer abstraction milestone is also being pulled ahead of timeline assembly and subtitle burn-in integration by explicit user request, and it remains limited to a technology-agnostic renderer boundary plus timestamped RGBA-oriented render contracts.
- The libassmod subtitle burn-in milestone is also being pulled ahead of intro/outro and broader timeline work by explicit user request, and it remains limited to main-source ASS subtitle burn-in before final encode.
- The timeline composition milestone now includes ordered intro/main/outro segment assembly, decoded-stream stitching with aligned normalized audio, and encode-job integration while the output backend remains video-only.
- The hardening and handoff milestone is limited to validation coverage for the current pipeline, practical build/setup notes, packaging and release guidance, and a clearer roadmap split between near-term and later work.
- The current milestone is limited to replacing the full-clip decoded buffering path with a bounded-memory streaming pipeline while preserving cadence rules, subtitle burn-in behavior, intro/outro sequencing, and streamed A/V output.
- The current milestone is also limited to centering the active transcoder on FFmpeg 7.1's `libavformat`/`libavcodec`/`libswscale`/`libswresample` APIs while keeping subtitle burn-in inside the app and out of `libavfilter`.
- The current milestone now also includes pinning FFmpeg 7.1.x as an isolated source-built dependency in CI and the documented MSYS2 workflow so the build does not silently resolve a newer system FFmpeg.
- The current M17 preview slice pins FFMS2 to upstream commit `25cef14386fcaaa58ee547065deee8f6e82c56a2` instead of the `5.0` tag because that is the FFMS2 revision already used in the related Aegisub toolchain.
- The current M16 slice is limited to migrating the libassmod subtitle adapter from the legacy `ASS_Image` path to the fork's RGBA-capable render path where required, while preserving the existing renderer abstraction, timestamp rules, and streaming subtitle burn-in flow.
- The current M16 subtitle-rendering slice now prefers the libassmod RGBA API unconditionally inside the adapter so gradient and other per-pixel effects are not exposed to legacy `ASS_Image` fallback behavior.
- The current M16 hardening slice also includes fixing cadence validation for real-world CFR sources whose container stream time base is too coarse to represent the authoritative frame rate exactly.
- The current M16 timing hardening slice also includes replacing exact-step decoded video timestamp validation in the streaming path with monotonic timestamp-driven frame timing, while keeping nominal frame rate as encoder/report metadata instead of a per-frame gate.
- The current M16 hardening slice also includes replacing compressed-audio byte-guard failures with timestamp-based audio throttling, bounded decoded-audio queues, and a startup preroll rule so normal H.264 decode delay does not abort streaming jobs.
- The current M16 audio slice also includes fixing the normalized/output audio timeline to use sample-based time units instead of inheriting arbitrary source stream time bases, and adding centralized audio output mode resolution for Auto / Copy / AAC / Disable across core and GUI surfaces.
- The current M16 audio slice now routes the active streamer through one resolved output-audio plan so AAC encode, single-segment stream copy, and disabled-audio jobs all use the same core compatibility rules and preview/report summaries.
- The current M16 UX slice also includes replacing coarse encode-stage step progress with throttled frame-driven streaming progress that reports percent, encoded frames, encoded media time, and live EFPS through the existing observer and Qt worker/controller pipeline.
- The current M16 throughput slice also includes backend-managed multi-core video encoder threading, one explicit 70-frame bounded video handoff queue with backpressure in the streaming path, and a safe GUI process-priority control that stays outside encoder-core platform policy.
- The current M16 throughput slice now also includes explicit decoder/encoder thread selection, Auto / Conservative / Aggressive CPU usage modes, bounded parallel video normalization/subtitle preparation ahead of ordered encode, and lightweight per-stage runtime instrumentation.
- The current M17 slice is limited to reshaping the Qt Widgets desktop shell around a queue-based batch encode workflow that matches the provided HTML reference as closely as practical without replacing the existing app framework.
- The current M17 slice also includes native Qt placeholders for unfinished backend-backed features, especially the thumbnail subtitle-title integration surface and richer preview playback behavior, so the intended workflow is obvious without pretending the full implementation exists.
- The current M17 slice excludes automatic output naming, automatic subtitle selection, and new encode-core media policy beyond the minimum UI/controller wiring needed to expose the current pipeline cleanly.
- The current M17 slice now also includes shipping the new SVG-backed desktop icon dependency through the Windows MSYS2 workflow and documented local setup so the refreshed app shell configures reproducibly in CI.
- The current M17 slice now also includes a targeted regression pass from commit `882e2d24c6e665f2f742a8045f2e3874df251a2e` for SVG-backed action icons, branding consistency, queue-table rendering, Preview-vs-Task-Log semantics, native Windows caption behavior, and overly constrained desktop-shell sizing/density in the refreshed window.
- The current M17 preview slice now also includes replacing the temporary full-clip preview decode with a bounded frame-at-time decode path so Preview remains opt-in, usable on long sources, and shares subtitle composition without buffering an entire video into memory.
- The current M17 preview slice now also includes upgrading the right-side Preview tab from still-frame inspection to an opt-in transport-controlled preview player with a larger default 16:9 surface, hover-only playback controls, and icon-first trim navigation.
- The current M17 preview playback slice now also includes fixing request-generation handling so continuous visual playback can present intermediate frames while paused/offline state changes still invalidate delayed in-flight renders correctly.
- The current M17 preview playback slice now also includes replacing per-frame reopen/seek preview decode with a small cached frame-window path so initial seek/index work can be reused across nearby playback frames.
- The current M17 preview playback slice now also includes preview-resolution frame normalization so the cache window can span several seconds instead of only about one second of full-resolution RGBA frames.
- The current M17 preview playback slice now also includes a persistent core preview decode session so playback can keep reading forward across cache boundaries instead of stalling on repeated reopen/seek window refills.
- The current M17 preview slice now also includes restoring opt-in main-source preview audio so Preview can play synchronized source audio alongside the existing subtitle-rendered video preview without turning the preview path into a full media-player rewrite.
- The current M17 preview-audio hardening slice now also includes correcting the preview audio master-clock path so buffered `QAudioSink` progress is not mistaken for already-audible playback time.
- The current M17 preview-audio hardening slice now also includes rebuilding the Qt audio output path on each preview run so pause/seek/preview-off transitions cannot reuse stale sink or pull-device state.
- The current M17 preview-backend slice now also includes replacing the custom FFmpeg preview session path with an FFMS2-indexed preview-only backend for selected-job main-source video, audio, and seek behavior while leaving the main encode/transcode pipeline on the existing FFmpeg/core path.
- The current M17 preview-backend slice now also includes isolated FFMS2 dependency/build wiring and preview-index file reuse rules, but excludes any FFMS2 adoption in final encode behavior.
- The current M17 preview-pane usability slice is limited to keeping preview usable in smaller non-maximized windows by moving trim/time controls into the preview surface as hover overlays, while preserving the existing trim/timeline behavior and Preview-vs-Task-Log tab split.
- The current M17 preview-pane usability slice now also includes a smaller seek bar, a compact bottom control row with transport buttons on the left, non-playing timeline seek clicks, and left/right arrow frame stepping when the preview surface has focus.
- The current M17 preview-pane usability slice now prefers a compact dedicated preview footer beneath the surface, instead of a floating overlay, because that tracks the reference workflow more closely and preserves more usable picture area.
- The current M17 preview-pane usability slice now also hides that dedicated preview footer unless the pointer is over the preview region, so the default state gives more height back to the video surface.
- The current M18 slice is limited to predictable default output-path generation plus GUI wiring for custom text and manual override preservation; it does not add a separate output-container selector.
- The current M18 slice assumes the generated default should reuse the current output-path extension when one is already present and otherwise fall back to `.mp4` until a dedicated container-selection surface exists.
- The current M19 slice is limited to same-folder subtitle discovery with strict exact-stem and exact `.fx`-qualified matches; it intentionally avoids recursive search and loose partial-name guessing.
- The current M20 slice is limited to explicit FontCollector-based recovery for ASS/SSA subtitle session preparation, staged into a job-scoped temporary font directory and applied through the existing subtitle renderer abstraction when available.
- The current M21 slice is limited to global batch-level parallel scheduling, bounded per-job thread/buffer planning, and pre-reserved auto output naming; it does not add new per-queue tabs or broader encode-policy changes beyond those execution-planning surfaces.
- The current M22 slice is limited to FFMS2-based preview responsiveness: timing instrumentation, lower-latency interactive seek behavior, request coalescing hardening, and safer prefetch/session reuse without changing unrelated encode or GUI layout behavior.
- The current M23 slice is limited to identifying streaming encode bottlenecks, surfacing stage timing/runtime diagnostics, removing subtitle-free RGBA normalization overhead where possible, and reusing video conversion buffers without changing output quality defaults or subtitle-enabled behavior.
- The current M24 slice is limited to the subtitle-enabled streaming path: removing subtitle bitmap-copy overhead, moving subtitle render/composition off the ordered encode lane where practical, then hardening that path with diagnostics, isolation switches, and safer defaults when crash risk appears, all while preserving existing burn-in/timestamp/mux behavior without changing codec quality defaults.
- The current M24 crash-hardening pass is also limited to subtitle-path diagnostics, runtime isolation toggles for direct-vs-copied bitmap handling and worker-vs-serialized composition, defensive lifetime/stride assertions, subtitle stress coverage, and CI sanitizer support; it does not add unrelated GUI features or broader encode-policy changes.
- The current M24 slice also permits one narrow user-requested desktop-shell branding follow-up, limited to wiring the provided `logo.svg` and `icon.svg` into the existing Qt app resources without changing encode/core behavior.
- The current M24 slice also permits one narrow user-requested Windows shell-icon follow-up, limited to generating and embedding an executable icon resource from the provided `icon.svg` so Explorer and taskbar surfaces match the in-app branding without changing encode/core behavior.
- The current M24 slice also permits one narrow user-requested multi-audio follow-up, limited to enumerating source audio streams, deterministically selecting one core-owned active audio stream with Japanese preference, and keeping the current single-selected-stream GUI/pipeline behavior without adding a manual picker yet.
- The current M24 slice also permits one narrow user-requested desktop-shell drag-and-drop follow-up, limited to whole-window source-video intake with one centralized main-window import path, a non-recursive folder expansion helper, and no new queue-reordering or subpanel-specific drop behaviors.
- The current M24 slice also permits one narrow user-requested timeline cadence follow-up, limited to keeping the main source frame rate authoritative while normalizing mismatched intro/outro decoded frame cadence in-core without relaxing the existing resolution, sample-aspect-ratio, audio sample-rate, or channel-layout compatibility rules.
- The current M24 slice also permits one narrow user-requested main-source trim follow-up, limited to threading explicit main-source trim ranges through the core job/timeline/streaming encode path so encoded output, progress totals, and subtitle timing respect the kept range without turning trim into a GUI-only seek hack.
- The current M24 CI-sanitizer slice now also includes using an MSYS2 Windows toolchain that actually ships AddressSanitizer runtime libraries for the dedicated subtitle stress job, while keeping the normal non-sanitized Windows build on the existing UCRT64 GCC toolchain.
- The current M24 CI slice now also includes bumping the Windows GitHub Actions JavaScript actions to Node 24-compatible releases, with a temporary `FORCE_JAVASCRIPT_ACTIONS_TO_NODE24` override during validation.

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

Status: Completed

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

Status: Completed

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

Status: Completed

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

### M5 Add core job configuration and muxed encode entry point

Status: Completed

Scope:
- Introduce the first stable core job/config types for source selection and output settings.
- Keep the fields limited to the current usable encode path: output path, codec, preset, and CRF.
- Keep the structure ready for later subtitle and timeline expansion without adding those fields yet.

Likely files/modules:
- `src/core/include/utsure/core/job/`
- `src/core/src/`
- `tests/core/`

Risks:
- Letting UI-oriented concerns leak into the core job/config model.
- Hard-coding today's encode path so tightly that subtitle or timeline fields become awkward later.

Validation:
- Core tests cover job construction and job-driven end-to-end muxed encode for one H.264 and one H.265 sample.
- Job/config contracts compile without Qt runtime dependencies.

Done criteria:
- Core public headers define the initial job/config contracts.
- The pipeline can produce muxed output through the job model.
- Output settings are explicit and remain separate from any UI layer.

### M6 Implement FFmpeg-based media probing and normalization rules

Status: Completed

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

### M7 Implement source decode and normalized processing flow

Status: Completed

Scope:
- Add a source decode service that reads the primary video and audio streams selected by the probe layer.
- Normalize decoded video frames and audio samples into explicit internal buffer types suitable for later composition and encode work.
- Keep timestamp handling, cadence preservation, and debug visibility explicit for the main-source-only path.

Likely files/modules:
- `src/core/include/utsure/core/media/`
- `src/core/src/media/`
- `tests/core/`
- `scripts/ci/`

Risks:
- Hiding cadence mistakes inside timestamp conversion or decode flush handling.
- Picking internal pixel/sample formats that are hard to reason about later.
- Letting decode details leak FFmpeg types into public core headers.

Validation:
- Decode generated sample media into internal video frame and audio sample objects.
- Verify representative frame/sample timestamps and normalization metadata.
- Verify the decode report is understandable enough to debug the flow from source packet timing to internal objects.

Done criteria:
- Source video frames decode into internal frame objects.
- Source audio samples decode into internal sample objects.
- Pixel/sample normalization rules are explicit in code and observable in tests.
- Timestamp handling is consistent and understandable for the main source path.

### M8 Assemble timeline segments for intro, main, and outro

Status: Completed

Scope:
- Build the timeline assembler that converts probed assets into an ordered output plan.
- Normalize or reject unsupported segment combinations according to the rules from M6.
- Keep intro/outro handling generic rather than branching around the main source path.
- Stitch decoded segment video and normalized audio into one composed timeline before subtitle burn-in and final encode.
- Keep subtitle timing explicit so the default behavior targets only the main segment, with an opt-in mode for full-output timeline timing later.

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
- Tests verify that the default subtitle scope stays confined to the main segment when intro/outro clips are present.
- GitHub Actions remains the authoritative compile-and-run validation environment for this milestone because the local machine does not expose a usable C++ compiler toolchain.

Done criteria:
- Timeline assembly works for the supported segment combinations.
- Unsupported combinations fail with explicit diagnostics.
- The encode-job path can emit intro+main+outro output video while keeping the stitched normalized-audio timeline aligned in-core for the later audio-output milestone.

### M9 Implement software encoding backends

Status: Completed

Scope:
- Add minimal software encoding backends for `libx264` and `libx265`.
- Encode the existing decoded main-source video frame stream into structurally valid outputs.
- Keep configuration minimal for v1: codec, preset, and CRF.

Likely files/modules:
- `src/core/include/utsure/core/media/`
- `src/core/src/media/`
- `tests/core/`
- `scripts/ci/`

Risks:
- Poor timestamp normalization between decoded frames and encoded packets.
- Overcoupling the backend implementation to one codec while claiming both.
- Shipping a path that produces technically writable files but structurally weak outputs.

Validation:
- End-to-end encode tests for one H.264 and one H.265 sample from the decoded main-source path.
- Verify that the output files can be inspected and decoded again with coherent video timestamps.
- Verify that output frame cadence remains tied to the main source.

Done criteria:
- A decoded frame stream can be encoded to H.264.
- The same path can encode to H.265.
- Output timestamps remain coherent.
- Resulting files are structurally valid.

### M10 Add subtitle renderer abstraction and libassmod adapter seam

Status: Completed

Scope:
- Introduce the subtitle renderer abstraction and session lifecycle in `encoder-core`.
- Define how the pipeline requests subtitle output at a given timestamp and receives RGBA-oriented overlay data.
- Keep `libassmod`-specific parsing and rendering behavior outside the abstraction and defer actual burn-in integration.

Likely files/modules:
- `src/core/include/utsure/core/subtitles/`
- `src/core/src/subtitles/`
- `src/core/adapters/libassmod/`
- `tests/core/`

Risks:
- Overfitting the abstraction to `libassmod` instead of a reusable renderer contract.
- Choosing a subtitle surface shape that makes later RGBA composition awkward.
- Pulling subtitle-specific policy into unrelated pipeline code before the timeline exists.

Validation:
- Core tests exercise subtitle session creation and timestamped render requests through the abstraction using a fake implementation.
- Public headers compile without exposing `libassmod` types or linking subtitle technology into unrelated core code.

Done criteria:
- There is a subtitle-renderer abstraction in `encoder-core`.
- The boundary is clean enough to support multiple implementations later.
- The rest of the pipeline depends on the abstraction, not directly on `libassmod`.

### M14 Integrate libassmod subtitle burn-in

Status: Completed

Scope:
- Implement the `libassmod`-backed subtitle renderer behind the core subtitle abstraction.
- Render ASS subtitle output at decoded frame timestamps and composite it onto RGBA video frames before final encode.
- Keep timestamp handling exact and defer intro/outro logic and broader timeline integration.

Likely files/modules:
- `src/core/include/utsure/core/subtitles/`
- `src/core/src/adapters/libassmod/`
- `src/core/src/subtitles/`
- `src/core/src/job/`
- `tests/core/`

Risks:
- Misinterpreting `libassmod` bitmap alpha/color semantics and producing weak burn-in output.
- Letting libass-specific choices leak out of the abstraction and into job or media headers.
- Counting subtitle timing correctly at frame boundaries while preserving the main-source cadence.

Validation:
- Core tests verify `libassmod` rendering visibility at representative timestamps for a sample ASS script.
- H.264 and H.265 burn-in job tests verify valid encoded outputs, preserved cadence, and visible subtitle changes in the encoded video.

Done criteria:
- The pipeline can load an ASS subtitle file.
- Subtitles render at the correct timestamps.
- Subtitle bitmaps composite onto output video frames before encode.
- The pipeline produces a valid encoded output with burned-in subtitles.

### M11 Extend encode support to H.265

Status: Merged into M9 software encoding backends

Scope:
- This scope was merged into M9 so the first backend milestone covers both H.264 and H.265.

Likely files/modules:
- `src/core/include/utsure/core/media/`
- `src/core/src/media/`
- `tests/core/`

Risks:
- Capability drift between H.264 and H.265 settings remains a risk inside M9.

Validation:
- Covered by M9 validation.

Done criteria:
- Covered by M9 done criteria.

### M12 Add thin Qt 6 Widgets desktop shell

Status: Completed

Scope:
- Replace the placeholder window with the first usable desktop encode workflow.
- Keep state management, validation, timeline assembly, and encode logic out of the GUI layer.
- Show asset selection, settings entry, stage-level progress, logs, and understandable error reporting.
- Support the current core scope only: main source, optional subtitle burn-in, optional intro/outro, H.264/H.265, preset, CRF, and output path.

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
- GitHub Actions remains the authoritative compile-and-launch validation environment for this milestone because the local machine does not expose a Qt-capable compiler toolchain.

Done criteria:
- Qt Widgets shell launches and drives the core successfully.
- GUI remains thin and does not own timeline or encode policy.
- The first usable window exposes the minimum controls needed to build and run the supported encode job.

### M13 Harden correctness, packaging, and portability

Status: Completed

Current slice:
- Add GitHub Actions packaging for one portable Windows app bundle.
- Keep this slice limited to deployable runtime bundling and artifact publication.
- Defer the broader correctness, cancellation, recoverability, and cross-platform portability work until after the packaging path is in place.
- Packaging slice status: completed.
- Usability and safety slice status: completed.
- Usability and safety slice scope: preflight validation, safer defaults, clearer logs/errors, overwrite confirmation, and lightweight preview feedback for the existing GUI flow.

Scope:
- Expand test coverage around probe, timeline, subtitle, and encode correctness.
- Improve diagnostics, cancellation, and recoverability.
- Add Windows packaging and document the portability work needed for Linux/macOS.

Scope change:
- M13 closes with the Windows portable packaging path, GUI safety/preflight improvements, and the existing core correctness baseline.
- Deeper cancellation, recoverability, and cross-platform portability work is deferred into the roadmap instead of blocking the handoff milestone.

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

### M15 Hardening and handoff

Status: Completed

Scope:
- Add a small number of high-signal validations around the most failure-prone current pipeline paths.
- Turn the Windows-first setup/build path into a practical developer handoff note.
- Document portable packaging and release considerations for the current Windows artifact.
- Separate the roadmap into near-term follow-up work versus later/deferred work.

Likely files/modules:
- `tests/core/`
- `tests/core/CMakeLists.txt`
- `README.md`
- `docs/setup/`
- `docs/release/`
- `docs/roadmap.md`

Risks:
- Spending effort on broad test expansion instead of the few code paths that remain materially under-validated.
- Leaving packaging/release expectations implicit and forcing later contributors to reverse-engineer CI behavior.
- Mixing next-step engineering work with longer-horizon portability and architecture ideas.

Validation:
- GitHub Actions builds the app, runs the focused core pipeline tests, smoke-tests the GUI, and validates the portable bundle.
- Repository docs describe the supported developer setup, packaging path, and staged roadmap clearly enough for a new contributor to follow them without tribal knowledge.

Completion notes:
- Added focused validation for preflight output-path conflicts that would overwrite the main input.
- Added end-to-end validation for `full_output_timeline` subtitle burn-in across intro/main/outro composition.
- Added explicit developer setup, Windows portable release, and staged roadmap documents for handoff.

Done criteria:
- Key pipeline components have at least basic validation coverage for their remaining high-risk paths.
- There is a practical developer setup/build note for the Windows-first workflow.
- Packaging and release considerations are documented for the current portable bundle.
- The roadmap separates near-term follow-up tasks from later/deferred work.

### M16 Refactor encode orchestration to a streaming transcoding pipeline

Status: In progress

Scope:
- Replace the current whole-clip decode, subtitle burn-in, and timeline composition path with a bounded-memory streaming pipeline.
- Keep the stage split explicit: demux, decode, subtitle/composite, encode, and mux.
- Process intro, main, and outro segments sequentially instead of decoding all segments into memory together.
- Preserve current output behavior, including main-source cadence ownership, subtitle timing modes, and muxed output with synchronized audio when the timeline defines one.
- Replace duration-scaled working-set estimation with queue-depth-scaled budgeting and document explicit frame/packet lifetime rules.

Scope change:
- M16 originally landed with a working streaming video path but regressed the older audible-output expectation by dropping audio after decode/resample.
- M16 now explicitly includes restoring incremental audio encode and mux support, plus clear failures when an audio-bearing timeline cannot be emitted correctly.
- The current M16 slice also includes upgrading the libassmod-backed subtitle adapter so RGBA-only features such as gradient color/alpha tags render through the correct RGBA API path without leaking libassmod-specific behavior into unrelated pipeline code.
- Scope update: the adapter now uses libassmod's RGBA rendering path for all subtitle frames instead of mixing legacy and RGBA render calls.
- Scope update: output video time-base selection now favors the exact inverse main-source frame rate over coarse container stream time bases so CFR validation does not reject normal 24000/1001 material.
- Scope update: the active transcode path is being tightened around FFmpeg 7.1 core-library APIs only, with `libavfilter` removed from the required dependency surface and the host-side streaming loop simplified where queueing was only buffering packets between synchronous stages.

Current slice status:
- Completed: libassmod RGBA subtitle rendering migration for gradient-capable scripts and shared premultiplied-RGBA subtitle composition.
- Completed: output video time-base selection now keeps the finer of the main stream timestamp base and inverse nominal frame step so streaming encode preserves decoded timestamp precision without using nominal fps as a per-frame gate.
- Completed: monotonic decoded-timestamp-driven main-segment video timing in the streaming path, replacing the exact-step cadence guard that rejected valid MP4 timestamp patterns.
- Completed: FFmpeg 7.1 core-library dependency gating plus removal of `libavfilter` from the active dependency surface.
- Completed: pinned FFmpeg 7.1.2 source-build workflow plus explicit prefix validation so CI no longer picks up MSYS2's newer FFmpeg package line.
- Completed: FFmpeg-only GitHub Actions dependency cache keyed to the pinned 7.1 recipe so later workflow runs can skip rebuilding FFmpeg without caching the app build tree.
- Completed: simplified the host-side streaming loop so redundant packet and mux staging queues were removed while bounded audio backpressure queues remained in place ahead of the later explicit 70-frame video handoff queue.
- Completed: fine-grained streaming encode progress for the desktop app, carried from the frame-driven transcoder back through the core observer and Qt worker/controller layers without changing the thread model.
- Completed: throughput and scheduling controls for the active encode path, using backend-managed encoder auto-threading, one explicit 70-frame bounded video queue with backpressure, a safe Windows process-priority selector in the GUI, and preview/log/report visibility for the resolved runtime behavior.
- Completed: explicit FFmpeg decoder/encoder thread selection, user-selectable Auto / Conservative / Aggressive CPU usage modes, bounded parallel video normalization ahead of ordered encode, and stage-level runtime timing visibility for CPU-bound software transcodes.
- Validation note: local build/test execution for this slice remains blocked by the missing C++ toolchain on the current machine, so the implementation still needs CI or a toolchain-equipped workstation to satisfy the milestone validation step fully.
- Deferred: host-side `\img` resource registration remains outside this slice, and `\img` scripts now fail explicitly until that registration path exists.

Likely files/modules:
- `src/core/include/utsure/core/job/`
- `src/core/include/utsure/core/media/`
- `src/core/src/job/`
- `src/core/src/media/`
- `src/core/src/subtitles/`
- `tests/core/`
- `docs/architecture/`

Risks:
- Breaking cadence preservation while moving timestamp ownership from full-timeline composition to streaming state.
- Regressing subtitle timing when switching from whole-clip burn-in to per-frame render/composite at encode time.
- Leaving frame ownership ambiguous between decode, composite, and encode stages.
- Accidentally preserving old full-buffer behavior behind the new API surface and keeping the memory pressure problem hidden.
- Regressing A/V sync while rebasing decoded audio blocks onto the output timeline and interleaving audio/video packets in one muxer.

Validation:
- Build and run the focused core encode/preflight tests that exercise main-only, intro/main/outro, and subtitle cases.
- Verify that memory-heavy 1080p jobs are no longer rejected by duration-scaled decoded-memory estimates.
- Verify that audio-bearing inputs produce muxed outputs with an expected audio stream and coherent durations.
- Verify one 24000/1001 sample whose input stream time base is forced to `1/1000`, and confirm the timeline keeps `1/1000` for streamed timestamp precision while still reporting `24000/1001` as the output frame rate.
- Verify that encode-job progress reaches a final completed update and carries throttled fine-grained encode metrics during the streaming encode stage without flooding the observer path.
- Verify that the active streamer reports backend-managed encoder threading, a 70-frame video queue, and the selected priority in preview/log/report surfaces without reintroducing whole-clip buffering or unbounded producer runahead.
- Document the new stage flow, queue limits, where decoded/composited frame memory is released, and where audio frames/packets are released.
- Verify the subtitle adapter against one normal ASS sample plus one RGBA-only subtitle sample that depends on libassmod gradient rendering, and include `\img` coverage if the host-side image registration path is wired in this slice.

Done criteria:
- Encode orchestration no longer scales decoded-memory usage with full clip duration.
- Queue ownership, queue limits, and frame lifetime rules are explicit in code and docs.
- Subtitle burn-in happens per frame during streaming encode, not by cloning whole decoded clips.
- Intro and outro segments are processed sequentially through the same streaming stages.
- Audio-bearing timelines emit synchronized audio instead of silently dropping it.
- Unsupported audio-output cases fail clearly instead of silently producing video-only output.
- The old full-clip buffering path is removed or isolated away from the active encode path.


### M17 Improve desktop GUI quality and usability

Status: Completed

Scope:
  * Improve the Qt 6 Widgets desktop shell so the main encode workflow feels cleaner, more understandable, and more efficient to use.
  * Keep encode policy, subtitle rules, and pipeline orchestration in `encoder-core` rather than moving them into the GUI layer.
  * Focus this milestone on visual polish, clearer control grouping, safer defaults, and more useful workflow feedback instead of adding unrelated encode features.
  * Reshape the window around a queue table, selected-job editor, selected-job summary, session log, and preview/task-log side panel modeled after the provided HTML reference.
  * Add obvious placeholder controls and notes for thumbnail pre-roll title/image behavior until the dedicated subtitle-file integration milestone exists.

Current slice status:
  * Completed: verified the shipped SVG toolbar/timeline assets are well-formed UTF-8 XML with no BOM, UTF-16 encoding, NUL bytes, or stray binary junk.
  * Completed: restored action-icon loading to Qt's resource-path icon pipeline with SVG renderer fallback after the post-`882e2d24c6e665f2f742a8045f2e3874df251a2e` raw-byte loader regression.
  * Completed: narrowed the icon loader so runtime fallback now reads the qrc SVG bytes directly before rasterizing, instead of depending only on filename-based SVG handling.
  * Completed: normalized queue-table display text so file, type, status, EFPS, speed, and output cells render explicit values instead of leaking empty strings from partially populated jobs.
  * Completed: kept the Preview tab reserved for video preview only, with the surface forced back offline when no queue row is selected.
  * Completed: fixed the remaining text-button regression by giving `app_resources.qrc` a deterministic AUTORCC resource name and explicitly initializing `app_resources` at startup so `:/icons/*.svg` resolves before toolbar/button setup.
  * Completed: hardened the Windows portable bundle to carry and validate Qt's SVG icon engine plugin explicitly so deployed toolbar icons do not regress to text fallback.
  * Completed: replaced the queue table's native blue selection paint with an explicit muted selection palette so selected rows stay readable without bright blue artifacting.
  * Completed: reduced desktop-shell height pressure by lowering the default/minimum window height, shrinking the preview surface minimum, and relaxing the initial splitter allocations.
  * Completed: fixed the second-source queue crash by blocking queue-table rebuild signals, avoiding row selection against stale pre-refresh row counts, and removing the redundant post-add full refresh that re-entered the editor/selection path.
  * Completed: removed the extra trim-note text from the Preview tab and replaced the placeholder label stack with a dedicated preview surface widget.
  * Completed: added an opt-in preview renderer worker/controller that decodes only the main-source video path on demand, keeps Preview fully idle while disabled, and renders the current playhead frame without coupling to the encode pipeline.
  * Completed: shared subtitle preview composition with the existing libassmod-backed burn-in path by adding a one-frame subtitle composition helper in core and reusing the same subtitle renderer/session plus RGBA bitmap compositor for Preview overlays.
  * Completed: replaced the old per-frame reopen/seek preview path with a bounded cached frame-window decoder so Preview can reuse nearby decoded frames instead of reopening the source for every visual step.
  * Completed: changed preview playback to advance from delivered frame timestamps and frame durations, instead of wall-clock drift, so delayed decode responses no longer make the picture stall and then jump backward.
  * Completed: added preview-resolution video normalization so Preview can cache a longer frame window without holding full-resolution RGBA frames for every buffered playback step.
  * Completed: added a persistent preview decode session in core so sequential playback can decode the next preview window from the already-open source instead of reopening and reseeking every few seconds.
  * Completed: fixed the persistent preview decode session so normal sequential window refills no longer send the decoder into drain/EOF mode before the input stream actually reaches EOF, and added focused core regression coverage for seek-plus-sequential preview windows across the first 96-frame cache boundary.
  * Completed: fixed the remaining preview playback freeze at the first 96-frame boundary inside the app-side request/cache/playback handoff by correcting the cache-end boundary semantics and adding targeted logging around request state, cache coverage, refill selection, and frame delivery.
  * Completed: smoothed preview playback across repeated 96-frame window boundaries by starting one-window-ahead background video prefetch during playback, appending prefetched frames into the existing cache before the current buffered range is exhausted, and ignoring stale prefetch results after seeks or cache resets.
  * Completed: restored opt-in Preview audio for the selected job's main source through a separate Qt-audio-backed preview path that reuses encoder-core FFmpeg decode/resample helpers, follows seek/pause/preview-off/selection resets, and keeps the existing subtitle-rendered video preview flow intact.
  * Completed: tightened preview-audio timing so the audio device remains the effective master clock by subtracting queued `QAudioSink` buffer latency from Qt's processed-output time and letting preview video follow that corrected audio clock directly during playback.
  * Completed: hardened preview-audio lifecycle resets so each preview run recreates a fresh `QAudioSink` pull path, discards any queued sink buffers with `reset()`, and refuses to reuse stale stopped-state clocks between pause/seek/selection changes.
  * Completed: removed the standalone branding/header row and always-on-top toggle, then reshaped the top bar into left controls, a centered brand mark, and right-side queue controls while rebalancing the queue/editor/preview spacing so the preview surface gets a larger default footprint closer to the Konayuki-inspired reference.
  * Completed: removed the output-strip helper note beneath `Same as input` so the queue/output stack gives a little more height back to the editor and preview area.
  * Completed: restyled the Qt shell toward the `shin.html` dark-mode reference while keeping the current widget structure, shifting the chrome, fields, tabs, preview, and icons to a black / gold / violet palette.
  * Completed: refined that dark-mode pass by restoring a gold native top bar, moving the preview controls back to a hover-only overlay inside the preview surface so they do not resize the video, unifying the preview/queue stop button treatment, darkening the timeline track, and adding explicit combo-box and spin-box arrow affordances.
  * Completed: moved the preview timer badge out of the video overlay and into the Preview/Task Log header corner, with the badge shown only while the Preview tab is active.
  * Deferred: remaining FFMS2 preview-backend follow-up and CI validation no longer block M17 completion.

Likely files/modules:
  * `src/app/`
  * `src/app/widgets/`
  * `src/app/viewmodels/`
  * `tests/app/`

Risks:
  * Letting presentation-layer cleanup turn into hidden encode-policy decisions in the GUI.
  * Adding visual complexity that makes the existing workflow less predictable instead of more useful.
  * Reworking layouts in ways that regress the Windows-first portable app flow.

Validation:
  * Launch the desktop app and verify that the supported encode flow remains understandable from asset selection through start, progress, and completion.
  * Verify that the revised window layout still works at practical desktop sizes without hiding required controls.
  * Verify that the GUI continues to delegate job construction and execution to the existing core boundaries.

Done criteria:
  * The desktop shell looks materially cleaner and more coherent than the first usable GUI.
  * The current workflow is easier to understand and use without expanding the core feature surface unnecessarily.
  * GUI polish remains separated from encode-core policy and pipeline logic.

### M18 Add automatic output naming

Status: Completed

Scope:
  * Add automatic output naming so the app can generate a predictable default filename from the current job context.
  * Use a structured naming pattern based on:
    * user-changeable text
    * source folder name
    * next available numeric suffix based on existing output filenames
    * selected video codec tag
    * selected audio codec tag
    * output container extension
  * Default the numeric suffix to `1` when no existing matching filenames are present.
  * Preserve manual output-path editing so automatic naming remains a default behavior rather than a forced one.

Current slice status:
  * Completed: added a dedicated core output-naming helper that builds `[custom text] [source folder] - [number] [video codec] [audio codec].[ext]` names, renders the numeric suffix as two digits such as `01` and `03`, normalizes tags/extensions, and scans the target directory for the next available matching number.
  * Completed: wired the desktop app to keep the generated output path in the front output strip while moving the custom-text editor into the Encode settings tab, and preserve manual output-path edits until the user explicitly restores automatic naming with the new `Auto` action.
  * Completed: added focused core tests for default naming, exact-pattern numbering, source-copy codec tags, silent-source `NoAudio` tagging, and extension normalization.
  * Completed: added the new output-naming test target to the Windows MSYS2 CI build script so CTest does not reference an executable that the scripted target list failed to build.
  * Validation note: local C++ build/test execution was intentionally not run because this repository's current policy keeps compile/test validation in GitHub Actions; the local validation step for this slice was limited to patch review and `git diff --check`.

Naming shape:
  * `[custom text] [folder name] - [next available number] [video codec] [audio codec].[ext]`
  * Example: `[Testing] Za Folder - 03 [x265] [AAC].mp4`

Likely files/modules:
  * `src/core/include/utsure/core/job/`
  * `src/core/src/job/`
  * `src/core/src/io/`
  * `src/app/`
  * `tests/core/`
  * `tests/app/`

Risks:
  * Producing filenames that are inconsistent when users partially edit the custom text.
  * Misdetecting the next available number when older outputs use slightly different naming variants.
  * Mixing display-oriented naming rules into unrelated encode-policy code.

Validation:
  * Verify that the default output name includes the custom text, folder name, numeric suffix, codec tags, and container extension in the expected order.
  * Verify that the rendered numeric suffix is zero-padded to two digits, such as `01` and `03`.
  * Verify that numbering starts at `1` when no matching outputs exist.
  * Verify that numbering increments to the next available value when matching filenames already exist.
  * Verify that codec and extension tags reflect the actual selected encode/output settings.
  * Verify that manual output naming still cleanly overrides the generated default.

Done criteria:
  * The app generates a predictable default output filename using the defined naming pattern.
  * Existing matching filenames are checked to choose the next available number.
  * Numbering defaults to `1` when no prior matching outputs are found.
  * Manual output naming remains supported without fighting the automatic default.

### M19 Add automatic subtitle selection with explicit priority rules

Status: Completed

Scope:
  * Add automatic subtitle discovery and selection for the current source workflow.
  * Define explicit priority rules when multiple candidate subtitle files exist beside the source media.
  * Prefer subtitle files whose names include `.fx` before `.ass` when both otherwise match the same source, while keeping the broader selection rules understandable and testable.

Likely files/modules:
  * `src/core/include/utsure/core/subtitles/`
  * `src/core/src/subtitles/`
  * `src/core/src/job/`
  * `src/app/`
  * `tests/core/`

Risks:
  * Choosing subtitle candidates with matching rules that are too loose and accidentally bind the wrong file.
  * Hiding file-pick behavior so users cannot understand why one subtitle was chosen over another.
  * Baking filename-specific heuristics too deeply into unrelated subtitle-rendering code.

Validation:
  * Verify subtitle auto-selection for one source with no matching subtitle, one matching `.ass` subtitle, and multiple matching subtitle candidates.
  * Verify that a candidate containing `.fx` before `.ass` is preferred over an otherwise matching plain `.ass` candidate.
  * Verify that manual subtitle selection can still override the automatic result.
  * Local validation for this slice is limited to patch-level checks because compile/test validation is reserved for GitHub Actions in this repository.

Done criteria:
  * The app can auto-select a subtitle file when a clear matching candidate exists.
  * Priority rules are explicit enough to test and explain.
  * `.fx`-qualified ASS subtitles win over lower-priority ASS candidates when both match the same source.
  * Implemented with a dedicated core selector, UI auto/manual override wiring, focused subtitle-selection tests, and CI target coverage for the new test executable.

### M20 Add FontCollector-based subtitle font recovery and fallback

Status: Completed

Scope:
  * Add a font-recovery path using `FontCollector` to improve subtitle font correctness when the normal Windows/system-font path fails in real-world encode runs.
  * Use `FontCollector` against ASS subtitle inputs to recover the fonts actually required by the subtitle script instead of depending only on local font resolution.
  * Treat this as a subtitle-font reliability layer, not as a replacement for the existing subtitle renderer abstraction.
  * Support feeding recovered fonts into the subtitle-rendering/burn-in path, and where the output/container flow allows it, support mux-oriented font attachment behavior as a separate integration step.

Likely files/modules:
  * `src/core/include/utsure/core/subtitles/`
  * `src/core/src/subtitles/`
  * `src/core/src/process/`
  * `src/core/src/job/`
  * `src/app/`
  * `tests/core/`
  * `tests/app/`

Risks:
  * Adding an external-tool dependency in a way that is brittle on Windows packaging or portable-app setups.
  * Mixing font-discovery fallback policy into unrelated subtitle rendering or encode orchestration code.
  * Relying on tool-specific behavior that differs between plain ASS font recovery and MKV mux-oriented flows.

Validation:
  * Verify that ASS subtitle jobs can trigger a `FontCollector`-based recovery step when enabled.
  * Verify that subtitle rendering can use recovered fonts when the normal system-font path would otherwise produce incorrect styling.
  * Verify that jobs still behave predictably when the recovery step finds no additional fonts or when the tool is unavailable.
  * Verify that the integration remains bounded and explicit, with clear logs/reporting about whether recovered fonts were used.
  * Local validation for this slice is limited to patch-level checks because compile/test validation is reserved for GitHub Actions in this repository.

Done criteria:
  * The app can use `FontCollector` as an explicit fallback path for ASS subtitle font recovery.
  * Recovered fonts can be made available to the subtitle pipeline so bad-font cases are reduced in practical Windows encode runs.
  * The integration preserves the existing `encoder-core` vs desktop-shell separation and does not bury tool-specific policy across unrelated modules.
  * Implemented with a dedicated subtitle session-preparation helper, a small core external-tool runner, explicit encode-path logs for recovered/no-font/tool-unavailable cases, and focused stub-driven core tests plus CI target coverage.

### M21 Add global parallel batch encoding with bounded job counts and pre-reserved output naming

Status: Completed

Scope:
  * Add parallel batch encoding as a global run-level feature rather than a per-queue setting.
  * Keep parallel encoding turned off by default so the existing single-job behavior remains the normal startup path.
  * Detect total usable system threads and allow only parallel job counts that divide that total exactly.
  * Keep parallel controls out of the per-queue `Main | Encode | Special | Logs` area and place them in the top execution toolbar near process priority and start/stop controls.
  * Use a compact global `Parallel` control with hover summary behavior and a click-open settings window so the main layout and preview space stay unchanged.
  * Pre-reserve automatic output names for the full batch before execution begins so parallel workers cannot race and choose the same number.

UI behavior:
  * Add a global `Parallel` control near the priority selector.
  * Parallel encoding is off by default on startup.
  * Hovering `Parallel` shows a short summary of current parallel state.
  * Clicking `Parallel` opens a small settings window for configuring the feature.
  * Do not place parallel controls inside per-queue tabs.

Scheduling rules:
  * Let `T` be the detected total usable thread count.
  * A parallel job count `N` is valid only when `T % N == 0`.
  * Per-job thread count is `T / N`.
  * When parallel encoding is off, retain the current single-job execution behavior.
  * Pre-reserve auto-generated output names for the checked batch before starting any worker so parallel jobs cannot race on the same numeric suffix.

Current slice status:
  * Completed: added a dedicated core `BatchParallelism` helper that detects usable threads, exposes only exact-divisor job counts, applies the tiered per-job buffer policy, and can inject planned thread/buffer overrides into encode jobs when global parallel mode is enabled.
  * Completed: extended output naming with batch reservation so the checked run can pre-allocate unique auto-name sequence numbers before any worker starts, keeping numbered outputs collision-safe even when multiple workers launch together.
  * Completed: rewired the desktop app queue runner from one active controller into global planned batch dispatch with multiple runner slots, stop-all cancellation, per-slot progress routing, and duplicate output-path rejection inside the batch plan so one failed job does not stop unrelated jobs.
  * Completed: added a top-toolbar `Parallel` control beside priority, with default-off behavior, hover summary tooltip, and a compact modal dialog for enabling/disabling the feature and choosing one of the valid job counts.
  * Completed: added focused core tests for divisor/job-count selection, thread allocation, buffer tiers, execution-setting injection, and batch output-name reservation, plus CMake/CI target wiring for the new parallel-planning test executable.

Validation:
  * Verify that parallel mode stays off on startup and the existing single-job path remains the default until the user explicitly enables parallel batch encoding.
  * Verify that the toolbar `Parallel` button opens the dialog, reports `On/Off`, `Jobs`, `Threads/job`, and `Buffer/job` through its tooltip summary, and leaves the preview/editor layout unchanged.
  * Verify that only exact divisors of the detected usable thread count are offered and that the resulting threads/job and buffer/job values follow the documented tiers.
  * Verify that the batch planner reserves auto-managed output paths before worker startup and rejects duplicate final output paths within the run.
  * Local validation for this slice is limited to patch-level checks because compile/test validation is reserved for GitHub Actions in this repository.

### M22 Improve FFMS2 preview responsiveness and latency visibility

Status: Completed

Scope:
  * Investigate the FFMS2-backed video preview path and add lightweight timing instrumentation so preview latency costs are visible.
  * Prioritize low-latency interactive seek/scrub requests over large cache-window refills while keeping playback/prefetch behavior intact.
  * Reuse preview setup more effectively, especially around prefetch, without violating FFMS2 per-source thread-safety rules.
  * Keep subtitle-enabled preview correctness, existing error handling, and the current preview UI layout intact.

Current slice status:
  * Completed: instrumented the preview controller, worker, and FFMS2 backend so preview logs now report queue wait, session-creation timing, index load/build reuse, video-source setup, frame-timing-table construction, decode-window cost, subtitle composition, final image-copy cost, and total request latency.
  * Completed: split preview refill behavior so interactive scrubs decode a single frame by default, while playback keeps the existing larger sequential window behavior for smoother forward motion and prefetch.
  * Completed: hardened newest-request-wins behavior by tagging each dispatched preview request with a controller-side generation and letting the worker skip superseded work before subtitle composition and QImage conversion when newer scrub requests arrive.
  * Completed: replaced per-prefetch session recreation with a reusable secondary preview session for the active source so background playback prefetch can reuse FFMS2 setup without touching the same source context concurrently.
  * Completed: reduced unnecessary frame copying by avoiding a full decoded-frame copy on subtitle-free preview requests, and added focused core regression coverage for one-frame interactive seek plus reusable sequential preview-session playback.
  * Validation note: local runtime measurements and local C++ build/test execution were intentionally not run because this repository keeps compile/test validation in GitHub Actions; the local validation step for this slice was limited to code-path inspection, targeted regression-test updates, and `git diff --check`.

### M23 Investigate encode throughput and add a subtitle-free fast path

Status: Completed

Scope:
  * Surface end-of-encode timing breakdowns so total elapsed time, average output FPS, and decode/process/subtitle/encode stage costs are visible in logs and reports.
  * Preserve the existing libassmod RGBA subtitle path for subtitle-enabled jobs.
  * Add a subtitle-free fast path that keeps frames in decoder-native `AVFrame` form until the encoder-preparation step, instead of forcing a normalize-to-RGBA and convert-back-to-YUV round trip.
  * Reduce avoidable per-frame allocations and copies by reusing swscale contexts, encoder input frames, and other working surfaces where practical.
  * Keep intro/main/outro sequencing, timestamps, CFR behavior, mux behavior, and current x264/x265 preset+CRF defaults unchanged unless diagnostics prove encode settings are the dominant bottleneck.

Current slice status:
  * Completed: traced the streaming decode -> process -> subtitle -> encode path and confirmed that subtitle-free jobs were still paying for a native-frame clone, RGBA normalization into owned bytes, and a second conversion into encoder input.
  * Completed: introduced an internal streaming frame representation so subtitle-disabled segments can stay in native `AVFrame` form until encoder handoff, while subtitle-enabled segments still materialize RGBA frames for libassmod composition.
  * Completed: removed one extra subtitle-path copy by scaling directly into owned RGBA plane storage instead of allocating a temporary RGBA `AVFrame` and then copying it row by row.
  * Completed: reused encoder input `AVFrame` storage and kept swscale contexts cached inside the output session so converted frames no longer allocate a fresh encoder surface every frame.
  * Completed: added a direct native-frame handoff when the decoder already delivers the encoder pixel format, so subtitle-free jobs can skip both RGBA normalization and the final conversion step entirely.
  * Completed: extended runtime logs and encode-job reports with total stage costs, percentages of wall-clock time, average output FPS, and effective decoder/encoder/worker/queue settings.
  * Completed: updated focused core tests so subtitle-free jobs assert zero subtitle-compose time while subtitle-enabled jobs assert non-zero subtitle-compose time and observe the expected fast-path / RGBA-path runtime logs.
  * Validation note: local C++ build/test execution and local before/after runtime measurements were intentionally not run because this repository reserves compile/test validation for GitHub Actions; the local validation step for this slice was limited to targeted test updates, code-path inspection, and `git diff --check`.

Validation:
  * Compare timing logs for one subtitle-free source and one ASS subtitle burn-in source before and after the patch.
  * Verify that subtitle-disabled jobs no longer report meaningful subtitle-stage cost and avoid the RGBA-only processing path.
  * Verify that subtitle-enabled jobs still preserve burn-in output correctness.
  * Verify that intro/main/outro sequencing, audio mux behavior, output timestamps, and CFR cadence remain unchanged.
  * Local validation for this slice remains limited to patch-level checks such as targeted test updates and `git diff --check`, because compile/test execution is reserved for GitHub Actions in this repository.

### M24 Investigate subtitle-enabled encode throughput

Status: In progress

Scope:
  * Focus on the real comparison workload: subtitle-enabled streaming encodes that keep libassmod burn-in active with the existing codec/CRF/preset defaults.
  * Reduce subtitle composition cost by avoiding per-bitmap subtitle copies where the renderer can blend directly into the RGBA frame.
  * Reduce pipeline serialization by moving subtitle render/composition work off the ordered encode lane and into the bounded video-worker stage while keeping output ordering unchanged.
  * Preserve subtitle appearance, frame timing, intro/main/outro sequencing, audio mux behavior, and existing end-of-encode timing breakdown logs.

Current slice status:
  * Completed: traced the remaining subtitle-enabled bottleneck to two app-side costs that stayed on the ordered encode thread: libassmod RGBA tile copying into `SubtitleBitmap.bytes` and serial subtitle render/blend immediately before encoder handoff.
  * Completed: extended the subtitle-session contract with a direct frame-composition hook so the default path still supports `render()` while renderer-specific fast paths can compose without materializing copied bitmap vectors.
  * Completed: taught the libassmod adapter to blend premultiplied RGBA subtitle tiles directly into the target frame, removing the extra per-bitmap heap allocation/copy from the hot subtitle-enabled burn-in path.
  * Completed: moved subtitle render/composition into worker-local subtitle sessions inside the existing bounded video processor so subtitle-enabled jobs no longer serialize render/blend on the main ordered encode handoff.
  * Completed: kept `video_process` and `subtitle_compose` timing buckets separate after the worker move so logs and reports still show whether normalization or subtitle render/blend dominates a real subtitle-enabled encode.
  * Completed: updated focused subtitle burn-in regression checks so runtime logs confirm the worker-local subtitle-session path is active.
  * Completed: aligned the clipped direct-RGBA compositor regression with the same premultiplied source-over math used by the copied-bitmap path, so CI keeps one contract for subtitle blend behavior while the newer libassmod pin is in use.
  * Completed: added explicit subtitle isolation/runtime-debug controls so CI can compare direct bitmap bytes vs copied bitmap bytes and worker-local sessions vs serialized subtitle composition without code edits; the safe default now stays on copied bitmap transfer plus serialized subtitle composition until the optimized modes are revalidated.
  * Completed: hardened the optimized subtitle path so invalid bitmap/session/frame state fails loudly with actionable diagnostics instead of risking silent late-session memory corruption.
  * Completed: extended subtitle-enabled validation with a longer stress encode, isolation-mode test coverage, compositor hardening checks, and a dedicated AddressSanitizer-oriented CI job to surface crash-prone combinations automatically.
  * Completed: wired the user-provided desktop branding assets so the toolbar brand mark now uses `logo.svg` and the Qt application/window icon now uses `icon.svg`, without changing encode behavior.
  * Completed: added a narrow Windows shell-icon follow-up that generates a multi-size `.ico` from the same `icon.svg` during the app build and embeds it into `utsure.exe`, so Explorer and taskbar surfaces can use the branded executable icon instead of the generic default.
  * Completed: fixed the follow-up Qt desktop-shell regression from the branding patch by updating the busy Start-button refresh path to the renamed generic widget-style helper, restoring MSYS2 CI app-target compilation.
  * Completed: added a narrow core-owned multi-audio slice that enumerates audio streams, captures language/title/disposition metadata, prefers explicitly Japanese streams over defaults, falls back deterministically by stream index, and threads that selected stream through inspection, decode, and main encode-path usage without adding a manual picker.
  * Completed: fixed the Windows subtitle AddressSanitizer CI setup by moving that dedicated job to MSYS2 `CLANG64`, parameterizing the CI scripts around the active MSYS2 prefix, and adding an early configure-time guard so unsupported MinGW GCC ASan requests fail with a clear message instead of a late `-lasan` link error.
  * Completed: fixed the follow-up `CLANG64` FFmpeg configure failure in that subtitle AddressSanitizer job by forwarding the explicit clang/LLVM tool env (`CC`, `CXX`, `AR`, `NM`, `RANLIB`, `STRIP`, `WINDRES`) into `ffmpeg/configure`, so it stops falling back to the default `gcc` probe on Windows.
  * Completed: added a narrow desktop-shell drag-and-drop source-import follow-up that keeps intake centralized in `MainWindow`, expands dropped folders one level only through a non-UI helper, filters to supported video extensions, and shows one whole-window overlay instead of per-panel drop targets.
  * Completed: fixed the follow-up Windows MSYS2 CI regression from that drag-and-drop slice by adding the new `utsure_core_source_import_paths_tests` target to the shared `windows-msys2-build.sh` target list before `ctest`, so the registered test executable is present when the April 5, 2026 workflow runs its smoke/test step.
  * Completed: polished the drag-and-drop overlay presentation after user review by widening the import card, switching to a stronger icon-plus-copy layout, and tightening the typography/backdrop styling without changing the underlying drag/drop intake path.
  * Completed: relaxed intro/outro inspection-time frame-rate matching, normalized mismatched intro/outro decoded video cadence onto the main segment's authoritative output cadence in both timeline composition and streaming encode paths, kept resolution/SAR/audio-layout compatibility strict, and updated focused timeline/preflight coverage for a `30 fps` intro against a `24000/1001` main source.
  * Completed: fixed the follow-up Windows CI regression from that cadence patch by restoring the missing `format_rational()` helper in `encode_job_preflight_tests.cpp` and cleaning two newly surfaced warning sites in subtitle bitmap composition and preview audio decode helpers without changing runtime behavior.
  * Completed: promoted main-source trim from UI-only state into the core encode/timeline contracts, validated invalid trim ranges during timeline assembly/preflight, clipped the main segment in timeline composition and the streaming encode path, blocked unsafe trimmed audio stream-copy fallback, and added focused trim coverage for main-only, intro/trimmed-main/outro, subtitle burn-in, and invalid-trim preflight cases while leaving existing no-trim behavior unchanged.
  * Completed: updated the trim follow-up validation so trimmed audio correctness is checked by kept duration, boundary timestamps, sample-count/runtime sync, and tolerance against extra post-trim audio instead of exact normalized block counts, and documented that split trim boundaries can legitimately increase audio block counts.
  * Completed: fixed the trimmed streaming follow-up where `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` could resume main-segment decode with per-frame SAR metadata that disagreed with the inspected main stream, by keeping the normalized inspected main-stream SAR authoritative for output/subtitle setup, relaxing that self-mismatch only for trimmed main-segment runtime decode, and adding a focused trimmed encode regression source that deliberately disagrees between filtered frame SAR and container DAR metadata.
  * Pending: capture real before/after timing data for the requested subtitle-enabled comparison encode, because this repository does not allow local compile/run validation and the current workspace does not contain a prebuilt binary to execute.
  * Pending: confirm in CI which isolation combination still reproduces or removes the historical long-session crash window, then decide whether the direct or worker-local modes can safely become default again.
  * Validation note: local validation for this slice was limited to code-path inspection, focused test updates, and `git diff --check`; actual benchmark numbers, sanitizer findings, and end-to-end sustained encode verification still need CI or another approved prebuilt runtime environment.

## Immediate next milestone

Finish M24 by running the real subtitle-enabled before/after encode benchmark and confirming output/timing correctness in CI or another approved runtime environment.
