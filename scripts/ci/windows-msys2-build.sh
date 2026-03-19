#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${third_party_root}/ffmpeg/prefix}"
ffmpeg_pcdir="${ffmpeg_prefix}/lib/pkgconfig"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"
cmake_build_type="${UTSURE_CMAKE_BUILD_TYPE:-Debug}"

export PATH="${ffmpeg_prefix}/bin:${libassmod_prefix}/bin:/ucrt64/bin:${PATH}"
export PKG_CONFIG_PATH="${ffmpeg_pcdir}:${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE="${cmake_build_type}" \
  -DCMAKE_PREFIX_PATH=/ucrt64 \
  -DUTSURE_BUILD_APP=ON \
  -DUTSURE_ENABLE_DEPENDENCY_AUDIT=ON \
  -DUTSURE_REQUIRE_FFMPEG=ON \
  -DUTSURE_FFMPEG_ROOT="${ffmpeg_prefix}" \
  -DUTSURE_REQUIRE_LIBASSMOD=ON \
  -DUTSURE_LIBASSMOD_ROOT="${libassmod_prefix}"

cmake --build build --target utsure_encoder_core --parallel
cmake --build build --target utsure_encoder_app --parallel
cmake --build build --target utsure_core_media_inspection_tests --parallel
cmake --build build --target utsure_core_media_decode_tests --parallel
cmake --build build --target utsure_core_media_encode_tests --parallel
cmake --build build --target utsure_core_encode_job_tests --parallel
cmake --build build --target utsure_core_encode_job_preflight_tests --parallel
cmake --build build --target utsure_core_timeline_tests --parallel
cmake --build build --target utsure_core_subtitle_renderer_tests --parallel
cmake --build build --target utsure_core_subtitle_bitmap_compositor_tests --parallel
cmake --build build --target utsure_core_subtitle_burn_in_tests --parallel

ctest --test-dir build --output-on-failure

export QT_PLUGIN_PATH="/ucrt64/share/qt6/plugins"
export QT_QPA_PLATFORM="offscreen"

./build/src/app/utsure.exe --dump-window-structure --smoke-test
