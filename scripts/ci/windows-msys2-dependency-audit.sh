#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${third_party_root}/ffmpeg/prefix}"
ffmpeg_pcdir="${ffmpeg_prefix}/lib/pkgconfig"
ffms2_prefix="${UTSURE_FFMS2_PREFIX:-${third_party_root}/ffms2/prefix}"
ffms2_pcdir="${ffms2_prefix}/lib/pkgconfig"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"
msys2_prefix="${UTSURE_MSYS2_PREFIX:-${MINGW_PREFIX:-/ucrt64}}"

normalize_path() {
  cygpath -m "$1" | tr '[:upper:]' '[:lower:]'
}

export PATH="${ffmpeg_prefix}/bin:${ffms2_prefix}/bin:${libassmod_prefix}/bin:${msys2_prefix}/bin:${PATH}"
export PKG_CONFIG_PATH="${ffmpeg_pcdir}:${ffms2_pcdir}:${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

test -f "${msys2_prefix}/lib/cmake/Qt6/Qt6Config.cmake"

ffmpeg_release_line="$(ffmpeg -version | head -n 1)"
case "${ffmpeg_release_line}" in
  "ffmpeg version 7.1."*|"ffmpeg version n7.1."*)
    ;;
  *)
    echo "Expected the pinned ffmpeg executable to report a 7.1.x release, but got: ${ffmpeg_release_line}"
    exit 1
    ;;
esac

pkg-config --modversion libavcodec libavformat libavutil libswresample libswscale ffms2 x264 x265 libass

assert_pcdir_under_prefix() {
  local module_name="$1"
  local expected_prefix="$2"
  local resolved_pcdir
  local normalized_expected_prefix
  local normalized_resolved_pcdir

  resolved_pcdir="$(pkg-config --variable=pcfiledir "${module_name}")"
  normalized_expected_prefix="$(normalize_path "${expected_prefix}")"
  normalized_resolved_pcdir="$(normalize_path "${resolved_pcdir}")"

  case "${normalized_resolved_pcdir}" in
    "${normalized_expected_prefix}"/*) ;;
    *)
      echo "Expected ${module_name} to resolve from ${expected_prefix}, but pkg-config resolved ${resolved_pcdir}."
      exit 1
      ;;
  esac
}

assert_pcdir_under_prefix libavcodec "${ffmpeg_prefix}"
assert_pcdir_under_prefix libavformat "${ffmpeg_prefix}"
assert_pcdir_under_prefix libavutil "${ffmpeg_prefix}"
assert_pcdir_under_prefix libswresample "${ffmpeg_prefix}"
assert_pcdir_under_prefix libswscale "${ffmpeg_prefix}"
assert_pcdir_under_prefix ffms2 "${ffms2_prefix}"
assert_pcdir_under_prefix libass "${libassmod_prefix}"

echo "Dependency audit passed."
