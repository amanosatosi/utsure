cmake_minimum_required(VERSION 3.26)

project(utsure VERSION 0.1.0 LANGUAGES CXX)

option(UTSURE_BUILD_APP "Build the Qt 6 Widgets desktop application." ON)
option(UTSURE_ENABLE_ADDRESS_SANITIZER "Enable AddressSanitizer for supported compiler toolchains." OFF)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(UTSURE_ENABLE_ADDRESS_SANITIZER)
    if(MSVC)
        message(FATAL_ERROR "UTSURE_ENABLE_ADDRESS_SANITIZER is not supported with MSVC in this repository.")
    endif()

    if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR
            "UTSURE_ENABLE_ADDRESS_SANITIZER is not supported with the MSYS2 MinGW GCC toolchain because "
            "that environment does not provide libasan. Use the MSYS2 CLANG64 toolchain for AddressSanitizer "
            "coverage on Windows."
        )
    endif()

    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(UtsureCompilerWarnings)
include(UtsureDependencies)

utsure_configure_dependencies()

add_subdirectory(src/core)

if(UTSURE_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
