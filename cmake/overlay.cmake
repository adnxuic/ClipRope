add_library(cliprope_overlay STATIC
    src/platform_win/overlay.cpp
    src/rendering/probe_card.cpp)
target_include_directories(cliprope_overlay PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cliprope_overlay PUBLIC user32 gdi32)

add_executable(cliprope_overlay_probe tests/compatibility/overlay_probe.cpp)
target_link_libraries(cliprope_overlay_probe PRIVATE cliprope_overlay shcore)
if(BUILD_TESTING)
    add_test(NAME p0.overlay.private_desktop COMMAND cliprope_overlay_probe --isolated)
    set_tests_properties(p0.overlay.private_desktop PROPERTIES TIMEOUT 25)
endif()
