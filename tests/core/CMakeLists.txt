find_program(UTSURE_FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)

set(UTSURE_TEST_MEDIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/media")
set(UTSURE_SAMPLE_MEDIA_FILE "${UTSURE_TEST_MEDIA_DIR}/inspection-sample.avi")
set(UTSURE_PREVIEW_SESSION_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/preview-session-sample.mp4")
set(UTSURE_MEMORY_HEAVY_MAIN_FILE "${UTSURE_TEST_MEDIA_DIR}/memory-heavy-main.mp4")
set(UTSURE_TIMELINE_INTRO_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-intro.avi")
set(UTSURE_TIMELINE_OUTRO_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-outro.avi")
set(UTSURE_TIMELINE_BAD_FPS_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-bad-fps.avi")
set(UTSURE_NTSC_COARSE_TIMEBASE_FILE "${UTSURE_TEST_MEDIA_DIR}/ntsc-coarse-timebase.mp4")
set(UTSURE_IRREGULAR_TIMESTAMP_FILE "${UTSURE_TEST_MEDIA_DIR}/irregular-timestamp-main.mp4")
set(UTSURE_SUBTITLE_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-sample.ass")
set(UTSURE_SUBTITLE_GRADIENT_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-gradient-sample.ass")
set(UTSURE_SUBTITLE_IMG_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-img-sample.ass")

file(MAKE_DIRECTORY "${UTSURE_TEST_MEDIA_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-sample.ass"
    "${UTSURE_SUBTITLE_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-gradient-sample.ass"
    "${UTSURE_SUBTITLE_GRADIENT_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-img-sample.ass"
    "${UTSURE_SUBTITLE_IMG_SAMPLE_FILE}"
    COPYONLY
)

add_custom_command(
    OUTPUT "${UTSURE_SAMPLE_MEDIA_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc=size=320x180:rate=24"
        -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=2"
        -t 2
        -c:v rawvideo
        -pix_fmt yuv420p
        -c:a pcm_s16le
        "${UTSURE_SAMPLE_MEDIA_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc=size=320x180:rate=24"
        -frames:v 144
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -an
        "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_MEMORY_HEAVY_MAIN_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=black:size=1920x1080:rate=24"
        -t 8
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -an
        "${UTSURE_MEMORY_HEAVY_MAIN_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_TIMELINE_INTRO_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=red:size=320x180:rate=24"
        -f lavfi -i "sine=frequency=600:sample_rate=48000:duration=1"
        -t 1
        -c:v rawvideo
        -pix_fmt yuv420p
        -c:a pcm_s16le
        "${UTSURE_TIMELINE_INTRO_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_TIMELINE_OUTRO_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=blue:size=320x180:rate=24"
        -t 1
        -c:v rawvideo
        -pix_fmt yuv420p
        "${UTSURE_TIMELINE_OUTRO_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_TIMELINE_BAD_FPS_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=yellow:size=320x180:rate=30"
        -f lavfi -i "sine=frequency=900:sample_rate=48000:duration=1"
        -t 1
        -c:v rawvideo
        -pix_fmt yuv420p
        -c:a pcm_s16le
        "${UTSURE_TIMELINE_BAD_FPS_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc=size=320x180:rate=24000/1001"
        -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=3"
        -frames:v 48
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -video_track_timescale 1000
        -c:a aac
        -b:a 128k
        -shortest
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_IRREGULAR_TIMESTAMP_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc=size=320x180:rate=48"
        -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=4"
        -vf "select='if(eq(n,0),1,eq(mod(n,2),0)*not(eq(mod(n,14),0)))'"
        -t 4
        -fps_mode vfr
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a 128k
        -shortest
        "${UTSURE_IRREGULAR_TIMESTAMP_FILE}"
    VERBATIM
)

add_custom_target(
    utsure_generate_sample_media
    DEPENDS
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
        "${UTSURE_MEMORY_HEAVY_MAIN_FILE}"
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
        "${UTSURE_TIMELINE_BAD_FPS_FILE}"
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
        "${UTSURE_IRREGULAR_TIMESTAMP_FILE}"
)

add_executable(utsure_core_media_inspection_tests
    media_inspection_tests.cpp
)

target_link_libraries(utsure_core_media_inspection_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_media_inspection_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_media_inspection_tests)

add_dependencies(utsure_core_media_inspection_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.media.inspect.sample
    COMMAND utsure_core_media_inspection_tests --sample "${UTSURE_SAMPLE_MEDIA_FILE}"
)

add_test(
    NAME utsure.core.media.inspect.missing
    COMMAND utsure_core_media_inspection_tests --missing "${UTSURE_TEST_MEDIA_DIR}/missing-input.avi"
)

add_executable(utsure_core_media_decode_tests
    media_decode_tests.cpp
)

target_link_libraries(utsure_core_media_decode_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_media_decode_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_media_decode_tests)

add_dependencies(utsure_core_media_decode_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.media.decode.sample
    COMMAND utsure_core_media_decode_tests --sample "${UTSURE_SAMPLE_MEDIA_FILE}"
)

add_test(
    NAME utsure.core.media.decode.missing
    COMMAND utsure_core_media_decode_tests --missing "${UTSURE_TEST_MEDIA_DIR}/missing-input.avi"
)

add_test(
    NAME utsure.core.media.preview_session.sequential
    COMMAND utsure_core_media_decode_tests --preview-session-sequential "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
)

add_executable(utsure_core_media_encode_tests
    media_encode_tests.cpp
)

target_link_libraries(utsure_core_media_encode_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_media_encode_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_media_encode_tests)

add_dependencies(utsure_core_media_encode_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.media.encode.h264
    COMMAND utsure_core_media_encode_tests --h264
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/encode-sample-h264.mp4"
)

add_test(
    NAME utsure.core.media.encode.h265
    COMMAND utsure_core_media_encode_tests --h265
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/encode-sample-h265.mp4"
)

add_executable(utsure_core_encode_job_tests
    encode_job_tests.cpp
)

target_link_libraries(utsure_core_encode_job_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_encode_job_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_encode_job_tests)

add_dependencies(utsure_core_encode_job_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.job.encode.threading_modes
    COMMAND utsure_core_encode_job_tests --threading-modes
)

add_test(
    NAME utsure.core.job.encode.h264
    COMMAND utsure_core_encode_job_tests --h264
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-sample-h264.mp4"
)

add_test(
    NAME utsure.core.job.encode.h265
    COMMAND utsure_core_encode_job_tests --h265
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-sample-h265.mp4"
)

add_test(
    NAME utsure.core.job.encode.timeline.h264
    COMMAND utsure_core_encode_job_tests --timeline-h264
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-timeline-h264.mp4"
)

add_test(
    NAME utsure.core.job.encode.streaming_memory_budget
    COMMAND utsure_core_encode_job_tests --streaming-memory-budget
        "${UTSURE_MEMORY_HEAVY_MAIN_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-memory-heavy.mp4"
)

add_test(
    NAME utsure.core.job.encode.disable_audio
    COMMAND utsure_core_encode_job_tests --disable-audio
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-disable-audio.mp4"
)

add_test(
    NAME utsure.core.job.encode.copy_audio
    COMMAND utsure_core_encode_job_tests --copy-audio
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-copy-audio.mp4"
)

add_test(
    NAME utsure.core.job.encode.ntsc_coarse_timebase
    COMMAND utsure_core_encode_job_tests --coarse-timebase-ntsc
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-ntsc-coarse-timebase.mp4"
)

add_test(
    NAME utsure.core.job.encode.irregular_timestamps
    COMMAND utsure_core_encode_job_tests --irregular-timestamps
        "${UTSURE_IRREGULAR_TIMESTAMP_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-irregular-timestamps.mp4"
)

add_executable(utsure_core_encode_job_preflight_tests
    encode_job_preflight_tests.cpp
)

target_link_libraries(utsure_core_encode_job_preflight_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_encode_job_preflight_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_encode_job_preflight_tests)

add_dependencies(utsure_core_encode_job_preflight_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.job.preflight.valid_preview
    COMMAND utsure_core_encode_job_preflight_tests --valid-preview
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-valid.mp4"
)

add_test(
    NAME utsure.core.job.preflight.invalid_fps
    COMMAND utsure_core_encode_job_preflight_tests --invalid-fps
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_BAD_FPS_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-invalid-fps.mp4"
)

add_test(
    NAME utsure.core.job.preflight.missing_subtitle
    COMMAND utsure_core_encode_job_preflight_tests --missing-subtitle
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/missing-subtitle.ass"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-missing-subtitle.mp4"
)

add_test(
    NAME utsure.core.job.preflight.overwrite_warning
    COMMAND utsure_core_encode_job_preflight_tests --overwrite-warning
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-overwrite.mp4"
)

add_test(
    NAME utsure.core.job.preflight.output_conflicts_main
    COMMAND utsure_core_encode_job_preflight_tests --output-conflicts-main
        "${UTSURE_SAMPLE_MEDIA_FILE}"
)

add_test(
    NAME utsure.core.job.preflight.streaming_memory_budget
    COMMAND utsure_core_encode_job_preflight_tests --streaming-memory-budget
        "${UTSURE_MEMORY_HEAVY_MAIN_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-memory-heavy.mp4"
)

add_test(
    NAME utsure.core.job.preflight.copy_blocked
    COMMAND utsure_core_encode_job_preflight_tests --copy-blocked
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-copy-blocked.mp4"
)

add_test(
    NAME utsure.core.job.preflight.auto_copy_preview
    COMMAND utsure_core_encode_job_preflight_tests --auto-copy-preview
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-auto-copy.mp4"
)

add_executable(utsure_core_timeline_tests
    timeline_tests.cpp
)

target_link_libraries(utsure_core_timeline_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_timeline_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_timeline_tests)

add_dependencies(utsure_core_timeline_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.timeline.main_only
    COMMAND utsure_core_timeline_tests --main-only
        "${UTSURE_SAMPLE_MEDIA_FILE}"
)

add_test(
    NAME utsure.core.timeline.intro_main
    COMMAND utsure_core_timeline_tests --intro-main
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
)

add_test(
    NAME utsure.core.timeline.main_outro
    COMMAND utsure_core_timeline_tests --main-outro
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
)

add_test(
    NAME utsure.core.timeline.intro_main_outro
    COMMAND utsure_core_timeline_tests --intro-main-outro
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
)

add_test(
    NAME utsure.core.timeline.subtitle_scope
    COMMAND utsure_core_timeline_tests --subtitle-scope
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
)

add_test(
    NAME utsure.core.timeline.mismatch_fps
    COMMAND utsure_core_timeline_tests --mismatch-fps
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_BAD_FPS_FILE}"
)

add_executable(utsure_core_subtitle_renderer_tests
    subtitle_renderer_tests.cpp
)

target_link_libraries(utsure_core_subtitle_renderer_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_subtitle_renderer_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_subtitle_renderer_tests)

add_test(
    NAME utsure.core.subtitles.renderer.flow
    COMMAND utsure_core_subtitle_renderer_tests --flow
)

add_test(
    NAME utsure.core.subtitles.renderer.error
    COMMAND utsure_core_subtitle_renderer_tests --error
)

add_executable(utsure_core_subtitle_bitmap_compositor_tests
    subtitle_bitmap_compositor_tests.cpp
)

target_link_libraries(utsure_core_subtitle_bitmap_compositor_tests
    PRIVATE
        utsure::encoder_core
)

target_include_directories(utsure_core_subtitle_bitmap_compositor_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/core/src"
)

target_compile_features(utsure_core_subtitle_bitmap_compositor_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_subtitle_bitmap_compositor_tests)

add_test(
    NAME utsure.core.subtitles.compositor.premultiplied_rgba
    COMMAND utsure_core_subtitle_bitmap_compositor_tests --premultiplied
)

add_executable(utsure_core_subtitle_burn_in_tests
    subtitle_burn_in_tests.cpp
)

target_link_libraries(utsure_core_subtitle_burn_in_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_subtitle_burn_in_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_subtitle_burn_in_tests)

add_dependencies(utsure_core_subtitle_burn_in_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.subtitles.libassmod.render
    COMMAND utsure_core_subtitle_burn_in_tests --render "${UTSURE_SUBTITLE_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.gradient_rgba
    COMMAND utsure_core_subtitle_burn_in_tests --render-gradient "${UTSURE_SUBTITLE_GRADIENT_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.img_unsupported
    COMMAND utsure_core_subtitle_burn_in_tests --render-unsupported-img "${UTSURE_SUBTITLE_IMG_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.burn_in.h264
    COMMAND utsure_core_subtitle_burn_in_tests --h264
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-plain-h264.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-h264.mp4"
)

add_test(
    NAME utsure.core.subtitles.burn_in.gradient.h264
    COMMAND utsure_core_subtitle_burn_in_tests --h264
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_SUBTITLE_GRADIENT_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-gradient-plain-h264.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-gradient-burn-h264.mp4"
)

add_test(
    NAME utsure.core.subtitles.burn_in.h265
    COMMAND utsure_core_subtitle_burn_in_tests --h265
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-plain-h265.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-h265.mp4"
)

add_test(
    NAME utsure.core.subtitles.burn_in.timeline.h264
    COMMAND utsure_core_subtitle_burn_in_tests --timeline-h264
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/timeline-subtitle-plain-h264.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/timeline-subtitle-burn-h264.mp4"
)

add_test(
    NAME utsure.core.subtitles.burn_in.timeline.full_output.h264
    COMMAND utsure_core_subtitle_burn_in_tests --timeline-full-h264
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/timeline-full-subtitle-plain-h264.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/timeline-full-subtitle-burn-h264.mp4"
)
