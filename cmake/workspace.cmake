add_library(cliprope_geometry STATIC
    src/presentation/hanging_layout.cpp
    src/presentation/swing.cpp
    src/presentation/viewport.cpp
    src/presentation/overview.cpp)
target_link_libraries(cliprope_geometry PUBLIC cliprope_domain cliprope_localization)

add_library(cliprope_workspace STATIC
    src/platform_win/card_drag.cpp
    src/platform_win/floating.cpp
    src/platform_win/inline_editor_host.cpp
    src/platform_win/find_popup.cpp
    src/platform_win/row_input.cpp
    src/platform_win/workspace.cpp
    src/rendering/workspace_paint.cpp)
target_include_directories(cliprope_workspace PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cliprope_workspace PUBLIC cliprope_geometry cliprope_fluent d2d1 dwrite user32 gdi32 msimg32 shcore imm32 comctl32)

if(BUILD_TESTING)
    add_executable(cliprope_tab_navigation_test tests/unit/tab_navigation.cpp)
    target_link_libraries(cliprope_tab_navigation_test PRIVATE cliprope_workspace)
    add_test(NAME KEY-01.tab_routing COMMAND cliprope_tab_navigation_test)
    if(CLIPROPE_DESKTOP_PROBES AND CLIPROPE_TEST_DESKTOP_INPUT)
        add_executable(cliprope_hover_shortcuts_test tests/compatibility/hover_shortcuts.cpp)
        target_link_libraries(cliprope_hover_shortcuts_test PRIVATE cliprope_workspace cliprope_history cliprope_hotkey_settings cliprope_clipboard)
        add_test(NAME KEY-02.hover_shortcuts COMMAND cliprope_hover_shortcuts_test $<TARGET_FILE:cliprope_desktop>)
        add_test(NAME FIND-05.desktop COMMAND cliprope_hover_shortcuts_test $<TARGET_FILE:cliprope_desktop> --find)
        add_test(NAME KEY-04.configurable COMMAND cliprope_hover_shortcuts_test $<TARGET_FILE:cliprope_desktop> --configure)
        set_tests_properties(KEY-04.configurable FIND-05.desktop KEY-02.hover_shortcuts PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    add_executable(cliprope_viewport_test tests/unit/viewport.cpp)
    target_link_libraries(cliprope_viewport_test PRIVATE cliprope_geometry)
    add_test(NAME P2-02.buffered_viewport COMMAND cliprope_viewport_test)
    add_executable(cliprope_hanging_test tests/unit/hanging_layout.cpp)
    target_link_libraries(cliprope_hanging_test PRIVATE cliprope_geometry)
    add_test(NAME VIS-01.hanging_layout COMMAND cliprope_hanging_test)
    add_test(NAME ARCH-11.pure_linkage COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/unit/pure_linkage.py"
        "${CMAKE_BINARY_DIR}" "${CMAKE_LINKER}" "$<TARGET_FILE:cliprope_invocation_unit>"
        "$<TARGET_FILE:cliprope_viewport_test>" "$<TARGET_FILE:cliprope_hanging_test>")
    if(CLIPROPE_DESKTOP_PROBES)
        add_executable(cliprope_find_appearance tests/compatibility/find_appearance.cpp)
        target_link_libraries(cliprope_find_appearance PRIVATE cliprope_workspace)
        add_test(NAME FIND-06.popup_appearance COMMAND "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:cliprope_find_appearance>" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/find-visuals"
            -P "${PROJECT_SOURCE_DIR}/tests/compatibility/find_appearance.cmake")
    endif()
    add_executable(cliprope_gallery_test tests/compatibility/card_gallery.cpp)
    target_link_libraries(cliprope_gallery_test PRIVATE cliprope_workspace)
    add_test(NAME VIS-02.card_gallery COMMAND cliprope_gallery_test)
    set_tests_properties(VIS-02.card_gallery PROPERTIES TIMEOUT 30)
    add_executable(cliprope_surface_test tests/compatibility/workspace_surface.cpp)
    target_link_libraries(cliprope_surface_test PRIVATE cliprope_workspace)
    add_test(NAME P2-02.workspace_surface COMMAND cliprope_surface_test --isolated $<TARGET_FILE:cliprope_overlay_probe>)
    set_tests_properties(P2-02.workspace_surface PROPERTIES TIMEOUT 30)
    if(CLIPROPE_TEST_DESKTOP_INPUT)
        add_test(NAME HOV-01.desktop_hover COMMAND cliprope_surface_test --physical $<TARGET_FILE:cliprope_overlay_probe>)
        set_tests_properties(HOV-01.desktop_hover PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endif()
    add_executable(cliprope_floating_surface_test tests/compatibility/floating_surface.cpp)
    target_link_libraries(cliprope_floating_surface_test PRIVATE cliprope_workspace)
    if(CLIPROPE_DESKTOP_PROBES)
        target_compile_definitions(cliprope_workspace PRIVATE CLIPROPE_DRAW_PROBES)
        target_compile_definitions(cliprope_floating_surface_test PRIVATE CLIPROPE_DRAW_PROBES)
        add_test(NAME ARCH-07.shared_draw_recovery COMMAND cliprope_floating_surface_test
            --draw-recovery "${CMAKE_CURRENT_BINARY_DIR}/arch-07-frames")
        set_tests_properties(ARCH-07.shared_draw_recovery PROPERTIES TIMEOUT 30)
        add_test(NAME OPT-05.editor_frame_retry COMMAND cliprope_floating_surface_test --editor-recovery
            "${CMAKE_BINARY_DIR}/opt-b-editor-frames")
        set_tests_properties(OPT-05.editor_frame_retry PROPERTIES TIMEOUT 15)
        add_test(NAME AR2-10.color_session COMMAND cliprope_floating_surface_test --color-frames
            "${CMAKE_CURRENT_BINARY_DIR}/ar2-c-frames")
        add_test(NAME ARCH-10.close_frames COMMAND cliprope_floating_surface_test
            --close-frames "${CMAKE_CURRENT_BINARY_DIR}/arch-10-frames")
        set_tests_properties(ARCH-10.close_frames PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME P3-01.floating_surface COMMAND cliprope_floating_surface_test)
    set_tests_properties(P3-01.floating_surface PROPERTIES TIMEOUT 30)
    add_executable(cliprope_overview_test tests/compatibility/overview_surface.cpp)
    target_link_libraries(cliprope_overview_test PRIVATE cliprope_workspace)
    add_test(NAME P4-04.virtualized_overview COMMAND cliprope_overview_test)
    add_test(NAME DRAG-06.card_edge_scroll COMMAND cliprope_overview_test --card-edge)
    set_tests_properties(DRAG-06.card_edge_scroll PROPERTIES TIMEOUT 45)
    set_tests_properties(P4-04.virtualized_overview PROPERTIES TIMEOUT 30)
    if(CLIPROPE_TEST_DESKTOP_INPUT)
        add_test(NAME ROPE-01.hover_wheel COMMAND cliprope_overview_test --physical-wheel)
        set_tests_properties(ROPE-01.hover_wheel PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
    endif()
endif()
