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
libassmod_buildtype="${UTSURE_LIBASSMOD_BUILDTYPE:-debugoptimized}"
libassmod_patch_dir="${UTSURE_LIBASSMOD_PATCH_DIR:-${project_root}/patches/libassmod}"
libassmod_apply_patches="${UTSURE_LIBASSMOD_APPLY_PATCHES:-ON}"

mkdir -p "${libassmod_root}"

echo "Using libassmod ref: ${libassmod_ref}"
echo "Using libassmod Meson buildtype: ${libassmod_buildtype}"

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

case "${libassmod_apply_patches}" in
  0|OFF|off|false|FALSE|no|NO)
    echo "Skipping libassmod patches because UTSURE_LIBASSMOD_APPLY_PATCHES=${libassmod_apply_patches}."
    ;;
  *)
    if [[ -d "${libassmod_patch_dir}" ]]; then
      shopt -s nullglob
      libassmod_patches=("${libassmod_patch_dir}"/*.patch)
      shopt -u nullglob
      for patch_file in "${libassmod_patches[@]}"; do
        echo "Applying libassmod patch: ${patch_file}"
        git -C "${libassmod_source_dir}" apply --whitespace=nowarn "${patch_file}"
      done
    fi
    ;;
esac

libassmod_cflags=()
append_mangetsu_define_if_enabled() {
  local name="$1"
  local value="${!name:-}"
  case "${value}" in
    1|ON|on|true|TRUE|yes|YES)
      libassmod_cflags+=("-D${name}=1")
      ;;
  esac
}

append_mangetsu_define_if_enabled MANGETSU_DISABLE_MULTI_BORDER_CACHE
append_mangetsu_define_if_enabled MANGETSU_DISABLE_CUSTOM_BORDER_LAYERS
append_mangetsu_define_if_enabled MANGETSU_DISABLE_DRAWING_CACHE_STRINGVIEWS

if (( ${#libassmod_cflags[@]} )); then
  echo "Using libassmod diagnostic CFLAGS: ${libassmod_cflags[*]}"
  export CFLAGS="${CFLAGS:-} ${libassmod_cflags[*]}"
fi

meson setup "${libassmod_build_dir}" "${libassmod_source_dir}" \
  --buildtype "${libassmod_buildtype}" \
  --default-library shared \
  --prefix "${libassmod_prefix}" \
  -Dfontconfig=disabled \
  -Dlibunibreak=disabled \
  --wipe

meson compile -C "${libassmod_build_dir}"
meson install -C "${libassmod_build_dir}"

echo "libassmod installed to ${libassmod_prefix}"
