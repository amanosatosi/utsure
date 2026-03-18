# PLANS.md

This is the living execution plan for the repository. Update it as milestones progress.

## Current state

- Repository initialized.
- Existing tracked file: `LICENSE`.
- No build files, source tree, tests, or docs yet.

## Working assumptions

- Product goals are captured in `AGENTS.md`.
- When a detail is unclear, choose the smallest reasonable assumption and record it here.

## Milestones

- [x] M0 Inspect repository state
  - Report what exists, what is missing, recommended initial structure, and key technical risks.
- [x] M1 Add repo scaffolding docs
  - Create `AGENTS.md`.
  - Create `PLANS.md`.
- [ ] M2 Bootstrap repository layout
  - Add the initial top-level folders needed for source, tests, docs, and build configuration.
  - Do not add product code yet.
- [ ] M3 Establish build foundation
  - Add the minimal project configuration needed to build an empty core library and desktop shell.
- [ ] M4 Define core boundaries
  - Introduce core-facing project, timeline, and encode-domain skeletons.
  - Keep the GUI as a thin shell.
- [ ] M5 Wire external dependencies
  - Add a reproducible development path for Qt 6, FFmpeg, and `libassmod`.
- [ ] M6 Add minimal app shell
  - Add application startup and a minimal Qt 6 Widgets window wired to the core boundary.
- [ ] M7 Add first pipeline slice
  - Probe a source, build a basic timeline, and prepare the path to encode validation.

## Open decisions to resolve early

- Build system and dependency acquisition strategy.
- FFmpeg pipeline shape: filtergraph-heavy vs. more custom orchestration.
- Handling intro/outro assets whose properties differ from the main source.
- Packaging and codec-availability constraints.

## Next milestone

- Bootstrap the repository layout and choose the initial build-system direction.
