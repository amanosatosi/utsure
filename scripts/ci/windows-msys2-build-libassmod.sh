#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
libassmod_root="${third_party_root}/libassmod"
libassmod_ref="${UTSURE_LIBASSMOD_REF:-1.0}"
libassmod_source_dir="${UTSURE_LIBASSMOD_SOURCE_DIR:-${libassmod_root}/src}"
libassmod_build_dir="${UTSURE_LIBASSMOD_BUILD_DIR:-${libassmod_root}/build}"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${libassmod_root}/prefix}"

find_first_matching_file() {
  local search_root="$1"
  local file_name="$2"

  find "${search_root}" -type f -name "${file_name}" -print -quit 2>/dev/null || true
}

ensure_libass_pkgconfig_override() {
  local expected_pc_dir="${libassmod_prefix}/lib/pkgconfig"
  local expected_pc_file="${expected_pc_dir}/libass.pc"
  local discovered_pc_file
  local discovered_import_library
  local discovered_static_library
  local import_library_name
  local library_directory="${libassmod_prefix}/lib"

  mkdir -p "${expected_pc_dir}"

  discovered_pc_file="$(find_first_matching_file "${libassmod_prefix}" libass.pc)"
  if [[ -n "${discovered_pc_file}" ]]; then
    if [[ "${discovered_pc_file}" != "${expected_pc_file}" ]]; then
      cp "${discovered_pc_file}" "${expected_pc_file}"
    fi
    echo "libassmod pkg-config file available at ${expected_pc_file}"
    return
  fi

  discovered_import_library="$(find_first_matching_file "${libassmod_prefix}" libass.dll.a)"
  discovered_static_library="$(find_first_matching_file "${libassmod_prefix}" libass.a)"
  import_library_name="libass"

  if [[ -n "${discovered_import_library}" ]]; then
    library_directory="$(dirname "${discovered_import_library}")"
    import_library_name="$(basename "${discovered_import_library}")"
    import_library_name="${import_library_name#lib}"
    import_library_name="${import_library_name%.dll.a}"
  elif [[ -n "${discovered_static_library}" ]]; then
    library_directory="$(dirname "${discovered_static_library}")"
    import_library_name="$(basename "${discovered_static_library}")"
    import_library_name="${import_library_name#lib}"
    import_library_name="${import_library_name%.a}"
  fi

  cat > "${expected_pc_file}" <<EOF
prefix=${libassmod_prefix}
exec_prefix=\${prefix}
libdir=${library_directory}
includedir=\${prefix}/include

Name: libass
Description: libass-compatible libassmod build
Version: ${libassmod_ref}
Libs: -L\${libdir} -l${import_library_name}
Cflags: -I\${includedir}
EOF

  echo "Generated libassmod pkg-config shim at ${expected_pc_file}"
}

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
ensure_libass_pkgconfig_override

echo "libassmod installed to ${libassmod_prefix}"
