#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${UTSURE_BUILD_DIR:-build}"
repeat_count="${UTSURE_SUBTITLE_STRESS_REPEAT:-8}"
diagnostics_mode="${UTSURE_SUBTITLE_DIAGNOSTICS:-off}"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${third_party_root}/ffmpeg/prefix}"
ffmpeg_pcdir="${ffmpeg_prefix}/lib/pkgconfig"
ffms2_prefix="${UTSURE_FFMS2_PREFIX:-${third_party_root}/ffms2/prefix}"
ffms2_pcdir="${ffms2_prefix}/lib/pkgconfig"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"
msys2_prefix="${UTSURE_MSYS2_PREFIX:-${MINGW_PREFIX:-/ucrt64}}"
results_dir="${project_root}/artifacts/subtitle-asan-stress"
summary_file="${results_dir}/summary.md"

cd "${project_root}"

export PATH="${ffmpeg_prefix}/bin:${ffms2_prefix}/bin:${libassmod_prefix}/bin:${msys2_prefix}/bin:${PATH}"
export PKG_CONFIG_PATH="${ffmpeg_pcdir}:${ffms2_pcdir}:${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

mkdir -p "${results_dir}"
: > "${summary_file}"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  : > "${GITHUB_STEP_SUMMARY}"
fi

append_summary() {
  printf '%s\n' "$1" >> "${summary_file}"
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '%s\n' "$1" >> "${GITHUB_STEP_SUMMARY}"
  fi
}

emit_github_annotation() {
  local level="$1"
  local title="$2"
  local message="$3"
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    printf '::%s title=%s::%s\n' "${level}" "${title}" "${message}"
  fi
}

sanitize_markdown_inline() {
  local value="$1"
  value="${value//\`/\'}"
  printf '%s' "${value}"
}

escape_ctest_regex() {
  local value="$1"
  local escaped=""
  local character
  local index
  for ((index = 0; index < ${#value}; ++index)); do
    character="${value:index:1}"
    case "${character}" in
      [][(){}.^$*+?|\\-])
        escaped+="\\${character}"
        ;;
      *)
        escaped+="${character}"
        ;;
    esac
  done

  printf '%s' "${escaped}"
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

contains_fixed_string() {
  local needle="$1"
  local file="$2"
  grep -Fq "${needle}" "${file}" 2>/dev/null
}

extract_stress_value() {
  local key="$1"
  local log_file="$2"
  grep -E "^stress\\.${key}=" "${log_file}" | tail -n 1 | sed -E "s/^stress\\.${key}=//" || true
}

extract_iteration_reached() {
  local log_file="$1"
  local iteration

  iteration="$(extract_stress_value "iteration" "${log_file}")"
  if [[ -n "${iteration}" ]]; then
    printf '%s' "${iteration}"
    return
  fi

  iteration="$(extract_stress_value "iteration_begin" "${log_file}")"
  if [[ -n "${iteration}" ]]; then
    printf '%s' "${iteration}"
    return
  fi

  printf '0'
}

extract_startup_stage_reached() {
  local log_file="$1"
  local stage

  stage="$(extract_stress_value "startup_stage" "${log_file}")"
  if [[ -n "${stage}" ]]; then
    printf '%s' "${stage}"
    return
  fi

  printf 'none'
}

test_launched() {
  local log_file="$1"
  if contains_fixed_string "stress.startup_marker=entered" "${log_file}"; then
    printf 'yes'
    return
  fi

  printf 'no'
}

run_matched_tests() {
  local log_file="$1"
  if grep -Eq 'No tests were found!!!' "${log_file}"; then
    printf 'no'
    return
  fi

  if grep -Eq '^[[:space:]]*Start[[:space:]]+[0-9]+:' "${log_file}"; then
    printf 'yes'
    return
  fi

  printf 'no'
}

check_test_exists() {
  local test_name="$1"
  local log_file="$2"
  local exact_regex="$3"
  local exit_code

  set +e
  ctest --test-dir "${build_dir}" -N "${ctest_config_args[@]}" --no-tests=error -R "${exact_regex}" \
    > "${log_file}" 2>&1
  exit_code=$?
  set -e

  if [[ "${exit_code}" -eq 0 ]]; then
    printf 'yes'
    return
  fi

  printf 'no'
}

extract_asan_excerpt() {
  local log_file="$1"
  grep -Eim 3 \
    'ERROR: AddressSanitizer|AddressSanitizer|heap-use-after-free|heap-buffer-overflow|stack-use-after-return|double-free|SEGV on unknown address|use-after-poison' \
    "${log_file}" || true
}

extract_failure_reason() {
  local log_file="$1"
  local reason

  reason="$(extract_stress_value "failure_reason" "${log_file}")"
  if [[ -n "${reason}" ]]; then
    printf '%s' "${reason}"
    return
  fi

  grep -Eim 1 \
    'No tests were found!!!|Process not started|Could not find executable|The system cannot find the file specified|The system cannot find the path specified|failed unexpectedly|unexpectedly|unexpected bytes|Unexpected .*subtitle|Unexpected .*frame count|AddressSanitizer' \
    "${log_file}" || true
}

extract_failure_stage() {
  local log_file="$1"
  local stage

  stage="$(extract_stress_value "failure_stage" "${log_file}")"
  if [[ -n "${stage}" ]]; then
    printf '%s' "${stage}"
    return
  fi

  if grep -Eq 'No tests were found!!!' "${log_file}"; then
    printf 'test_discovery'
    return
  fi

  if grep -Eq 'Process not started|Could not find executable|The system cannot find the file specified|The system cannot find the path specified' "${log_file}"; then
    printf 'dependency_resolve'
    return
  fi

  local startup_stage
  startup_stage="$(extract_stress_value "startup_stage" "${log_file}")"
  if [[ -n "${startup_stage}" ]]; then
    printf '%s' "${startup_stage}"
    return
  fi

  printf 'unstructured'
}

truncate_annotation_text() {
  local value="$1"
  local max_length="${2:-220}"
  if (( ${#value} > max_length )); then
    printf '%s...' "${value:0:max_length}"
    return
  fi

  printf '%s' "${value}"
}

modes=(
  "copied serialized copied_serialized utsure.core.subtitles.burn_in.stress.copied_serialized.h264"
  "copied worker_local copied_worker utsure.core.subtitles.burn_in.stress.copied_worker.h264"
  "direct serialized direct_serialized utsure.core.subtitles.burn_in.stress.direct_serialized.h264"
  "direct worker_local direct_worker utsure.core.subtitles.burn_in.stress.direct_worker.h264"
)
representative_entry="${modes[0]}"

failed_modes=()
failed_bitmap_modes=()
failed_composition_modes=()
failed_stages=()
overall_status=0

append_summary "## Subtitle ASan Isolation Matrix"
append_summary ""
append_summary "- Repeat count: \`${repeat_count}\`"
append_summary "- Diagnostics mode: \`${diagnostics_mode}\`"
append_summary "- Build directory: \`${build_dir}\`"
append_summary "- FFmpeg prefix: \`${ffmpeg_prefix}\`"
append_summary "- FFMS2 prefix: \`${ffms2_prefix}\`"
append_summary "- libassmod prefix: \`${libassmod_prefix}\`"
append_summary ""
append_summary "### Representative Startup Check"
append_summary ""

read -r representative_bitmap_mode representative_composition_mode representative_mode_key representative_test_name <<< "${representative_entry}"
representative_discovery_log="${results_dir}/${representative_mode_key}.discovery.log"
representative_startup_log="${results_dir}/${representative_mode_key}.startup.log"
representative_exact_regex="^$(escape_ctest_regex "${representative_test_name}")$"
ctest_config="${UTSURE_CTEST_CONFIG:-${UTSURE_CMAKE_BUILD_TYPE:-}}"
ctest_config_args=()
if [[ -n "${ctest_config}" ]]; then
  ctest_config_args=(-C "${ctest_config}")
fi
representative_run_command=(
  ctest
  --test-dir "${build_dir}"
  "${ctest_config_args[@]}"
  --output-on-failure
  -V
  --no-tests=error
  -R "${representative_exact_regex}"
)

representative_test_exists="$(check_test_exists "${representative_test_name}" "${representative_discovery_log}" "${representative_exact_regex}")"
append_summary "- Discovery method: \`ctest -N --no-tests=error\`"
append_summary "- Exact test name: \`${representative_test_name}\`"
append_summary "- Escaped exact regex: \`${representative_exact_regex}\`"
append_summary "- CTest config: \`${ctest_config:-<default>}\`"
append_summary "- Test exists: \`${representative_test_exists}\`"

if [[ "${representative_test_exists}" != "yes" ]]; then
  representative_reason="$(extract_failure_reason "${representative_discovery_log}")"
  if [[ -z "${representative_reason}" ]]; then
    representative_reason="CTest did not find the representative stress test."
  fi
  emit_github_annotation \
    error \
    "Subtitle stress discovery failed" \
    "mode=${representative_mode_key}; stage=test_discovery; reason=$(truncate_annotation_text "${representative_reason}")"
  append_summary "- Representative startup failed before execution at \`test_discovery\`: \`$(sanitize_markdown_inline "${representative_reason}")\`"
  exit 1
fi

append_summary "- Representative startup discovery passed."
append_summary ""

append_summary "| Bitmap mode | Composition mode | Test exists | Launched | Result | Time (ms) | Startup stage | Iteration | Failure stage | ASan |"
append_summary "| --- | --- | --- | --- | --- | ---: | --- | ---: | --- | --- |"

echo "Running representative startup check for ${representative_mode_key}."
echo "Representative exact test name: ${representative_test_name}"
echo "Representative escaped regex: ${representative_exact_regex}"
echo "Representative ctest command: $(format_command_for_log "${representative_run_command[@]}")"
append_summary "- Representative ctest command: \`$(sanitize_markdown_inline "$(format_command_for_log "${representative_run_command[@]}")")\`"
set +e
env \
  UTSURE_SUBTITLE_STRESS_REPEAT=1 \
  UTSURE_SUBTITLE_DIAGNOSTICS="${diagnostics_mode}" \
  UTSURE_FFMPEG_PREFIX="${ffmpeg_prefix}" \
  UTSURE_FFMS2_PREFIX="${ffms2_prefix}" \
  UTSURE_LIBASSMOD_PREFIX="${libassmod_prefix}" \
  "${representative_run_command[@]}" \
  2>&1 | tee "${representative_startup_log}"
representative_startup_exit=${PIPESTATUS[0]}
set -e

representative_startup_stage="$(extract_failure_stage "${representative_startup_log}")"
representative_startup_reason="$(extract_failure_reason "${representative_startup_log}")"
representative_startup_stage_reached="$(extract_startup_stage_reached "${representative_startup_log}")"
representative_test_launched="$(test_launched "${representative_startup_log}")"
representative_run_matched="$(run_matched_tests "${representative_startup_log}")"
representative_iteration_reached="$(extract_iteration_reached "${representative_startup_log}")"
representative_startup_time_ms="$(extract_stress_value "time_to_failure_ms" "${representative_startup_log}")"
if [[ -z "${representative_startup_time_ms}" ]]; then
  representative_startup_time_ms="$(extract_stress_value "total_elapsed_ms" "${representative_startup_log}")"
fi
if [[ -z "${representative_startup_time_ms}" ]]; then
  representative_startup_time_ms="0"
fi

if [[ "${representative_run_matched}" != "yes" ]]; then
  representative_startup_stage="test_selection_run"
  representative_startup_reason="CTest found the representative test during discovery, but the execution command matched zero tests."
elif [[ "${representative_test_launched}" != "yes" ]]; then
  representative_startup_stage="test_launch"
  if [[ -z "${representative_startup_reason}" ]]; then
    representative_startup_reason="CTest matched the representative test, but the process never reached the test startup marker."
  fi
elif [[ "${representative_startup_exit}" -ne 0 ]] || ! contains_fixed_string "stress.startup_stage=first_iteration_begin" "${representative_startup_log}" || ! contains_fixed_string "stress.iteration=1" "${representative_startup_log}"; then
  if [[ -z "${representative_startup_reason}" ]]; then
    representative_startup_reason="Representative startup run entered the test body but never reached first_iteration_begin and iteration 1."
  fi
fi

if [[ "${representative_run_matched}" != "yes" ]] || [[ "${representative_test_launched}" != "yes" ]] || [[ "${representative_startup_exit}" -ne 0 ]] || ! contains_fixed_string "stress.startup_stage=first_iteration_begin" "${representative_startup_log}" || ! contains_fixed_string "stress.iteration=1" "${representative_startup_log}"; then
  emit_github_annotation \
    error \
    "Subtitle stress startup failed" \
    "mode=${representative_mode_key}; stage=${representative_startup_stage}; time_ms=${representative_startup_time_ms}; reason=$(truncate_annotation_text "${representative_startup_reason}")"
  append_summary "- Representative startup failed at \`${representative_startup_stage}\` after \`${representative_startup_time_ms}ms\`: \`$(sanitize_markdown_inline "${representative_startup_reason}")\`"
  append_summary "- Run matched tests: \`${representative_run_matched}\`"
  append_summary "- Test launched: \`${representative_test_launched}\`"
  append_summary "- Startup stage reached: \`${representative_startup_stage_reached}\`"
  append_summary "- Iteration reached: \`${representative_iteration_reached}\`"
  append_summary ""
  append_summary "<details><summary>Representative Raw Output</summary>"
  append_summary ""
  append_file_as_code_block "${representative_startup_log}"
  append_summary "</details>"
  while IFS= read -r line; do
    append_summary "- \`${line}\`"
  done < <(
    grep -E '^stress\.(startup_marker|startup_stage|failure_stage|failure_reason|source\.|subtitle\.|output\.|prefix\.|iteration_begin|iteration|time_to_failure_ms|total_elapsed_ms)=' "${representative_startup_log}" \
      | tail -n 40 || true
  )
  exit 1
fi

append_summary "- Representative startup reached \`first_iteration_begin\` and completed iteration \`1\`."
append_summary "- Run matched tests: \`${representative_run_matched}\`"
append_summary "- Test launched: \`${representative_test_launched}\`"
append_summary ""
append_summary "<details><summary>Representative Raw Output</summary>"
append_summary ""
append_file_as_code_block "${representative_startup_log}"
append_summary "</details>"
append_summary ""
append_summary "| Bitmap mode | Composition mode | Test exists | Launched | Result | Time (ms) | Startup stage | Iteration | Failure stage | ASan |"
append_summary "| --- | --- | --- | --- | --- | ---: | --- | ---: | --- | --- |"

for entry in "${modes[@]}"; do
  read -r bitmap_mode composition_mode mode_key test_name <<< "${entry}"

  discovery_log="${results_dir}/${mode_key}.discovery.log"
  log_file="${results_dir}/${mode_key}.log"
  start_time="$(date +%s)"
  exact_regex="^$(escape_ctest_regex "${test_name}")$"
  test_exists="$(check_test_exists "${test_name}" "${discovery_log}" "${exact_regex}")"
  run_command=(
    ctest
    --test-dir "${build_dir}"
    "${ctest_config_args[@]}"
    --output-on-failure
    -V
    --no-tests=error
    -R "${exact_regex}"
  )

  echo "Running ${mode_key} (${test_name}) with repeat count ${repeat_count}."
  echo "Mode exact test name: ${test_name}"
  echo "Mode escaped regex: ${exact_regex}"
  echo "Mode ctest command: $(format_command_for_log "${run_command[@]}")"

  if [[ "${test_exists}" != "yes" ]]; then
    failure_stage="test_discovery"
    failure_reason="$(extract_failure_reason "${discovery_log}")"
    if [[ -z "${failure_reason}" ]]; then
      failure_reason="CTest did not find the requested stress test."
    fi
    result="FAIL"
    elapsed_ms="0"
    asan_status="clean"
    launched="no"
    startup_stage_reached="none"
    iteration_reached="0"
    overall_status=1
    failed_modes+=("${mode_key}")
    failed_bitmap_modes+=("${bitmap_mode}")
    failed_composition_modes+=("${composition_mode}")
    failed_stages+=("${failure_stage}")

    append_summary "| \`${bitmap_mode}\` | \`${composition_mode}\` | ${test_exists} | ${launched} | ${result} | ${elapsed_ms} | \`${startup_stage_reached}\` | ${iteration_reached} | \`${failure_stage}\` | ${asan_status} |"
    append_summary ""
    append_summary "<details><summary>${mode_key}</summary>"
    append_summary ""
    append_summary "- Test: \`${test_name}\`"
    append_summary "- Test exists: \`${test_exists}\`"
    append_summary "- Test launched: \`${launched}\`"
    append_summary "- Startup stage reached: \`${startup_stage_reached}\`"
    append_summary "- Iteration reached: \`${iteration_reached}\`"
    append_summary "- Failure stage: \`${failure_stage}\`"
    append_summary "- Failure reason: \`$(sanitize_markdown_inline "${failure_reason}")\`"
    append_summary ""
    append_summary "</details>"
    append_summary ""

    emit_github_annotation \
      error \
      "Subtitle stress mode failed" \
      "mode=${mode_key}; bitmap=${bitmap_mode}; composition=${composition_mode}; exists=${test_exists}; launched=${launched}; stage=${failure_stage}; asan=${asan_status}; reason=$(truncate_annotation_text "${failure_reason}")"
    continue
  fi

  set +e
  env \
    UTSURE_SUBTITLE_STRESS_REPEAT="${repeat_count}" \
    UTSURE_SUBTITLE_DIAGNOSTICS="${diagnostics_mode}" \
    UTSURE_FFMPEG_PREFIX="${ffmpeg_prefix}" \
    UTSURE_FFMS2_PREFIX="${ffms2_prefix}" \
    UTSURE_LIBASSMOD_PREFIX="${libassmod_prefix}" \
    "${run_command[@]}" \
    2>&1 | tee "${log_file}"
  exit_code=${PIPESTATUS[0]}
  set -e

  end_time="$(date +%s)"
  elapsed_seconds=$((end_time - start_time))
  asan_excerpt="$(extract_asan_excerpt "${log_file}")"
  failure_stage="$(extract_failure_stage "${log_file}")"
  failure_reason="$(extract_failure_reason "${log_file}")"
  startup_stage_reached="$(extract_startup_stage_reached "${log_file}")"
  launched="$(test_launched "${log_file}")"
  run_matched="$(run_matched_tests "${log_file}")"
  iteration_reached="$(extract_iteration_reached "${log_file}")"
  failed_iteration="$(extract_stress_value "failed_iteration" "${log_file}")"
  time_to_failure_ms="$(extract_stress_value "time_to_failure_ms" "${log_file}")"
  total_elapsed_ms="$(extract_stress_value "total_elapsed_ms" "${log_file}")"
  elapsed_ms="${total_elapsed_ms}"
  if [[ "${exit_code}" -ne 0 ]]; then
    elapsed_ms="${time_to_failure_ms}"
  fi
  if [[ -z "${elapsed_ms}" ]]; then
    elapsed_ms=$((elapsed_seconds * 1000))
  fi
  asan_status="clean"
  if [[ -n "${asan_excerpt}" ]]; then
    asan_status="reported"
  fi

  result="PASS"
  if [[ "${run_matched}" != "yes" ]]; then
    result="FAIL"
    overall_status=1
    failure_stage="test_selection_run"
    failure_reason="CTest found the stress test during discovery, but the execution command matched zero tests."
    failed_modes+=("${mode_key}")
    failed_bitmap_modes+=("${bitmap_mode}")
    failed_composition_modes+=("${composition_mode}")
    failed_stages+=("${failure_stage}")
  elif [[ "${launched}" != "yes" ]]; then
    result="FAIL"
    overall_status=1
    if [[ "${failure_stage}" == "unstructured" || -z "${failure_stage}" ]]; then
      failure_stage="test_launch"
    fi
    if [[ -z "${failure_reason}" ]]; then
      failure_reason="CTest matched the stress test, but the process never reached test_entry."
    fi
    failed_modes+=("${mode_key}")
    failed_bitmap_modes+=("${bitmap_mode}")
    failed_composition_modes+=("${composition_mode}")
    failed_stages+=("${failure_stage}")
  elif [[ "${exit_code}" -ne 0 ]]; then
    result="FAIL"
    overall_status=1
    failed_modes+=("${mode_key}")
    failed_bitmap_modes+=("${bitmap_mode}")
    failed_composition_modes+=("${composition_mode}")
    failed_stages+=("${failure_stage}")
  fi

  append_summary "| \`${bitmap_mode}\` | \`${composition_mode}\` | ${test_exists} | ${launched} | ${result} | ${elapsed_ms} | \`${startup_stage_reached}\` | ${iteration_reached} | \`${failure_stage}\` | ${asan_status} |"
  append_summary ""
  append_summary "<details><summary>${mode_key}</summary>"
  append_summary ""
  append_summary "- Test: \`${test_name}\`"
  append_summary "- Escaped exact regex: \`${exact_regex}\`"
  append_summary "- CTest command: \`$(sanitize_markdown_inline "$(format_command_for_log "${run_command[@]}")")\`"
  append_summary "- Test exists: \`${test_exists}\`"
  append_summary "- Run matched tests: \`${run_matched}\`"
  append_summary "- Test launched: \`${launched}\`"
  append_summary "- Exit code: \`${exit_code}\`"
  append_summary "- Wall time: \`${elapsed_seconds}s\`"
  append_summary "- Effective elapsed: \`${elapsed_ms}ms\`"
  append_summary "- Startup stage reached: \`${startup_stage_reached}\`"
  append_summary "- Iteration reached: \`${iteration_reached}\`"
  append_summary "- Failed iteration: \`${failed_iteration:-0}\`"
  append_summary "- Failure stage: \`${failure_stage}\`"
  if [[ -n "${failure_reason}" ]]; then
    append_summary "- Failure reason: \`$(sanitize_markdown_inline "${failure_reason}")\`"
  fi

  if [[ -n "${asan_excerpt}" ]]; then
    while IFS= read -r line; do
      append_summary "- ASan: \`$(sanitize_markdown_inline "${line}")\`"
    done <<< "${asan_excerpt}"
  else
    append_summary "- ASan: none"
  fi

  while IFS= read -r line; do
    append_summary "- \`${line}\`"
  done < <(
    grep -E '^stress\.(mode|bitmap_mode|composition_mode|repeat_count|startup_marker|startup_stage|iteration_begin|iteration|iteration_elapsed_ms|failed_iteration|failure_stage|failure_reason|time_to_failure_ms|total_elapsed_ms|status|subtitle_workers|subtitled_frames|source\.|subtitle\.|output\.|prefix\.)=' "${log_file}" \
      | tail -n 40 || true
  )

  append_summary ""
  append_summary "</details>"
  append_summary ""

  if [[ "${exit_code}" -ne 0 ]]; then
    annotation_reason="$(truncate_annotation_text "${failure_reason:-unavailable}")"
    emit_github_annotation \
      error \
      "Subtitle stress mode failed" \
      "mode=${mode_key}; bitmap=${bitmap_mode}; composition=${composition_mode}; exists=${test_exists}; matched=${run_matched}; launched=${launched}; time_ms=${elapsed_ms}; startup=${startup_stage_reached}; iteration=${iteration_reached}; stage=${failure_stage}; asan=${asan_status}; reason=${annotation_reason}"
  fi
done

suspect_message="No failing mode observed."
if [[ "${#failed_modes[@]}" -eq 1 ]]; then
  suspect_message="Primary suspect: ${failed_modes[0]}."
elif [[ "${#failed_modes[@]}" -gt 1 ]]; then
  common_stage="${failed_stages[0]}"
  common_failure_stage=1
  for failure_stage in "${failed_stages[@]}"; do
    if [[ "${failure_stage}" != "${common_stage}" ]]; then
      common_failure_stage=0
      break
    fi
  done

  if [[ "${common_failure_stage}" -eq 1 && -n "${common_stage}" ]]; then
    suspect_message="All failing modes stopped at the common stage ${common_stage}; mode isolation remains inconclusive."
  fi

  direct_common_factor=1
  for bitmap_mode in "${failed_bitmap_modes[@]}"; do
    if [[ "${bitmap_mode}" != "direct" ]]; then
      direct_common_factor=0
      break
    fi
  done

  worker_common_factor=1
  for composition_mode in "${failed_composition_modes[@]}"; do
    if [[ "${composition_mode}" != "worker_local" ]]; then
      worker_common_factor=0
      break
    fi
  done

  if [[ "${common_failure_stage}" -eq 1 && -n "${common_stage}" ]]; then
    :
  elif [[ "${direct_common_factor}" -eq 1 ]]; then
    suspect_message="Common factor across failing modes: direct subtitle bitmap bytes."
  elif [[ "${worker_common_factor}" -eq 1 ]]; then
    suspect_message="Common factor across failing modes: worker-local subtitle sessions."
  else
    suspect_message="Multiple modes failed without a single shared direct-bitmap or worker-local factor."
  fi
fi

append_summary "### Interpretation"
append_summary ""
append_summary "- ${suspect_message}"
append_summary "- Safe default remains \`copied + serialized\` until a faster mode is proven stable."

if [[ "${overall_status}" -ne 0 ]]; then
  emit_github_annotation error "Subtitle stress suspect" "${suspect_message}"
else
  emit_github_annotation notice "Subtitle stress suspect" "${suspect_message}"
fi

if [[ "${overall_status}" -ne 0 ]]; then
  echo "At least one subtitle stress isolation mode failed."
fi

exit "${overall_status}"
