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
    # NOTE: brew onnxruntime 1.24.1 produces NaN for EVA02-based models (LINE CLIP v2).
    # The official Microsoft build (pip onnxruntime) works correctly.
    # Use pip's dylib if available, fall back to brew.
    set(_ORT_PIP_DIR "${CMAKE_SOURCE_DIR}/.venv/lib")
    file(GLOB _ORT_PIP_DYLIB "${_ORT_PIP_DIR}/python*/site-packages/onnxruntime/capi/libonnxruntime.*.dylib")
    if(_ORT_PIP_DYLIB)
        list(GET _ORT_PIP_DYLIB 0 _ORT_PIP_DYLIB)
        # Use brew headers (version-compatible) + pip dylib (correct build)
        pkg_check_modules(ORT REQUIRED libonnxruntime)
        target_include_directories(${PROJECT_NAME} PRIVATE ${ORT_INCLUDE_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE "${_ORT_PIP_DYLIB}")
        message(STATUS "[${PROJECT_NAME}] Using pip onnxruntime: ${_ORT_PIP_DYLIB}")
    else()
        pkg_check_modules(ORT REQUIRED libonnxruntime)
        target_include_directories(${PROJECT_NAME} PRIVATE ${ORT_INCLUDE_DIRS})
        target_link_directories(${PROJECT_NAME} PRIVATE ${ORT_LIBRARY_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${ORT_LIBRARIES})
        message(STATUS "[${PROJECT_NAME}] Found onnxruntime (brew): ${ORT_VERSION}")
    endif()
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
    SQLITE_THREADSAFE=2
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)
target_compile_options(sqlite3_lib PRIVATE -w)
target_link_libraries(${PROJECT_NAME} PRIVATE sqlite3_lib)
