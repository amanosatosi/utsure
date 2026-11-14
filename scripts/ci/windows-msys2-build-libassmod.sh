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

echo "Using live libassmod branch: ${libassmod_ref}"
if [[ -n "${UTSURE_LIBASSMOD_COMMIT:-}" ]]; then
  echo "UTSURE_LIBASSMOD_COMMIT is unsupported: Utsure builds the current Mangetsu branch HEAD." >&2
  exit 2
fi

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

if ! git check-ref-format --branch "${libassmod_ref}" >/dev/null 2>&1; then
  echo "Invalid Mangetsu branch name: ${libassmod_ref}" >&2
  exit 2
fi

if [[ ! -d "${libassmod_source_dir}/.git" ]]; then
  git clone --no-checkout "${libassmod_repo_url}" "${libassmod_source_dir}"
else
  git -C "${libassmod_source_dir}" remote set-url origin "${libassmod_repo_url}"
  git -C "${libassmod_source_dir}" clean -ffdx
fi

# Always fetch the actual remote branch tip (including force-pushes), not a
# cached checkout, a local tag, or a fixed historical commit.
if ! git -C "${libassmod_source_dir}" fetch --no-tags --force origin \
  "+refs/heads/${libassmod_ref}:refs/remotes/origin/${libassmod_ref}"; then
  echo "Cannot fetch latest Mangetsu branch '${libassmod_ref}' from ${libassmod_repo_url}." >&2
  exit 1
fi
git -C "${libassmod_source_dir}" checkout --force --detach "refs/remotes/origin/${libassmod_ref}"
resolved_commit="$(git -C "${libassmod_source_dir}" rev-parse HEAD)"
remote_commit="$(git -C "${libassmod_source_dir}" rev-parse "refs/remotes/origin/${libassmod_ref}")"
if [[ "${resolved_commit}" != "${remote_commit}" ]]; then
  echo "Mangetsu checkout does not match the fetched branch tip." >&2
  exit 1
fi
printf '%s\n' "${resolved_commit}" > "${libassmod_root}/resolved-commit.txt"
echo "Building Mangetsu ${libassmod_ref} at ${resolved_commit}"

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
