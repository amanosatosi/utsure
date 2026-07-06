#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
third_party_root="${UTSURE_THIRD_PARTY_ROOT:-${project_root}/.deps}"
ffmpeg_prefix="${UTSURE_FFMPEG_PREFIX:-${third_party_root}/ffmpeg/prefix}"
ffmpeg_pcdir="${ffmpeg_prefix}/lib/pkgconfig"
ffms2_prefix="${UTSURE_FFMS2_PREFIX:-${third_party_root}/ffms2/prefix}"
ffms2_pcdir="${ffms2_prefix}/lib/pkgconfig"
libassmod_prefix="${UTSURE_LIBASSMOD_PREFIX:-${third_party_root}/libassmod/prefix}"
libassmod_pcdir="${libassmod_prefix}/lib/pkgconfig"
msys2_prefix="${UTSURE_MSYS2_PREFIX:-/ucrt64}"
cmake_build_type="${UTSURE_CMAKE_BUILD_TYPE:-Debug}"
build_dir="${UTSURE_BUILD_DIR:-build}"
build_app="${UTSURE_BUILD_APP:-ON}"
run_app_smoke_test="${UTSURE_RUN_APP_SMOKE_TEST:-${build_app}}"

cmake_extra_args=()
if [[ -n "${UTSURE_CMAKE_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  cmake_extra_args=(${UTSURE_CMAKE_EXTRA_ARGS})
fi

ctest_args=()
if [[ -n "${UTSURE_CTEST_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  ctest_args=(${UTSURE_CTEST_ARGS})
fi
if [[ -n "${UTSURE_CTEST_REGEX:-}" ]]; then
  ctest_args+=(-R "${UTSURE_CTEST_REGEX}")
fi

export PATH="${ffmpeg_prefix}/bin:${ffms2_prefix}/bin:${libassmod_prefix}/bin:${msys2_prefix}/bin:${PATH}"
export PKG_CONFIG_PATH="${ffmpeg_pcdir}:${ffms2_pcdir}:${libassmod_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

cmake -S . -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${cmake_build_type}" \
  -DCMAKE_PREFIX_PATH="${msys2_prefix}" \
  -DUTSURE_BUILD_APP="${build_app}" \
  -DUTSURE_LIBASSMOD_REF="${UTSURE_LIBASSMOD_REF:-mangetsu}" \
  -DUTSURE_ENABLE_DEPENDENCY_AUDIT=ON \
  -DUTSURE_REQUIRE_FFMPEG=ON \
  -DUTSURE_FFMPEG_ROOT="${ffmpeg_prefix}" \
  -DUTSURE_REQUIRE_FFMS2=ON \
  -DUTSURE_FFMS2_ROOT="${ffms2_prefix}" \
  -DUTSURE_REQUIRE_LIBASSMOD=ON \
  -DUTSURE_LIBASSMOD_ROOT="${libassmod_prefix}" \
  "${cmake_extra_args[@]}"

cmake --build "${build_dir}" --target utsure_encoder_core --parallel
if [[ "${build_app}" == "ON" ]]; then
  cmake --build "${build_dir}" --target utsure_encoder_app --parallel
  cmake --build "${build_dir}" --target utsure_app_settings_tests --parallel
  cmake --build "${build_dir}" --target utsure_app_batch_import_planner_tests --parallel
  cmake --build "${build_dir}" --target utsure_app_ui_font_tests --parallel
  cmake --build "${build_dir}" --target utsure_app_crash_dump_writer_tests --parallel
  cmake --build "${build_dir}" --target utsure_app_encode_job_runner_lifecycle_tests --parallel
fi
cmake --build "${build_dir}" --target utsure_core_media_inspection_tests --parallel
cmake --build "${build_dir}" --target utsure_core_media_decode_tests --parallel
cmake --build "${build_dir}" --target utsure_core_media_encode_tests --parallel
cmake --build "${build_dir}" --target utsure_core_encode_job_tests --parallel
cmake --build "${build_dir}" --target utsure_core_encode_job_preflight_tests --parallel
cmake --build "${build_dir}" --target utsure_core_output_naming_tests --parallel
cmake --build "${build_dir}" --target utsure_core_resize_tests --parallel
cmake --build "${build_dir}" --target utsure_core_audio_stream_selection_tests --parallel
cmake --build "${build_dir}" --target utsure_core_source_import_paths_tests --parallel
cmake --build "${build_dir}" --target utsure_core_batch_parallelism_tests --parallel
cmake --build "${build_dir}" --target utsure_core_subtitle_auto_selection_tests --parallel
cmake --build "${build_dir}" --target utsure_core_subtitle_image_assets_tests --parallel
cmake --build "${build_dir}" --target utsure_core_thumbnail_preroll_tests --parallel
cmake --build "${build_dir}" --target utsure_core_subtitle_font_recovery_tests --parallel
cmake --build "${build_dir}" --target utsure_core_timeline_tests --parallel
cmake --build "${build_dir}" --target utsure_core_subtitle_mangetsu_metadata_tests --parallel
cmake --build "${build_dir}" --target utsure_core_subtitle_renderer_tests --parallel
cmake --build "${build_dir}" --target utsure_core_libassmod_subtitle_adapter_tests --parallel
cmake --build "${build_dir}" --target utsure_core_libassmod_rgba_reproducer --parallel
cmake --build "${build_dir}" --target utsure_core_subtitle_bitmap_compositor_tests --parallel
cmake --build "${build_dir}" --target utsure_core_subtitle_burn_in_tests --parallel

ctest --test-dir "${build_dir}" -N
ctest --test-dir "${build_dir}" --show-only=json-v1 > "${build_dir}/ctest-tests.json"
python - "${build_dir}/ctest-tests.json" <<'PY'
import json
import sys
from pathlib import Path

def executable_candidates(raw_command):
    normalized = raw_command.replace("\\", "/")
    candidates = [Path(raw_command), Path(normalized)]
    if len(normalized) >= 3 and normalized[1] == ":" and normalized[2] == "/":
        candidates.append(Path(f"/{normalized[0].lower()}{normalized[2:]}"))
    return candidates

report_path = Path(sys.argv[1])
with report_path.open("r", encoding="utf-8") as stream:
    report = json.load(stream)

missing = []
for test in report.get("tests", []):
    command = test.get("command", [])
    if not command:
        continue
    candidates = executable_candidates(command[0])
    candidates.extend(
        (report_path.parent / candidate).resolve()
        for candidate in list(candidates)
        if not candidate.is_absolute()
    )
    if not any(candidate.exists() for candidate in candidates):
        missing.append((test.get("name", "<unnamed>"), command[0]))

if missing:
    print("CTest build-wiring error: registered test executables are missing:", file=sys.stderr)
    for name, executable in missing:
        print(f"  {name}: {executable}", file=sys.stderr)
    sys.exit(1)

print(f"Verified {len(report.get('tests', []))} registered CTest command executable(s) exist.")
PY
ctest --test-dir "${build_dir}" --print-labels
ctest --test-dir "${build_dir}" --output-on-failure "${ctest_args[@]}"

if [[ "${build_app}" == "ON" && "${run_app_smoke_test}" == "ON" ]]; then
  export QT_PLUGIN_PATH="${msys2_prefix}/share/qt6/plugins"
  export QT_QPA_PLATFORM="offscreen"

  "./${build_dir}/src/app/utsure.exe" --dump-window-structure --smoke-test
fi
