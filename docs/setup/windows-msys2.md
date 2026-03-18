# Windows MSYS2 Setup

This is the Windows-first development path and the same dependency strategy used in GitHub Actions.

## Install MSYS2 UCRT64 packages

```bash
pacman -S --needed \
  git \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-qt6-base \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-libx264 \
  mingw-w64-ucrt-x86_64-x265 \
  mingw-w64-ucrt-x86_64-freetype \
  mingw-w64-ucrt-x86_64-fribidi \
  mingw-w64-ucrt-x86_64-harfbuzz
```

## Build the pinned `libassmod` dependency

The project currently expects the `libassmod` source dependency to be built into an isolated prefix.

```bash
export UTSURE_LIBASSMOD_REF=1.0
./scripts/ci/windows-msys2-build-libassmod.sh
```

That produces:

- source checkout under `.deps/libassmod/src`
- build directory under `.deps/libassmod/build`
- install prefix under `.deps/libassmod/prefix`

## Audit dependencies and build the project

```bash
./scripts/ci/windows-msys2-dependency-audit.sh
./scripts/ci/windows-msys2-build.sh
```

## Manual setup notes

- `UTSURE_LIBASSMOD_ROOT` is passed to CMake by the CI script and should point at the `libassmod` install prefix if you run CMake manually.
- `PKG_CONFIG_PATH` must resolve `libass` from the `libassmod` prefix before any system `libass`.
- The local machine in this thread is not the source of truth for compiler validation; GitHub Actions is.
