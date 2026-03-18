cmake_minimum_required(VERSION 3.26)

project(utsure VERSION 0.1.0 LANGUAGES CXX)

option(UTSURE_BUILD_APP "Build the Qt 6 Widgets desktop application." ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(UtsureCompilerWarnings)

add_subdirectory(src/core)

if(UTSURE_BUILD_APP)
    add_subdirectory(src/app)
endif()
