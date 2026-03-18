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

echo "Using libassmod ref: ${libassmod_ref}"

if [ ! -d "${libassmod_source_dir}/.git" ]; then
  if ! git clone --branch "${libassmod_ref}" --depth 1 https://github.com/amanosatosi/libassmod.git "${libassmod_source_dir}"; then
    echo "Failed to resolve libassmod ref '${libassmod_ref}'. Check .github/workflows/windows-msys2.yml and ensure the ref is quoted as a string."
    exit 1
  fi
else
  git -C "${libassmod_source_dir}" fetch --tags --force origin
  if ! git -C "${libassmod_source_dir}" checkout --force "${libassmod_ref}"; then
    echo "Failed to resolve libassmod ref '${libassmod_ref}'. Check .github/workflows/windows-msys2.yml and ensure the ref is quoted as a string."
    exit 1
  fi
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
