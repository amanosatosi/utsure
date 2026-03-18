#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
libassmod_root="${third_party_root}/libassmod"
libassmod_ref="${UTSURE_LIBASSMOD_REF:-1.0}"
libassmod_source_dir="${UTSURE_LIBASSMOD_SOURCE_DIR:-${libassmod_root}/src}"
libassmod_build_dir="${UTSURE_LIBASSMOD_BUILD_DIR:-${libassmod_root}/build}"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${libassmod_root}/prefix}"

mkdir -p "${libassmod_root}"

if [ ! -d "${libassmod_source_dir}/.git" ]; then
  git clone --branch "${libassmod_ref}" --depth 1 https://github.com/amanosatosi/libassmod.git "${libassmod_source_dir}"
else
  git -C "${libassmod_source_dir}" fetch --tags --force origin
  git -C "${libassmod_source_dir}" checkout --force "${libassmod_ref}"
fi

meson setup "${libassmod_build_dir}" "${libassmod_source_dir}" \
  --buildtype release \
  --default-library shared \
  --prefix "${libassmod_prefix}" \
  -Dfontconfig=disabled \
  -Dlibunibreak=disabled \
  --wipe

meson compile -C "${libassmod_build_dir}"
meson install -C "${libassmod_build_dir}"

echo "libassmod installed to ${libassmod_prefix}"
