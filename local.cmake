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
endif()
