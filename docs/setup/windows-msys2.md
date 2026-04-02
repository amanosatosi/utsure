# Windows MSYS2 Developer Setup

This is the supported local developer path today. It matches the Windows GitHub Actions job and is the expected setup for anyone picking up the repository.

## Use the right shell

Run the commands below from the MSYS2 `UCRT64` shell, not from PowerShell or the plain MSYS shell.

## One-time package install

```bash
pacman -S --needed \
  autoconf \
  automake \
  git \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-nasm \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-qt6-base \
  mingw-w64-ucrt-x86_64-qt6-svg \
  mingw-w64-ucrt-x86_64-libx264 \
  mingw-w64-ucrt-x86_64-x265 \
  mingw-w64-ucrt-x86_64-zlib \
  mingw-w64-ucrt-x86_64-freetype \
  mingw-w64-ucrt-x86_64-fribidi \
  mingw-w64-ucrt-x86_64-harfbuzz \
  curl \
  libtool \
  make
```

## Fresh clone quick start

From a clean repository checkout:

```bash
export UTSURE_LIBASSMOD_REF=88a338192faf50505eb4cedfe7d1320265f1081f
export UTSURE_FFMPEG_VERSION=7.1.2
export UTSURE_FFMS2_REF=25cef14386fcaaa58ee547065deee8f6e82c56a2
./scripts/ci/windows-msys2-build-ffmpeg.sh
./scripts/ci/windows-msys2-build-ffms2.sh
./scripts/ci/windows-msys2-build-libassmod.sh
./scripts/ci/windows-msys2-dependency-audit.sh
UTSURE_CMAKE_BUILD_TYPE=Debug ./scripts/ci/windows-msys2-build.sh
```

What that does:

- builds the pinned FFmpeg 7.1.2 source dependency into `.deps/ffmpeg/prefix`
- builds the pinned FFMS2 preview dependency into `.deps/ffms2/prefix`
- builds the pinned `libassmod` source dependency into `.deps/libassmod/prefix`
- audits configure-time dependency discovery before the main build
- configures `build/` with CMake and Ninja
- builds the core library, desktop app, and core test executables
- runs `ctest --output-on-failure`
- launches the Qt app in offscreen smoke-test mode

## Daily commands

Rebuild and rerun the current validation set:

```bash
UTSURE_CMAKE_BUILD_TYPE=Debug ./scripts/ci/windows-msys2-build.sh
```

Create the current portable release artifact:

```bash
UTSURE_CMAKE_BUILD_TYPE=Release ./scripts/ci/windows-msys2-build.sh
./scripts/ci/windows-msys2-package-portable.sh
```

## Important paths

- `.deps/libassmod/src`: pinned `libassmod` checkout
- `.deps/libassmod/build`: `libassmod` build directory
- `.deps/libassmod/prefix`: `libassmod` install prefix used by CMake and `pkg-config`
- `.deps/ffmpeg/src`: pinned FFmpeg source tree
- `.deps/ffmpeg/build`: FFmpeg build directory
- `.deps/ffmpeg/prefix`: pinned FFmpeg install prefix used by CMake, `pkg-config`, and the sample-media tests
- `.deps/ffms2/src`: pinned FFMS2 source tree
- `.deps/ffms2/build`: FFMS2 build directory
- `.deps/ffms2/prefix`: pinned FFMS2 install prefix used by CMake, `pkg-config`, and preview-only runtime packaging
- `build/`: local CMake build tree
- `artifacts/encoder-windows-x64-portable`: unpacked portable bundle
- `artifacts/encoder-windows-x64-portable.zip`: zipped portable artifact

## Manual CMake notes

- `UTSURE_FFMPEG_ROOT` should point at `.deps/ffmpeg/prefix` if you invoke CMake manually.
- `UTSURE_FFMS2_ROOT` should point at `.deps/ffms2/prefix` if you invoke CMake manually.
- `UTSURE_LIBASSMOD_ROOT` should point at `.deps/libassmod/prefix` if you invoke CMake manually.
- `PKG_CONFIG_PATH` must resolve `libavcodec`, `libavformat`, `libavutil`, `libswresample`, and `libswscale` from the FFmpeg prefix before any system FFmpeg entry.
- `PKG_CONFIG_PATH` must resolve `ffms2` from the FFMS2 prefix before any system FFMS2 entry.
- `PKG_CONFIG_PATH` must resolve `libass` from the `libassmod` prefix before any system `libass`.
- `CMAKE_PREFIX_PATH` should include `/ucrt64` for the MSYS2 Qt and other packaged dependencies.
- The desktop app now uses SVG-backed toolbar and timeline icons, so the MSYS2 setup must include both `mingw-w64-ucrt-x86_64-qt6-base` and `mingw-w64-ucrt-x86_64-qt6-svg`.
- Configure now fails if the discovered FFmpeg core libraries or FFMS2 preview library are outside their pinned prefixes, or if the pinned `ffmpeg` executable is outside the supported `7.1.x` series.

## Current limits

- GitHub Actions is still the authoritative source of build verification for this repository state.
- The supported local path is Windows-first via MSYS2 UCRT64.
- The only documented release output today is the portable Windows bundle.
- Large jobs can still be rejected during preflight if the bounded-memory streaming pipeline estimate exceeds its safety limit.
- libassmod `\img` subtitle scripts still require future host-side image registration and currently fail explicitly.
- Local build verification is still secondary to GitHub Actions for this repository state; the FFMS2 preview backend was integrated for CI-first validation, not for local compile/testing.
