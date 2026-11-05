#!/usr/bin/env bash
# Compare rendered pixels with Mangetsu style metadata against equivalent inline overrides.
set -euo pipefail

tmpdir="$(mktemp -d "./mangetsu-style-smoke.XXXXXX")"
trap 'rm -rf "${tmpdir}"' EXIT

ffmpeg_bin="${UTSURE_FFMPEG_PREFIX:-${UTSURE_THIRD_PARTY_ROOT:-.deps}/ffmpeg/prefix}/bin/ffmpeg"
if [[ ! -x "${ffmpeg_bin}" ]]; then
  echo "Missing pinned FFmpeg executable: ${ffmpeg_bin}" >&2
  exit 1
fi

cat > "${tmpdir}/metadata.ass" <<'ASS'
[Script Info]
ScriptType: v4.00+
PlayResX: 320
PlayResY: 180
WrapStyle: 0

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,32,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,5,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\2bs3\2bc&H00FF00&}
Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,Mangetsu style check
ASS

cat > "${tmpdir}/inline.ass" <<'ASS'
[Script Info]
ScriptType: v4.00+
PlayResX: 320
PlayResY: 180
WrapStyle: 0

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,32,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,5,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,{\2bs3\2bc&H00FF00&}Mangetsu style check
ASS

render() {
  local filter="$1"
  local script="$2"
  local output="$3"
  "${ffmpeg_bin}" -hide_banner -nostdin -loglevel error \
    -f lavfi -i 'color=c=black:s=320x180:r=1:d=1' \
    -vf "${filter}=filename='${tmpdir}/${script}':mangetsu_rgba=on" \
    -frames:v 1 -an -pix_fmt bgra -f framemd5 "${output}"
  test -s "${output}"
}

for filter in ass subtitles; do
  render "${filter}" metadata.ass "${tmpdir}/${filter}-metadata.md5"
  render "${filter}" inline.ass "${tmpdir}/${filter}-inline.md5"
  if ! cmp -s "${tmpdir}/${filter}-metadata.md5" "${tmpdir}/${filter}-inline.md5"; then
    echo "Mangetsu style metadata differs from inline overrides with ${filter} filter" >&2
    diff -u "${tmpdir}/${filter}-inline.md5" "${tmpdir}/${filter}-metadata.md5" >&2 || true
    exit 1
  fi
done

if ! cmp -s "${tmpdir}/ass-metadata.md5" "${tmpdir}/subtitles-metadata.md5"; then
  echo "ass/subtitles filters differ for the same full-script Mangetsu style" >&2
  exit 1
fi

echo "Mangetsu style metadata: identical pixels with inline tags for ass/subtitles filters."
