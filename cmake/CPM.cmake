# ============================================================================
# Warploom - CPM.cmake Integration
# ============================================================================
# CPM.cmake adds the single third-party dependency the engine has.
# https://github.com/cpm-cmake/CPM.cmake
# ============================================================================

set(CPM_VERSION "0.40.2")

# The bootstrap script is vendored in-tree (cmake/CPM_0.40.2.cmake) and used
# from the source tree. It used to be downloaded into ${CMAKE_BINARY_DIR} on
# every fresh binary dir, which meant even a warm-cache configure needed the
# network -- and the vendored copy was never referenced by anything.
set(CPM_DOWNLOAD_LOCATION "${CMAKE_CURRENT_LIST_DIR}/CPM_${CPM_VERSION}.cmake")

if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
    message(FATAL_ERROR
        "Warploom: vendored CPM.cmake is missing from ${CMAKE_CURRENT_LIST_DIR}. "
        "Restore cmake/CPM_${CPM_VERSION}.cmake, or point CPM_DOWNLOAD_LOCATION at "
        "a local copy.")
endif()

# ============================================================================
# Source cache
# ============================================================================
# Shared across binary directories, so switching preset does not re-download
# googletest. This is also the path CI should cache.
#
# This MUST be decided before CPM is included: CPM reads
# CPM_SOURCE_CACHE / ENV{CPM_SOURCE_CACHE} at include() time and creates the
# cache entry itself (defaulting to OFF). Setting it afterwards silently
# loses to the entry CPM just wrote, which is how this ended up as OFF.
if(NOT DEFINED CPM_SOURCE_CACHE)
    if(DEFINED ENV{CPM_SOURCE_CACHE})
        set(CPM_SOURCE_CACHE "$ENV{CPM_SOURCE_CACHE}")
    elseif(DEFINED ENV{HOME})
        set(CPM_SOURCE_CACHE "$ENV{HOME}/.cache/warploom/cpm")
    else()
        set(CPM_SOURCE_CACHE "${CMAKE_BINARY_DIR}/cpm_cache")
    endif()
    set(CPM_SOURCE_CACHE "${CPM_SOURCE_CACHE}" CACHE PATH "CPM source cache")
endif()

include("${CPM_DOWNLOAD_LOCATION}")

# ============================================================================
# CPM.cmake Configuration
# ============================================================================
file(MAKE_DIRECTORY "${CPM_SOURCE_CACHE}")

set(CPM_USE_LOCAL_PACKAGES ON CACHE BOOL "Prefer a find_package() hit over a download")
set(CPM_LOCAL_PACKAGES_ONLY OFF CACHE BOOL "Never download; fail if a package is missing")

# ============================================================================
# Helper
# ============================================================================
function(warploom_add_cpm_package PACKAGE_NAME)
    cmake_parse_arguments(ARGS
        "OPTIONAL"
        "VERSION;GIT_TAG;GIT_REPOSITORY;GITHUB_REPOSITORY;URL"
        "OPTIONS"
        ${ARGN}
    )

    if(NOT WARPLOOM_USE_CPM)
        return()
    endif()

    # A package is REQUIRED unless it is explicitly marked OPTIONAL: a caller
    # that forgets the keyword should get the package, not silence.
    if(NOT ARGS_OPTIONAL)
        CPMAddPackage(
            NAME ${PACKAGE_NAME}
            VERSION ${ARGS_VERSION}
            GIT_TAG ${ARGS_GIT_TAG}
            GIT_REPOSITORY ${ARGS_GIT_REPOSITORY}
            GITHUB_REPOSITORY ${ARGS_GITHUB_REPOSITORY}
            URL ${ARGS_URL}
            OPTIONS ${ARGS_OPTIONS}
        )
    else()
        CPMTryAddPackage(
            NAME ${PACKAGE_NAME}
            VERSION ${ARGS_VERSION}
            GIT_TAG ${ARGS_GIT_TAG}
            GIT_REPOSITORY ${ARGS_GIT_REPOSITORY}
            GITHUB_REPOSITORY ${ARGS_GITHUB_REPOSITORY}
            URL ${ARGS_URL}
            OPTIONS ${ARGS_OPTIONS}
        )
    endif()
endfunction()

message(STATUS "CPM.cmake v${CPM_VERSION} (source cache: ${CPM_SOURCE_CACHE})")