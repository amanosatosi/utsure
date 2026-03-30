#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${third_party_root}/ffmpeg/prefix}"
ffmpeg_pcdir="${ffmpeg_prefix}/lib/pkgconfig"
ffms2_root="${third_party_root}/ffms2"
ffms2_repo_url="${UTSURE_FFMS2_SOURCE_URL:-https://github.com/FFMS/ffms2.git}"
ffms2_ref="${UTSURE_FFMS2_REF:-25cef14386fcaaa58ee547065deee8f6e82c56a2}"
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
  if ! git clone "${ffms2_repo_url}" "${ffms2_source_dir}"; then
    echo "Failed to resolve FFMS2 ref '${ffms2_ref}'. Check .github/workflows/windows-msys2.yml and ensure the ref is quoted as a string."
    exit 1
  fi
else
  git -C "${ffms2_source_dir}" remote set-url origin "${ffms2_repo_url}"
  git -C "${ffms2_source_dir}" fetch --tags --force origin
  git -C "${ffms2_source_dir}" clean -fdx
fi

if ! git -C "${ffms2_source_dir}" checkout --force "${ffms2_ref}"; then
  echo "Failed to resolve FFMS2 ref '${ffms2_ref}'. Check .github/workflows/windows-msys2.yml and ensure the ref is reachable from ${ffms2_repo_url}."
  exit 1
fi

rm -rf "${ffms2_build_dir}" "${ffms2_prefix}"
mkdir -p "${ffms2_build_dir}" "${ffms2_prefix}"

export PATH="${ffmpeg_prefix}/bin:/ucrt64/bin:${PATH}"
export PKG_CONFIG_PATH="${ffmpeg_pcdir}:/ucrt64/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

pushd "${ffms2_source_dir}" >/dev/null
# The pinned upstream FFMS2 snapshot's autogen.sh always runs configure in the
# source tree.
# Re-run only the bootstrap steps here so the actual configure step stays in
# the separate build directory below.
# This snapshot also wires AC_ARG_ENABLE([avisynth], ...) incorrectly and
# ignores the provided option value, so patch configure.ac before autoreconf so
# --enable-avisynth=no actually disables the plugin.
sed -i 's/\[enable_avisynth=yes\],/[enable_avisynth=$enableval],/' configure.ac
# We only need the FFMS2 library, headers, and pkg-config metadata for preview
# integration. Skip the optional ffmsindex utility, which fails to link on the
# Windows CI image and is not used by this repository.
sed -i 's#^bin_PROGRAMS = src/index/ffmsindex$#bin_PROGRAMS =#' Makefile.am
mkdir -p src/config
echo "Running autoreconf..."
autoreconf -ivf
popd >/dev/null

pushd "${ffms2_build_dir}" >/dev/null
# configure.ac is patched above so the explicit "=no" spelling here cleanly
# disables Avisynth on the Windows CI image.
"${ffms2_source_dir}/configure" \
  --prefix="${ffms2_prefix}" \
  --enable-shared \
  --disable-static \
  --enable-avisynth=no
make -j"$(nproc)"
make install
popd >/dev/null

printf '%s\n' "${ffms2_build_id}" > "${ffms2_stamp_file}"

echo "FFMS2 ${ffms2_ref} installed to ${ffms2_prefix}"
