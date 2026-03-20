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
7. Rebase video frames onto the output timeline from decoded timestamps, then encode video/audio incrementally.
8. Mux packets incrementally with `av_interleaved_write_frame(...)`.
9. Finalize the muxer after the last segment.

Decoded media from earlier segments is never retained after that segment has cleared the downstream stage boundaries.

## Bounded Buffers

The current bounded state comes from `media::streaming::kDefaultPipelineQueueLimits`:

- Queued normalized video frames waiting for subtitle/composite + encode: `70`
- Decoded normalized audio block queue: `8`
- Startup audio preroll before the first written video frame: `250 ms`

Video flow keeps one explicit bounded handoff queue plus one single-frame timing hold:

- `PendingVideoFrameOutput` keeps at most one normalized RGBA frame so the next decoded timestamp can define its exact output duration.
- Once that duration is known, `enqueue_video_frame(...)` moves the frame into `BoundedQueue<QueuedVideoFrameOutput>` with a hard depth of `70`.
- If that queue is full, `enqueue_video_frame(...)` immediately calls `drain_video_frame_queue(false)` before accepting another frame.
- `drain_video_frame_queue(false)` dequeues one frame, subtitle-composites it if needed, sends it to the encoder, muxes any produced packets, and returns.
- Decode/normalize cannot continue once the queue is full, so the producer side cannot outrun encode indefinitely.

There is still no extra queue for encoded video packets or mux packets. Those packets are drained and released immediately after the encoder emits them.

Audio flow control now uses timestamp horizons in a common microsecond time base:

- written video horizon: video frames already emitted onto the output timeline
- known video horizon: the furthest video packet timeline already demuxed into the decoder
- written audio horizon: audio blocks already emitted onto the output timeline
- buffered decoded-audio horizon: decoded audio blocks plus any resampled-but-not-yet-queued samples

Non-final audio drain is allowed only while the next audio block would stay within the allowed audio horizon.
That horizon is:

- `250 ms` during startup before the first written video frame
- otherwise the known video horizon

When buffered decoded audio has already reached that horizon, audio decode pauses, compressed audio packets remain pending, and the loop keeps draining downstream work until video advances.

## Allocation And Release

### Video

For each decoded video frame:

1. `avcodec_receive_frame(...)` fills a reusable FFmpeg decode frame.
2. `normalize_video_frame(...)` allocates one temporary RGBA `AVFrame`, copies its bytes into one owned `DecodedVideoFrame`, and unrefs the temporary FFmpeg frame.
3. The segment loop keeps at most one normalized frame in `PendingVideoFrameOutput` so the previous frame duration can be derived from the next decoded timestamp.
4. Once that duration is known, ownership moves into `BoundedQueue<QueuedVideoFrameOutput>`, which holds at most `70` RGBA frames for the segment.
5. When the queue drains, subtitle composition mutates that owned RGBA frame in place immediately before encode.
6. `StreamingOutputSession::build_encoded_video_frame(...)` allocates one YUV encoder frame, uses `sws_scale(...)`, sends it to the encoder, and releases it after the send/drain step.
7. `avcodec_receive_packet(...)` fills a reusable packet, `av_interleaved_write_frame(...)` muxes it, and `av_packet_unref(...)` releases packet storage immediately.

### Audio

For each decoded audio frame:

1. `avcodec_receive_frame(...)` fills a reusable FFmpeg audio decode frame.
2. `resample_audio_frame(...)` converts it to planar-float channel vectors.
3. Normalized samples accumulate into bounded decoded-audio blocks, while any partial resample tail stays in the per-segment pending channel vectors.
4. When enough output-timeline audio budget exists, a rebased `DecodedAudioSamples` block is built and passed to the audio encoder.
5. `StreamingOutputSession::build_encoded_audio_frame(...)` allocates an encoder input frame, copies planar floats into FFmpeg-owned buffers, sends it to the encoder, and releases it after the send/drain step.
6. Encoded audio packets are muxed immediately and unreffed immediately.

For stream-copy audio:

1. The source `AVPacket` is cloned only long enough to retime it onto the output stream.
2. `copy_audio_packet(...)` subtracts the source stream start PTS, rescales timestamps into the muxer stream time base, writes the packet, and unreferences it immediately.
3. No audio decoder, resampler, or encoded-audio carry buffer is created for that path.

### Subtitle Tiles

Subtitle tiles are owned only for one frame render/composite pass:

1. `ass_render_frame_rgba(...)` returns libassmod-owned RGBA images.
2. The adapter copies each image into an owned `SubtitleBitmap`.
3. `ass_free_images_rgba(...)` releases the libassmod allocation immediately after the copy.
4. `subtitle_bitmap_compositor.cpp` blends the copied premultiplied RGBA bitmaps into the current frame.
5. The subtitle bitmap vectors are destroyed after that frame is encoded.

## Audio Rules

The active streaming path resolves one `ResolvedAudioOutputPlan` before the muxer is created and uses that plan for preflight, GUI preview text, stream setup, and runtime flow control.

Rules:

- `Disable` omits the output audio stream entirely.
- `Copy` is currently allowed only for single-segment jobs where the output container is a safe fit for the source codec and the user did not request sample-rate or channel-layout changes.
- `Auto` uses stream copy only when that copy path is clearly safe; otherwise it falls back to AAC encode.
- AAC encode always rebases audio onto a sample-based output timeline (`1 / sample_rate`) instead of inheriting an arbitrary source stream time base.
- If a segment has no audio stream but the output timeline does, bounded silence blocks are synthesized for that segment.
- If decoded segment audio exceeds the written video duration on the output timeline, the final drain trims the overflow instead of extending the output timeline.
- If decoded segment audio falls short of the written video duration, the pipeline pads the tail with silence.
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
- keeping `TimelinePlan.output_video_time_base` at the finer of the main stream timestamp base and the inverse nominal frame step so decoded timestamps are not collapsed before encode
- choosing each decoded frame timestamp from `AVFrame.pts`, then `best_effort_timestamp`, and only falling back to a synthesized stream cursor when neither exists
- comparing successive decoded frame timestamps for monotonic forward progress instead of requiring one exact nominal frame step every time
- rebasing decoded frame timestamps into `TimelinePlan.output_video_time_base` before writing them to the encoder
- deriving each emitted frame duration from the next decoded frame timestamp delta, with a decoded-frame duration or nominal-cadence fallback only for the final frame in a segment

FFmpeg 7.1-specific implementation choices:

- decoded frame timing uses `AVFrame.pts`, `best_effort_timestamp`, and `AVFrame.duration` as exposed by FFmpeg 7.1, with a synthesized fallback only when FFmpeg does not provide usable frame timestamps
- channel-layout handling uses `AVChannelLayout`-based APIs that are available in FFmpeg 7.1
- audio normalization uses `swr_alloc_set_opts2(...)`, which matches the FFmpeg 7.1 channel-layout model

## Runtime Controls

Video threading stays backend-managed inside one encoder context:

- `create_video_encoder_context(...)` sets `AVCodecContext.thread_count = 0`, so FFmpeg/libx264/libx265 stay in auto-thread mode instead of the app building extra encode worker pools.
- When the selected encoder advertises FFmpeg frame and/or slice threading flags, the pipeline applies those flags to `AVCodecContext.thread_type`.
- When the encoder does not advertise explicit FFmpeg frame/slice flags, the app leaves `thread_type` untouched and relies on backend-managed threading instead of inventing a duplicate thread pool.
- The Qt desktop shell still uses one worker thread to keep the UI responsive, but the core streaming pipeline itself does not fan out extra host-side encode thread pools around the encoder backend.

Windows priority stays outside `encoder-core` and is applied only by the desktop app worker:

- The Run section exposes `High`, `Above Normal`, `Normal`, `Below Normal`, and `Low`.
- Default is `Below Normal`.
- `Low` maps internally to Windows `IDLE_PRIORITY_CLASS`, which is the lowest safe non-realtime class.
- `Real Time` is intentionally not exposed.
- The worker applies the selected priority with `SetPriorityClass(...)` before `EncodeJobRunner::run(...)` and restores the previous class afterward when possible.

Preview/log/report surfaces show the resolved runtime summary, including encoder auto-threading, the `70`-frame video queue, and the selected priority.

## Memory Budget

The working-set estimate now scales with queue depth and in-flight surfaces, not clip duration.

Current estimate formula:

- one in-flight owned RGBA video surface
- `+ video_frame_queue_depth * rgba_frame_bytes` for the bounded queued RGBA video frames
- `+ rgba_frame_bytes` subtitle scratch when subtitles are enabled
- `2 * yuv420_frame_bytes` for encoder-side working surfaces
- `decoded_audio_block_queue * audio_block_bytes`
- `1 * audio_block_bytes` for the audio encoder carry buffer

That keeps peak memory proportional to resolution plus the bounded decoded-audio queue, not to total clip length.

## GitHub Actions Validation

The Windows GitHub Actions job should verify:

1. dependency audit succeeds with source-built FFmpeg `7.1.2`, `libx264`, `libx265`, and the pinned `libassmod` prefix
2. CMake configure/build succeeds without requiring `libavfilter`
3. core tests still cover inspection, decode, legacy encode helpers, timeline assembly, encode-job streaming output, subtitle rendering, subtitle composition, and subtitle burn-in
4. audio-bearing encode-job outputs still contain synchronized audio streams even when decoded video timestamps do not advance by one exact nominal frame step, including a regression sample with monotonic irregular MP4 frame deltas
5. RGBA-capable libassmod subtitle scripts still render and burn in correctly
6. preflight and encode-job tests still surface `encoder threads`, `video queue 70 frames`, and the selected priority in preview/log/report output
7. the packaged Windows bundle still launches

## Remaining Limits

- Host-side `\img` resource registration is still not wired. Scripts that reference `\img` fail explicitly during subtitle-session creation.
- The active output audio encoder is AAC-only.
- Hardware-accelerated decode/encode is not part of this slice.
- The last video frame in a segment still falls back to decoded-frame duration metadata or the nominal cadence when no later timestamp exists to derive its exact duration.
- The legacy full-buffer `TimelineComposer` path still carries stricter CFR-oriented cadence assumptions and is not the active encode-job path.
- Extremely pathological muxes with unusually long front-loaded audio packet runs can still enlarge the pending compressed-audio backlog until video packets are demuxed, although decoded-audio memory remains bounded by the normalized queue depth.
- Preview/log/report threading text reflects the selected encoder backend's declared FFmpeg threading capabilities plus auto-threading mode; it does not attempt to report the encoder library's internal live worker count.

## Maintenance Notes

When changing this pipeline:

- keep queue ownership single-owner and move-only
- do not reintroduce vectors of whole decoded segments into the active job path
- keep subtitle rendering on the frame path, not as a pre-rendered clip transform
- keep segment sequencing generic so intro/main/outro all use the same pipeline code
- if queue depths or startup timing thresholds change, update both `kDefaultPipelineQueueLimits` and the memory-budget explanation above
