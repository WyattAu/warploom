# ============================================================================
# Warploom - vcpkg Integration
# ============================================================================
# vcpkg package manager integration
# https://vcpkg.io/
# ============================================================================

# Check if vcpkg toolchain is being used
if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
    set(OMNICPP_VCPKG_ENABLED ON)
    message(STATUS "vcpkg toolchain detected: ${CMAKE_TOOLCHAIN_FILE}")
else()
    set(OMNICPP_VCPKG_ENABLED OFF)
endif()

if(OMNICPP_USE_VCPKG)
    if(NOT OMNICPP_VCPKG_ENABLED)
        message(WARNING "vcpkg requested but toolchain file not set.")
        message(WARNING "Set CMAKE_TOOLCHAIN_FILE to vcpkg toolchain file.")
        message(WARNING "Example: -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(OMNICPP_USE_VCPKG OFF)
    else()
        message(STATUS "vcpkg integration enabled")

        # vcpkg triplet
        if(OMNICPP_PLATFORM_WINDOWS)
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(VCPKG_TARGET_TRIPLET "x64-windows")
            else()
                set(VCPKG_TARGET_TRIPLET "x86-windows")
            endif()
        elseif(OMNICPP_PLATFORM_LINUX)
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(VCPKG_TARGET_TRIPLET "x64-linux")
            else()
                set(VCPKG_TARGET_TRIPLET "x86-linux")
            endif()
        elseif(OMNICPP_PLATFORM_MACOS)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
                set(VCPKG_TARGET_TRIPLET "arm64-osx")
            else()
                set(VCPKG_TARGET_TRIPLET "x64-osx")
            endif()
        elseif(OMNICPP_PLATFORM_WASM)
            set(VCPKG_TARGET_TRIPLET "wasm32-emscripten")
        endif()

        message(STATUS "vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")

        # vcpkg manifest mode (vcpkg.json)
        if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg.json")
            message(STATUS "vcpkg manifest mode detected (vcpkg.json)")
        endif()
    endif()
else()
    message(STATUS "vcpkg integration disabled")
endif()
