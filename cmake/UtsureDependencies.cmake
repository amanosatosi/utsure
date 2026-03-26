include_guard(GLOBAL)

set(UTSURE_QT_MIN_VERSION "6.5" CACHE STRING "Minimum supported Qt 6 version.")
set(UTSURE_FFMPEG_REQUIRED_SERIES "7.1" CACHE STRING "Required FFmpeg major.minor series for the active media pipeline.")
set(UTSURE_FFMPEG_NEXT_UNTESTED_SERIES "7.2" CACHE STRING "First FFmpeg series outside the currently supported range.")
option(UTSURE_ENABLE_DEPENDENCY_AUDIT "Verify the planned external dependency stack at configure time." ON)
option(UTSURE_REQUIRE_FFMPEG "Require FFmpeg to be available from an explicit isolated prefix." ON)
option(UTSURE_REQUIRE_LIBASSMOD "Require libassmod to be available during dependency audit." ON)
set(
    UTSURE_FFMPEG_ROOT
    ""
    CACHE PATH
    "Installed FFmpeg prefix pinned to the supported release series."
)
set(
    UTSURE_LIBASSMOD_ROOT
    ""
    CACHE PATH
    "Installed libassmod prefix. The prefix must provide a libass-compatible pkg-config file."
)

function(utsure_normalize_path output_variable input_path)
    if("${input_path}" STREQUAL "")
        set(${output_variable} "" PARENT_SCOPE)
        return()
    endif()

    file(TO_CMAKE_PATH "${input_path}" _normalized_path)
    cmake_path(NORMAL_PATH _normalized_path OUTPUT_VARIABLE _normalized_path)
    set(${output_variable} "${_normalized_path}" PARENT_SCOPE)
endfunction()

function(utsure_resolve_existing_path output_variable input_path)
    if(NOT EXISTS "${input_path}")
        set(${output_variable} "" PARENT_SCOPE)
        return()
    endif()

    file(REAL_PATH "${input_path}" _resolved_path)
    file(TO_CMAKE_PATH "${_resolved_path}" _resolved_path)
    string(TOLOWER "${_resolved_path}" _resolved_path)
    set(${output_variable} "${_resolved_path}" PARENT_SCOPE)
endfunction()

function(utsure_require_ffmpeg_release_series ffmpeg_root status_variable)
    set(_ffmpeg_executable_candidates
        "${ffmpeg_root}/bin/ffmpeg.exe"
        "${ffmpeg_root}/bin/ffmpeg"
    )
    set(_ffmpeg_executable "")
    foreach(_candidate IN LISTS _ffmpeg_executable_candidates)
        if(EXISTS "${_candidate}")
            set(_ffmpeg_executable "${_candidate}")
            break()
        endif()
    endforeach()

    if("${_ffmpeg_executable}" STREQUAL "")
        message(FATAL_ERROR
            "FFmpeg was expected under '${ffmpeg_root}', but no ffmpeg executable was found in its bin directory."
        )
    endif()

    execute_process(
        COMMAND "${_ffmpeg_executable}" -version
        RESULT_VARIABLE _ffmpeg_version_result
        OUTPUT_VARIABLE _ffmpeg_version_output
        ERROR_VARIABLE _ffmpeg_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _ffmpeg_version_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to query the pinned FFmpeg executable at '${_ffmpeg_executable}'. "
            "The executable returned: ${_ffmpeg_version_error}"
        )
    endif()

    string(REGEX MATCH "ffmpeg version n?([0-9]+\\.[0-9]+(\\.[0-9]+)?)" _ffmpeg_version_line "${_ffmpeg_version_output}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR
            "Could not parse an FFmpeg release version from '${_ffmpeg_executable} -version'."
        )
    endif()

    set(_ffmpeg_release_version "${CMAKE_MATCH_1}")
    if(NOT "${_ffmpeg_release_version}" VERSION_GREATER_EQUAL "${UTSURE_FFMPEG_REQUIRED_SERIES}" OR
       "${_ffmpeg_release_version}" VERSION_GREATER_EQUAL "${UTSURE_FFMPEG_NEXT_UNTESTED_SERIES}")
        message(FATAL_ERROR
            "The pinned FFmpeg executable resolved to release '${_ffmpeg_release_version}', but this project is "
            "pinned to the FFmpeg ${UTSURE_FFMPEG_REQUIRED_SERIES}.x series."
        )
    endif()

    set(${status_variable} "validated from ${ffmpeg_root} (ffmpeg ${_ffmpeg_release_version})" PARENT_SCOPE)
endfunction()

macro(utsure_configure_dependencies)
    if(UTSURE_BUILD_APP)
        find_package(Qt6 ${UTSURE_QT_MIN_VERSION} REQUIRED COMPONENTS Widgets Svg)
    endif()

    if(NOT UTSURE_ENABLE_DEPENDENCY_AUDIT)
        message(STATUS "UTSURE dependency audit is disabled.")
    else()
        find_package(PkgConfig REQUIRED)

        pkg_check_modules(UTSURE_LIBAVCODEC REQUIRED IMPORTED_TARGET GLOBAL libavcodec)
        pkg_check_modules(UTSURE_LIBAVFORMAT REQUIRED IMPORTED_TARGET GLOBAL libavformat)
        pkg_check_modules(UTSURE_LIBAVUTIL REQUIRED IMPORTED_TARGET GLOBAL libavutil)
        pkg_check_modules(UTSURE_LIBSWRESAMPLE REQUIRED IMPORTED_TARGET GLOBAL libswresample)
        pkg_check_modules(UTSURE_LIBSWSCALE REQUIRED IMPORTED_TARGET GLOBAL libswscale)
        pkg_check_modules(UTSURE_X264 REQUIRED IMPORTED_TARGET GLOBAL x264)
        pkg_check_modules(UTSURE_X265 REQUIRED IMPORTED_TARGET GLOBAL x265)

        set(_ffmpeg_status "not validated")
        if(UTSURE_REQUIRE_FFMPEG OR NOT "${UTSURE_FFMPEG_ROOT}" STREQUAL "")
            if("${UTSURE_FFMPEG_ROOT}" STREQUAL "")
                message(FATAL_ERROR
                    "UTSURE_REQUIRE_FFMPEG is ON, but UTSURE_FFMPEG_ROOT is empty. "
                    "Build FFmpeg into an isolated prefix and point UTSURE_FFMPEG_ROOT at that prefix."
                )
            endif()

            utsure_normalize_path(_ffmpeg_root "${UTSURE_FFMPEG_ROOT}")
            set(_ffmpeg_pc_candidates
                "${_ffmpeg_root}/lib/pkgconfig/libavcodec.pc"
                "${_ffmpeg_root}/lib64/pkgconfig/libavcodec.pc"
            )
            set(_ffmpeg_pc_expected "")
            foreach(_candidate IN LISTS _ffmpeg_pc_candidates)
                if(EXISTS "${_candidate}")
                    set(_ffmpeg_pc_expected "${_candidate}")
                    break()
                endif()
            endforeach()

            if("${_ffmpeg_pc_expected}" STREQUAL "")
                message(FATAL_ERROR
                    "FFmpeg was expected under '${_ffmpeg_root}', but no libavcodec pkg-config file was found."
                )
            endif()

            foreach(_component
                    libavcodec
                    libavformat
                    libavutil
                    libswresample
                    libswscale)
                pkg_get_variable(_pcfiledir "${_component}" pcfiledir)
                utsure_normalize_path(_actual_pcfiledir "${_pcfiledir}")
                set(_actual_pcfile "${_actual_pcfiledir}/${_component}.pc")

                if(NOT EXISTS "${_actual_pcfile}")
                    message(FATAL_ERROR
                        "pkg-config resolved ${_component} from '${_actual_pcfiledir}', but '${_actual_pcfile}' does not exist."
                    )
                endif()

                set(_expected_pcfile "${_ffmpeg_root}/lib/pkgconfig/${_component}.pc")
                if(NOT EXISTS "${_expected_pcfile}")
                    set(_expected_pcfile "${_ffmpeg_root}/lib64/pkgconfig/${_component}.pc")
                endif()

                if(NOT EXISTS "${_expected_pcfile}")
                    message(FATAL_ERROR
                        "FFmpeg was expected under '${_ffmpeg_root}', but '${_component}.pc' was not found there."
                    )
                endif()

                utsure_resolve_existing_path(_expected_pcfile_resolved "${_expected_pcfile}")
                utsure_resolve_existing_path(_actual_pcfile_resolved "${_actual_pcfile}")
                if(NOT _expected_pcfile_resolved STREQUAL _actual_pcfile_resolved)
                    message(FATAL_ERROR
                        "pkg-config resolved '${_component}' from '${_actual_pcfile_resolved}', but pinned FFmpeg was expected at "
                        "'${_expected_pcfile_resolved}'. Ensure the pinned FFmpeg pkg-config prefix is ahead of any system entry."
                    )
                endif()
            endforeach()

            utsure_require_ffmpeg_release_series("${_ffmpeg_root}" _ffmpeg_status)
        endif()

        if(NOT TARGET utsure_ffmpeg)
            add_library(utsure_ffmpeg INTERFACE)
            add_library(utsure::ffmpeg ALIAS utsure_ffmpeg)
            target_link_libraries(utsure_ffmpeg
                INTERFACE
                    PkgConfig::UTSURE_LIBAVCODEC
                    PkgConfig::UTSURE_LIBAVFORMAT
                    PkgConfig::UTSURE_LIBAVUTIL
                    PkgConfig::UTSURE_LIBSWRESAMPLE
                    PkgConfig::UTSURE_LIBSWSCALE
            )
        endif()

        if(NOT TARGET utsure_x264)
            add_library(utsure_x264 INTERFACE)
            add_library(utsure::x264 ALIAS utsure_x264)
            target_link_libraries(utsure_x264 INTERFACE PkgConfig::UTSURE_X264)
        endif()

        if(NOT TARGET utsure_x265)
            add_library(utsure_x265 INTERFACE)
            add_library(utsure::x265 ALIAS utsure_x265)
            target_link_libraries(utsure_x265 INTERFACE PkgConfig::UTSURE_X265)
        endif()

        set(_libassmod_status "not validated")
        set(_libassmod_pcfiledir "")

        if(UTSURE_REQUIRE_LIBASSMOD OR NOT "${UTSURE_LIBASSMOD_ROOT}" STREQUAL "")
            if("${UTSURE_LIBASSMOD_ROOT}" STREQUAL "")
                message(FATAL_ERROR
                    "UTSURE_REQUIRE_LIBASSMOD is ON, but UTSURE_LIBASSMOD_ROOT is empty. "
                    "Build libassmod into an isolated prefix and point UTSURE_LIBASSMOD_ROOT at that prefix."
                )
            endif()

            utsure_normalize_path(_libassmod_root "${UTSURE_LIBASSMOD_ROOT}")

            set(_libassmod_header_path "${_libassmod_root}/include/ass/ass.h")
            set(_libassmod_pc_candidates
                "${_libassmod_root}/lib/pkgconfig/libass.pc"
                "${_libassmod_root}/lib64/pkgconfig/libass.pc"
            )
            set(_libassmod_pc_expected "")

            foreach(_candidate IN LISTS _libassmod_pc_candidates)
                if(EXISTS "${_candidate}")
                    set(_libassmod_pc_expected "${_candidate}")
                    break()
                endif()
            endforeach()

            if(NOT EXISTS "${_libassmod_header_path}")
                message(FATAL_ERROR
                    "libassmod was expected under '${_libassmod_root}', but '${_libassmod_header_path}' was not found."
                )
            endif()

            if("${_libassmod_pc_expected}" STREQUAL "")
                message(FATAL_ERROR
                    "libassmod was expected under '${_libassmod_root}', but no libass pkg-config file was found."
                )
            endif()

            pkg_check_modules(UTSURE_LIBASS REQUIRED IMPORTED_TARGET GLOBAL libass)
            pkg_get_variable(UTSURE_LIBASS_PCFILEDIR libass pcfiledir)
            utsure_normalize_path(_libassmod_pcfiledir "${UTSURE_LIBASS_PCFILEDIR}")
            set(_libassmod_actual_pcfile "${_libassmod_pcfiledir}/libass.pc")

            if(NOT EXISTS "${_libassmod_actual_pcfile}")
                message(FATAL_ERROR
                    "pkg-config resolved libass from '${_libassmod_pcfiledir}', but '${_libassmod_actual_pcfile}' does not exist."
                )
            endif()

            utsure_resolve_existing_path(_libassmod_expected_pcfile_resolved "${_libassmod_pc_expected}")
            utsure_resolve_existing_path(_libassmod_actual_pcfile_resolved "${_libassmod_actual_pcfile}")

            if(NOT _libassmod_expected_pcfile_resolved STREQUAL _libassmod_actual_pcfile_resolved)
                message(FATAL_ERROR
                    "pkg-config resolved 'libass' from '${_libassmod_actual_pcfile_resolved}', but libassmod was expected at "
                    "'${_libassmod_expected_pcfile_resolved}'. Ensure libassmod's pkg-config prefix is ahead of any system libass entry."
                )
            endif()

            if(NOT TARGET utsure_subtitle_renderer_dependency)
                add_library(utsure_subtitle_renderer_dependency INTERFACE)
                add_library(utsure::subtitle_renderer_dependency ALIAS utsure_subtitle_renderer_dependency)
                target_link_libraries(utsure_subtitle_renderer_dependency INTERFACE PkgConfig::UTSURE_LIBASS)
            endif()

            set(_libassmod_status "validated from ${_libassmod_pcfiledir}")
            set(_libassmod_pcfiledir "${_libassmod_pcfiledir}")
        endif()

        message(STATUS "UTSURE dependency audit summary:")
        if(UTSURE_BUILD_APP)
            message(STATUS "  Qt6 Widgets/Svg: found via CMake package config")
        endif()
        message(STATUS "  FFmpeg pin: ${_ffmpeg_status}")
        message(STATUS "  FFmpeg libavcodec: ${UTSURE_LIBAVCODEC_VERSION}")
        message(STATUS "  FFmpeg libavformat: ${UTSURE_LIBAVFORMAT_VERSION}")
        message(STATUS "  FFmpeg libavutil: ${UTSURE_LIBAVUTIL_VERSION}")
        message(STATUS "  FFmpeg libswresample: ${UTSURE_LIBSWRESAMPLE_VERSION}")
        message(STATUS "  FFmpeg libswscale: ${UTSURE_LIBSWSCALE_VERSION}")
        message(STATUS "  x264: ${UTSURE_X264_VERSION}")
        message(STATUS "  x265: ${UTSURE_X265_VERSION}")
        message(STATUS "  libassmod: ${_libassmod_status}")
    endif()
endmacro()
