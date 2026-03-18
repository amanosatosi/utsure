add_library(utsure_encoder_core STATIC
    src/build_info.cpp
)

add_library(utsure::encoder_core ALIAS utsure_encoder_core)

target_compile_features(utsure_encoder_core PUBLIC cxx_std_20)

target_include_directories(utsure_encoder_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(utsure_encoder_core
    PRIVATE
        UTSURE_PROJECT_VERSION="${PROJECT_VERSION}"
)

utsure_enable_warnings(utsure_encoder_core)
