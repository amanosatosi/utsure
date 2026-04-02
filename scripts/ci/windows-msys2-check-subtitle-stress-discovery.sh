#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${UTSURE_BUILD_DIR:-build}"
ctest_config="${UTSURE_CTEST_CONFIG:-${UTSURE_CMAKE_BUILD_TYPE:-}}"
results_dir="${project_root}/artifacts/subtitle-asan-stress"
full_discovery_log="${results_dir}/ctest-discovery.log"
matching_tests_file="${results_dir}/ctest-discovery-matching-tests.txt"
representative_test_name="utsure.core.subtitles.burn_in.stress.copied_serialized.h264"
source_cmake_file="${project_root}/tests/core/CMakeLists.txt"

mkdir -p "${results_dir}"
: > "${matching_tests_file}"

append_summary() {
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '%s\n' "$1" >> "${GITHUB_STEP_SUMMARY}"
  fi
}

format_command_for_log() {
  local formatted=""
  local argument
  for argument in "$@"; do
    if [[ -n "${formatted}" ]]; then
      formatted+=" "
    fi
    formatted+="$(printf '%q' "${argument}")"
  done

  printf '%s' "${formatted}"
}

append_file_as_code_block() {
  local log_file="$1"
  append_summary '```text'
  while IFS= read -r line; do
    append_summary "${line//\`/\'}"
  done < "${log_file}"
  append_summary '```'
}

extract_discovered_test_names() {
  local log_file="$1"
  sed -nE 's/^[[:space:]]*Test[[:space:]]*#?[0-9]+:[[:space:]]+(.+)$/\1/p' "${log_file}"
}

emit_github_annotation() {
  local level="$1"
  local title="$2"
  local message="$3"
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    printf '::%s title=%s::%s\n' "${level}" "${title}" "${message}"
  fi
}

build_dir_path="${build_dir}"
if [[ "${build_dir}" != /* && ! "${build_dir}" =~ ^[A-Za-z]:[\\/].* ]]; then
  build_dir_path="${project_root}/${build_dir}"
fi
ctest_manifest="${build_dir_path}/CTestTestfile.cmake"

source_registration="no"
if grep -Fq "NAME ${representative_test_name}" "${source_cmake_file}"; then
  source_registration="yes"
fi

discovery_command=(
  ctest
  -N
  --test-dir "${build_dir}"
)
if [[ -n "${ctest_config}" ]]; then
  discovery_command+=(-C "${ctest_config}")
fi

append_summary "## Subtitle ASan CTest Discovery"
append_summary ""
append_summary "- Build directory: \`${build_dir}\`"
append_summary "- CTest config: \`${ctest_config:-<default>}\`"
append_summary "- Representative test: \`${representative_test_name}\`"
append_summary "- Source registration in \`tests/core/CMakeLists.txt\`: \`${source_registration}\`"
append_summary "- CTest manifest path: \`${ctest_manifest}\`"
append_summary "- CTest manifest exists: \`$([[ -f "${ctest_manifest}" ]] && printf 'yes' || printf 'no')\`"
append_summary "- Discovery command: \`$(format_command_for_log "${discovery_command[@]}")\`"
append_summary ""

set +e
"${discovery_command[@]}" > "${full_discovery_log}" 2>&1
discovery_exit=$?
set -e

echo "Full CTest discovery output:"
cat "${full_discovery_log}"

mapfile -t discovered_test_names < <(extract_discovered_test_names "${full_discovery_log}")

for test_name in "${discovered_test_names[@]}"; do
  case "${test_name}" in
    *burn_in*|*stress*|*copied_serialized*|*h264*)
      if ! grep -Fxq "${test_name}" "${matching_tests_file}" 2>/dev/null; then
        printf '%s\n' "${test_name}" >> "${matching_tests_file}"
      fi
      ;;
  esac
done

representative_listed="no"
if printf '%s\n' "${discovered_test_names[@]}" | grep -Fxq "${representative_test_name}"; then
  representative_listed="yes"
fi

echo "Matching discovered test names:"
if [[ -s "${matching_tests_file}" ]]; then
  cat "${matching_tests_file}"
else
  echo "<none>"
fi

append_summary "- Discovery exit code: \`${discovery_exit}\`"
append_summary "- Total discovered test names: \`${#discovered_test_names[@]}\`"
append_summary "- Representative listed in full discovery: \`${representative_listed}\`"
append_summary "- Matching discovered test names:"
if [[ -s "${matching_tests_file}" ]]; then
  while IFS= read -r test_name; do
    append_summary "- \`${test_name}\`"
  done < "${matching_tests_file}"
else
  append_summary "- none"
fi
append_summary ""
append_summary "<details><summary>Full ctest -N Output</summary>"
append_summary ""
append_file_as_code_block "${full_discovery_log}"
append_summary "</details>"

if [[ "${discovery_exit}" -ne 0 ]]; then
  emit_github_annotation error "Subtitle stress discovery failed" "ctest -N failed for build_dir=${build_dir}; config=${ctest_config:-default}"
  exit 1
fi

if [[ "${representative_listed}" != "yes" ]]; then
  failure_reason="Representative stress test is missing from CTest discovery."
  if [[ "${source_registration}" == "yes" ]]; then
    failure_reason="${failure_reason} The test is still registered in tests/core/CMakeLists.txt, so this build tree is missing generated CTest metadata or testing is disabled in the configured build."
  fi
  emit_github_annotation error "Subtitle stress test missing" "${failure_reason}"
  printf '%s\n' "${failure_reason}" >&2
  exit 1
fi
