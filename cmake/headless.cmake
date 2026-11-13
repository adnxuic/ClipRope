add_library(cliprope_application STATIC
    src/application/capture.cpp
    src/application/invoke.cpp)
target_link_libraries(cliprope_application PUBLIC cliprope_domain)
add_library(cliprope_history_commands STATIC
    src/platform_win/capture_admissions.cpp
    src/platform_win/history_clipboard.cpp)
include(cmake/worker.cmake)
target_link_libraries(cliprope_history_commands PUBLIC cliprope_application cliprope_delivery cliprope_clipboard)
add_executable(cliprope_headless src/host/headless.cpp)
target_link_libraries(cliprope_headless PRIVATE cliprope_history_commands cliprope_history cliprope_hotkey_settings)

if(BUILD_TESTING)
    add_executable(cliprope_invocation_unit tests/unit/invocation.cpp)
    target_link_libraries(cliprope_invocation_unit PRIVATE cliprope_application)
    add_test(NAME ARCH-11.pure_invocations COMMAND cliprope_invocation_unit)
    add_executable(cliprope_capture_order_test tests/integration/capture_order.cpp)
    target_link_libraries(cliprope_capture_order_test PRIVATE cliprope_application cliprope_history)
    add_test(NAME ARCH-00.bound_capture_order COMMAND cliprope_capture_order_test)
    add_executable(cliprope_virtual_restore_test tests/integration/virtual_restore.cpp)
    target_link_libraries(cliprope_virtual_restore_test PRIVATE cliprope_history_commands cliprope_history)
    add_test(NAME P5-01.adapter_bounds COMMAND cliprope_virtual_restore_test --bounds)
    add_test(NAME P5-01.flush_native_reader COMMAND cliprope_station_test $<TARGET_FILE:cliprope_virtual_restore_test>
        --locked-flush $<TARGET_FILE:cliprope_ole_source>)
    add_test(NAME P5-01.adapter_contract COMMAND cliprope_station_test $<TARGET_FILE:cliprope_virtual_restore_test>
        --contract $<TARGET_FILE:cliprope_ole_source> $<TARGET_FILE:cliprope_headless>)
    add_test(NAME P5-04.virtual_files COMMAND cliprope_station_test $<TARGET_FILE:cliprope_virtual_restore_test>
        $<TARGET_FILE:cliprope_ole_source> $<TARGET_FILE:cliprope_headless>)
    if(CLIPROPE_DESKTOP_PROBES)
        add_test(NAME ARCH-13.partial_virtual_flush COMMAND cliprope_station_test $<TARGET_FILE:cliprope_virtual_restore_test>
            --partial-install $<TARGET_FILE:cliprope_ole_source> $<TARGET_FILE:cliprope_headless> $<TARGET_FILE:cliprope_desktop>)
        set_tests_properties(ARCH-13.partial_virtual_flush PROPERTIES TIMEOUT 60)
    endif()
    set_tests_properties(P5-01.adapter_contract P5-01.flush_native_reader P5-04.virtual_files PROPERTIES TIMEOUT 120)
    add_executable(cliprope_headless_test tests/integration/headless_test.cpp)
    target_link_libraries(cliprope_headless_test PRIVATE cliprope_history_commands cliprope_history cliprope_hotkey_settings)
    add_test(NAME AR2-15.profile_compatibility COMMAND cliprope_station_test
        $<TARGET_FILE:cliprope_headless_test> reader $<TARGET_FILE:cliprope_ole_source> $<TARGET_FILE:cliprope_headless>)
    set_tests_properties(AR2-15.profile_compatibility PROPERTIES RUN_SERIAL TRUE TIMEOUT 45)
    add_test(NAME FOLLOW-01.collector_head COMMAND cliprope_station_test
        $<TARGET_FILE:cliprope_headless_test> follow $<TARGET_FILE:cliprope_ole_source> $<TARGET_FILE:cliprope_headless>)
    add_test(NAME P1-05.capture_admission COMMAND cliprope_station_test
        $<TARGET_FILE:cliprope_headless_test> capture $<TARGET_FILE:cliprope_ole_source> $<TARGET_FILE:cliprope_headless>)
    add_test(NAME P1-06.headless_restart COMMAND cliprope_station_test
        $<TARGET_FILE:cliprope_headless_test> invoke $<TARGET_FILE:cliprope_ole_source> $<TARGET_FILE:cliprope_headless>)
    set_tests_properties(P1-05.capture_admission P1-06.headless_restart PROPERTIES TIMEOUT 80)
endif()
