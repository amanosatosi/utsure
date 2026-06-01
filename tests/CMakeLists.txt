function(utsure_label_tests_matching regex)
    get_property(_utsure_tests DIRECTORY PROPERTY TESTS)
    foreach(_utsure_test IN LISTS _utsure_tests)
        if(_utsure_test MATCHES "${regex}")
            set_property(TEST "${_utsure_test}" APPEND PROPERTY LABELS ${ARGN})
        endif()
    endforeach()
endfunction()

add_subdirectory(core)

if(UTSURE_BUILD_APP)
    add_subdirectory(app)
endif()
