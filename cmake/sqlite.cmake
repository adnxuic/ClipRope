# The release archive is kept locally so clean builds work offline.
include(FetchContent)
FetchContent_Declare(sqlite_source
    URL "${PROJECT_SOURCE_DIR}/third_party/sqlite/sqlite-amalgamation-3540100.zip"
    URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite_source)
add_library(cliprope_sqlite STATIC "${sqlite_source_SOURCE_DIR}/sqlite3.c")
target_include_directories(cliprope_sqlite PUBLIC "${sqlite_source_SOURCE_DIR}")
target_compile_definitions(cliprope_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
    set_target_properties(cliprope_sqlite PROPERTIES COMPILE_OPTIONS "/W0;/utf-8;/bigobj")
endif()
