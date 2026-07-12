find_program(UTSURE_FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)

set(UTSURE_TEST_MEDIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/media")
set(UTSURE_SAMPLE_MEDIA_FILE "${UTSURE_TEST_MEDIA_DIR}/inspection-sample.avi")
set(UTSURE_PREVIEW_SESSION_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/preview-session-sample.mp4")
set(UTSURE_MEMORY_HEAVY_MAIN_FILE "${UTSURE_TEST_MEDIA_DIR}/memory-heavy-main.mp4")
set(UTSURE_TIMELINE_INTRO_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-intro.avi")
set(UTSURE_TIMELINE_LONG_INTRO_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-long-intro.avi")
set(UTSURE_TIMELINE_OUTRO_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-outro.avi")
set(UTSURE_TIMELINE_BAD_FPS_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-bad-fps.avi")
set(UTSURE_TIMELINE_NORMALIZED_INTRO_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-normalized-intro.mp4")
set(UTSURE_TIMELINE_NORMALIZED_OUTRO_FILE "${UTSURE_TEST_MEDIA_DIR}/timeline-normalized-outro.mp4")
set(UTSURE_NTSC_COARSE_TIMEBASE_FILE "${UTSURE_TEST_MEDIA_DIR}/ntsc-coarse-timebase.mp4")
set(UTSURE_IRREGULAR_TIMESTAMP_FILE "${UTSURE_TEST_MEDIA_DIR}/irregular-timestamp-main.mp4")
set(UTSURE_TRIM_SAR_SEEK_REPRO_FILE "${UTSURE_TEST_MEDIA_DIR}/trim-sar-seek-repro.mp4")
set(UTSURE_TRIM_SAR_SEEK_REPRO_SOURCE_FILE "${UTSURE_TEST_MEDIA_DIR}/trim-sar-seek-repro-source.mp4")
set(UTSURE_MULTI_AUDIO_JAPANESE_FILE "${UTSURE_TEST_MEDIA_DIR}/multi-audio-japanese.mkv")
set(UTSURE_MULTI_AUDIO_DEFAULT_FILE "${UTSURE_TEST_MEDIA_DIR}/multi-audio-default.mp4")
set(UTSURE_MULTI_AUDIO_FALLBACK_FILE "${UTSURE_TEST_MEDIA_DIR}/multi-audio-fallback.mkv")
set(UTSURE_SUBTITLE_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-sample.ass")
set(UTSURE_SUBTITLE_GRADIENT_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-gradient-sample.ass")
set(UTSURE_SUBTITLE_BS4_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-bs4-sample.ass")
set(UTSURE_SUBTITLE_BS4_GRADIENT_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-bs4-gradient-sample.ass")
set(UTSURE_SUBTITLE_BS4_FORCED_RGBA_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-bs4-forced-rgba-sample.ass")
set(UTSURE_SUBTITLE_EMPTY_BITMAP_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-empty-bitmap-sample.ass")
set(UTSURE_SUBTITLE_IMG_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-img-sample.ass")
set(UTSURE_SUBTITLE_STRESS_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-stress-sample.ass")
set(UTSURE_SUBTITLE_OUTPUT_TIMELINE_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-output-timeline-sample.ass")
set(UTSURE_SUBTITLE_TRANSFORM_SCHEDULE_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-transform-schedule-sample.ass")
set(UTSURE_SUBTITLE_MOVE_SCHEDULE_SAMPLE_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-move-schedule-sample.ass")
set(UTSURE_SUBTITLE_STRESS_MAIN_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-main.mp4")
set(UTSURE_SUBTITLE_SPEED_15X_MAIN_FILE "${UTSURE_TEST_MEDIA_DIR}/subtitle-speed-15x-main.mp4")
set(UTSURE_THUMBNAIL_PREROLL_IMAGE_FILE "${UTSURE_TEST_MEDIA_DIR}/thumbnail.png")
set(UTSURE_THUMBNAIL_PREROLL_LARGE_IMAGE_FILE "${UTSURE_TEST_MEDIA_DIR}/thumbnail-large.png")
set(UTSURE_THUMBNAIL_PREROLL_4_3_IMAGE_FILE "${UTSURE_TEST_MEDIA_DIR}/thumbnail-4x3.png")
set(UTSURE_THUMBNAIL_PREROLL_SUBTITLE_FILE "${UTSURE_TEST_MEDIA_DIR}/thumbnail-source.ass")
set(UTSURE_THUMBNAIL_PREROLL_OVERLAY_FILE "${UTSURE_TEST_MEDIA_DIR}/thumbnail.ass")

file(MAKE_DIRECTORY "${UTSURE_TEST_MEDIA_DIR}")
if(WIN32)
    set(UTSURE_TEST_FONTCOLLECTOR_STUB "${CMAKE_CURRENT_BINARY_DIR}/fontcollector-test-stub.cmd")
    file(WRITE "${UTSURE_TEST_FONTCOLLECTOR_STUB}"
        "@echo off\r\n"
        "setlocal EnableExtensions\r\n"
        "set \"LOG_PATH=\"\r\n"
        ":parse\r\n"
        "if \"%~1\"==\"\" goto parsed\r\n"
        "if /I \"%~1\"==\"--logging\" (\r\n"
        "  set \"LOG_PATH=%~2\"\r\n"
        "  shift\r\n"
        "  shift\r\n"
        "  goto parse\r\n"
        ")\r\n"
        "shift\r\n"
        "goto parse\r\n"
        ":parsed\r\n"
        "if not \"%LOG_PATH%\"==\"\" echo fontcollector ctest stub>\"%LOG_PATH%\"\r\n"
        "exit /b 0\r\n"
    )
else()
    set(UTSURE_TEST_FONTCOLLECTOR_STUB "${CMAKE_CURRENT_BINARY_DIR}/fontcollector-test-stub.sh")
    file(WRITE "${UTSURE_TEST_FONTCOLLECTOR_STUB}"
        "#!/bin/sh\n"
        "LOG_PATH=\"\"\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  case \"$1\" in\n"
        "    --logging)\n"
        "      LOG_PATH=\"$2\"\n"
        "      shift 2\n"
        "      ;;\n"
        "    *)\n"
        "      shift\n"
        "      ;;\n"
        "  esac\n"
        "done\n"
        "if [ -n \"$LOG_PATH\" ]; then printf \"fontcollector ctest stub\" > \"$LOG_PATH\"; fi\n"
        "exit 0\n"
    )
    file(CHMOD "${UTSURE_TEST_FONTCOLLECTOR_STUB}"
        FILE_PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
    )
endif()

function(utsure_enable_fontcollector_stub_for_test test_name)
    set_tests_properties("${test_name}" PROPERTIES
        ENVIRONMENT "UTSURE_FONTCOLLECTOR_PATH=${UTSURE_TEST_FONTCOLLECTOR_STUB}"
    )
endfunction()

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
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-bs4-sample.ass"
    "${UTSURE_SUBTITLE_BS4_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-bs4-gradient-sample.ass"
    "${UTSURE_SUBTITLE_BS4_GRADIENT_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-bs4-forced-rgba-sample.ass"
    "${UTSURE_SUBTITLE_BS4_FORCED_RGBA_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-empty-bitmap-sample.ass"
    "${UTSURE_SUBTITLE_EMPTY_BITMAP_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-img-sample.ass"
    "${UTSURE_SUBTITLE_IMG_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-stress-sample.ass"
    "${UTSURE_SUBTITLE_STRESS_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-output-timeline-sample.ass"
    "${UTSURE_SUBTITLE_OUTPUT_TIMELINE_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-transform-schedule-sample.ass"
    "${UTSURE_SUBTITLE_TRANSFORM_SCHEDULE_SAMPLE_FILE}"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/subtitle-burn-move-schedule-sample.ass"
    "${UTSURE_SUBTITLE_MOVE_SCHEDULE_SAMPLE_FILE}"
    COPYONLY
)

file(WRITE "${UTSURE_THUMBNAIL_PREROLL_SUBTITLE_FILE}"
    "[Script Info]\n"
    "Title: thumbnail source placeholder\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
)
file(WRITE "${UTSURE_THUMBNAIL_PREROLL_OVERLAY_FILE}"
    "[Script Info]\n"
    "Title: thumbnail overlay\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 320\n"
    "PlayResY: 180\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,28,&H00FFFFFF,&H000000FF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,1,0,5,10,10,10,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:05.00,Default,utsure_data,0,0,0,,{\\an5}Episode 06\n"
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
        -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=6"
        -frames:v 144
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a 128k
        -shortest
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
    OUTPUT "${UTSURE_TIMELINE_LONG_INTRO_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=red:size=320x180:rate=24"
        -f lavfi -i "sine=frequency=600:sample_rate=48000:duration=10"
        -t 10
        -c:v rawvideo
        -pix_fmt yuv420p
        -c:a pcm_s16le
        "${UTSURE_TIMELINE_LONG_INTRO_FILE}"
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
    OUTPUT "${UTSURE_TIMELINE_NORMALIZED_INTRO_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=magenta:size=640x360:rate=30,setsar=4/3"
        -f lavfi -i "sine=frequency=700:sample_rate=44100:duration=1"
        -t 1
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a 128k
        -ac 2
        -shortest
        "${UTSURE_TIMELINE_NORMALIZED_INTRO_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_TIMELINE_NORMALIZED_OUTRO_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=cyan:size=160x90:rate=25,setsar=3/2"
        -f lavfi -i "sine=frequency=350:sample_rate=32000:duration=1"
        -t 1
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a 96k
        -ac 2
        -shortest
        "${UTSURE_TIMELINE_NORMALIZED_OUTRO_FILE}"
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

add_custom_command(
    OUTPUT "${UTSURE_TRIM_SAR_SEEK_REPRO_FILE}"
    BYPRODUCTS "${UTSURE_TRIM_SAR_SEEK_REPRO_SOURCE_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc=size=320x180:rate=24,setsar=4/3"
        -f lavfi -i "sine=frequency=730:sample_rate=48000:duration=2"
        -t 2
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -g 48
        -keyint_min 48
        -sc_threshold 0
        -c:a aac
        -b:a 128k
        -shortest
        "${UTSURE_TRIM_SAR_SEEK_REPRO_SOURCE_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -i "${UTSURE_TRIM_SAR_SEEK_REPRO_SOURCE_FILE}"
        -map 0:v:0
        -map 0:a:0
        -c copy
        -aspect 16:9
        "${UTSURE_TRIM_SAR_SEEK_REPRO_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_MULTI_AUDIO_JAPANESE_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc=size=320x180:rate=24"
        -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2"
        -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=2"
        -map 0:v:0
        -map 1:a:0
        -map 2:a:0
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a:0 128k
        -b:a:1 96k
        -ac:a:0 2
        -ac:a:1 1
        -metadata:s:a:0 language=eng
        -metadata:s:a:0 title=English
        -disposition:a:0 default
        -metadata:s:a:1 language=jpn
        -metadata:s:a:1 title=Japanese
        -disposition:a:1 0
        -shortest
        "${UTSURE_MULTI_AUDIO_JAPANESE_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_MULTI_AUDIO_DEFAULT_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc=size=320x180:rate=24"
        -f lavfi -i "sine=frequency=520:sample_rate=48000:duration=2"
        -f lavfi -i "sine=frequency=1040:sample_rate=48000:duration=2"
        -map 0:v:0
        -map 1:a:0
        -map 2:a:0
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a:0 96k
        -b:a:1 128k
        -ac:a:0 1
        -ac:a:1 2
        -metadata:s:a:0 language=eng
        -metadata:s:a:0 title=English
        -disposition:a:0 0
        -metadata:s:a:1 language=deu
        -metadata:s:a:1 title=Deutsch
        -disposition:a:1 default
        -shortest
        "${UTSURE_MULTI_AUDIO_DEFAULT_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_MULTI_AUDIO_FALLBACK_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc=size=320x180:rate=24"
        -f lavfi -i "sine=frequency=610:sample_rate=48000:duration=2"
        -f lavfi -i "sine=frequency=1220:sample_rate=48000:duration=2"
        -map 0:v:0
        -map 1:a:0
        -map 2:a:0
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a:0 96k
        -b:a:1 128k
        -ac:a:0 1
        -ac:a:1 2
        -disposition:a:0 0
        -disposition:a:1 0
        -shortest
        "${UTSURE_MULTI_AUDIO_FALLBACK_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_SUBTITLE_STRESS_MAIN_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc2=size=320x180:rate=24"
        -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=20"
        -t 20
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a 128k
        -shortest
        "${UTSURE_SUBTITLE_STRESS_MAIN_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_SUBTITLE_SPEED_15X_MAIN_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc2=size=320x180:rate=24"
        -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=9"
        -t 9
        -filter_complex "[0:v]setpts=PTS/1.5[v];[1:a]atempo=1.5[a]"
        -map "[v]"
        -map "[a]"
        -c:v libx264
        -preset ultrafast
        -pix_fmt yuv420p
        -c:a aac
        -b:a 128k
        -shortest
        "${UTSURE_SUBTITLE_SPEED_15X_MAIN_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_THUMBNAIL_PREROLL_IMAGE_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=black:size=320x180"
        -frames:v 1
        "${UTSURE_THUMBNAIL_PREROLL_IMAGE_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_THUMBNAIL_PREROLL_LARGE_IMAGE_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=black:size=1920x1080"
        -frames:v 1
        "${UTSURE_THUMBNAIL_PREROLL_LARGE_IMAGE_FILE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${UTSURE_THUMBNAIL_PREROLL_4_3_IMAGE_FILE}"
    COMMAND "${UTSURE_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
        -f lavfi -i "color=c=black:size=640x480"
        -frames:v 1
        "${UTSURE_THUMBNAIL_PREROLL_4_3_IMAGE_FILE}"
    VERBATIM
)

add_custom_target(
    utsure_generate_sample_media
    DEPENDS
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
        "${UTSURE_MEMORY_HEAVY_MAIN_FILE}"
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_TIMELINE_LONG_INTRO_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
        "${UTSURE_TIMELINE_BAD_FPS_FILE}"
        "${UTSURE_TIMELINE_NORMALIZED_INTRO_FILE}"
        "${UTSURE_TIMELINE_NORMALIZED_OUTRO_FILE}"
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
        "${UTSURE_IRREGULAR_TIMESTAMP_FILE}"
        "${UTSURE_TRIM_SAR_SEEK_REPRO_FILE}"
        "${UTSURE_MULTI_AUDIO_JAPANESE_FILE}"
        "${UTSURE_MULTI_AUDIO_DEFAULT_FILE}"
        "${UTSURE_MULTI_AUDIO_FALLBACK_FILE}"
        "${UTSURE_SUBTITLE_STRESS_MAIN_FILE}"
        "${UTSURE_SUBTITLE_SPEED_15X_MAIN_FILE}"
        "${UTSURE_THUMBNAIL_PREROLL_IMAGE_FILE}"
        "${UTSURE_THUMBNAIL_PREROLL_LARGE_IMAGE_FILE}"
        "${UTSURE_THUMBNAIL_PREROLL_4_3_IMAGE_FILE}"
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

add_test(
    NAME utsure.core.media.inspect.multi_audio_japanese
    COMMAND utsure_core_media_inspection_tests --multi-audio-japanese "${UTSURE_MULTI_AUDIO_JAPANESE_FILE}"
)

add_test(
    NAME utsure.core.media.inspect.multi_audio_default
    COMMAND utsure_core_media_inspection_tests --multi-audio-default "${UTSURE_MULTI_AUDIO_DEFAULT_FILE}"
)

add_test(
    NAME utsure.core.media.inspect.multi_audio_fallback
    COMMAND utsure_core_media_inspection_tests --multi-audio-fallback "${UTSURE_MULTI_AUDIO_FALLBACK_FILE}"
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

add_test(
    NAME utsure.core.media.preview_session.seek_churn
    COMMAND utsure_core_media_decode_tests --preview-session-seek-churn "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.media.preview_audio_session.sequential
    COMMAND utsure_core_media_decode_tests --preview-audio-session "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.media.preview_video_trim_window
    COMMAND utsure_core_media_decode_tests --preview-video-trim-window "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.media.preview_video_end_selection
    COMMAND utsure_core_media_decode_tests --preview-video-end-selection "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.media.preview_audio_trim_window
    COMMAND utsure_core_media_decode_tests --preview-audio-trim-window "${UTSURE_PREVIEW_SESSION_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.media.decode.multi_audio_selected
    COMMAND utsure_core_media_decode_tests --multi-audio-selected "${UTSURE_MULTI_AUDIO_JAPANESE_FILE}"
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

target_include_directories(utsure_core_encode_job_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/core/src"
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
    NAME utsure.core.job.encode.thumbnail_preroll
    COMMAND utsure_core_encode_job_tests --thumbnail-preroll
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-thumbnail-preroll.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.job.encode.thumbnail_preroll)

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
    NAME utsure.core.job.encode.timeline.normalized_common
    COMMAND utsure_core_encode_job_tests --timeline-normalized-common
        "${UTSURE_TIMELINE_NORMALIZED_INTRO_FILE}"
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
        "${UTSURE_TIMELINE_NORMALIZED_OUTRO_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-timeline-normalized-common.mp4"
)

add_test(
    NAME utsure.core.job.encode.trim_main
    COMMAND utsure_core_encode_job_tests --trim-main
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-trim-main.mp4"
)

add_test(
    NAME utsure.core.job.encode.trim_main.sar_seek_repro
    COMMAND utsure_core_encode_job_tests --trim-main-sar-seek-repro
        "${UTSURE_TRIM_SAR_SEEK_REPRO_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-trim-main-sar-seek-repro.mp4"
)

add_test(
    NAME utsure.core.job.encode.timeline.trim_main
    COMMAND utsure_core_encode_job_tests --timeline-trim-main
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-timeline-trim-main.mp4"
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
    NAME utsure.core.job.encode.copy_audio_fallback
    COMMAND utsure_core_encode_job_tests --copy-audio-fallback
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-copy-audio-fallback.mp4"
)

add_test(
    NAME utsure.core.job.encode.partial_cleanup_cancel
    COMMAND utsure_core_encode_job_tests --partial-cleanup-cancel
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-partial-cleanup-cancel.mp4"
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

add_test(
    NAME utsure.core.job.encode.multi_audio_selected
    COMMAND utsure_core_encode_job_tests --multi-audio-selected
        "${UTSURE_MULTI_AUDIO_JAPANESE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/job-multi-audio-selected.mp4"
)

add_executable(utsure_core_ffmpeg_filter_hardsub_backend_tests
    ffmpeg_filter_hardsub_backend_tests.cpp
)

target_link_libraries(utsure_core_ffmpeg_filter_hardsub_backend_tests
    PRIVATE
        utsure::encoder_core
)

target_include_directories(utsure_core_ffmpeg_filter_hardsub_backend_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/core/src"
)

target_compile_features(utsure_core_ffmpeg_filter_hardsub_backend_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_ffmpeg_filter_hardsub_backend_tests)

add_test(
    NAME utsure.core.job.ffmpeg_filter_hardsub.escape
    COMMAND $<TARGET_FILE:utsure_core_ffmpeg_filter_hardsub_backend_tests> --escape
)

add_test(
    NAME utsure.core.job.ffmpeg_filter_hardsub.command
    COMMAND $<TARGET_FILE:utsure_core_ffmpeg_filter_hardsub_backend_tests> --command
)

set_tests_properties(utsure.core.job.ffmpeg_filter_hardsub.command PROPERTIES
    ENVIRONMENT "UTSURE_FFMPEG_PATH=${UTSURE_FFMPEG_EXECUTABLE}"
)

add_test(
    NAME utsure.core.job.ffmpeg_filter_hardsub.strict_same_thread
    COMMAND $<TARGET_FILE:utsure_core_ffmpeg_filter_hardsub_backend_tests> --strict-same-thread
)

set_tests_properties(utsure.core.job.ffmpeg_filter_hardsub.strict_same_thread PROPERTIES
    ENVIRONMENT "UTSURE_FFMPEG_PATH=${UTSURE_FFMPEG_EXECUTABLE};UTSURE_SUBTITLE_STRICT_SAME_THREAD=1"
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
utsure_enable_fontcollector_stub_for_test(utsure.core.job.preflight.valid_preview)

add_test(
    NAME utsure.core.job.preflight.mismatched_fps_preview
    COMMAND utsure_core_encode_job_preflight_tests --mismatched-fps-preview
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
    NAME utsure.core.job.preflight.invalid_trim
    COMMAND utsure_core_encode_job_preflight_tests --invalid-trim
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-invalid-trim.mp4"
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
    NAME utsure.core.job.preflight.copy_fallback_warning
    COMMAND utsure_core_encode_job_preflight_tests --copy-fallback-warning
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-copy-fallback.mp4"
)

add_test(
    NAME utsure.core.job.preflight.auto_copy_preview
    COMMAND utsure_core_encode_job_preflight_tests --auto-copy-preview
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-auto-copy.mp4"
)

add_test(
    NAME utsure.core.job.preflight.thumbnail_preroll
    COMMAND utsure_core_encode_job_preflight_tests --thumbnail-preroll
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/preflight-thumbnail-preroll.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.job.preflight.thumbnail_preroll)

add_executable(utsure_core_output_naming_tests
    output_naming_tests.cpp
)

target_link_libraries(utsure_core_output_naming_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_output_naming_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_output_naming_tests)

add_test(
    NAME utsure.core.job.output_naming
    COMMAND utsure_core_output_naming_tests
)

add_executable(utsure_core_resize_tests
    resize_tests.cpp
)

target_link_libraries(utsure_core_resize_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_resize_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_resize_tests)

add_test(
    NAME utsure.core.job.resize
    COMMAND $<TARGET_FILE:utsure_core_resize_tests>
)

add_executable(utsure_core_audio_stream_selection_tests
    audio_stream_selection_tests.cpp
)

target_link_libraries(utsure_core_audio_stream_selection_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_audio_stream_selection_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_audio_stream_selection_tests)

add_test(
    NAME utsure.core.media.audio_stream_selection
    COMMAND $<TARGET_FILE:utsure_core_audio_stream_selection_tests>
)

add_executable(utsure_core_source_import_paths_tests
    source_import_paths_tests.cpp
)

target_link_libraries(utsure_core_source_import_paths_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_source_import_paths_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_source_import_paths_tests)

add_test(
    NAME utsure.core.media.source_import_paths
    COMMAND utsure_core_source_import_paths_tests
)

add_executable(utsure_core_batch_parallelism_tests
    batch_parallelism_tests.cpp
)

target_link_libraries(utsure_core_batch_parallelism_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_batch_parallelism_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_batch_parallelism_tests)

add_test(
    NAME utsure.core.job.batch_parallelism
    COMMAND utsure_core_batch_parallelism_tests
)

add_executable(utsure_core_subtitle_auto_selection_tests
    subtitle_auto_selection_tests.cpp
)

target_link_libraries(utsure_core_subtitle_auto_selection_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_subtitle_auto_selection_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_subtitle_auto_selection_tests)

add_test(
    NAME utsure.core.subtitles.auto_selection
    COMMAND utsure_core_subtitle_auto_selection_tests
)

add_executable(utsure_core_subtitle_image_assets_tests
    subtitle_image_assets_tests.cpp
)

target_link_libraries(utsure_core_subtitle_image_assets_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_subtitle_image_assets_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_subtitle_image_assets_tests)

add_dependencies(utsure_core_subtitle_image_assets_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.subtitles.image_assets.scan
    COMMAND utsure_core_subtitle_image_assets_tests --scan
)

add_test(
    NAME utsure.core.subtitles.image_assets.resolve_decode
    COMMAND utsure_core_subtitle_image_assets_tests --resolve-decode "${UTSURE_THUMBNAIL_PREROLL_IMAGE_FILE}"
)

add_executable(utsure_core_thumbnail_preroll_tests
    thumbnail_preroll_tests.cpp
)

target_link_libraries(utsure_core_thumbnail_preroll_tests
    PRIVATE
        utsure::encoder_core
)

target_compile_features(utsure_core_thumbnail_preroll_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_thumbnail_preroll_tests)

add_dependencies(utsure_core_thumbnail_preroll_tests utsure_generate_sample_media)

add_test(
    NAME utsure.core.subtitles.thumbnail_preroll.parse
    COMMAND utsure_core_thumbnail_preroll_tests --parse
)

add_test(
    NAME utsure.core.subtitles.thumbnail_preroll.resolve
    COMMAND utsure_core_thumbnail_preroll_tests --resolve
        "${UTSURE_THUMBNAIL_PREROLL_SUBTITLE_FILE}"
        "${UTSURE_THUMBNAIL_PREROLL_LARGE_IMAGE_FILE}"
        "${UTSURE_THUMBNAIL_PREROLL_4_3_IMAGE_FILE}"
)

add_executable(utsure_core_subtitle_font_recovery_tests
    subtitle_font_recovery_tests.cpp
)

target_link_libraries(utsure_core_subtitle_font_recovery_tests
    PRIVATE
        utsure::encoder_core
)

target_include_directories(utsure_core_subtitle_font_recovery_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/core/src"
)

target_compile_features(utsure_core_subtitle_font_recovery_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_subtitle_font_recovery_tests)

add_test(
    NAME utsure.core.subtitles.font_recovery
    COMMAND utsure_core_subtitle_font_recovery_tests
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
    NAME utsure.core.timeline.normalized_fps
    COMMAND utsure_core_timeline_tests --normalized-fps
        "${UTSURE_TIMELINE_BAD_FPS_FILE}"
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
)

add_test(
    NAME utsure.core.timeline.normalized_common
    COMMAND utsure_core_timeline_tests --normalized-common
        "${UTSURE_TIMELINE_NORMALIZED_INTRO_FILE}"
        "${UTSURE_NTSC_COARSE_TIMEBASE_FILE}"
        "${UTSURE_TIMELINE_NORMALIZED_OUTRO_FILE}"
)

add_test(
    NAME utsure.core.timeline.audio_target_required
    COMMAND utsure_core_timeline_tests --missing-main-audio-target
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
)

add_test(
    NAME utsure.core.timeline.trimmed_main
    COMMAND utsure_core_timeline_tests --trimmed-main
        "${UTSURE_SAMPLE_MEDIA_FILE}"
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

add_test(
    NAME utsure.core.subtitles.renderer.timestamp_conversion
    COMMAND utsure_core_subtitle_renderer_tests --timestamp-conversion
)

add_executable(utsure_core_libassmod_subtitle_adapter_tests
    libassmod_subtitle_adapter_tests.cpp
)

target_link_libraries(utsure_core_libassmod_subtitle_adapter_tests
    PRIVATE
        utsure::encoder_core
        utsure::subtitle_renderer_dependency
)

target_include_directories(utsure_core_libassmod_subtitle_adapter_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/core/src"
)

target_compile_features(utsure_core_libassmod_subtitle_adapter_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_libassmod_subtitle_adapter_tests)

add_test(
    NAME utsure.core.subtitles.libassmod.adapter.empty_copied
    COMMAND utsure_core_libassmod_subtitle_adapter_tests --empty-copied
)

add_test(
    NAME utsure.core.subtitles.libassmod.adapter.empty_direct
    COMMAND utsure_core_libassmod_subtitle_adapter_tests --empty-direct
)

add_test(
    NAME utsure.core.subtitles.libassmod.adapter.trusted_positive
    COMMAND utsure_core_libassmod_subtitle_adapter_tests --trusted-positive
)

add_test(
    NAME utsure.core.subtitles.libassmod.adapter.large_clipped
    COMMAND utsure_core_libassmod_subtitle_adapter_tests --large-clipped
)

add_executable(utsure_core_libassmod_rgba_reproducer
    libassmod_rgba_reproducer.cpp
)

target_link_libraries(utsure_core_libassmod_rgba_reproducer
    PRIVATE
        utsure::subtitle_renderer_dependency
)

target_compile_definitions(utsure_core_libassmod_rgba_reproducer
    PRIVATE
        UTSURE_LIBASSMOD_REF="${UTSURE_LIBASSMOD_REF}"
)

target_compile_features(utsure_core_libassmod_rgba_reproducer PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_core_libassmod_rgba_reproducer)

add_test(
    NAME utsure.core.subtitles.libassmod.rgba_reproducer.bs4
    COMMAND utsure_core_libassmod_rgba_reproducer --rgba
        "${UTSURE_SUBTITLE_BS4_SAMPLE_FILE}"
        320
        180
        1
        1
        41667
)

add_test(
    NAME utsure.core.subtitles.libassmod.rgba_reproducer.bs4_gradient
    COMMAND utsure_core_libassmod_rgba_reproducer --rgba
        "${UTSURE_SUBTITLE_BS4_GRADIENT_SAMPLE_FILE}"
        320
        180
        1
        1
        41667
)

add_test(
    NAME utsure.core.subtitles.libassmod.rgba_reproducer.bs4_forced_rgba
    COMMAND utsure_core_libassmod_rgba_reproducer --rgba
        "${UTSURE_SUBTITLE_BS4_FORCED_RGBA_SAMPLE_FILE}"
        320
        180
        1
        1
        1204800000
)

add_test(
    NAME utsure.core.subtitles.libassmod.rgba_reproducer.repeat.normal
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_LIBASSMOD_REPRO_REPEAT=200
        utsure_core_libassmod_rgba_reproducer --rgba
            "${UTSURE_SUBTITLE_BS4_FORCED_RGBA_SAMPLE_FILE}"
            320
            180
            1
            1
            1204800000
)

add_test(
    NAME utsure.core.subtitles.libassmod.rgba_reproducer.repeat.safe_mode
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_SAFE_MODE=1
        UTSURE_LIBASSMOD_REPRO_REPEAT=200
        utsure_core_libassmod_rgba_reproducer --rgba
            "${UTSURE_SUBTITLE_BS4_FORCED_RGBA_SAMPLE_FILE}"
            320
            180
            1
            1
            1204800000
)

add_test(
    NAME utsure.core.subtitles.libassmod.rgba_reproducer.repeat.reported_timestamp
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_LIBASSMOD_REPRO_REPEAT=200
        utsure_core_libassmod_rgba_reproducer --rgba
            "${UTSURE_SUBTITLE_BS4_FORCED_RGBA_SAMPLE_FILE}"
            320
            180
            1
            1
        1138596000
)

add_test(
    NAME utsure.core.subtitles.libassmod.auto_reproducer.repeat.reported_timestamp
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_REQUIRE_LIBASSMOD_ALIGNED_ALLOCATION_DEBUG=1
        UTSURE_LIBASSMOD_REPRO_REPEAT=200
        utsure_core_libassmod_rgba_reproducer --auto
            "${UTSURE_SUBTITLE_BS4_FORCED_RGBA_SAMPLE_FILE}"
            320
            180
            1
            1
            1138596000
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

add_test(
    NAME utsure.core.subtitles.compositor.direct_clipped_rgba
    COMMAND utsure_core_subtitle_bitmap_compositor_tests --direct-clipped
)

add_test(
    NAME utsure.core.subtitles.compositor.direct_offscreen_rgba
    COMMAND utsure_core_subtitle_bitmap_compositor_tests --direct-offscreen
)

add_test(
    NAME utsure.core.subtitles.compositor.direct_zero_visible_rgba
    COMMAND utsure_core_subtitle_bitmap_compositor_tests --direct-zero-visible
)

add_test(
    NAME utsure.core.subtitles.compositor.invalid_bitmap
    COMMAND utsure_core_subtitle_bitmap_compositor_tests --invalid-bitmap
)

add_test(
    NAME utsure.core.subtitles.compositor.null_visible_bitmap
    COMMAND utsure_core_subtitle_bitmap_compositor_tests --null-visible-bitmap
)

add_executable(utsure_core_subtitle_burn_in_tests
    subtitle_burn_in_tests.cpp
)

target_link_libraries(utsure_core_subtitle_burn_in_tests
    PRIVATE
        utsure::encoder_core
)

if(WIN32)
    target_link_libraries(utsure_core_subtitle_burn_in_tests PRIVATE psapi)
endif()

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
    NAME utsure.core.subtitles.libassmod.render.bs4
    COMMAND utsure_core_subtitle_burn_in_tests --render-bs4 "${UTSURE_SUBTITLE_BS4_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.bs4_gradient_rgba
    COMMAND utsure_core_subtitle_burn_in_tests --render-bs4-gradient "${UTSURE_SUBTITLE_BS4_GRADIENT_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.bs4_forced_rgba
    COMMAND utsure_core_subtitle_burn_in_tests --render-bs4-forced-rgba "${UTSURE_SUBTITLE_BS4_FORCED_RGBA_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.empty_bitmap_effect
    COMMAND utsure_core_subtitle_burn_in_tests --render-empty-effect "${UTSURE_SUBTITLE_EMPTY_BITMAP_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.img_asset
    COMMAND utsure_core_subtitle_burn_in_tests --render-img-asset "${UTSURE_SUBTITLE_IMG_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.img_missing_asset
    COMMAND utsure_core_subtitle_burn_in_tests --render-img-missing "${UTSURE_SUBTITLE_IMG_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.lifecycle_trace_off
    COMMAND utsure_core_subtitle_burn_in_tests --render-lifecycle-trace-off "${UTSURE_SUBTITLE_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.lifecycle_trace_throttled
    COMMAND utsure_core_subtitle_burn_in_tests --render-lifecycle-trace-throttled "${UTSURE_SUBTITLE_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.lifecycle_trace_full
    COMMAND utsure_core_subtitle_burn_in_tests --render-lifecycle-trace-full "${UTSURE_SUBTITLE_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.libassmod.render.lifecycle_trace_strict_same_thread
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_STRICT_SAME_THREAD=1
        utsure_core_subtitle_burn_in_tests --render-lifecycle-trace-off "${UTSURE_SUBTITLE_SAMPLE_FILE}"
)

add_test(
    NAME utsure.core.subtitles.burn_in.h264
    COMMAND utsure_core_subtitle_burn_in_tests --h264
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-plain-h264.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.strict_same_thread.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_STRICT_SAME_THREAD=1
        UTSURE_SUBTITLE_COMPOSITION_MODE=worker_local
        UTSURE_SUBTITLE_RENDER_TRACE=1
        utsure_core_subtitle_burn_in_tests --h264
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_SUBTITLE_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-strict-same-thread-plain-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-strict-same-thread-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.strict_same_thread.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.gradient.h264
    COMMAND utsure_core_subtitle_burn_in_tests --h264
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_SUBTITLE_GRADIENT_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-gradient-plain-h264.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-gradient-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.gradient.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.empty_bitmap_effect.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=verbose
        utsure_core_subtitle_burn_in_tests --empty-bitmap-h264
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_SUBTITLE_EMPTY_BITMAP_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-empty-bitmap-plain-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-empty-bitmap-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.empty_bitmap_effect.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.img_asset.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=verbose
        utsure_core_subtitle_burn_in_tests --img-asset-h264
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_SUBTITLE_IMG_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-img-asset-plain-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-img-asset-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.img_asset.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.schedule.transform.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=frame
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --schedule-h264
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_SUBTITLE_TRANSFORM_SCHEDULE_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-schedule-transform-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.schedule.transform.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.schedule.move.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=frame
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --schedule-h264
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_SUBTITLE_MOVE_SCHEDULE_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-schedule-move-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.schedule.move.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.h265
    COMMAND utsure_core_subtitle_burn_in_tests --h265
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-plain-h265.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/subtitle-burn-h265.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.h265)

add_test(
    NAME utsure.core.subtitles.burn_in.trimmed.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=frame
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --trimmed-h264
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_SUBTITLE_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-trimmed-plain-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-trimmed-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.trimmed.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.timeline.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=frame
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --timeline-h264
            "${UTSURE_TIMELINE_INTRO_FILE}"
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_TIMELINE_OUTRO_FILE}"
            "${UTSURE_SUBTITLE_OUTPUT_TIMELINE_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/timeline-subtitle-plain-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/timeline-subtitle-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.timeline.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.timeline.full_output.rejected
    COMMAND utsure_core_subtitle_burn_in_tests --timeline-full-rejected
        "${UTSURE_TIMELINE_INTRO_FILE}"
        "${UTSURE_SAMPLE_MEDIA_FILE}"
        "${UTSURE_TIMELINE_OUTRO_FILE}"
        "${UTSURE_SUBTITLE_SAMPLE_FILE}"
        "${UTSURE_TEST_MEDIA_DIR}/timeline-full-subtitle-plain-h264.mp4"
        "${UTSURE_TEST_MEDIA_DIR}/timeline-full-subtitle-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.timeline.full_output.rejected)

add_test(
    NAME utsure.core.subtitles.burn_in.timeline.thumbnail_preroll.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=frame
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --timeline-thumbnail-h264
            "${UTSURE_TIMELINE_LONG_INTRO_FILE}"
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_TIMELINE_OUTRO_FILE}"
            "${UTSURE_SUBTITLE_OUTPUT_TIMELINE_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/timeline-thumbnail-subtitle-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.timeline.thumbnail_preroll.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.timeline.resize.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=frame
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --timeline-resize-h264
            "${UTSURE_TIMELINE_INTRO_FILE}"
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_TIMELINE_OUTRO_FILE}"
            "${UTSURE_SUBTITLE_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/timeline-resize-subtitle-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.timeline.resize.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.timeline.thumbnail_preroll.resize.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_DIAGNOSTICS=frame
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --timeline-thumbnail-resize-h264
            "${UTSURE_TIMELINE_INTRO_FILE}"
            "${UTSURE_SAMPLE_MEDIA_FILE}"
            "${UTSURE_TIMELINE_OUTRO_FILE}"
            "${UTSURE_SUBTITLE_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/timeline-thumbnail-resize-subtitle-burn-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.timeline.thumbnail_preroll.resize.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.stress.copied_serialized.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_VIDEO_WORKERS=2
        UTSURE_SUBTITLE_BITMAP_MODE=copied
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --stress-h264
            "${UTSURE_SUBTITLE_STRESS_MAIN_FILE}"
            "${UTSURE_SUBTITLE_STRESS_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-plain-copied-serialized-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-burn-copied-serialized-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.stress.copied_serialized.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.stress.direct_serialized.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_VIDEO_WORKERS=2
        UTSURE_SUBTITLE_BITMAP_MODE=direct
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --stress-h264
            "${UTSURE_SUBTITLE_STRESS_MAIN_FILE}"
            "${UTSURE_SUBTITLE_STRESS_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-plain-direct-serialized-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-burn-direct-serialized-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.stress.direct_serialized.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.stress.copied_worker.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_BITMAP_MODE=copied
        UTSURE_SUBTITLE_COMPOSITION_MODE=worker_local
        utsure_core_subtitle_burn_in_tests --stress-h264
            "${UTSURE_SUBTITLE_STRESS_MAIN_FILE}"
            "${UTSURE_SUBTITLE_STRESS_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-plain-copied-worker-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-burn-copied-worker-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.stress.copied_worker.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.stress.direct_worker.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_BITMAP_MODE=direct
        UTSURE_SUBTITLE_COMPOSITION_MODE=worker_local
        utsure_core_subtitle_burn_in_tests --stress-h264
            "${UTSURE_SUBTITLE_STRESS_MAIN_FILE}"
            "${UTSURE_SUBTITLE_STRESS_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-plain-direct-worker-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-stress-burn-direct-worker-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.stress.direct_worker.h264)

add_test(
    NAME utsure.core.subtitles.burn_in.sequential_stability.h264
    COMMAND "${CMAKE_COMMAND}" -E env
        UTSURE_SUBTITLE_COMPOSITION_MODE=serialized
        utsure_core_subtitle_burn_in_tests --sequential-stability-h264
            "${UTSURE_SUBTITLE_SPEED_15X_MAIN_FILE}"
            "${UTSURE_SUBTITLE_STRESS_SAMPLE_FILE}"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-sequential-stability-1-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-sequential-stability-2-h264.mp4"
            "${UTSURE_TEST_MEDIA_DIR}/subtitle-sequential-stability-3-h264.mp4"
)
utsure_enable_fontcollector_stub_for_test(utsure.core.subtitles.burn_in.sequential_stability.h264)

utsure_label_tests_matching("^utsure\\.core\\.(output_naming|resize|audio_stream_selection|source_import_paths|batch_parallelism|subtitle_auto_selection|thumbnail_preroll|subtitle_renderer|subtitle_bitmap_compositor|libassmod_subtitle_adapter|job\\.ffmpeg_filter_hardsub)" unit smoke)
utsure_label_tests_matching("^utsure\\.core\\.(media|job\\.encode|job\\.preflight|timeline|subtitles\\.burn_in|subtitles\\.image_assets|subtitles\\.font_recovery)" integration media)
utsure_label_tests_matching("^utsure\\.core\\.subtitles\\.burn_in\\.(h264|img_asset\\.h264|timeline\\.h264|timeline\\.resize\\.h264|timeline\\.thumbnail_preroll\\.resize\\.h264|sequential_stability\\.h264)$" smoke)
utsure_label_tests_matching("^utsure\\.core\\.media\\.(inspection|decode|encode)\\." smoke)
utsure_label_tests_matching("^utsure\\.core\\.job\\.(encode|preflight)\\." smoke)
utsure_label_tests_matching("^utsure\\.core\\.subtitles\\.burn_in\\.(h265|gradient\\.h264|empty_bitmap_effect\\.h264|schedule\\..*\\.h264)$" slow media regression)
utsure_label_tests_matching("^utsure\\.core\\.subtitles\\.burn_in\\.stress\\." slow media stress regression)
