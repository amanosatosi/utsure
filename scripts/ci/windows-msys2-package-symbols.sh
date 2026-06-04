#!/usr/bin/env bash

set -euo pipefail
shopt -s nullglob

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${UTSURE_BUILD_DIR:-${project_root}/build}"
artifact_root="${UTSURE_ARTIFACT_ROOT:-${project_root}/artifacts}"
bundle_name="${UTSURE_PORTABLE_BUNDLE_NAME:-encoder-windows-x64-portable}"
bundle_dir="${artifact_root}/${bundle_name}"
commit_sha="${GITHUB_SHA:-$(git -C "${project_root}" rev-parse HEAD 2>/dev/null || printf 'unknown')}"
short_commit="${commit_sha:0:12}"
symbols_name="utsure-windows-symbols-${short_commit}"
symbols_dir="${artifact_root}/${symbols_name}"
symbols_zip="${artifact_root}/${symbols_name}.zip"
app_executable="${build_dir}/src/app/utsure.exe"

rm -rf "${symbols_dir}" "${symbols_zip}"
mkdir -p "${symbols_dir}/build" "${symbols_dir}/bundle"

if [[ ! -f "${app_executable}" ]]; then
  echo "Expected app executable was not found at ${app_executable}."
  exit 1
fi

cp "${app_executable}" "${symbols_dir}/build/"

if [[ -d "${bundle_dir}" ]]; then
  find "${bundle_dir}" -maxdepth 1 -type f \( -name '*.exe' -o -name '*.dll' \) -print0 |
    while IFS= read -r -d '' binary_path; do
      cp "${binary_path}" "${symbols_dir}/bundle/"
    done
fi

{
  printf 'commit=%s\n' "${commit_sha}"
  printf 'build_type=%s\n' "${UTSURE_CMAKE_BUILD_TYPE:-unknown}"
  printf 'toolchain=%s\n' "${UTSURE_TOOLCHAIN_ID_PREFIX:-unknown}"
  printf 'bundle_name=%s\n' "${bundle_name}"
  printf 'generated_utc=%s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
} > "${symbols_dir}/build-metadata.txt"

(
  cd "${symbols_dir}"
  find . -type f | LC_ALL=C sort
) > "${symbols_dir}/symbols-file-manifest.txt"

symbols_dir_windows="$(cygpath -m "${symbols_dir}")"
symbols_zip_windows="$(cygpath -m "${symbols_zip}")"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "\
if (Test-Path '${symbols_zip_windows}') { Remove-Item -Force '${symbols_zip_windows}' }; \
Compress-Archive -Path '${symbols_dir_windows}' -DestinationPath '${symbols_zip_windows}'"

echo "Created Windows symbols artifact at ${symbols_zip}."
