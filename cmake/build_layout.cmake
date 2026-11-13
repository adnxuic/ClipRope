# One checkout has one mutable build tree and one development desktop image.
file(REAL_PATH "${CMAKE_SOURCE_DIR}/build/cmake" expected_binary)
file(REAL_PATH "${CMAKE_BINARY_DIR}" actual_binary)
if(NOT actual_binary STREQUAL expected_binary)
    message(FATAL_ERROR "Use tools/build.ps1. Only build/cmake is supported; extra desktop trees are prohibited.")
endif()
if(NOT DEFINED ENV{CLIPROPE_BUILD_SESSION} OR NOT EXISTS "$ENV{CLIPROPE_BUILD_SESSION}")
    message(FATAL_ERROR "Configure through tools/build.ps1 so the repository build lock is held.")
endif()
# A generator expression prevents Visual Studio from appending Debug/Release.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/app/$<0:>")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/symbols/$<CONFIG>")
