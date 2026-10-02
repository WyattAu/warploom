# ============================================================================
# OmniCpp Template - Conan Integration
# ============================================================================
# Conan package manager integration
# https://conan.io/
# ============================================================================

# Check for system-wide Vulkan SDK installation
# If VULKAN_SDK is set, use system SDK instead of Conan dependencies
if(DEFINED ENV{VULKAN_SDK})
    message(STATUS "VULKAN_SDK environment variable detected: $ENV{VULKAN_SDK}")
    message(STATUS "Using system-wide Vulkan SDK. Conan will skip Vulkan dependencies.")
    set(WARPLOOM_USE_SYSTEM_VULKAN ON)
else()
    message(STATUS "VULKAN_SDK not set. Conan will provide Vulkan SDK.")
    set(WARPLOOM_USE_SYSTEM_VULKAN OFF)
endif()

# Find Conan executable
find_program(CONAN_EXECUTABLE conan)

if(WARPLOOM_USE_CONAN)
    if(NOT CONAN_EXECUTABLE)
        message(WARNING "Conan requested but not found. Install Conan from https://conan.io/")
        set(WARPLOOM_USE_CONAN OFF)
    else()
        message(STATUS "Conan found: ${CONAN_EXECUTABLE}")

        # Conan profiles directory
        set(CONAN_PROFILES_DIR "${CMAKE_SOURCE_DIR}/conan/profiles")

        # Determine Conan profile based on platform and compiler
        if(WARPLOOM_PLATFORM_WINDOWS)
            if(MSVC)
                if(CMAKE_BUILD_TYPE STREQUAL "Debug")
                    set(CONAN_PROFILE "${CONAN_PROFILES_DIR}/msvc-debug")
                else()
                    set(CONAN_PROFILE "${CONAN_PROFILES_DIR}/msvc-release")
                endif()
            elseif(WARPLOOM_COMPILER_MSVC_CLANG)
                set(CONAN_PROFILE "${CONAN_PROFILES_DIR}/clang-msvc")
            elseif(WARPLOOM_COMPILER_MINGW_GCC)
                set(CONAN_PROFILE "${CONAN_PROFILES_DIR}/gcc-mingw-ucrt")
            elseif(WARPLOOM_COMPILER_MINGW_CLANG)
                if(CMAKE_BUILD_TYPE STREQUAL "Debug")
                    set(CONAN_PROFILE "${CONAN_PROFILES_DIR}/mingw-clang-debug")
                else()
                    set(CONAN_PROFILE "${CONAN_PROFILES_DIR}/mingw-clang-release")
                endif()
            endif()
        elseif(WARPLOOM_PLATFORM_LINUX)
            set(CONAN_PROFILE "default")
        elseif(WARPLOOM_PLATFORM_WASM)
            set(CONAN_PROFILE "${CONAN_PROFILES_DIR}/emscripten")
        endif()

        # Conan install directory
        set(CONAN_INSTALL_DIR "${CMAKE_BINARY_DIR}/conan")

        # Conan build type
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(CONAN_BUILD_TYPE "Debug")
        elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
            set(CONAN_BUILD_TYPE "Release")
        elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
            set(CONAN_BUILD_TYPE "RelWithDebInfo")
        elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
            set(CONAN_BUILD_TYPE "MinSizeRel")
        else()
            set(CONAN_BUILD_TYPE "Release")
        endif()

        # Run Conan install
        if(EXISTS "${CMAKE_SOURCE_DIR}/conanfile.py")
            message(STATUS "Running Conan install...")
            execute_process(
                COMMAND ${CONAN_EXECUTABLE} install
                . --build=missing
                -s build_type=${CONAN_BUILD_TYPE}
                -s os=${CMAKE_SYSTEM_NAME}
                -s arch=${CMAKE_SYSTEM_PROCESSOR}
                -s compiler=${CMAKE_CXX_COMPILER_ID}
                -s compiler.version=${CMAKE_CXX_COMPILER_VERSION}
                -s compiler.libcxx=libstdc++11
                -if ${CONAN_INSTALL_DIR}
                -pr:h ${CONAN_PROFILE}
                -pr:b ${CONAN_PROFILE}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                RESULT_VARIABLE CONAN_INSTALL_RESULT
                OUTPUT_VARIABLE CONAN_INSTALL_OUTPUT
                ERROR_VARIABLE CONAN_INSTALL_ERROR
            )

            if(NOT CONAN_INSTALL_RESULT EQUAL 0)
                message(WARNING "Conan install failed: ${CONAN_INSTALL_ERROR}")
                set(WARPLOOM_USE_CONAN OFF)
            else()
                message(STATUS "Conan install completed successfully")

                # Include Conan-generated files
                if(EXISTS "${CONAN_INSTALL_DIR}/conan_toolchain.cmake")
                    include("${CONAN_INSTALL_DIR}/conan_toolchain.cmake")
                endif()

                if(EXISTS "${CONAN_INSTALL_DIR}/conandeps_legacy.cmake")
                    include("${CONAN_INSTALL_DIR}/conandeps_legacy.cmake")
                endif()
            endif()
        else()
            message(WARNING "conanfile.py not found, skipping Conan integration")
            set(WARPLOOM_USE_CONAN OFF)
        endif()
    endif()
else()
    message(STATUS "Conan integration disabled")
endif()
