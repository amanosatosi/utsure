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
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${third_party_root}/ffmpeg/prefix}"
ffmpeg_pcdir="${ffmpeg_prefix}/lib/pkgconfig"
ffms2_prefix="${UTSURE_FFMS2_PREFIX:-${third_party_root}/ffms2/prefix}"
ffms2_pcdir="${ffms2_prefix}/lib/pkgconfig"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"
fontcollector_tool_dir="${UTSURE_FONTCOLLECTOR_TOOL_DIR:-${third_party_root}/fontcollector/prefix/tools/fontcollector}"
package_fontcollector="${UTSURE_PACKAGE_FONTCOLLECTOR:-ON}"
msys2_prefix="${UTSURE_MSYS2_PREFIX:-/ucrt64}"
msys2_bin_dir="${msys2_prefix}/bin"
qt_plugin_root="${msys2_prefix}/share/qt6/plugins"
qt_iconengines_dir="${qt_plugin_root}/iconengines"
qt_imageformats_dir="${qt_plugin_root}/imageformats"
app_executable="${build_dir}/src/app/utsure.exe"
qt_runtime_manifest="${bundle_dir}/qt-runtime-files.txt"
non_qt_runtime_manifest="${bundle_dir}/non-qt-runtime-dependencies.txt"
bundle_manifest="${bundle_dir}/bundle-file-manifest.txt"

export PATH="${ffmpeg_prefix}/bin:${ffms2_prefix}/bin:${libassmod_prefix}/bin:${msys2_bin_dir}:${PATH}"
export PKG_CONFIG_PATH="${ffmpeg_pcdir}:${ffms2_pcdir}:${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

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

# Read actual PE imports, including those of libraries copied by windeployqt.
# Unlike ldd, this is independent of the build machine's working PATH.
list_imported_dll_names() {
  local binary_path="$1"
  objdump -p "${binary_path}" |
    sed -nE 's/^[[:space:]]*DLL Name:[[:space:]]*([^[:space:]]+).*$/\1/p' |
    LC_ALL=C sort -fu
}

# Never copy Windows system DLLs or virtual API-set forwarders into the zip.
windows_system_dir="$(cygpath -u "${SystemRoot:-${WINDIR:-C:\\Windows}}")/System32"

resolve_runtime_dll() {
  local dependency_name="$1"
  local importing_dir="$2"
  local directory=""
  local candidate=""
  for directory in \
    "${importing_dir}" \
    "${bundle_dir}" \
    "${ffmpeg_prefix}/bin" \
    "${ffms2_prefix}/bin" \
    "${libassmod_prefix}/bin" \
    "${msys2_bin_dir}"; do
    candidate="${directory}/${dependency_name}"
    if [[ -f "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

is_windows_runtime_dll() {
  local dependency_name="${1,,}"
  case "${dependency_name}" in
    api-ms-win-*.dll|ext-ms-win-*.dll)
      return 0
      ;;
  esac
  [[ -f "${windows_system_dir}/${dependency_name}" ]]
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

verify_mangetsu_dll() {
  if [[ "${UTSURE_WINDOWS_MANGETSU_DLL:-0}" != "1" ]]; then
    return 0
  fi
  if [[ ! -f "${bundle_dir}/mangetsu.dll" ]]; then
    echo "Portable bundle must contain mangetsu.dll, not a version-named libass DLL." >&2
    exit 1
  fi
  if find "${bundle_dir}" -maxdepth 1 -type f -iname "libass*.dll" | grep -q .; then
    echo "Portable bundle contains a legacy libass*.dll alongside mangetsu.dll." >&2
    exit 1
  fi
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

strip_portable_debug_symbols() {
  if [[ "${UTSURE_STRIP_PORTABLE_DEBUG:-ON}" != "ON" ]]; then
    return 0
  fi

  if ! command -v strip >/dev/null 2>&1; then
    echo "Portable packaging expected strip to be available for RelWithDebInfo bundle slimming."
    exit 1
  fi

  while IFS= read -r binary_path; do
    strip --strip-debug "${binary_path}" || {
      echo "Failed to strip debug symbols from '${binary_path}'."
      exit 1
    }
  done < <(find "${bundle_dir}" -maxdepth 1 -type f \( -name '*.exe' -o -name '*.dll' \) | LC_ALL=C sort)
}

copy_fontcollector_tool() {
  if [[ "${package_fontcollector}" != "ON" ]]; then
    return 0
  fi

  if [ ! -f "${fontcollector_tool_dir}/fontcollector.exe" ]; then
    echo "Portable packaging requires FontCollector at '${fontcollector_tool_dir}/fontcollector.exe'."
    echo "Run scripts/ci/windows-build-fontcollector.ps1 before packaging, or set UTSURE_PACKAGE_FONTCOLLECTOR=OFF for a non-release diagnostic bundle."
    exit 1
  fi

  mkdir -p "${bundle_dir}/tools/fontcollector"
  cp -R "${fontcollector_tool_dir}/." "${bundle_dir}/tools/fontcollector/"

  if [ ! -f "${bundle_dir}/tools/fontcollector/fontcollector.exe" ]; then
    echo "Portable bundle is missing tools/fontcollector/fontcollector.exe after copy."
    exit 1
  fi
}

copy_ffmpeg_cli_tools() {
  local ffmpeg_bin_dir="${ffmpeg_prefix}/bin"
  local tool_name=""
  for tool_name in ffmpeg.exe ffprobe.exe; do
    if [ ! -f "${ffmpeg_bin_dir}/${tool_name}" ]; then
      echo "Portable packaging requires ${tool_name} at '${ffmpeg_bin_dir}/${tool_name}'."
      exit 1
    fi

    cp "${ffmpeg_bin_dir}/${tool_name}" "${bundle_dir}/${tool_name}"
  done
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
mkdir -p "${bundle_dir}/fonts" "${bundle_dir}/licenses"
cp "${project_root}/src/app/resources/fonts/Pyidaungsu-2.5.3_Regular.ttf" "${bundle_dir}/fonts/"
cp "${project_root}/src/app/resources/fonts/Pyidaungsu-2.5.3_Bold.ttf" "${bundle_dir}/fonts/"
cp "${project_root}/third_party/licenses/Pyidaungsu-OFL-1.1.txt" "${bundle_dir}/licenses/"

"${windeployqt}" --no-translations "${bundle_dir}/utsure.exe"

if [ -f "${qt_iconengines_dir}/qsvgicon.dll" ]; then
  mkdir -p "${bundle_dir}/iconengines"
  cp "${qt_iconengines_dir}/qsvgicon.dll" "${bundle_dir}/iconengines/"
fi

if [ -f "${qt_imageformats_dir}/qsvg.dll" ]; then
  mkdir -p "${bundle_dir}/imageformats"
  cp "${qt_imageformats_dir}/qsvg.dll" "${bundle_dir}/imageformats/"
fi

if [ -f "${qt_plugin_root}/platforms/qoffscreen.dll" ]; then
  mkdir -p "${bundle_dir}/platforms"
  cp "${qt_plugin_root}/platforms/qoffscreen.dll" "${bundle_dir}/platforms/"
fi

if [ -f "${qt_iconengines_dir}/qsvgicon.dll" ] && [ ! -f "${bundle_dir}/iconengines/qsvgicon.dll" ]; then
  echo "Portable bundle is missing Qt's SVG icon engine plugin (iconengines/qsvgicon.dll)."
  exit 1
fi

write_qt_runtime_manifest

copy_fontcollector_tool
copy_ffmpeg_cli_tools

if ! command -v objdump >/dev/null 2>&1; then
  echo "Portable packaging requires objdump from MSYS2 binutils." >&2
  exit 1
fi

declare -A scanned_binaries=()
declare -A copied_non_qt_dependencies=()
declare -a scan_queue=()

# Keep known FFmpeg 9 / QtMultimedia runtimes even when a library loads them
# dynamically rather than through a regular PE import.
required_msys2_ffmpeg_dlls=(
  libgme.dll
  libbluray-4.dll
  libgnutls-30.dll
  libmodplug-1.dll
  libaom.dll
  libcairo-2.dll
  libdav1d-7.dll
  libgobject-2.0-0.dll
)

for dependency_name in "${required_msys2_ffmpeg_dlls[@]}"; do
  dependency_path="${msys2_bin_dir}/${dependency_name}"
  if [[ ! -f "${dependency_path}" ]]; then
    echo "Missing required MSYS2 runtime DLL: ${dependency_path}" >&2
    exit 1
  fi
  cp "${dependency_path}" "${bundle_dir}/${dependency_name}"
  copied_non_qt_dependencies["${dependency_name,,}"]=1
done

# Examine each EXE and DLL as it will be distributed. This includes the Qt
# platform/image/icon plugins, FFmpeg CLI tools and nested FontCollector helper.
while IFS= read -r -d '' bundled_binary; do
  scan_queue+=("${bundled_binary}")
done < <(find "${bundle_dir}" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)

while [[ ${#scan_queue[@]} -gt 0 ]]; do
  current_binary="${scan_queue[0]}"
  scan_queue=("${scan_queue[@]:1}")

  if [[ -n "${scanned_binaries["${current_binary}"]:-}" ]]; then
    continue
  fi
  scanned_binaries["${current_binary}"]=1

  if ! imports="$(list_imported_dll_names "${current_binary}")"; then
    echo "Failed to inspect PE imports for '${current_binary}'." >&2
    exit 1
  fi

  while IFS= read -r dependency_name; do
    [[ -z "${dependency_name}" ]] && continue

    if dependency_path="$(resolve_runtime_dll "${dependency_name}" "$(dirname "${current_binary}")")"; then
      dependency_filename="$(basename "${dependency_path}")"
      if [[ "${dependency_path}" != "${bundle_dir}/"* ]]; then
        if [[ ! -f "${bundle_dir}/${dependency_filename}" ]]; then
          cp "${dependency_path}" "${bundle_dir}/${dependency_filename}"
        fi
        dependency_path="${bundle_dir}/${dependency_filename}"
      fi

      if [[ ! "${dependency_filename}" =~ ^(Qt6|qt).*\.dll$ ]]; then
        copied_non_qt_dependencies["${dependency_filename,,}"]=1
      fi
      if [[ -z "${scanned_binaries["${dependency_path}"]:-}" ]]; then
        scan_queue+=("${dependency_path}")
      fi
    elif ! is_windows_runtime_dll "${dependency_name}"; then
      echo "Unresolved portable DLL import '${dependency_name}' in '${current_binary}'." >&2
      echo "Not present in the package, pinned dependency prefixes, MSYS2 or Windows System32." >&2
      exit 1
    fi
  done <<< "${imports}"
done

for dependency_name in "${required_msys2_ffmpeg_dlls[@]}"; do
  if [[ ! -f "${bundle_dir}/${dependency_name}" ]]; then
    echo "Portable bundle is missing required runtime '${dependency_name}'." >&2
    exit 1
  fi
done

{
  for dependency_name in "${!copied_non_qt_dependencies[@]}"; do
    printf '%s\n' "${dependency_name}"
  done
} | LC_ALL=C sort > "${non_qt_runtime_manifest}"

strip_portable_debug_symbols

verify_mangetsu_dll
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
if (!(Test-Path \"\$portablePath/fonts/Pyidaungsu-2.5.3_Regular.ttf\")) { throw \"Portable bundle is missing bundled Pyidaungsu Regular font\" }; \
if (!(Test-Path \"\$portablePath/fonts/Pyidaungsu-2.5.3_Bold.ttf\")) { throw \"Portable bundle is missing bundled Pyidaungsu Bold font\" }; \
if (!(Test-Path \"\$portablePath/licenses/Pyidaungsu-OFL-1.1.txt\")) { throw \"Portable bundle is missing Pyidaungsu OFL license notice\" }; \
if ('${package_fontcollector}' -eq 'ON') { \
  \$fontCollectorPath=\"\$portablePath/tools/fontcollector/fontcollector.exe\"; \
  if (!(Test-Path \$fontCollectorPath)) { throw \"Portable bundle is missing tools/fontcollector/fontcollector.exe\" }; \
  & \$fontCollectorPath --help | Out-Null; \
}; \
& \"\$portablePath/utsure.exe\" --dump-window-structure --smoke-test; \
if (\$LASTEXITCODE -ne 0) { throw \"Portable utsure.exe smoke test failed with exit code \$LASTEXITCODE\" }"

echo "Portable bundle created at ${bundle_dir}"
echo "Portable zip created at ${bundle_zip}"
