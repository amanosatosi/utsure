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
