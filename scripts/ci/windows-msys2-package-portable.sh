#!/usr/bin/env bash

set -euo pipefail
shopt -s nullglob

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${UTSURE_BUILD_DIR:-${project_root}/build}"
artifact_root="${UTSURE_ARTIFACT_ROOT:-${project_root}/artifacts}"
bundle_name="${UTSURE_PORTABLE_BUNDLE_NAME:-encoder-windows-x64-portable}"
bundle_dir="${artifact_root}/${bundle_name}"
bundle_zip="${artifact_root}/${bundle_name}.zip"
validation_root="${artifact_root}/portable-validation"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"
msys2_prefix="${UTSURE_MSYS2_PREFIX:-/ucrt64}"
msys2_bin_dir="${msys2_prefix}/bin"
qt_plugin_root="${msys2_prefix}/share/qt6/plugins"
app_executable="${build_dir}/src/app/utsure.exe"
qt_runtime_manifest="${bundle_dir}/qt-runtime-files.txt"
non_qt_runtime_manifest="${bundle_dir}/non-qt-runtime-dependencies.txt"
bundle_manifest="${bundle_dir}/bundle-file-manifest.txt"

export PATH="${libassmod_prefix}/bin:${msys2_bin_dir}:${PATH}"
export PKG_CONFIG_PATH="${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

find_windeployqt() {
  local candidates=(
    "${msys2_bin_dir}/windeployqt6.exe"
    "${msys2_bin_dir}/windeployqt6"
    "${msys2_bin_dir}/windeployqt.exe"
    "${msys2_bin_dir}/windeployqt"
  )

  local candidate=""
  for candidate in "${candidates[@]}"; do
    if [ -x "${candidate}" ]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done

  local command_candidate=""
  for command_candidate in windeployqt6.exe windeployqt6 windeployqt.exe windeployqt; do
    if command -v "${command_candidate}" >/dev/null 2>&1; then
      command -v "${command_candidate}"
      return 0
    fi
  done

  echo "Failed to locate windeployqt in the MSYS2 Qt installation."
  return 1
}

list_dependency_paths() {
  local binary_path="$1"
  ldd "${binary_path}" 2>/dev/null | sed -nE \
    -e 's#.*=> (/[^ ]+) .*#\1#p' \
    -e 's#^[[:space:]]*(/[^ ]+) .*#\1#p' | sort -u
}

should_package_dependency() {
  local dependency_path="$1"

  if [ ! -f "${dependency_path}" ]; then
    return 1
  fi

  case "${dependency_path}" in
    "${msys2_bin_dir}"/*|\
    "${libassmod_prefix}/bin/"*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

is_qt_runtime() {
  local dependency_name="$1"

  case "${dependency_name}" in
    Qt6*.dll|qt*.dll)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

write_qt_runtime_manifest() {
  (
    cd "${bundle_dir}"
    find . -type f \
      ! -name 'utsure.exe' \
      ! -name 'LICENSE' \
      ! -name 'qt-runtime-files.txt' \
      ! -name 'non-qt-runtime-dependencies.txt' \
      ! -name 'bundle-file-manifest.txt' \
      | LC_ALL=C sort
  ) > "${qt_runtime_manifest}"
}

write_bundle_manifest() {
  {
    (
      cd "${bundle_dir}"
      find . -type f ! -name 'bundle-file-manifest.txt' | LC_ALL=C sort
    )
    printf './bundle-file-manifest.txt\n'
  } | LC_ALL=C sort > "${bundle_manifest}"
}

windeployqt="$(find_windeployqt)"

if [ ! -f "${app_executable}" ]; then
  echo "Portable packaging expected '${app_executable}' to exist."
  exit 1
fi

rm -rf "${bundle_dir}" "${validation_root}"
rm -f "${bundle_zip}"
mkdir -p "${bundle_dir}" "${artifact_root}"

cp "${app_executable}" "${bundle_dir}/"
cp "${project_root}/LICENSE" "${bundle_dir}/"

"${windeployqt}" --no-translations "${bundle_dir}/utsure.exe"

if [ -f "${qt_plugin_root}/platforms/qoffscreen.dll" ]; then
  mkdir -p "${bundle_dir}/platforms"
  cp "${qt_plugin_root}/platforms/qoffscreen.dll" "${bundle_dir}/platforms/"
fi

write_qt_runtime_manifest

declare -A scanned_binaries=()
declare -A copied_non_qt_dependencies=()
declare -a scan_queue=("${app_executable}")

while [ "${#scan_queue[@]}" -gt 0 ]; do
  current_binary="${scan_queue[0]}"
  scan_queue=("${scan_queue[@]:1}")

  if [ -n "${scanned_binaries["${current_binary}"]:-}" ]; then
    continue
  fi
  scanned_binaries["${current_binary}"]=1

  while IFS= read -r dependency_path; do
    if ! should_package_dependency "${dependency_path}"; then
      continue
    fi

    dependency_name="$(basename "${dependency_path}")"
    if is_qt_runtime "${dependency_name}"; then
      continue
    fi

    if [ ! -f "${bundle_dir}/${dependency_name}" ]; then
      cp "${dependency_path}" "${bundle_dir}/${dependency_name}"
    fi

    copied_non_qt_dependencies["${dependency_name}"]=1

    if [ -z "${scanned_binaries["${dependency_path}"]:-}" ]; then
      scan_queue+=("${dependency_path}")
    fi
  done < <(list_dependency_paths "${current_binary}")
done

{
  for dependency_name in "${!copied_non_qt_dependencies[@]}"; do
    printf '%s\n' "${dependency_name}"
  done
} | LC_ALL=C sort > "${non_qt_runtime_manifest}"

write_bundle_manifest

bundle_dir_windows="$(cygpath -m "${bundle_dir}")"
bundle_zip_windows="$(cygpath -m "${bundle_zip}")"
validation_root_windows="$(cygpath -m "${validation_root}")"
validation_bundle_windows="${validation_root_windows}/${bundle_name}"

powershell.exe -NoProfile -Command "\
\$ErrorActionPreference='Stop'; \
if (Test-Path '${bundle_zip_windows}') { Remove-Item -Force '${bundle_zip_windows}' }; \
Compress-Archive -Path '${bundle_dir_windows}' -DestinationPath '${bundle_zip_windows}'"

powershell.exe -NoProfile -Command "\
\$ErrorActionPreference='Stop'; \
if (Test-Path '${validation_root_windows}') { Remove-Item -Recurse -Force '${validation_root_windows}' }; \
Expand-Archive -Path '${bundle_zip_windows}' -DestinationPath '${validation_root_windows}'; \
\$portablePath='${validation_bundle_windows}'; \
\$env:PATH=\"\$portablePath;\$env:SystemRoot\\System32;\$env:SystemRoot\"; \
\$env:QT_QPA_PLATFORM='offscreen'; \
\$env:QT_PLUGIN_PATH=''; \
\$env:QML2_IMPORT_PATH=''; \
& \"\$portablePath/utsure.exe\" --dump-window-structure --smoke-test"

echo "Portable bundle created at ${bundle_dir}"
echo "Portable zip created at ${bundle_zip}"
