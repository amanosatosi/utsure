# Roadmap

This roadmap is split into the next practical engineering steps versus later work that should stay deferred until the current pipeline is more complete.

## Near-term

1. Add audio output encode and muxing so the composed normalized audio timeline is preserved in final output.
2. Add end-to-end cancellation and safe cleanup so the GUI can stop long-running jobs without leaving confusing partial outputs behind.
3. Strengthen recoverability and diagnostics around failed jobs, especially temporary-file handling and actionable user-visible errors.
4. Establish the first Linux/macOS dependency and CI strategy so portability risks stop accumulating only on Windows.
5. Harden release operations with versioned artifacts, clean-machine validation, and an explicit decision on signing and installer scope.

## Later

- Expand timeline capabilities beyond the current intro/main/outro model.
- Expose richer subtitle controls in the GUI, including timing-mode selection and clearer preview behavior.
- Add project/session persistence so encode jobs can be saved and resumed from structured state.
- Investigate performance work such as hardware acceleration and larger-media throughput improvements.
- Revisit distribution shape after the portable bundle path is stable enough to justify an installer or updater.
