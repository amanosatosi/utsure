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
  * M17 Desktop GUI quality and usability in progress.
  * M18 Automatic output naming planned.
  * M19 Automatic subtitle selection planned.
  * M20 FontCollector-based subtitle font recovery and fallback planned.

## Active assumptions

- The project uses CMake.
- The architecture stays split between a reusable `encoder-core` and a thin Qt 6 Widgets desktop shell.
- Subtitle burn-in is hidden behind an adapter boundary so `libassmod` is not coupled directly to the rest of the core.
- Intro and outro clips are modeled as timeline segments, not one-off pipeline branches.
- Output frame rate always follows the main source video.
- Correctness comes before performance work.
- Windows build validation is currently anchored in GitHub Actions because the local machine does not expose a Qt-capable compiler toolchain.
- `libassmod` is currently treated as a pinned source dependency built into an isolated prefix because it installs as a `libass`-compatible package name rather than a uniquely named `libassmod` package.
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

Status: In progress

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
  * Completed: kept the qrc-backed icon path on the existing target-embedded Qt resource pipeline after confirming the app build does not emit a standalone `qInitResources_app_resources()` entry point for manual initialization.
  * Completed: hardened the Windows portable bundle to carry and validate Qt's SVG icon engine plugin explicitly so deployed toolbar icons do not regress to text fallback.
  * Completed: replaced the queue table's native blue selection paint with an explicit muted selection palette so selected rows stay readable without bright blue artifacting.
  * Completed: reduced desktop-shell height pressure by lowering the default/minimum window height, shrinking the preview surface minimum, and relaxing the initial splitter allocations.

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

Status: Planned

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

Status: Planned

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

Done criteria:
  * The app can auto-select a subtitle file when a clear matching candidate exists.
  * Priority rules are explicit enough to test and explain.
  * `.fx`-qualified ASS subtitles win over lower-priority ASS candidates when both match the same source.

### M20 Add FontCollector-based subtitle font recovery and fallback

Status: Planned

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

Done criteria:
  * The app can use `FontCollector` as an explicit fallback path for ASS subtitle font recovery.
  * Recovered fonts can be made available to the subtitle pipeline so bad-font cases are reduced in practical Windows encode runs.
  * The integration preserves the existing `encoder-core` vs desktop-shell separation and does not bury tool-specific policy across unrelated modules.

## Immediate next milestone
