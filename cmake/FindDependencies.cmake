# ============================================================================
# Warploom - External Dependencies
# ============================================================================
# Warploom has exactly ONE third-party dependency: GoogleTest, and only for
# the test suites.
#
# This file used to declare six CPM packages (quill, glm, stb, glfw,
# nlohmann_json, googletest). Five of them are included by zero translation
# units in the tree, and because they all defaulted to ON, every preset
# other than the headless ones downloaded and built four unused GitHub
# repositories before compiling a line of engine code. The engine is
# self-contained by design -- the math, the JSON, the image codecs and the
# glTF parser are all in-tree -- so those packages were never needed.
#
# Vulkan and xcb are system packages found with find_package/pkg-config, not
# vendored.
# ============================================================================

# ============================================================================
# Vulkan (the rendering backend)
# ============================================================================
if(WARPLOOM_USE_VULKAN)
    find_package(Vulkan QUIET)

    if(NOT Vulkan_FOUND AND DEFINED ENV{VULKAN_SDK})
        message(STATUS "Vulkan not found by find_package; trying VULKAN_SDK")
        find_path(Vulkan_INCLUDE_DIR
            NAMES vulkan/vulkan.h
            HINTS $ENV{VULKAN_SDK}/include
        )
        find_library(Vulkan_LIBRARIES
            NAMES vulkan
            HINTS $ENV{VULKAN_SDK}/lib
        )
        if(Vulkan_INCLUDE_DIR AND Vulkan_LIBRARIES)
            set(Vulkan_FOUND TRUE)
        endif()
    endif()

    if(NOT Vulkan_FOUND)
        # Deliberately no /nix/store globbing here. CMakeLists.txt states that
        # this file must not manufacture host-specific paths, and globbing the
        # Nix store made configure succeed on NixOS while behaving differently
        # everywhere else. Use the nix-* presets or a nix develop shell instead.
        message(STATUS "Vulkan not found -- warploom-render will report errors at runtime")
        set(WARPLOOM_USE_VULKAN OFF CACHE BOOL "Use Vulkan" FORCE)
    else()
        message(STATUS "Found Vulkan ${Vulkan_VERSION}")
    endif()
else()
    message(STATUS "Vulkan disabled (WARPLOOM_USE_VULKAN=OFF)")
endif()

# ============================================================================
# GoogleTest (test suites only)
# ============================================================================
if(WARPLOOM_BUILD_TESTS)
    warploom_add_cpm_package(
        NAME googletest
        VERSION 1.14.0
        GITHUB_REPOSITORY google/googletest
        OPTIONS "BUILD_GMOCK OFF"
                "INSTALL_GTEST OFF"
    )
    message(STATUS "googletest ${googletest_VERSION}")
endif()