find_program(UTSURE_FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)

set(UTSURE_TEST_MEDIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/media")
set(UTSURE_SAMPLE_MEDIA_FILE "${UTSURE_TEST_MEDIA_DIR}/inspection-sample.avi")

file(MAKE_DIRECTORY "${UTSURE_TEST_MEDIA_DIR}")

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

add_custom_target(utsure_generate_sample_media DEPENDS "${UTSURE_SAMPLE_MEDIA_FILE}")

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
