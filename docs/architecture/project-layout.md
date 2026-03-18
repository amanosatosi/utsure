# Project Layout

The repository starts with two concrete build targets:

- `utsure_encoder_core`
  - Reusable C++ library.
  - Owns non-GUI logic.
  - This is where future media probing, timeline assembly, subtitle rendering requests, and encode orchestration will live.
- `utsure_encoder_app`
  - Qt 6 Widgets desktop application.
  - Owns windowing and user interaction only.

Current boundary rule:

- The app can depend on the core.
- The core must not depend on Qt Widgets.
