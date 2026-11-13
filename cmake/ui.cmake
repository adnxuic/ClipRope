# Source rules run before the shared renderer, including incremental builds.
add_custom_target(cliprope_ui_contracts
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_ui.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
add_dependencies(cliprope_fluent cliprope_ui_contracts)
if(BUILD_TESTING)
    add_test(NAME UI-18.source_contracts
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_ui.py")
    add_test(NAME UI-18.scanner_tests
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/unit/ui_contracts.py")
endif()
