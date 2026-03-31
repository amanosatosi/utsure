add_library(utsure_encoder_core STATIC
    src/adapters/ffms2/ffms2_preview_backend.cpp
    src/adapters/libassmod/libassmod_subtitle_renderer.cpp
    src/build_info.cpp
    src/job/batch_parallelism.cpp
    src/job/encode_job.cpp
    src/job/encode_job_preflight.cpp
    src/job/encode_job_report.cpp
    src/job/output_naming.cpp
    src/process/external_tool_runner.cpp
    src/media/audio_output.cpp
    src/media/decoded_media.cpp
    src/media/ffmpeg_media_support.cpp
    src/media/media_decode_report.cpp
    src/media/media_decoder.cpp
    src/media/media_encoder.cpp
    src/media/media_inspector.cpp
    src/media/media_inspection_report.cpp
    src/media/streaming_transcode_pipeline.cpp
    src/subtitles/subtitle_bitmap_compositor.cpp
    src/subtitles/subtitle_burn_in.cpp
    src/subtitles/subtitle_auto_selection.cpp
    src/subtitles/subtitle_font_recovery.cpp
    src/subtitles/subtitle_frame_composer.cpp
    src/subtitles/subtitle_renderer.cpp
    src/timeline/timeline.cpp
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
        utsure::ffms2
        utsure::subtitle_renderer_dependency
        utsure::x264
        utsure::x265
)

target_compile_definitions(utsure_encoder_core
    PRIVATE
        UTSURE_PROJECT_VERSION="${PROJECT_VERSION}"
)

utsure_enable_warnings(utsure_encoder_core)
