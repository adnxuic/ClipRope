if(NOT DEFINED ENV{CLIPROPE_BUILD_SESSION} OR NOT EXISTS "$ENV{CLIPROPE_BUILD_SESSION}")
    message(FATAL_ERROR "Build through tools/build.ps1; the checkout build session is missing.")
endif()
