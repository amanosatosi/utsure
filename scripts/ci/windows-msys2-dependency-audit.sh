#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"

normalize_path() {
  cygpath -m "$1" | tr '[:upper:]' '[:lower:]'
}

export PATH="${libassmod_prefix}/bin:/ucrt64/bin:${PATH}"
export PKG_CONFIG_PATH="${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

test -f /ucrt64/lib/cmake/Qt6/Qt6Config.cmake

pkg-config --modversion libavcodec libavfilter libavformat libavutil libswresample libswscale x264 x265 libass

resolved_libass_pcdir="$(pkg-config --variable=pcfiledir libass)"
normalized_libassmod_prefix="$(normalize_path "${libassmod_prefix}")"
normalized_resolved_libass_pcdir="$(normalize_path "${resolved_libass_pcdir}")"

case "${normalized_resolved_libass_pcdir}" in
  "${normalized_libassmod_prefix}"/*) ;;
  *)
    echo "Expected libass to resolve from ${libassmod_prefix}, but pkg-config resolved ${resolved_libass_pcdir}."
    exit 1
    ;;
esac

echo "Dependency audit passed."
