# FFmpeg Filter Hardsub Backend

Utsure keeps the internal libassmod subtitle renderer for debugging and future repair, but production hardsub encodes can opt into an FFmpeg-owned subtitle path:

```text
UTSURE_HARDSUB_BACKEND=ffmpeg_filter
```

This backend exists as a production workaround for nondeterministic crashes in Utsure's internal subtitle render/composite pipeline. When selected for a subtitle-enabled encode, Utsure remains the GUI and queue manager, while FFmpeg owns decode, scale/filter ordering, subtitle filtering/compositing, video encode, audio handling, and mux.

## FFmpeg Source

The Windows CI build can use either upstream FFmpeg or the Mangetsu-enabled fork:

```text
UTSURE_FFMPEG_SOURCE=upstream
UTSURE_FFMPEG_SOURCE=mangetsu
```

The Mangetsu mode is pinned to:

```text
Repo: https://github.com/amanosatosi/FFmpeg
Branch: 7.1
Commit: 07d5fc59c586fee18b2f85c291013099f4112cb3
Commit title: lavfi/subtitles: preserve attachment filename metadata
```

That commit includes the preceding `lavfi/subtitles: add Mangetsu destination-aware blend support` change. The Mangetsu build enables FFmpeg's existing `ass` and `subtitles` filters with libassmod-backed Mangetsu options. It does not introduce new FFmpeg filter names.

Mangetsu `\blend` is handled through libassmod's destination-aware BGRA compositor. When `mangetsu_blend=auto` or `on` is active, FFmpeg negotiates a BGRA surface around the subtitle filter so each ordered subtitle tile can blend against the video and all earlier subtitle pixels. FFmpeg may convert back to the requested output pixel format after subtitle composition.

## Runtime Behavior

The default remains:

```text
UTSURE_HARDSUB_BACKEND=internal
```

Set this before launching Utsure to force the FFmpeg filter backend:

```powershell
$env:UTSURE_HARDSUB_BACKEND = "ffmpeg_filter"
.\utsure.exe
```

For sidecar `.ass`/`.ssa` subtitles, Utsure generates an FFmpeg filtergraph using:

```bash
ass=filename='PATH_TO_SUBS.ass':mangetsu_rgba=auto:mangetsu_actor_colorcoding=auto
```

`mangetsu_blend` defaults to `auto`, so the existing Utsure filtergraph automatically uses destination-aware `\blend` when the pinned Mangetsu FFmpeg/libassmod pair exposes the compositor API. It can also be set explicitly in manual FFmpeg invocations.

Embedded subtitle streams are not exposed in the current Utsure job model. When selected stream metadata is added, the intended FFmpeg form is:

```bash
subtitles=filename='INPUT.mkv':si=0:mangetsu_rgba=auto:mangetsu_actor_colorcoding=auto:mangetsu_blend=auto
```

If `ffmpeg_filter` is selected, Utsure must not silently fall back to the internal compositor. Missing or incompatible FFmpeg builds fail with a clear error.

## Smoke Tests

Check that the expected filters exist:

```bash
ffmpeg -filters | findstr /i "ass subtitles"
```

Check that Mangetsu options are exposed. The output must include `mangetsu_rgba`, `mangetsu_actor_colorcoding`, and `mangetsu_blend`:

```bash
ffmpeg -hide_banner -h filter=ass
ffmpeg -hide_banner -h filter=subtitles
```

Manual sidecar encode:

```bash
ffmpeg -y -i input.mkv -vf "ass=filename='subs.ass':mangetsu_rgba=auto:mangetsu_actor_colorcoding=auto:mangetsu_blend=auto" -c:v libx265 -crf 18 output.mp4
```

Manual embedded-stream encode:

```bash
ffmpeg -y -i input.mkv -vf "subtitles=filename='input.mkv':si=0:mangetsu_rgba=auto:mangetsu_actor_colorcoding=auto:mangetsu_blend=auto" -c:v libx265 -crf 18 output.mp4
```

## Known Limitations

- Destination-aware `\blend` composition requires the Mangetsu RGBA path. `mangetsu_blend=on` is incompatible with `mangetsu_rgba=off`.
- The internal compositor remains available for debugging and development.
- The FFmpeg filter backend should be preferred for real production hardsub encodes while the internal crash is being repaired.
- Thumbnail pre-roll is rejected when `ffmpeg_filter` is forced because it currently depends on Utsure's internal subtitle renderer.
