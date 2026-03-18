#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"

export PATH="${libassmod_prefix}/bin:/ucrt64/bin:${PATH}"
export PKG_CONFIG_PATH="${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

test -f /ucrt64/lib/cmake/Qt6/Qt6Config.cmake

pkg-config --modversion libavcodec libavfilter libavformat libavutil libswresample libswscale x264 x265 libass

resolved_libass_pcdir="$(pkg-config --variable=pcfiledir libass)"
case "${resolved_libass_pcdir}" in
  "${libassmod_prefix}"/*) ;;
  *)
    echo "Expected libass to resolve from ${libassmod_prefix}, but pkg-config resolved ${resolved_libass_pcdir}."
    exit 1
    ;;
esac

echo "Dependency audit passed."
