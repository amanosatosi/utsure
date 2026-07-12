#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
libassmod_root="${third_party_root}/libassmod"
libassmod_repo_url="${UTSURE_LIBASSMOD_SOURCE_URL:-https://github.com/amanosatosi/libassmod.git}"
libassmod_ref="${UTSURE_LIBASSMOD_REF:-mangetsu}"
libassmod_source_dir="${UTSURE_LIBASSMOD_SOURCE_DIR:-${libassmod_root}/src}"
libassmod_build_dir="${UTSURE_LIBASSMOD_BUILD_DIR:-${libassmod_root}/build}"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${libassmod_root}/prefix}"
libassmod_aligned_allocation_debug="${UTSURE_LIBASSMOD_ALIGNED_ALLOCATION_DEBUG:-0}"

mkdir -p "${libassmod_root}"

echo "Using libassmod ref: ${libassmod_ref}"

case "${libassmod_aligned_allocation_debug,,}" in
  1|true|yes|on)
    libassmod_aligned_allocation_debug=true
    ;;
  0|false|no|off|"")
    libassmod_aligned_allocation_debug=false
    ;;
  *)
    echo "UTSURE_LIBASSMOD_ALIGNED_ALLOCATION_DEBUG must be a boolean value."
    exit 2
    ;;
esac

echo "libassmod aligned-allocation diagnostics: ${libassmod_aligned_allocation_debug}"

if [ ! -d "${libassmod_source_dir}/.git" ]; then
  if ! git clone "${libassmod_repo_url}" "${libassmod_source_dir}"; then
    echo "Failed to resolve libassmod ref '${libassmod_ref}'. Check .github/workflows/windows-msys2.yml and ensure the ref is reachable from ${libassmod_repo_url}."
    exit 1
  fi
else
  git -C "${libassmod_source_dir}" remote set-url origin "${libassmod_repo_url}"
  git -C "${libassmod_source_dir}" fetch --tags --force origin
  git -C "${libassmod_source_dir}" clean -fdx
fi

libassmod_checkout_ref="${libassmod_ref}"
if git -C "${libassmod_source_dir}" rev-parse --verify --quiet "origin/${libassmod_ref}^{commit}" >/dev/null; then
  libassmod_checkout_ref="origin/${libassmod_ref}"
fi

if ! git -C "${libassmod_source_dir}" checkout --force "${libassmod_checkout_ref}"; then
  echo "Failed to resolve libassmod ref '${libassmod_ref}'. Check .github/workflows/windows-msys2.yml and ensure the ref is reachable from ${libassmod_repo_url}."
  exit 1
fi

libassmod_meson_args=(
  --buildtype release
  --default-library shared
  --prefix "${libassmod_prefix}"
  -Dfontconfig=disabled
  -Dlibunibreak=disabled
)
if [[ "${libassmod_aligned_allocation_debug}" == "true" ]]; then
  libassmod_meson_args+=( -Daligned-allocation-debug=true )
fi

meson setup "${libassmod_build_dir}" "${libassmod_source_dir}" \
  "${libassmod_meson_args[@]}" \
  --wipe

meson compile -C "${libassmod_build_dir}"
meson install -C "${libassmod_build_dir}"

echo "libassmod installed to ${libassmod_prefix}"
