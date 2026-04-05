qt_standard_project_setup()

set_source_files_properties(
    resources/app_resources.qrc
    PROPERTIES
        AUTORCC_OPTIONS "-name;app_resources"
)

qt_add_executable(utsure_encoder_app WIN32
    encode_job_runner_controller.cpp
    encode_job_runner_controller.hpp
    encode_job_runner_worker.cpp
    encode_job_runner_worker.hpp
    main.cpp
    main_window.cpp
    main_window.hpp
    preview_audio_controller.cpp
    preview_audio_controller.hpp
    preview_audio_worker.cpp
    preview_audio_worker.hpp
    preview_frame_renderer_controller.cpp
    preview_frame_renderer_controller.hpp
    preview_frame_renderer_worker.cpp
    preview_frame_renderer_worker.hpp
    preview_surface_widget.cpp
    preview_surface_widget.hpp
    resources/app_resources.qrc
    trim_timeline_widget.cpp
    trim_timeline_widget.hpp
)

set_target_properties(utsure_encoder_app PROPERTIES AUTORCC ON)

target_link_libraries(utsure_encoder_app
    PRIVATE
        Qt6::Multimedia
        Qt6::Widgets
        Qt6::Svg
        utsure::encoder_core
)

if(WIN32)
    add_executable(utsure_svg_to_ico EXCLUDE_FROM_ALL
        tools/svg_to_ico.cpp
    )

    target_link_libraries(utsure_svg_to_ico
        PRIVATE
            Qt6::Gui
            Qt6::Svg
    )

    target_compile_features(utsure_svg_to_ico PRIVATE cxx_std_20)

    set(utsure_windows_icon_svg "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/icon.svg")
    set(utsure_windows_icon_ico "${CMAKE_CURRENT_BINARY_DIR}/resources/utsure.ico")

    add_custom_command(
        OUTPUT "${utsure_windows_icon_ico}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/resources"
        COMMAND $<TARGET_FILE:utsure_svg_to_ico> "${utsure_windows_icon_svg}" "${utsure_windows_icon_ico}"
        DEPENDS
            utsure_svg_to_ico
            "${utsure_windows_icon_svg}"
        VERBATIM
    )

    set(UTSURE_WINDOWS_APP_ICON_PATH "${utsure_windows_icon_ico}")
    configure_file(
        resources/windows_app_icon.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/windows_app_icon.rc"
        @ONLY
    )

    add_custom_target(utsure_windows_shell_icon DEPENDS "${utsure_windows_icon_ico}")
    add_dependencies(utsure_encoder_app utsure_windows_shell_icon)
    target_sources(utsure_encoder_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/windows_app_icon.rc")
    set_source_files_properties(
        "${CMAKE_CURRENT_BINARY_DIR}/windows_app_icon.rc"
        PROPERTIES
            OBJECT_DEPENDS "${utsure_windows_icon_ico}"
    )

    target_link_libraries(utsure_encoder_app PRIVATE dwmapi)
    utsure_enable_warnings(utsure_svg_to_ico)
endif()

target_compile_features(utsure_encoder_app PRIVATE cxx_std_20)

set_target_properties(utsure_encoder_app PROPERTIES OUTPUT_NAME "utsure")

utsure_enable_warnings(utsure_encoder_app)
