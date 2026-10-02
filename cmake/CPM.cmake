# ============================================================================
# OmniCpp Template - CPM.cmake Integration
# ============================================================================
# CPM.cmake is a CMake script that adds dependency management
# https://github.com/cpm-cmake/CPM.cmake
# ============================================================================

# CPM version
set(CPM_VERSION "0.40.2")
set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_VERSION}.cmake")

# Download CPM.cmake if not present
if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
    message(STATUS "Downloading CPM.cmake v${CPM_VERSION}...")
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake
        "${CPM_DOWNLOAD_LOCATION}"
        EXPECTED_HASH SHA256=c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d
        SHOW_PROGRESS
    )

    if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
        message(FATAL_ERROR "Failed to download CPM.cmake")
    endif()

    message(STATUS "CPM.cmake downloaded successfully")
endif()

# Include CPM.cmake
include("${CPM_DOWNLOAD_LOCATION}")

# ============================================================================
# CPM.cmake Configuration
# ============================================================================
set(CPM_USE_LOCAL_PACKAGES ON CACHE BOOL "Use local packages if available")
set(CPM_LOCAL_PACKAGES_ONLY OFF CACHE BOOL "Only use local packages")
set(CPM_DOWNLOAD_ALL ON CACHE BOOL "Download all dependencies")
set(CPM_DONT_UPDATE_PACKAGE_CACHE OFF CACHE BOOL "Don't update package cache")
set(CPM_SOURCE_CACHE "${CMAKE_BINARY_DIR}/CPM_cache" CACHE PATH "CPM source cache")

# Create CPM cache directory
if(NOT EXISTS "${CPM_SOURCE_CACHE}")
    file(MAKE_DIRECTORY "${CPM_SOURCE_CACHE}")
endif()

# ============================================================================
# CPM.cmake Helper Functions
# ============================================================================
function(omnicpp_add_cpm_package PACKAGE_NAME)
    cmake_parse_arguments(ARGS
        "REQUIRED;OPTIONAL"
        "VERSION;GIT_TAG;GIT_REPOSITORY;GITHUB_REPOSITORY;URL"
        ""
        ${ARGN}
    )

    if(WARPLOOM_USE_CPM)
        if(ARGS_REQUIRED)
            CPMAddPackage(
                NAME ${PACKAGE_NAME}
                VERSION ${ARGS_VERSION}
                GIT_TAG ${ARGS_GIT_TAG}
                GIT_REPOSITORY ${ARGS_GIT_REPOSITORY}
                GITHUB_REPOSITORY ${ARGS_GITHUB_REPOSITORY}
                URL ${ARGS_URL}
            )
        elseif(ARGS_OPTIONAL)
            CPMTryAddPackage(
                NAME ${PACKAGE_NAME}
                VERSION ${ARGS_VERSION}
                GIT_TAG ${ARGS_GIT_TAG}
                GIT_REPOSITORY ${ARGS_GIT_REPOSITORY}
                GITHUB_REPOSITORY ${ARGS_GITHUB_REPOSITORY}
                URL ${ARGS_URL}
            )
        endif()
    endif()
endfunction()

message(STATUS "CPM.cmake integration loaded (v${CPM_VERSION})")
