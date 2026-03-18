# Windows Portable Packaging And Release

This repository currently ships one documented release shape: a Windows x64 portable `.zip` bundle produced by the MSYS2 GitHub Actions workflow.

## What the portable bundle contains

- `utsure.exe`
- Qt runtime files deployed by `windeployqt`
- manually copied non-Qt runtime DLLs from MSYS2 and the pinned `libassmod` prefix
- `LICENSE`
- bundle manifests:
  - `qt-runtime-files.txt`
  - `non-qt-runtime-dependencies.txt`
  - `bundle-file-manifest.txt`

The packaging entry point is `scripts/ci/windows-msys2-package-portable.sh`.

## What the packaging script already validates

- the built app exists before packaging starts
- `windeployqt` can stage the Qt runtime
- non-Qt DLL dependencies can be discovered and copied
- the `.zip` artifact can be created successfully
- the `.zip` artifact can be extracted into a clean validation directory
- the extracted app can launch in offscreen smoke-test mode outside the build tree

## Current release assumptions

- target platform: Windows x64
- toolchain/runtime source: MSYS2 UCRT64
- distribution format: portable zip, not an installer
- signing: not implemented yet
- auto-update: not implemented yet

## Release checklist

1. Start from a green `windows-msys2` workflow run on the target commit.
2. Confirm the portable bundle smoke test passed after re-extracting the zip.
3. Keep the bundle manifests with the artifact so dependency contents remain auditable.
4. Record the pinned `UTSURE_LIBASSMOD_REF` and any dependency version changes made for the release.
5. Test the zip on a clean Windows machine before treating it as a user-facing release.
6. Review redistribution and licensing obligations for FFmpeg, x264, x265, Qt, and `libassmod`.

## Known release gaps

- No installer, file association, or uninstaller path exists yet.
- No code signing is wired into CI.
- The smoke test proves launchability, not full encode validation on a clean machine.
- Linux and macOS packaging are not implemented.
- Audio output encode is still not part of the shipped pipeline.

## What GitHub Actions should continue to verify

- configure and build the core library, app, and test executables
- run the core pipeline tests, including preflight, timeline, encode, and subtitle-burn paths
- launch the Qt Widgets app in offscreen smoke-test mode
- package the portable bundle, extract it, and launch it again outside the build tree
