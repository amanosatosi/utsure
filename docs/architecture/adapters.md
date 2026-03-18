# Adapter Boundaries

Dependency wiring is centralized, but third-party APIs must stay out of the core domain model.

## Rules

- `src/app/` may depend on Qt 6 Widgets.
- `src/core/` public headers must not expose Qt, FFmpeg, `libx264`, `libx265`, or `libassmod` types.
- Future adapter implementations are the only places that should link the media and subtitle dependency targets.

## Planned adapter targets

- `src/core/include/utsure/core/subtitles/`
  - Defines the technology-agnostic subtitle renderer/session contracts that the rest of the core depends on.
  - Owns timestamp-based subtitle render requests and RGBA-oriented overlay results.
- `src/core/adapters/ffmpeg/`
  - Links `utsure::ffmpeg`.
  - Owns probing, decode, filter, and encode API integration.
- `src/core/adapters/ffmpeg/encoders/`
  - Links `utsure::x264` and `utsure::x265` where codec-specific wiring is needed.
- `src/core/src/adapters/libassmod/`
  - Links `utsure::subtitle_renderer_dependency` through `utsure_encoder_core`.
  - Implements the core subtitle renderer/session contracts, subtitle parsing/shaping details, and libassmod-backed bitmap generation.

## Why this boundary matters

- The GUI remains a thin desktop shell.
- The core can be tested without dragging GUI concerns into domain contracts.
- Future Linux/macOS bring-up stays focused on dependency providers and adapter implementations instead of rewriting project or timeline logic.
