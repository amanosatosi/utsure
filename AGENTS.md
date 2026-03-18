# AGENTS.md

This repository is being built from scratch. Follow these repo-level rules:

- Work milestone by milestone. Finish the current milestone before starting the next.
- Before editing, inspect the current repository state and briefly state the implementation approach.
- Update `PLANS.md` whenever a milestone starts, changes scope, or completes.
- Keep GUI code thin. Put business logic, media pipeline logic, and timeline rules in the core library.
- Prefer clear, maintainable architecture over clever shortcuts.
- When something is ambiguous, make the smallest reasonable assumption and state it clearly.
- Validate each milestone with the smallest relevant build, test, or run step.
- Do not add features or infrastructure outside the active milestone.

Current product direction:

- Windows-first desktop app, with future Linux/macOS portability.
- C++ core.
- Qt 6 Widgets GUI.
- FFmpeg-based media pipeline.
- H.264/H.265 output.
- `libassmod` subtitle burn-in.
- Intro/outro clip support.
- Output frame rate follows the main source video.
