# ============================================================================
# OmniCpp Template - Find Dependencies
# ============================================================================
# This module finds and configures external dependencies for the project

# ============================================================================
# Quill (Logging Library)
# ============================================================================
# Quill is a high-performance, C++17/20/23 compatible logging library
# https://github.com/odygrd/quill
if(WARPLOOM_USE_QUILL)
    CPMAddPackage(
        NAME quill
        VERSION 8.2.0
        GITHUB_REPOSITORY odygrd/quill
        OPTIONS "QUILL_BUILD_EXAMPLES OFF"
                "QUILL_BUILD_TESTS OFF"
                "QUILL_FMT_EXTERNAL OFF"
    )
    if(quill_ADDED)
        message(STATUS "Found quill: ${quill_VERSION}")
    else()
        message(WARNING "Quill not found, logging will be disabled")
        set(WARPLOOM_USE_QUILL OFF CACHE BOOL "Use Quill logging" FORCE)
    endif()
endif()

# ============================================================================
# GLM (Math Library)
# ============================================================================
if(WARPLOOM_USE_GLM)
    CPMAddPackage(
        NAME glm
        GIT_TAG 1.0.3
        GITHUB_REPOSITORY g-truc/glm
        OPTIONS "GLM_BUILD_TESTS OFF"
                "GLM_BUILD_STATIC OFF"
    )
    message(STATUS "Found glm: ${glm_VERSION}")
endif()

# ============================================================================
# STB (Image Library)
# ============================================================================
if(WARPLOOM_USE_STB)
    CPMAddPackage(
        NAME stb
        GITHUB_REPOSITORY nothings/stb
        GIT_TAG master
        DOWNLOAD_ONLY YES
    )
    message(STATUS "Found stb")
endif()

# ============================================================================
# GLFW (Windowing Library)
# ============================================================================
if(WARPLOOM_USE_GLFW)
    CPMAddPackage(
        NAME glfw
        GIT_TAG 3.4
        GITHUB_REPOSITORY glfw/glfw
        OPTIONS "GLFW_BUILD_DOCS OFF"
                "GLFW_BUILD_TESTS OFF"
                "GLFW_BUILD_EXAMPLES OFF"
    )
    message(STATUS "Found glfw: ${glfw_VERSION}")
endif()

# ============================================================================
# Vulkan (Graphics API)
# ============================================================================
message(STATUS "WARPLOOM_USE_VULKAN: ${WARPLOOM_USE_VULKAN}")
if(WARPLOOM_USE_VULKAN)
    message(STATUS "Processing Vulkan configuration...")
    
    # First try standard find_package
    find_package(Vulkan QUIET)
    
    if(Vulkan_FOUND)
        message(STATUS "Found Vulkan via find_package: ${Vulkan_VERSION}")
        message(STATUS "  Include: ${Vulkan_INCLUDE_DIRS}")
        message(STATUS "  Library: ${Vulkan_LIBRARIES}")
    else()
        # Fallback: Try environment variable VULKAN_SDK
        if(DEFINED ENV{VULKAN_SDK})
            set(VULKAN_SDK $ENV{VULKAN_SDK})
            message(STATUS "Using VULKAN_SDK from environment: ${VULKAN_SDK}")
            
            set(Vulkan_INCLUDE_DIRS "${VULKAN_SDK}/include")
            if(EXISTS "${VULKAN_SDK}/lib/libvulkan.so")
                set(Vulkan_LIBRARIES "${VULKAN_SDK}/lib/libvulkan.so")
            elseif(EXISTS "${VULKAN_SDK}/lib64/libvulkan.so")
                set(Vulkan_LIBRARIES "${VULKAN_SDK}/lib64/libvulkan.so")
            elseif(EXISTS "${VULKAN_SDK}/lib/x86_64-linux-gnu/libvulkan.so")
                set(Vulkan_LIBRARIES "${VULKAN_SDK}/lib/x86_64-linux-gnu/libvulkan.so")
            endif()
            
            if(EXISTS "${Vulkan_INCLUDE_DIRS}" AND EXISTS "${Vulkan_LIBRARIES}")
                set(Vulkan_FOUND TRUE CACHE BOOL "Vulkan found" FORCE)
                set(Vulkan_VERSION "1.0.0" CACHE STRING "Vulkan version" FORCE)
                message(STATUS "Found Vulkan via VULKAN_SDK")
                message(STATUS "  Include: ${Vulkan_INCLUDE_DIRS}")
                message(STATUS "  Library: ${Vulkan_LIBRARIES}")
            else()
                message(WARNING "VULKAN_SDK set but paths not found: ${VULKAN_SDK}")
            endif()
        endif()
        
        # Last resort: Try Nix store paths (for NixOS/CachyOS with Nix)
        if(NOT Vulkan_FOUND)
            # Try to find vulkan-headers in Nix store
            file(GLOB NIX_VULKAN_HEADERS "/nix/store/*-vulkan-headers-*/include" NO_FOLLOW_SYMLINKS)
            file(GLOB NIX_VULKAN_LOADER "/nix/store/*-vulkan-loader-*/lib/libvulkan.so" NO_FOLLOW_SYMLINKS)
            
            message(STATUS "NIX_VULKAN_HEADERS = ${NIX_VULKAN_HEADERS}")
            message(STATUS "NIX_VULKAN_LOADER = ${NIX_VULKAN_LOADER}")
            
            if(NIX_VULKAN_HEADERS AND NIX_VULKAN_LOADER)
                list(GET NIX_VULKAN_HEADERS 0 Vulkan_INCLUDE_DIRS)
                list(GET NIX_VULKAN_LOADER 0 Vulkan_LIBRARIES)
                set(Vulkan_FOUND TRUE CACHE BOOL "Vulkan found" FORCE)
                set(Vulkan_VERSION "1.0.0" CACHE STRING "Vulkan version" FORCE)
                message(STATUS "Found Vulkan via Nix store auto-detection")
                message(STATUS "  Include: ${Vulkan_INCLUDE_DIRS}")
                message(STATUS "  Library: ${Vulkan_LIBRARIES}")
            else()
                message(WARNING "Vulkan not found. Install Vulkan SDK or set VULKAN_SDK environment variable")
                set(WARPLOOM_USE_VULKAN OFF CACHE BOOL "Use Vulkan" FORCE)
            endif()
        endif()
    endif()
    
    if(Vulkan_FOUND)
        set(Vulkan_INCLUDE_DIRS ${Vulkan_INCLUDE_DIRS} CACHE STRING "Vulkan include directories" FORCE)
        set(Vulkan_INCLUDE_DIR ${Vulkan_INCLUDE_DIRS} CACHE PATH "Vulkan include directory" FORCE)
        set(Vulkan_LIBRARIES ${Vulkan_LIBRARIES} CACHE STRING "Vulkan libraries" FORCE)
        
        # Save the correct values before Qt6 find_package resets them
        set(_vulkan_include_after "${Vulkan_INCLUDE_DIRS}")
        set(_vulkan_lib_after "${Vulkan_LIBRARIES}")
        message(STATUS "After Vulkan cache set: _vulkan_include_after = ${_vulkan_include_after}")
    endif()
else()
    message(STATUS "Skipping Vulkan configuration (WARPLOOM_USE_VULKAN is OFF)")
endif()

# ============================================================================
# OpenGL (Graphics API)
# ============================================================================
if(WARPLOOM_USE_OPENGL)
    find_package(OpenGL QUIET)
    if(OPENGL_FOUND)
        message(STATUS "Found OpenGL: ${OpenGL_VERSION}")
    else()
        message(WARNING "OpenGL requested but not found")
        set(WARPLOOM_USE_OPENGL OFF)
    endif()
endif()

# ============================================================================
# Qt6 (GUI Framework)
# ============================================================================
if(WARPLOOM_USE_QT6)
    find_package(Qt6 COMPONENTS Core Gui Widgets QUIET)
    
    # Restore Vulkan variables after Qt6 might have reset them
    if(DEFINED _vulkan_include_after)
        set(Vulkan_INCLUDE_DIRS "${_vulkan_include_after}" CACHE STRING "Vulkan include directories" FORCE)
        set(Vulkan_LIBRARIES "${_vulkan_lib_after}" CACHE STRING "Vulkan libraries" FORCE)
    endif()
    
    message(STATUS "After Qt6 find: Vulkan_INCLUDE_DIRS = ${Vulkan_INCLUDE_DIRS}")
    message(STATUS "After Qt6 find: Vulkan_LIBRARIES = ${Vulkan_LIBRARIES}")
    if(Qt6_FOUND)
        message(STATUS "Found Qt6: ${Qt6_VERSION}")
    else()
        message(WARNING "Qt6 requested but not found")
        set(WARPLOOM_USE_QT6 OFF)
    endif()
endif()

# ============================================================================
# nlohmann/json (JSON Library)
# ============================================================================
if(WARPLOOM_USE_NLOHMANN_JSON)
    CPMAddPackage(
        NAME nlohmann_json
        VERSION 3.11.3
        GITHUB_REPOSITORY nlohmann/json
    )
    message(STATUS "Found nlohmann_json: ${nlohmann_json_VERSION}")
endif()

# ============================================================================
# Google Test (Testing Framework)
# ============================================================================
if(WARPLOOM_BUILD_TESTS)
    CPMAddPackage(
        NAME googletest
        VERSION 1.14.0
        GITHUB_REPOSITORY google/googletest
        OPTIONS "BUILD_GMOCK OFF"
                "INSTALL_GTEST OFF"
    )
    message(STATUS "Found googletest: ${googletest_VERSION}")
endif()
