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

sanitize_markdown_inline() {
  local value="$1"
  value="${value//\`/\'}"
  printf '%s' "${value}"
}

extract_asan_excerpt() {
  local log_file="$1"
  grep -Eim 3 \
    'ERROR: AddressSanitizer|AddressSanitizer|heap-use-after-free|heap-buffer-overflow|stack-use-after-return|double-free|SEGV on unknown address|use-after-poison' \
    "${log_file}" || true
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
append_summary "| Bitmap mode | Composition mode | Result | Wall time (s) | ASan |"
append_summary "| --- | --- | --- | ---: | --- |"

for entry in "${modes[@]}"; do
  read -r bitmap_mode composition_mode mode_key test_name <<< "${entry}"

  log_file="${results_dir}/${mode_key}.log"
  start_time="$(date +%s)"

  echo "Running ${mode_key} (${test_name}) with repeat count ${repeat_count}."

  set +e
  env \
    UTSURE_SUBTITLE_STRESS_REPEAT="${repeat_count}" \
    UTSURE_SUBTITLE_DIAGNOSTICS="${diagnostics_mode}" \
    ctest --test-dir "${build_dir}" --output-on-failure -V -R "^${test_name}$" \
    2>&1 | tee "${log_file}"
  exit_code=${PIPESTATUS[0]}
  set -e

  end_time="$(date +%s)"
  elapsed_seconds=$((end_time - start_time))
  asan_excerpt="$(extract_asan_excerpt "${log_file}")"
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
  fi

  append_summary "| \`${bitmap_mode}\` | \`${composition_mode}\` | ${result} | ${elapsed_seconds} | ${asan_status} |"
  append_summary ""
  append_summary "<details><summary>${mode_key}</summary>"
  append_summary ""
  append_summary "- Test: \`${test_name}\`"
  append_summary "- Exit code: \`${exit_code}\`"
  append_summary "- Wall time: \`${elapsed_seconds}s\`"

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
    grep -E '^stress\.(mode|bitmap_mode|composition_mode|repeat_count|iteration|iteration_elapsed_ms|failed_iteration|time_to_failure_ms|total_elapsed_ms|status|subtitle_workers|subtitled_frames)=' "${log_file}" \
      | tail -n 16 || true
  )

  append_summary ""
  append_summary "</details>"
  append_summary ""
done

suspect_message="No failing mode observed."
if [[ "${#failed_modes[@]}" -eq 1 ]]; then
  suspect_message="Primary suspect: ${failed_modes[0]}."
elif [[ "${#failed_modes[@]}" -gt 1 ]]; then
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

  if [[ "${direct_common_factor}" -eq 1 ]]; then
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
  echo "At least one subtitle stress isolation mode failed."
fi

exit "${overall_status}"
