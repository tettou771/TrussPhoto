# =============================================================================
# TrussPhoto - project-specific dependencies
# =============================================================================
# System libraries installed via brew, used only by this project.
# =============================================================================

if(NOT EMSCRIPTEN)
    find_package(PkgConfig REQUIRED)

    # exiv2 - EXIF/IPTC/XMP/MakerNote metadata
    # brew install exiv2
    pkg_check_modules(EXIV2 REQUIRED exiv2)
    target_include_directories(${PROJECT_NAME} PRIVATE ${EXIV2_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${EXIV2_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${EXIV2_LIBRARIES})
    message(STATUS "[${PROJECT_NAME}] Found exiv2: ${EXIV2_VERSION}")

    # libjxl - JPEG XL smart preview encode/decode
    # brew install jpeg-xl
    pkg_check_modules(JXL REQUIRED libjxl libjxl_threads)
    target_include_directories(${PROJECT_NAME} PRIVATE ${JXL_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${JXL_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${JXL_LIBRARIES})
    message(STATUS "[${PROJECT_NAME}] Found libjxl: ${JXL_VERSION}")

    # onnxruntime - CLIP embedding inference
    # brew install onnxruntime
    pkg_check_modules(ORT REQUIRED libonnxruntime)
    target_include_directories(${PROJECT_NAME} PRIVATE ${ORT_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${ORT_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${ORT_LIBRARIES})
    message(STATUS "[${PROJECT_NAME}] Found onnxruntime: ${ORT_VERSION}")
endif()

# SQLite amalgamation - embedded database for photo library
include(FetchContent)
FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)

add_library(sqlite3_lib STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3_lib PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3_lib PRIVATE
    SQLITE_THREADSAFE=2
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)
target_compile_options(sqlite3_lib PRIVATE -w)
target_link_libraries(${PROJECT_NAME} PRIVATE sqlite3_lib)
