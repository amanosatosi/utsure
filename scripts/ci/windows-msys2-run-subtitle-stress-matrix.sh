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
results_dir="${project_root}/artifacts/subtitle-asan-stress"
summary_file="${results_dir}/summary.md"

cd "${project_root}"

export PATH="${ffmpeg_prefix}/bin:${ffms2_prefix}/bin:${libassmod_prefix}/bin:/ucrt64/bin:${PATH}"
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

extract_stress_value() {
  local key="$1"
  local log_file="$2"
  grep -E "^stress\\.${key}=" "${log_file}" | tail -n 1 | sed -E "s/^stress\\.${key}=//" || true
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
    'No tests were found!!!|Could not find executable|The system cannot find the file specified|The system cannot find the path specified|failed unexpectedly|unexpectedly|unexpected bytes|Unexpected .*subtitle|Unexpected .*frame count|AddressSanitizer' \
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
    printf 'ctest_selection'
    return
  fi

  if grep -Eq 'Could not find executable|The system cannot find the file specified|The system cannot find the path specified' "${log_file}"; then
    printf 'launch'
    return
  fi

  printf 'unstructured'
}

truncate_annotation_text() {
  local value="$1"
  local max_length="${2:-220}"
  if (( ${#value} > max_length )); then
    printf '%s…' "${value:0:max_length}"
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
append_summary "| Bitmap mode | Composition mode | Result | Time (ms) | Iteration | Failure stage | ASan |"
append_summary "| --- | --- | --- | ---: | ---: | --- | --- |"

for entry in "${modes[@]}"; do
  read -r bitmap_mode composition_mode mode_key test_name <<< "${entry}"

  log_file="${results_dir}/${mode_key}.log"
  start_time="$(date +%s)"

  echo "Running ${mode_key} (${test_name}) with repeat count ${repeat_count}."

  set +e
  env \
    UTSURE_SUBTITLE_STRESS_REPEAT="${repeat_count}" \
    UTSURE_SUBTITLE_DIAGNOSTICS="${diagnostics_mode}" \
    ctest --test-dir "${build_dir}" --output-on-failure -V --no-tests=error -R "^${test_name}$" \
    2>&1 | tee "${log_file}"
  exit_code=${PIPESTATUS[0]}
  set -e

  end_time="$(date +%s)"
  elapsed_seconds=$((end_time - start_time))
  asan_excerpt="$(extract_asan_excerpt "${log_file}")"
  failure_stage="$(extract_failure_stage "${log_file}")"
  failure_reason="$(extract_failure_reason "${log_file}")"
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
  if [[ "${exit_code}" -ne 0 ]]; then
    result="FAIL"
    overall_status=1
    failed_modes+=("${mode_key}")
    failed_bitmap_modes+=("${bitmap_mode}")
    failed_composition_modes+=("${composition_mode}")
    failed_stages+=("${failure_stage}")
  fi

  append_summary "| \`${bitmap_mode}\` | \`${composition_mode}\` | ${result} | ${elapsed_ms} | ${failed_iteration:-0} | \`${failure_stage}\` | ${asan_status} |"
  append_summary ""
  append_summary "<details><summary>${mode_key}</summary>"
  append_summary ""
  append_summary "- Test: \`${test_name}\`"
  append_summary "- Exit code: \`${exit_code}\`"
  append_summary "- Wall time: \`${elapsed_seconds}s\`"
  append_summary "- Effective elapsed: \`${elapsed_ms}ms\`"
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
    grep -E '^stress\.(mode|bitmap_mode|composition_mode|repeat_count|iteration|iteration_elapsed_ms|failed_iteration|failure_stage|failure_reason|time_to_failure_ms|total_elapsed_ms|status|subtitle_workers|subtitled_frames)=' "${log_file}" \
      | tail -n 20 || true
  )

  append_summary ""
  append_summary "</details>"
  append_summary ""

  if [[ "${exit_code}" -ne 0 ]]; then
    annotation_reason="$(truncate_annotation_text "${failure_reason:-unavailable}")"
    emit_github_annotation \
      error \
      "Subtitle stress mode failed" \
      "mode=${mode_key}; bitmap=${bitmap_mode}; composition=${composition_mode}; time_ms=${elapsed_ms}; iteration=${failed_iteration:-0}; stage=${failure_stage}; asan=${asan_status}; reason=${annotation_reason}"
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
