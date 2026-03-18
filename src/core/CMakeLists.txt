add_library(utsure_encoder_core STATIC
    src/build_info.cpp
    src/job/encode_job.cpp
    src/job/encode_job_report.cpp
    src/media/decoded_media.cpp
    src/media/ffmpeg_media_support.cpp
    src/media/media_decode_report.cpp
    src/media/media_decoder.cpp
    src/media/media_encoder.cpp
    src/media/media_inspector.cpp
    src/media/media_inspection_report.cpp
)

add_library(utsure::encoder_core ALIAS utsure_encoder_core)

target_compile_features(utsure_encoder_core PUBLIC cxx_std_20)

target_include_directories(utsure_encoder_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(utsure_encoder_core
    PRIVATE
        utsure::ffmpeg
        utsure::x264
        utsure::x265
)

target_compile_definitions(utsure_encoder_core
    PRIVATE
        UTSURE_PROJECT_VERSION="${PROJECT_VERSION}"
)

utsure_enable_warnings(utsure_encoder_core)
