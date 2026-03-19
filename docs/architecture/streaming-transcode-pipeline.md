# Streaming Transcode Pipeline

This document describes the active encode path used by `EncodeJobRunner`.

## Foundation

The active transcoder is intentionally pinned to FFmpeg `7.1.x` and uses only the core libraries that the current app pipeline needs:

- `libavformat` for demux and mux
- `libavcodec` for decode and encode
- `libswscale` for video normalization and encoder pixel-format conversion
- `libswresample` for audio normalization

`libavfilter` is not part of the active dependency surface. Subtitle burn-in stays inside the app through the subtitle renderer/compositor boundary.

The old full-buffer helpers still exist for isolated decode/report tests, but the job runner no longer uses:

- `MediaDecoder::decode(...)`
- `TimelineComposer::compose(...)`
- `subtitles::burn_in::apply(...)`

## Segment Flow

Intro, main, and outro clips are still assembled by `TimelineAssembler`, but they are processed sequentially through one streaming output session:

1. Build one output muxer plus video/audio encoders from the main-segment cadence and output settings.
2. Open one input segment at a time with `libavformat`.
3. Demux packets incrementally.
4. Decode video/audio frames incrementally with `libavcodec`.
5. Normalize video to RGBA with `libswscale` and audio to planar float with `libswresample`.
6. Render and composite subtitle bitmaps onto each video frame inside the app when the segment/timing mode requires it.
7. Encode video/audio incrementally.
8. Mux packets incrementally with `av_interleaved_write_frame(...)`.
9. Finalize the muxer after the last segment.

Decoded media from earlier segments is never retained after that segment has cleared the downstream stage boundaries.

## Bounded Buffers

The current bounded state comes from `media::streaming::kDefaultPipelineQueueLimits`:

- Pending compressed audio packet queue: `32`
- Decoded normalized audio block queue: `8`

Video is now a direct synchronous path inside the single-threaded loop:

- demux packet
- decode frame
- normalize to RGBA
- optionally subtitle-composite
- encode
- mux

There is no longer a separate bounded queue for video packets, decoded video frames, composited video frames, or encoded mux packets in the active path.

The compressed-audio packet queue exists only to prevent audio decode from outrunning the video-timed audio budget when interleaved packet order temporarily favors audio.

## Allocation And Release

### Video

For each decoded video frame:

1. `avcodec_receive_frame(...)` fills a reusable FFmpeg decode frame.
2. `normalize_video_frame(...)` allocates one temporary RGBA `AVFrame`, copies its bytes into one owned `DecodedVideoFrame`, and unrefs the temporary FFmpeg frame.
3. Subtitle composition mutates that owned RGBA frame in place.
4. `StreamingOutputSession::build_encoded_video_frame(...)` allocates one YUV encoder frame, uses `sws_scale(...)`, sends it to the encoder, and releases it after the send/drain step.
5. `avcodec_receive_packet(...)` fills a reusable packet, `av_interleaved_write_frame(...)` muxes it, and `av_packet_unref(...)` releases packet storage immediately.

### Audio

For each decoded audio frame:

1. `avcodec_receive_frame(...)` fills a reusable FFmpeg audio decode frame.
2. `resample_audio_frame(...)` converts it to planar-float channel vectors.
3. Normalized samples accumulate into bounded decoded-audio blocks.
4. When enough output-timeline audio budget exists, a rebased `DecodedAudioSamples` block is built and passed to the audio encoder.
5. `StreamingOutputSession::build_encoded_audio_frame(...)` allocates an encoder input frame, copies planar floats into FFmpeg-owned buffers, sends it to the encoder, and releases it after the send/drain step.
6. Encoded audio packets are muxed immediately and unreffed immediately.

### Subtitle Tiles

Subtitle tiles are owned only for one frame render/composite pass:

1. `ass_render_frame_rgba(...)` returns libassmod-owned RGBA images.
2. The adapter copies each image into an owned `SubtitleBitmap`.
3. `ass_free_images_rgba(...)` releases the libassmod allocation immediately after the copy.
4. `subtitle_bitmap_compositor.cpp` blends the copied premultiplied RGBA bitmaps into the current frame.
5. The subtitle bitmap vectors are destroyed after that frame is encoded.

## Audio Rules

The active streaming path emits muxed audio when `TimelinePlan.output_audio_stream` is present.

Rules:

- If a segment has no audio stream but the output timeline does, bounded silence blocks are synthesized for that segment.
- If decoded segment audio exceeds the video-defined segment duration, the final drain trims the overflow instead of extending the output timeline.
- If decoded segment audio falls short of the expected segment duration, the pipeline pads the tail with silence.
- If the output container, audio encoder, sample rate, or channel layout is unsupported, the job fails clearly instead of silently dropping audio.

## Subtitle Path

Subtitle rendering stays behind the existing abstraction. The active libassmod adapter uses the RGBA-capable path unconditionally for rendered frames so RGBA-only effects are not forced back through the old `ASS_Image` bitmap flow.

The libassmod calls currently used in the adapter are:

- `ass_library_init`
- `ass_set_extract_fonts`
- `ass_renderer_init`
- `ass_set_frame_size`
- `ass_set_storage_size`
- `ass_set_pixel_aspect`
- `ass_set_margins`
- `ass_set_use_margins`
- `ass_set_fonts`
- `ass_read_file`
- `ass_render_frame_rgba`
- `ass_free_images_rgba`
- `ass_clear_tag_images`
- `ass_renderer_done`
- `ass_free_track`
- `ass_library_done`

The compositor expects premultiplied RGBA subtitle tiles and blends them into RGBA video frames with source-over math in premultiplied space.

## Cadence Rules

The output frame rate still comes from the main source.

The streaming pipeline enforces this by:

- building the output encoder from `TimelinePlan.output_frame_rate` and `TimelinePlan.output_video_time_base`
- validating every streamed frame duration against the main cadence after rescaling into the output time base
- validating inter-frame timestamp deltas per segment
- rebasing output frame timestamps onto a single monotonically increasing output timeline

FFmpeg 7.1-specific implementation choices:

- output video time base is chosen from the inverse authoritative frame rate when the container stream time base is too coarse for exact CFR validation
- channel-layout handling uses `AVChannelLayout`-based APIs that are available in FFmpeg 7.1
- audio normalization uses `swr_alloc_set_opts2(...)`, which matches the FFmpeg 7.1 channel-layout model

## Memory Budget

The working-set estimate now scales with queue depth and in-flight surfaces, not clip duration.

Current estimate formula:

- one in-flight owned RGBA video surface
- `+ rgba_frame_bytes` subtitle scratch when subtitles are enabled
- `2 * yuv420_frame_bytes` for encoder-side working surfaces
- `decoded_audio_block_queue * audio_block_bytes`
- `audio_packet_queue * 128 KiB` compressed-audio reserve
- `1 * audio_block_bytes` for the audio encoder carry buffer

That keeps peak memory proportional to resolution plus the small bounded audio queues, not to total clip length.

## GitHub Actions Validation

The Windows GitHub Actions job should verify:

1. dependency audit succeeds with source-built FFmpeg `7.1.2`, `libx264`, `libx265`, and the pinned `libassmod` prefix
2. CMake configure/build succeeds without requiring `libavfilter`
3. core tests still cover inspection, decode, legacy encode helpers, timeline assembly, encode-job streaming output, subtitle rendering, subtitle composition, and subtitle burn-in
4. audio-bearing encode-job outputs still contain synchronized audio streams
5. RGBA-capable libassmod subtitle scripts still render and burn in correctly
6. the packaged Windows bundle still launches

## Remaining Limits

- Host-side `\img` resource registration is still not wired. Scripts that reference `\img` fail explicitly during subtitle-session creation.
- The active output audio encoder is AAC-only.
- Hardware-accelerated decode/encode is not part of this slice.

## Maintenance Notes

When changing this pipeline:

- keep queue ownership single-owner and move-only
- do not reintroduce vectors of whole decoded segments into the active job path
- keep subtitle rendering on the frame path, not as a pre-rendered clip transform
- keep segment sequencing generic so intro/main/outro all use the same pipeline code
- if queue depths or packet reserves change, update both `kDefaultPipelineQueueLimits` and the memory-budget explanation above
