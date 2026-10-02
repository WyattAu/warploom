# ============================================================================
# Warploom - Platform Configuration
# ============================================================================
# Defines platform-specific settings for Windows, Linux, and WASM
# ============================================================================

# ============================================================================
# Platform Detection
# ============================================================================
if(WIN32)
    set(WARPLOOM_PLATFORM_WINDOWS ON)
    set(WARPLOOM_PLATFORM_NAME "Windows")
elseif(UNIX AND NOT APPLE)
    set(WARPLOOM_PLATFORM_LINUX ON)
    set(WARPLOOM_PLATFORM_NAME "Linux")
elseif(APPLE)
    set(WARPLOOM_PLATFORM_MACOS ON)
    set(WARPLOOM_PLATFORM_NAME "macOS")
elseif(EMSCRIPTEN)
    set(WARPLOOM_PLATFORM_WASM ON)
    set(WARPLOOM_PLATFORM_NAME "WASM")
else()
    message(WARNING "Unknown platform: ${CMAKE_SYSTEM_NAME}")
    set(WARPLOOM_PLATFORM_NAME "Unknown")
endif()

# ============================================================================
# Architecture Detection
# ============================================================================
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x64")
    set(WARPLOOM_ARCH_X64 ON)
    set(WARPLOOM_ARCH_NAME "x64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i386|i686|x86")
    set(WARPLOOM_ARCH_X86 ON)
    set(WARPLOOM_ARCH_NAME "x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(WARPLOOM_ARCH_ARM64 ON)
    set(WARPLOOM_ARCH_NAME "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|ARM")
    set(WARPLOOM_ARCH_ARM ON)
    set(WARPLOOM_ARCH_NAME "arm")
else()
    message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    set(WARPLOOM_ARCH_NAME "Unknown")
endif()

# ============================================================================
# Windows-Specific Configuration
# ============================================================================
if(WARPLOOM_PLATFORM_WINDOWS)
    # Windows libraries
    set(WARPLOOM_PLATFORM_LIBRARIES "ws2_32;winmm;version")

    # Windows definitions
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-DNOMINMAX)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
    add_definitions(-D_SCL_SECURE_NO_WARNINGS)

    # Windows subsystem
    if(WARPLOOM_PLATFORM_WASM)
        # WASM doesn't use Windows subsystem
    else()
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:CONSOLE")
    endif()

    # Windows-specific paths
    set(WARPLOOM_PATH_SEPARATOR "\\")

    # Windows-specific compiler flags
    if(MSVC)
        add_compile_options(/utf-8) # Use UTF-8 source files
    endif()
endif()

# ============================================================================
# Linux-Specific Configuration
# ============================================================================
if(WARPLOOM_PLATFORM_LINUX)
    # Linux libraries
    set(WARPLOOM_PLATFORM_LIBRARIES "pthread;dl;rt")

    # Linux definitions
    add_definitions(-D_POSIX_C_SOURCE=200809L)
    add_definitions(-D_XOPEN_SOURCE=700)

    # Linux-specific paths
    set(WARPLOOM_PATH_SEPARATOR "/")

    # Linux-specific compiler flags
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fPIC)
    endif()

    # Linux-specific linker flags
    add_link_options(-Wl,--as-needed)
    add_link_options(-Wl,--no-undefined)
endif()

# ============================================================================
# WASM-Specific Configuration
# ============================================================================
if(WARPLOOM_PLATFORM_WASM)
    # WASM libraries (Emscripten provides most)
    set(WARPLOOM_PLATFORM_LIBRARIES "")

    # WASM definitions
    add_definitions(-D__EMSCRIPTEN__)

    # WASM-specific paths
    set(WARPLOOM_PATH_SEPARATOR "/")

    # WASM-specific compiler flags
    add_compile_options(-s USE_SDL=2)
    add_compile_options(-s WASM=1)
    add_compile_options(-s ALLOW_MEMORY_GROWTH=1)

    # WASM-specific linker flags
    add_link_options(-s USE_SDL=2)
    add_link_options(-s WASM=1)
    add_link_options(-s ALLOW_MEMORY_GROWTH=1)
    add_link_options(-s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]')
    add_link_options(-s EXPORTED_FUNCTIONS='["_main","_malloc","_free"]')

    # Disable threading for WASM (not fully supported yet)
    set(CMAKE_THREAD_PREFER_PTHREAD OFF)
endif()

# ============================================================================
# macOS-Specific Configuration
# ============================================================================
if(WARPLOOM_PLATFORM_MACOS)
    # macOS libraries
    set(WARPLOOM_PLATFORM_LIBRARIES "-framework Cocoa -framework IOKit -framework CoreVideo")

    # macOS definitions
    add_definitions(-D__APPLE__)

    # macOS-specific paths
    set(WARPLOOM_PATH_SEPARATOR "/")

    # macOS-specific compiler flags
    add_compile_options(-fPIC)

    # macOS-specific linker flags
    add_link_options(-Wl,-headerpad_max_install_names)
endif()

# ============================================================================
# Cross-Compilation Detection
# ============================================================================
if(CMAKE_CROSSCOMPILING)
    set(WARPLOOM_CROSS_COMPILING ON)
    message(STATUS "Cross-compiling for ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(WARPLOOM_CROSS_COMPILING OFF)
endif()

# ============================================================================
# Platform-Specific Libraries
# ============================================================================
if(WARPLOOM_PLATFORM_WINDOWS)
    find_library(WS2_32_LIBRARY ws2_32)
    find_library(WINMM_LIBRARY winmm)
    find_library(VERSION_LIBRARY version)
    list(APPEND WARPLOOM_PLATFORM_LIBRARIES ${WS2_32_LIBRARY} ${WINMM_LIBRARY} ${VERSION_LIBRARY})
elseif(WARPLOOM_PLATFORM_LINUX)
    find_package(Threads REQUIRED)
    list(APPEND WARPLOOM_PLATFORM_LIBRARIES Threads::Threads)
endif()

message(STATUS "Platform configuration loaded: ${WARPLOOM_PLATFORM_NAME}-${WARPLOOM_ARCH_NAME}")
