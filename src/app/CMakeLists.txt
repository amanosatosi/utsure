qt_standard_project_setup()

qt_add_executable(utsure_encoder_app WIN32
    main.cpp
    main_window.cpp
    main_window.hpp
)

target_link_libraries(utsure_encoder_app
    PRIVATE
        Qt6::Widgets
        utsure::encoder_core
)

target_compile_features(utsure_encoder_app PRIVATE cxx_std_20)

set_target_properties(utsure_encoder_app PROPERTIES OUTPUT_NAME "utsure")

utsure_enable_warnings(utsure_encoder_app)
