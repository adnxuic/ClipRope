if(BUILD_TESTING)
    add_executable(cliprope_office_matrix_host tests/compatibility/office_matrix_host.cpp src/platform_win/office_selection.cpp)
    target_link_libraries(cliprope_office_matrix_host PRIVATE cliprope_card_browser cliprope_history_commands cliprope_history oleacc oleaut32)
    option(CLIPROPE_TEST_OFFICE_MATRIX "Run isolated Excel and PowerPoint matrix on installed desktop apps" OFF)
    if(CLIPROPE_TEST_OFFICE_MATRIX)
        if(NOT CLIPROPE_DESKTOP_PROBES)
            message(FATAL_ERROR "Office matrix requires -DCLIPROPE_DESKTOP_PROBES=ON")
        endif()
        find_program(CLIPROPE_MATRIX_POWERSHELL powershell REQUIRED)
        add_test(NAME P5-06.office_matrix COMMAND cliprope_station_test "${CLIPROPE_MATRIX_POWERSHELL}"
            -NoProfile -NoLogo -Sta -File "${PROJECT_SOURCE_DIR}/tests/compatibility/office_matrix.ps1"
            -DesktopExe $<TARGET_FILE:cliprope_desktop>
            -MatrixExe $<TARGET_FILE:cliprope_office_matrix_host> -Report "${CMAKE_CURRENT_BINARY_DIR}/office-matrix.log")
        set_tests_properties(P5-06.office_matrix PROPERTIES TIMEOUT 360 RUN_SERIAL TRUE)
    endif()
endif()
