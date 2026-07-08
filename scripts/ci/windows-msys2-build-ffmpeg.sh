#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
ffmpeg_root="${third_party_root}/ffmpeg"
ffmpeg_source_mode="${UTSURE_FFMPEG_SOURCE:-upstream}"
ffmpeg_version="${UTSURE_FFMPEG_VERSION:-7.1.2}"
ffmpeg_source_url="${UTSURE_FFMPEG_SOURCE_URL:-https://ffmpeg.org/releases/ffmpeg-${ffmpeg_version}.tar.xz}"
ffmpeg_mangetsu_repo_url="${UTSURE_FFMPEG_MANGETSU_REPO_URL:-https://github.com/amanosatosi/FFmpeg.git}"
ffmpeg_mangetsu_ref="${UTSURE_FFMPEG_MANGETSU_REF:-7.1}"
ffmpeg_mangetsu_commit="${UTSURE_FFMPEG_MANGETSU_COMMIT:-6282c1941e3611ce43a4dcbe83a679c0323b8b13}"
ffmpeg_archive_path="${UTSURE_FFMPEG_ARCHIVE_PATH:-${ffmpeg_root}/ffmpeg-${ffmpeg_version}.tar.xz}"
ffmpeg_source_dir="${UTSURE_FFMPEG_SOURCE_DIR:-${ffmpeg_root}/src}"
ffmpeg_build_dir="${UTSURE_FFMPEG_BUILD_DIR:-${ffmpeg_root}/build}"
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${ffmpeg_root}/prefix}"
ffmpeg_configure_flags_string="${UTSURE_FFMPEG_CONFIGURE_FLAGS:---enable-gpl --enable-libx264 --enable-libx265 --enable-shared --disable-static --disable-debug --disable-doc --disable-ffplay --disable-programs --enable-ffmpeg --enable-ffprobe}"
msys2_prefix="${UTSURE_MSYS2_PREFIX:-/ucrt64}"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"
if [[ "${ffmpeg_source_mode}" == "mangetsu" && "${ffmpeg_configure_flags_string}" != *"--enable-libass"* ]]; then
  ffmpeg_configure_flags_string="${ffmpeg_configure_flags_string} --enable-libass"
fi
ffmpeg_source_identity="${ffmpeg_source_mode}:${ffmpeg_version}"
if [[ "${ffmpeg_source_mode}" == "mangetsu" ]]; then
  ffmpeg_source_identity="${ffmpeg_source_mode}:${ffmpeg_mangetsu_ref}:${ffmpeg_mangetsu_commit}"
fi
ffmpeg_build_fingerprint="$(
  printf '%s\n%s\n%s\n%s\n' \
    "${ffmpeg_source_identity}" \
    "${ffmpeg_configure_flags_string}" \
    "${UTSURE_TOOLCHAIN_ID_PREFIX:-unknown-toolchain}" \
    "${msys2_prefix}" |
    sha256sum |
    awk '{print substr($1, 1, 16)}'
)"
ffmpeg_build_id="${UTSURE_FFMPEG_BUILD_ID:-ffmpeg-${ffmpeg_version}-${ffmpeg_build_fingerprint}}"
ffmpeg_stamp_file="${ffmpeg_prefix}/.utsure-ffmpeg-build-id"
ffmpeg_cc="${CC:-}"
ffmpeg_cxx="${CXX:-}"
ffmpeg_ar="${AR:-}"
ffmpeg_nm="${NM:-}"
ffmpeg_ranlib="${RANLIB:-}"
ffmpeg_strip="${STRIP:-}"
ffmpeg_windres="${WINDRES:-}"

mkdir -p "${ffmpeg_root}"

echo "Using FFmpeg source mode: ${ffmpeg_source_mode}"
echo "Using FFmpeg version: ${ffmpeg_version}"
if [[ "${ffmpeg_source_mode}" == "mangetsu" ]]; then
  echo "Using FFmpeg Mangetsu repo: ${ffmpeg_mangetsu_repo_url}"
  echo "Using FFmpeg Mangetsu ref: ${ffmpeg_mangetsu_ref}"
  echo "Using FFmpeg Mangetsu commit: ${ffmpeg_mangetsu_commit}"
fi
echo "Using FFmpeg build id: ${ffmpeg_build_id}"
if [[ -n "${ffmpeg_cc}" ]]; then
  echo "Using FFmpeg C compiler: ${ffmpeg_cc}"
fi
if [[ -n "${ffmpeg_cxx}" ]]; then
  echo "Using FFmpeg C++ compiler: ${ffmpeg_cxx}"
fi

if [[ "${ffmpeg_source_mode}" == "upstream" && ! -f "${ffmpeg_archive_path}" ]]; then
  curl -L --fail --output "${ffmpeg_archive_path}" "${ffmpeg_source_url}"
fi

if [ -f "${ffmpeg_stamp_file}" ] && [ "$(cat "${ffmpeg_stamp_file}")" = "${ffmpeg_build_id}" ]; then
  echo "Pinned FFmpeg build '${ffmpeg_build_id}' is already installed at ${ffmpeg_prefix}"
  exit 0
fi

rm -rf "${ffmpeg_source_dir}" "${ffmpeg_build_dir}" "${ffmpeg_prefix}"
mkdir -p "${ffmpeg_root}" "${ffmpeg_build_dir}" "${ffmpeg_prefix}"

case "${ffmpeg_source_mode}" in
  upstream)
    tar -xf "${ffmpeg_archive_path}" -C "${ffmpeg_root}"
    extracted_source_dir="${ffmpeg_root}/ffmpeg-${ffmpeg_version}"
    if [ ! -d "${extracted_source_dir}" ]; then
      echo "Expected FFmpeg source directory '${extracted_source_dir}' after extraction."
      exit 1
    fi

    mv "${extracted_source_dir}" "${ffmpeg_source_dir}"
    ;;
  mangetsu)
    git clone --branch "${ffmpeg_mangetsu_ref}" "${ffmpeg_mangetsu_repo_url}" "${ffmpeg_source_dir}"
    git -C "${ffmpeg_source_dir}" checkout --force "${ffmpeg_mangetsu_commit}"
    resolved_commit="$(git -C "${ffmpeg_source_dir}" rev-parse HEAD)"
    if [[ "${resolved_commit}" != "${ffmpeg_mangetsu_commit}" ]]; then
      echo "Expected FFmpeg Mangetsu commit '${ffmpeg_mangetsu_commit}', got '${resolved_commit}'."
      exit 1
    fi
    ;;
  *)
    echo "Unsupported UTSURE_FFMPEG_SOURCE '${ffmpeg_source_mode}'. Expected 'upstream' or 'mangetsu'."
    exit 1
    ;;
esac

export PATH="${libassmod_prefix}/bin:${msys2_prefix}/bin:${PATH}"
export PKG_CONFIG_PATH="${libassmod_pcdir}:${msys2_prefix}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
read -r -a ffmpeg_configure_flags <<< "${ffmpeg_configure_flags_string}"

configure_tool_args=()
if [[ -n "${ffmpeg_cc}" ]]; then
  configure_tool_args+=("--cc=${ffmpeg_cc}")
fi
if [[ -n "${ffmpeg_cxx}" ]]; then
  configure_tool_args+=("--cxx=${ffmpeg_cxx}")
fi
if [[ -n "${ffmpeg_ar}" ]]; then
  configure_tool_args+=("--ar=${ffmpeg_ar}")
fi
if [[ -n "${ffmpeg_nm}" ]]; then
  configure_tool_args+=("--nm=${ffmpeg_nm}")
fi
if [[ -n "${ffmpeg_ranlib}" ]]; then
  configure_tool_args+=("--ranlib=${ffmpeg_ranlib}")
fi
if [[ -n "${ffmpeg_strip}" ]]; then
  configure_tool_args+=("--strip=${ffmpeg_strip}")
fi
if [[ -n "${ffmpeg_windres}" ]]; then
  configure_tool_args+=("--windres=${ffmpeg_windres}")
fi

pushd "${ffmpeg_build_dir}" >/dev/null
"${ffmpeg_source_dir}/configure" \
  --prefix="${ffmpeg_prefix}" \
  "${configure_tool_args[@]}" \
  "${ffmpeg_configure_flags[@]}" \
  --pkg-config=pkg-config
make -j"$(nproc)"
make install
popd >/dev/null

printf '%s\n' "${ffmpeg_build_id}" > "${ffmpeg_stamp_file}"

echo "FFmpeg ${ffmpeg_version} installed to ${ffmpeg_prefix}"
