# Streaming Transcode Pipeline

This document describes the active encode path used by `EncodeJobRunner`.

## Goal

The encode path now runs as a bounded-memory streaming pipeline instead of decoding, compositing, and storing whole clips in RAM.

The active stages are:

1. Demux packets from one segment input at a time.
2. Decode video and audio incrementally from bounded packet queues.
3. Composite subtitles per video frame at streaming time.
4. Encode video frames incrementally.
5. Mux encoded packets incrementally.

The old full-buffer helpers still exist for isolated decode/report tests, but the job runner no longer uses:

- `MediaDecoder::decode(...)`
- `TimelineComposer::compose(...)`
- `subtitles::burn_in::apply(...)`

## Segment Model

Intro, main, and outro clips are still assembled by `TimelineAssembler`, but they are processed sequentially.

The streaming runner:

1. Opens the output encoder and muxer once from the main-segment cadence and output settings.
2. Streams the intro segment through the pipeline.
3. Streams the main segment through the same pipeline.
4. Streams the outro segment through the same pipeline.
5. Finalizes the encoder and muxer.

Decoded media from earlier segments is never retained after that segment has cleared the downstream queues.

## Queue Limits

The active queue depths come from `media::streaming::kDefaultPipelineQueueLimits`:

- Video packet queue: `16`
- Audio packet queue: `16`
- Decoded video frame queue: `2`
- Composited video frame queue: `2`
- Decoded audio block queue: `8`
- Encoded packet queue: `16`

These are ownership boundaries, not advisory targets. Each queue owns the items moved into it until the downstream stage pops them.

## Lifetime Rules

Frame and packet lifetime is explicit:

- Demux owns compressed packets until they are moved into a packet queue.
- A packet queue owns each packet until decode pops it and sends it to FFmpeg.
- Decode owns the temporary FFmpeg frame only until normalization finishes.
- The decoded-video queue owns a normalized `DecodedVideoFrame` until the composite stage pops it.
- The composite stage mutates the frame in place, rebases its output timestamp, and moves it into the composited-video queue.
- The composited-video queue owns that frame until encode pops it.
- Encode converts the RGBA frame to a temporary YUV frame, sends it to the codec, then the RGBA frame is destroyed when the local variable leaves scope.
- The encoded-packet queue owns muxable packets until mux pops and writes them.
- Audio blocks are decoded into the audio-block queue, validated/counted, and destroyed immediately because the current output remains video-only.

In practice, frame memory is released at three points:

- After a packet has been decoded and the temporary FFmpeg decode frame is unreferenced.
- After a composited `DecodedVideoFrame` is encoded and falls out of scope in the encode stage.
- After an encoded packet is muxed and its owning packet handle is destroyed.

## Memory Budget

The old guard scaled memory with full clip duration. The new guard scales with queue depth.

The new estimate is built from:

- RGBA frame bytes for the main resolution
- YUV420 encoder frame bytes for the main resolution
- Audio block bytes for one normalized audio block
- Fixed packet-queue reserve bytes
- Queue depths listed above

The estimate does not multiply by segment duration or by total frame count.

Current estimate formula:

- RGBA surfaces: `(decoded_video_queue + composited_video_queue + 2 in-flight surfaces) * rgba_frame_bytes`
- Subtitle scratch: `+ rgba_frame_bytes` when subtitles are enabled
- Encoder surfaces: `2 * yuv420_frame_bytes`
- Audio queue: `decoded_audio_block_queue * audio_block_bytes`
- Packet reserve: `(video_packets + audio_packets + encoded_packets) * 512 KiB`

That keeps peak memory roughly proportional to resolution and queue depth, not clip length.

For a 1920x1080 job with the default queue depths:

- No subtitles, no audio: about `77.39 MiB`
- Subtitles enabled, no audio: about `85.30 MiB`
- Stereo 48 kHz audio adds only about `64 KiB` for the audio-block queue

## Why The Old Path Reported About 66.77 GiB

The previous guard in `encode_job_working_set_guard.hpp` estimated whole-clip decoded storage and then multiplied it by pipeline copies.

For a 1920x1080, 24 fps, about 90 second job with subtitles:

- One decoded RGBA frame = `1920 * 1080 * 4 = 8,294,400` bytes
- About `90 * 24 = 2160` frames
- One full decoded clip = `8,294,400 * 2160 = 17,915,904,000` bytes, about `16.69 GiB`

The old guard then assumed:

- Full decoded segments in memory: `+16.69 GiB`
- Another full decoded copy for composed timeline output: `+16.69 GiB`
- Another full decoded copy for encode input staging: `+16.69 GiB`
- Another full decoded copy for subtitle burn-in target when subtitles were enabled: `+16.69 GiB`

Total: about `66.75 GiB`, which matches the reported `~66.77 GiB` once container-duration rounding is included.

The important flaw was architectural, not arithmetic: the estimate grew linearly with full clip duration because the pipeline really did keep whole decoded clips alive.

## Cadence Rules

The output frame rate still comes from the main source.

The streaming pipeline enforces this by:

- Building the output encoder from `TimelinePlan.output_frame_rate` and `TimelinePlan.output_video_time_base`
- Validating every streamed frame duration against the main cadence after rescaling into the output time base
- Validating inter-frame timestamp deltas per segment
- Rebasing output frame timestamps onto a single monotonically increasing output timeline

## Subtitle Timing

The subtitle renderer stays behind the existing abstraction boundary.

The streaming runner creates one subtitle render session and uses it per frame:

- `main_segment_only`: render timestamps come from the main segment's decoded frame timestamps
- `full_output_timeline`: render timestamps come from the rebased output-timeline timestamps, so intro and outro frames participate too

No full subtitle-burned clip is created anymore. Bitmaps are rendered, composited into one frame, encoded, and then released.

## Audio In The Current Milestone

Audio is still decoded incrementally even though the muxed output remains video-only.

That preserves the existing behavior needed for validation and reporting:

- Segment audio compatibility is still checked
- Audio block counts are still reported
- Silence insertion accounting for clips without audio still matches the existing timeline summary

Decoded audio blocks are not retained after validation/counting.

## Maintenance Notes

When changing this pipeline:

- Keep queue ownership single-owner and move-only.
- Do not reintroduce vectors of whole decoded segments into the active job path.
- Keep subtitle rendering on the frame path, not as a pre-rendered clip transform.
- Keep segment sequencing generic so intro/main/outro all use the same pipeline code.
- If queue depths change, update both `kDefaultPipelineQueueLimits` and the memory-budget explanation above.
