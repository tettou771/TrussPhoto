# =============================================================================
# TrussPhoto - project-specific dependencies
# =============================================================================
# These are system libraries installed via brew, used only by this project.
# Not worth making into shared addons.
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

    # lensfun - lens correction (distortion, vignetting, CA)
    # brew install lensfun
    pkg_check_modules(LENSFUN REQUIRED lensfun)
    target_include_directories(${PROJECT_NAME} PRIVATE ${LENSFUN_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${LENSFUN_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${LENSFUN_LIBRARIES})
    target_compile_definitions(${PROJECT_NAME} PRIVATE TCX_HAS_LENSFUN=1)
    message(STATUS "[${PROJECT_NAME}] Found lensfun: ${LENSFUN_VERSION}")
endif()
