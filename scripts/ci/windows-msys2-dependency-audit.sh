#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${third_party_root}/ffmpeg/prefix}"
ffmpeg_source_mode="${UTSURE_FFMPEG_SOURCE:-upstream}"
ffmpeg_mangetsu_commit="${UTSURE_FFMPEG_MANGETSU_COMMIT:-6282c1941e3611ce43a4dcbe83a679c0323b8b13}"
ffmpeg_pcdir="${ffmpeg_prefix}/lib/pkgconfig"
ffms2_prefix="${UTSURE_FFMS2_PREFIX:-${third_party_root}/ffms2/prefix}"
ffms2_pcdir="${ffms2_prefix}/lib/pkgconfig"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"
msys2_prefix="${UTSURE_MSYS2_PREFIX:-/ucrt64}"
build_app="${UTSURE_BUILD_APP:-ON}"

normalize_path() {
  cygpath -m "$1" | tr '[:upper:]' '[:lower:]'
}

export PATH="${ffmpeg_prefix}/bin:${ffms2_prefix}/bin:${libassmod_prefix}/bin:${msys2_prefix}/bin:${PATH}"
export PKG_CONFIG_PATH="${ffmpeg_pcdir}:${ffms2_pcdir}:${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

if [[ "${build_app}" == "ON" ]]; then
  test -f "${msys2_prefix}/lib/cmake/Qt6/Qt6Config.cmake"
fi

ffmpeg_release_line="$(ffmpeg -version | head -n 1)"
if [[ "${ffmpeg_source_mode}" == "mangetsu" ]]; then
  ffmpeg_mangetsu_short_commit="${ffmpeg_mangetsu_commit:0:10}"
  if [[ "${ffmpeg_release_line}" != *"${ffmpeg_mangetsu_short_commit}"* ]]; then
    echo "Expected the Mangetsu FFmpeg executable to report pinned commit '${ffmpeg_mangetsu_short_commit}', but got: ${ffmpeg_release_line}"
    exit 1
  fi
  echo "Mangetsu FFmpeg source selected; accepting commit-version ffmpeg string: ${ffmpeg_mangetsu_short_commit}"
else
  case "${ffmpeg_release_line}" in
    "ffmpeg version 7.1."*|"ffmpeg version n7.1."*)
      ;;
    *)
      echo "Expected the upstream/release pinned ffmpeg executable to report a 7.1.x release, but got: ${ffmpeg_release_line}"
      exit 1
      ;;
  esac
fi

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

if [[ "${ffmpeg_source_mode}" == "mangetsu" ]]; then
  ffmpeg_filters="$(ffmpeg -hide_banner -filters)"
  if ! grep -Ei '(^|[[:space:]])ass[[:space:]]' <<< "${ffmpeg_filters}" >/dev/null; then
    echo "Expected the Mangetsu FFmpeg build to expose the ass filter, but it was missing from ffmpeg -filters."
    exit 1
  fi
  if ! grep -Ei '(^|[[:space:]])subtitles[[:space:]]' <<< "${ffmpeg_filters}" >/dev/null; then
    echo "Expected the Mangetsu FFmpeg build to expose the subtitles filter, but it was missing from ffmpeg -filters."
    exit 1
  fi

  ffmpeg_buildconf="$(ffmpeg -hide_banner -buildconf 2>&1)"
  if [[ "${ffmpeg_buildconf}" != *"--enable-libass"* ]]; then
    echo "Expected the Mangetsu FFmpeg build configuration to include --enable-libass."
    exit 1
  fi

  ass_filter_help="$(ffmpeg -hide_banner -h filter=ass 2>&1)"
  if [[ "${ass_filter_help}" != *"mangetsu_rgba"* ||
        "${ass_filter_help}" != *"mangetsu_actor_colorcoding"* ]]; then
    echo "Expected the Mangetsu FFmpeg ass filter to expose mangetsu_rgba and mangetsu_actor_colorcoding options."
    exit 1
  fi

  subtitles_filter_help="$(ffmpeg -hide_banner -h filter=subtitles 2>&1)"
  if [[ "${subtitles_filter_help}" != *"mangetsu_rgba"* ||
        "${subtitles_filter_help}" != *"mangetsu_actor_colorcoding"* ]]; then
    echo "Expected the Mangetsu FFmpeg subtitles filter to expose mangetsu_rgba and mangetsu_actor_colorcoding options."
    exit 1
  fi
fi

echo "Dependency audit passed."
