add_library(cliprope_clipboard STATIC
    src/platform_win/clipboard.cpp
    src/platform_win/virtual_files.cpp
    src/capture_worker/stream_reader.cpp
    src/capture_worker/virtual_streams.cpp
    src/capture_worker/capture.cpp)
target_include_directories(cliprope_clipboard PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(cliprope_clipboard PUBLIC ole32 user32 shell32 uuid PRIVATE cliprope_media shlwapi)

if(BUILD_TESTING)
    target_compile_definitions(cliprope_clipboard PRIVATE CLIPROPE_FLUSH_PROBES)
    add_executable(cliprope_station_test tests/integration/clipboard_station.cpp)
    target_link_libraries(cliprope_station_test PRIVATE user32)
    add_executable(cliprope_ole_source tests/clipboard_sources/ole_source.cpp)
    target_link_libraries(cliprope_ole_source PRIVATE cliprope_clipboard)
    add_executable(cliprope_clipboard_test tests/integration/clipboard_test.cpp)
    target_link_libraries(cliprope_clipboard_test PRIVATE cliprope_clipboard)
    add_test(NAME P0-03.external_capture COMMAND cliprope_station_test
        $<TARGET_FILE:cliprope_clipboard_test> $<TARGET_FILE:cliprope_ole_source> capture)
    add_test(NAME P0-04.publication_identity COMMAND cliprope_station_test
        $<TARGET_FILE:cliprope_clipboard_test> $<TARGET_FILE:cliprope_ole_source> publication)
    set_tests_properties(P0-03.external_capture P0-04.publication_identity PROPERTIES TIMEOUT 50)
endif()
