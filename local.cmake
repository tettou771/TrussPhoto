# =============================================================================
# TrussPhoto - project-specific dependencies
# =============================================================================
# System libraries installed via brew, used only by this project.
# =============================================================================

if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-framework MetalPerformanceShaders" "-framework Accelerate")
endif()

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
    # Official Microsoft release (includes CoreML execution provider)
    set(ORT_DIR "${CMAKE_SOURCE_DIR}/lib/onnxruntime-osx-arm64-1.24.1")
    target_include_directories(${PROJECT_NAME} PRIVATE "${ORT_DIR}/include")
    target_link_directories(${PROJECT_NAME} PRIVATE "${ORT_DIR}/lib")
    target_link_libraries(${PROJECT_NAME} PRIVATE onnxruntime)
    message(STATUS "[${PROJECT_NAME}] Using onnxruntime: ${ORT_DIR}")
endif()

# SQLite amalgamation - embedded database for photo library
include(FetchContent)
FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)

# SentencePiece - tokenizer for Japanese Stable CLIP text encoder
set(SPM_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_Declare(
    sentencepiece
    GIT_REPOSITORY https://github.com/google/sentencepiece.git
    GIT_TAG        v0.2.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sentencepiece)
target_link_libraries(${PROJECT_NAME} PRIVATE sentencepiece-static)
target_include_directories(${PROJECT_NAME} PRIVATE ${sentencepiece_SOURCE_DIR}/src)

add_library(sqlite3_lib STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3_lib PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3_lib PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)
target_compile_options(sqlite3_lib PRIVATE -w)
target_link_libraries(${PROJECT_NAME} PRIVATE sqlite3_lib)

# Allow subfolder headers to include sibling headers
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
