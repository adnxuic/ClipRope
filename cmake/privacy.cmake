target_sources(cliprope_database PRIVATE src/persistence/privacy.cpp)
if(BUILD_TESTING AND CLIPROPE_DESKTOP_PROBES)
    add_executable(cliprope_privacy_test tests/integration/privacy.cpp)
    target_link_libraries(cliprope_privacy_test PRIVATE cliprope_history_commands cliprope_history)
    add_test(NAME P6-04.privacy_controls COMMAND cliprope_station_test $<TARGET_FILE:cliprope_privacy_test>
        $<TARGET_FILE:cliprope_desktop> $<TARGET_FILE:cliprope_ole_source> $<TARGET_FILE:cliprope_headless>)
    set_tests_properties(P6-04.privacy_controls PROPERTIES TIMEOUT 75 RUN_SERIAL TRUE)
endif()
