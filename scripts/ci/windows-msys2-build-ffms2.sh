#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${third_party_root}/ffmpeg/prefix}"
ffmpeg_pcdir="${ffmpeg_prefix}/lib/pkgconfig"
ffms2_root="${third_party_root}/ffms2"
ffms2_ref="${UTSURE_FFMS2_REF:-5.0}"
ffms2_source_dir="${UTSURE_FFMS2_SOURCE_DIR:-${ffms2_root}/src}"
ffms2_build_dir="${UTSURE_FFMS2_BUILD_DIR:-${ffms2_root}/build}"
ffms2_prefix="${UTSURE_FFMS2_PREFIX:-${ffms2_root}/prefix}"
ffms2_build_id="${UTSURE_FFMS2_BUILD_ID:-ffms2-${ffms2_ref}}"
ffms2_stamp_file="${ffms2_prefix}/.utsure-ffms2-build-id"

mkdir -p "${ffms2_root}"

echo "Using FFMS2 ref: ${ffms2_ref}"
echo "Using FFMS2 build id: ${ffms2_build_id}"

if [ -f "${ffms2_stamp_file}" ] && [ "$(cat "${ffms2_stamp_file}")" = "${ffms2_build_id}" ]; then
  echo "Pinned FFMS2 build '${ffms2_build_id}' is already installed at ${ffms2_prefix}"
  exit 0
fi

if [ ! -d "${ffms2_source_dir}/.git" ]; then
  if ! git clone --branch "${ffms2_ref}" --depth 1 https://github.com/FFMS/ffms2.git "${ffms2_source_dir}"; then
    echo "Failed to resolve FFMS2 ref '${ffms2_ref}'. Check .github/workflows/windows-msys2.yml and ensure the ref is quoted as a string."
    exit 1
  fi
else
  git -C "${ffms2_source_dir}" fetch --tags --force origin
  if ! git -C "${ffms2_source_dir}" checkout --force "${ffms2_ref}"; then
    echo "Failed to resolve FFMS2 ref '${ffms2_ref}'. Check .github/workflows/windows-msys2.yml and ensure the ref is quoted as a string."
    exit 1
  fi
  git -C "${ffms2_source_dir}" clean -fdx
fi

rm -rf "${ffms2_build_dir}" "${ffms2_prefix}"
mkdir -p "${ffms2_build_dir}" "${ffms2_prefix}"

export PATH="${ffmpeg_prefix}/bin:/ucrt64/bin:${PATH}"
export PKG_CONFIG_PATH="${ffmpeg_pcdir}:/ucrt64/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

pushd "${ffms2_source_dir}" >/dev/null
./autogen.sh
popd >/dev/null

pushd "${ffms2_build_dir}" >/dev/null
"${ffms2_source_dir}/configure" \
  --prefix="${ffms2_prefix}" \
  --enable-shared \
  --disable-static \
  --disable-avisynth
make -j"$(nproc)"
make install
popd >/dev/null

printf '%s\n' "${ffms2_build_id}" > "${ffms2_stamp_file}"

echo "FFMS2 ${ffms2_ref} installed to ${ffms2_prefix}"
