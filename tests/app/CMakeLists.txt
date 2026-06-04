add_executable(utsure_app_settings_tests
    app_settings_tests.cpp
    "${CMAKE_SOURCE_DIR}/src/app/app_settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/encode_job_duplicate.cpp"
)

target_include_directories(utsure_app_settings_tests
    PRIVATE
        "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(utsure_app_settings_tests
    PRIVATE
        Qt6::Core
        utsure::encoder_core
)

target_compile_features(utsure_app_settings_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_app_settings_tests)

add_test(
    NAME utsure.app.settings.json
    COMMAND utsure_app_settings_tests
)

set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/src/app/resources/app_resources.qrc"
    PROPERTIES
        AUTORCC_OPTIONS "-name;app_resources"
)

add_executable(utsure_app_ui_font_tests
    ui_font_tests.cpp
    "${CMAKE_SOURCE_DIR}/src/app/app_settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/ui_font.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/resources/app_resources.qrc"
)

set_target_properties(utsure_app_ui_font_tests PROPERTIES AUTORCC ON)

target_include_directories(utsure_app_ui_font_tests
    PRIVATE
        "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(utsure_app_ui_font_tests
    PRIVATE
        Qt6::Widgets
        utsure::encoder_core
)

target_compile_features(utsure_app_ui_font_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_app_ui_font_tests)

add_test(
    NAME utsure.app.ui_font
    COMMAND utsure_app_ui_font_tests
)

set_tests_properties(utsure.app.ui_font PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

find_program(UTSURE_APP_FFMPEG_EXECUTABLE NAMES ffmpeg)
if(NOT UTSURE_APP_FFMPEG_EXECUTABLE AND DEFINED ENV{CI})
    message(FATAL_ERROR "ffmpeg is required in CI for the app encode-runner real cancel/destroy lifecycle smoke.")
endif()
set(UTSURE_APP_FFMPEG_HAS_LIBX265 FALSE)
set(UTSURE_APP_FFMPEG_HAS_LIBX265_YUV420P10LE FALSE)
if(UTSURE_APP_FFMPEG_EXECUTABLE)
    execute_process(
        COMMAND "${UTSURE_APP_FFMPEG_EXECUTABLE}" -hide_banner -encoders
        RESULT_VARIABLE UTSURE_APP_FFMPEG_ENCODERS_RESULT
        OUTPUT_VARIABLE UTSURE_APP_FFMPEG_ENCODERS_OUTPUT
        ERROR_VARIABLE UTSURE_APP_FFMPEG_ENCODERS_ERROR
    )
    if(UTSURE_APP_FFMPEG_ENCODERS_RESULT EQUAL 0 AND UTSURE_APP_FFMPEG_ENCODERS_OUTPUT MATCHES "libx265")
        set(UTSURE_APP_FFMPEG_HAS_LIBX265 TRUE)
        execute_process(
            COMMAND "${UTSURE_APP_FFMPEG_EXECUTABLE}" -hide_banner -h encoder=libx265
            RESULT_VARIABLE UTSURE_APP_FFMPEG_LIBX265_HELP_RESULT
            OUTPUT_VARIABLE UTSURE_APP_FFMPEG_LIBX265_HELP_OUTPUT
            ERROR_VARIABLE UTSURE_APP_FFMPEG_LIBX265_HELP_ERROR
        )
        if(UTSURE_APP_FFMPEG_LIBX265_HELP_RESULT EQUAL 0 AND UTSURE_APP_FFMPEG_LIBX265_HELP_OUTPUT MATCHES "yuv420p10le")
            set(UTSURE_APP_FFMPEG_HAS_LIBX265_YUV420P10LE TRUE)
        endif()
    endif()
endif()
if(UTSURE_APP_FFMPEG_EXECUTABLE AND NOT UTSURE_APP_FFMPEG_HAS_LIBX265)
    if(DEFINED ENV{CI})
        message(FATAL_ERROR "ffmpeg with the libx265 encoder is required in CI for HEVC source crash reproduction tests.")
    else()
        message(WARNING "ffmpeg was found without libx265; local HEVC app encode-runner smokes are not registered.")
    endif()
endif()

set(UTSURE_APP_TEST_MEDIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/media")
set(UTSURE_APP_LIFECYCLE_MAIN_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-lifecycle-main.mp4")
set(UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-lifecycle-main-hevc.mp4")
set(UTSURE_APP_LIFECYCLE_HEVC10_MAIN_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-lifecycle-main-hevc10.mp4")
set(UTSURE_APP_LIFECYCLE_SUBTITLE_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-lifecycle-subtitle.ass")
set(UTSURE_APP_LIFECYCLE_CANCEL_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-lifecycle-cancel-output.mp4")
set(UTSURE_APP_LIFECYCLE_DESTROY_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-lifecycle-destroy-output.mp4")
set(UTSURE_APP_PARALLEL_NO_SUBTITLE_FIRST_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-parallel-no-subtitle-a.mp4")
set(UTSURE_APP_PARALLEL_NO_SUBTITLE_SECOND_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-parallel-no-subtitle-b.mp4")
set(UTSURE_APP_PARALLEL_SUBTITLE_FIRST_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-parallel-subtitle-a.mp4")
set(UTSURE_APP_PARALLEL_SUBTITLE_SECOND_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-parallel-subtitle-b.mp4")
set(UTSURE_APP_PARALLEL_CANCEL_FIRST_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-parallel-cancel-a.mp4")
set(UTSURE_APP_PARALLEL_CANCEL_SECOND_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-parallel-cancel-b.mp4")
set(UTSURE_APP_HEVC_SINGLE_NO_SUBTITLE_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-hevc-single-no-subtitle.mp4")
set(UTSURE_APP_HEVC_SINGLE_SUBTITLE_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-hevc-single-subtitle.mp4")
set(UTSURE_APP_HEVC_PARALLEL_NO_SUBTITLE_FIRST_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-hevc-parallel-no-subtitle-a.mp4")
set(UTSURE_APP_HEVC_PARALLEL_NO_SUBTITLE_SECOND_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-hevc-parallel-no-subtitle-b.mp4")
set(UTSURE_APP_HEVC_PARALLEL_SUBTITLE_FIRST_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-hevc-parallel-subtitle-a.mp4")
set(UTSURE_APP_HEVC_PARALLEL_SUBTITLE_SECOND_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-hevc-parallel-subtitle-b.mp4")
set(UTSURE_APP_HEVC_PARALLEL_CANCEL_FIRST_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-hevc-parallel-cancel-a.mp4")
set(UTSURE_APP_HEVC_PARALLEL_CANCEL_SECOND_OUTPUT_FILE "${UTSURE_APP_TEST_MEDIA_DIR}/runner-hevc-parallel-cancel-b.mp4")
file(MAKE_DIRECTORY "${UTSURE_APP_TEST_MEDIA_DIR}")
file(WRITE "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
    "[Script Info]\n"
    "Title: app runner lifecycle subtitle\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 320\n"
    "PlayResY: 180\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,20,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:15.00,Default,,0,0,0,,Lifecycle cancellation smoke\n"
)

if(WIN32)
    set(UTSURE_APP_TEST_FONTCOLLECTOR_STUB "${CMAKE_CURRENT_BINARY_DIR}/fontcollector-app-test-stub.cmd")
    file(WRITE "${UTSURE_APP_TEST_FONTCOLLECTOR_STUB}"
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
        "if not \"%LOG_PATH%\"==\"\" echo fontcollector app ctest stub>\"%LOG_PATH%\"\r\n"
        "exit /b 0\r\n"
    )
else()
    set(UTSURE_APP_TEST_FONTCOLLECTOR_STUB "${CMAKE_CURRENT_BINARY_DIR}/fontcollector-app-test-stub.sh")
    file(WRITE "${UTSURE_APP_TEST_FONTCOLLECTOR_STUB}"
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
        "if [ -n \"$LOG_PATH\" ]; then printf \"fontcollector app ctest stub\" > \"$LOG_PATH\"; fi\n"
        "exit 0\n"
    )
    file(CHMOD "${UTSURE_APP_TEST_FONTCOLLECTOR_STUB}"
        FILE_PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
    )
endif()

if(UTSURE_APP_FFMPEG_EXECUTABLE)
    set(UTSURE_APP_GENERATED_LIFECYCLE_MEDIA_OUTPUTS)
    add_custom_command(
        OUTPUT "${UTSURE_APP_LIFECYCLE_MAIN_FILE}"
        COMMAND "${UTSURE_APP_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
            -f lavfi -i "testsrc2=size=320x180:rate=24"
            -f lavfi -i "sine=frequency=720:sample_rate=48000:duration=15"
            -t 15
            -filter_complex "[0:v]setpts=PTS/1.5[v];[1:a]atempo=1.5[a]"
            -map "[v]"
            -map "[a]"
            -c:v libx264
            -preset ultrafast
            -pix_fmt yuv420p
            -c:a aac
            -b:a 96k
            -shortest
            "${UTSURE_APP_LIFECYCLE_MAIN_FILE}"
        VERBATIM
    )
    list(APPEND UTSURE_APP_GENERATED_LIFECYCLE_MEDIA_OUTPUTS "${UTSURE_APP_LIFECYCLE_MAIN_FILE}")

    if(UTSURE_APP_FFMPEG_HAS_LIBX265)
        add_custom_command(
            OUTPUT "${UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE}"
            COMMAND "${UTSURE_APP_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
                -f lavfi -i "testsrc2=size=320x180:rate=24000/1001"
                -f lavfi -i "sine=frequency=720:sample_rate=48000:duration=15"
                -t 15
                -filter_complex "[0:v]setpts=PTS/1.5[v];[1:a]atempo=1.5[a]"
                -map "[v]"
                -map "[a]"
                -c:v libx265
                -preset ultrafast
                -x265-params "log-level=error:keyint=24:min-keyint=24:scenecut=0"
                -pix_fmt yuv420p
                -c:a aac
                -b:a 96k
                -shortest
                "${UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE}"
            VERBATIM
        )
        list(APPEND UTSURE_APP_GENERATED_LIFECYCLE_MEDIA_OUTPUTS "${UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE}")

        if(UTSURE_APP_FFMPEG_HAS_LIBX265_YUV420P10LE)
            add_custom_command(
                OUTPUT "${UTSURE_APP_LIFECYCLE_HEVC10_MAIN_FILE}"
                COMMAND "${UTSURE_APP_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
                    -f lavfi -i "testsrc2=size=320x180:rate=24000/1001"
                    -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=6"
                    -t 6
                    -filter_complex "[0:v]format=yuv420p10le[v]"
                    -map "[v]"
                    -map 1:a
                    -c:v libx265
                    -preset ultrafast
                    -x265-params "profile=main10:log-level=error:keyint=24:min-keyint=24:scenecut=0"
                    -pix_fmt yuv420p10le
                    -c:a aac
                    -b:a 96k
                    -shortest
                    "${UTSURE_APP_LIFECYCLE_HEVC10_MAIN_FILE}"
                VERBATIM
            )
            list(APPEND UTSURE_APP_GENERATED_LIFECYCLE_MEDIA_OUTPUTS "${UTSURE_APP_LIFECYCLE_HEVC10_MAIN_FILE}")
        endif()
    endif()

    add_custom_target(
        utsure_app_generate_lifecycle_media
        DEPENDS
            ${UTSURE_APP_GENERATED_LIFECYCLE_MEDIA_OUTPUTS}
            "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
    )
else()
    message(WARNING "ffmpeg was not found; app lifecycle tests will run fake queue/controller coverage, but the real cancel/destroy encode smoke is skipped for this local configure.")
endif()


qt_add_executable(utsure_app_encode_job_runner_lifecycle_tests
    encode_job_runner_lifecycle_tests.cpp
    "${CMAKE_SOURCE_DIR}/src/app/encode_job_runner_controller.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/encode_job_runner_controller.hpp"
    "${CMAKE_SOURCE_DIR}/src/app/encode_job_runner_worker.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/encode_job_runner_worker.hpp"
    "${CMAKE_SOURCE_DIR}/src/app/encode_job_progress_qt_types.hpp"
)

set_target_properties(utsure_app_encode_job_runner_lifecycle_tests PROPERTIES
  AUTOMOC ON
)

target_include_directories(utsure_app_encode_job_runner_lifecycle_tests
    PRIVATE
        "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(utsure_app_encode_job_runner_lifecycle_tests
    PRIVATE
        Qt6::Core
        utsure::encoder_core
)
if(WIN32)
    target_link_libraries(utsure_app_encode_job_runner_lifecycle_tests PRIVATE psapi)
endif()

target_compile_features(utsure_app_encode_job_runner_lifecycle_tests PRIVATE cxx_std_20)

utsure_enable_warnings(utsure_app_encode_job_runner_lifecycle_tests)

if(UTSURE_APP_FFMPEG_EXECUTABLE)
    add_dependencies(utsure_app_encode_job_runner_lifecycle_tests utsure_app_generate_lifecycle_media)

    add_test(
        NAME utsure.app.encode_job_runner.lifecycle
        COMMAND utsure_app_encode_job_runner_lifecycle_tests
    )

    add_test(
        NAME utsure.app.encode_job_runner.real_cancel_destroy
        COMMAND utsure_app_encode_job_runner_lifecycle_tests --real-cancel-destroy
            "${UTSURE_APP_LIFECYCLE_MAIN_FILE}"
            "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
            "${UTSURE_APP_LIFECYCLE_CANCEL_OUTPUT_FILE}"
            "${UTSURE_APP_LIFECYCLE_DESTROY_OUTPUT_FILE}"
    )

    add_test(
        NAME utsure.app.encode_job_runner.parallel_smokes
        COMMAND utsure_app_encode_job_runner_lifecycle_tests --real-parallel-smokes
            "${UTSURE_APP_LIFECYCLE_MAIN_FILE}"
            "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
            "${UTSURE_APP_PARALLEL_NO_SUBTITLE_FIRST_OUTPUT_FILE}"
            "${UTSURE_APP_PARALLEL_NO_SUBTITLE_SECOND_OUTPUT_FILE}"
            "${UTSURE_APP_PARALLEL_SUBTITLE_FIRST_OUTPUT_FILE}"
            "${UTSURE_APP_PARALLEL_SUBTITLE_SECOND_OUTPUT_FILE}"
            "${UTSURE_APP_PARALLEL_CANCEL_FIRST_OUTPUT_FILE}"
            "${UTSURE_APP_PARALLEL_CANCEL_SECOND_OUTPUT_FILE}"
    )

    if(UTSURE_APP_FFMPEG_HAS_LIBX265)
        add_test(
            NAME utsure.app.encode_job_runner.hevc_single_no_subtitle
            COMMAND utsure_app_encode_job_runner_lifecycle_tests --real-single-encode
                "runner_lifecycle.hevc_single_no_subtitle"
                "${UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE}"
                "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
                "${UTSURE_APP_HEVC_SINGLE_NO_SUBTITLE_OUTPUT_FILE}"
                "0"
        )

        add_test(
            NAME utsure.app.encode_job_runner.hevc_single_subtitle
            COMMAND utsure_app_encode_job_runner_lifecycle_tests --real-single-encode
                "runner_lifecycle.hevc_single_subtitle"
                "${UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE}"
                "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
                "${UTSURE_APP_HEVC_SINGLE_SUBTITLE_OUTPUT_FILE}"
                "1"
        )

        add_test(
            NAME utsure.app.encode_job_runner.hevc_parallel_no_subtitle
            COMMAND utsure_app_encode_job_runner_lifecycle_tests --real-parallel-pair
                "runner_lifecycle.hevc_parallel_no_subtitle"
                "hevc"
                "${UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE}"
                "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
                "${UTSURE_APP_HEVC_PARALLEL_NO_SUBTITLE_FIRST_OUTPUT_FILE}"
                "${UTSURE_APP_HEVC_PARALLEL_NO_SUBTITLE_SECOND_OUTPUT_FILE}"
                "0"
                "0"
        )

        add_test(
            NAME utsure.app.encode_job_runner.hevc_parallel_subtitle
            COMMAND utsure_app_encode_job_runner_lifecycle_tests --real-parallel-pair
                "runner_lifecycle.hevc_parallel_subtitle"
                "hevc"
                "${UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE}"
                "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
                "${UTSURE_APP_HEVC_PARALLEL_SUBTITLE_FIRST_OUTPUT_FILE}"
                "${UTSURE_APP_HEVC_PARALLEL_SUBTITLE_SECOND_OUTPUT_FILE}"
                "1"
                "0"
        )

        add_test(
            NAME utsure.app.encode_job_runner.hevc_parallel_cancel_one
            COMMAND utsure_app_encode_job_runner_lifecycle_tests --real-parallel-pair
                "runner_lifecycle.hevc_parallel_cancel_one"
                "hevc"
                "${UTSURE_APP_LIFECYCLE_HEVC_MAIN_FILE}"
                "${UTSURE_APP_LIFECYCLE_SUBTITLE_FILE}"
                "${UTSURE_APP_HEVC_PARALLEL_CANCEL_FIRST_OUTPUT_FILE}"
                "${UTSURE_APP_HEVC_PARALLEL_CANCEL_SECOND_OUTPUT_FILE}"
                "0"
                "1"
        )
    endif()
else()
    add_test(
        NAME utsure.app.encode_job_runner.lifecycle
        COMMAND utsure_app_encode_job_runner_lifecycle_tests
    )
endif()

set_tests_properties(utsure.app.encode_job_runner.lifecycle PROPERTIES
    ENVIRONMENT "UTSURE_FONTCOLLECTOR_PATH=${UTSURE_APP_TEST_FONTCOLLECTOR_STUB}"
    TIMEOUT 45
)
if(UTSURE_APP_FFMPEG_EXECUTABLE)
    set_tests_properties(utsure.app.encode_job_runner.real_cancel_destroy PROPERTIES
        ENVIRONMENT "UTSURE_FONTCOLLECTOR_PATH=${UTSURE_APP_TEST_FONTCOLLECTOR_STUB}"
        TIMEOUT 45
    )
    set_tests_properties(utsure.app.encode_job_runner.parallel_smokes PROPERTIES
        ENVIRONMENT "UTSURE_FONTCOLLECTOR_PATH=${UTSURE_APP_TEST_FONTCOLLECTOR_STUB}"
        TIMEOUT 90
    )
    if(UTSURE_APP_FFMPEG_HAS_LIBX265)
        set_tests_properties(
            utsure.app.encode_job_runner.hevc_single_no_subtitle
            utsure.app.encode_job_runner.hevc_single_subtitle
            PROPERTIES
                ENVIRONMENT "UTSURE_FONTCOLLECTOR_PATH=${UTSURE_APP_TEST_FONTCOLLECTOR_STUB}"
                TIMEOUT 90
        )
        set_tests_properties(
            utsure.app.encode_job_runner.hevc_parallel_no_subtitle
            utsure.app.encode_job_runner.hevc_parallel_subtitle
            utsure.app.encode_job_runner.hevc_parallel_cancel_one
            PROPERTIES
                ENVIRONMENT "UTSURE_FONTCOLLECTOR_PATH=${UTSURE_APP_TEST_FONTCOLLECTOR_STUB}"
                TIMEOUT 120
        )
    endif()
endif()

utsure_label_tests_matching("^utsure\\.app\\.settings\\." unit smoke)
utsure_label_tests_matching("^utsure\\.app\\.ui_font$" integration smoke)
utsure_label_tests_matching("^utsure\\.app\\.encode_job_runner\\.lifecycle$" integration smoke)
utsure_label_tests_matching("^utsure\\.app\\.encode_job_runner\\.real_cancel_destroy$" integration media smoke)
utsure_label_tests_matching("^utsure\\.app\\.encode_job_runner\\.parallel_smokes$" integration media smoke)
utsure_label_tests_matching("^utsure\\.app\\.encode_job_runner\\.hevc_" integration media smoke)
