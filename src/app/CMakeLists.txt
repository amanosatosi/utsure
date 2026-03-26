qt_standard_project_setup()

qt_add_executable(utsure_encoder_app WIN32
    encode_job_runner_controller.cpp
    encode_job_runner_controller.hpp
    encode_job_runner_worker.cpp
    encode_job_runner_worker.hpp
    main.cpp
    main_window.cpp
    main_window.hpp
    resources/app_resources.qrc
    trim_timeline_widget.cpp
    trim_timeline_widget.hpp
)

target_link_libraries(utsure_encoder_app
    PRIVATE
        Qt6::Widgets
        Qt6::Svg
        utsure::encoder_core
)

target_compile_features(utsure_encoder_app PRIVATE cxx_std_20)

set_target_properties(utsure_encoder_app PROPERTIES OUTPUT_NAME "utsure")

utsure_enable_warnings(utsure_encoder_app)
