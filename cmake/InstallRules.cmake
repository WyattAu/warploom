# ============================================================================
# OmniCpp Template - Installation Rules
# ============================================================================
# Defines installation rules for all targets
# ============================================================================

# Include GNUInstallDirs for standard installation paths
include(GNUInstallDirs)

# ============================================================================
# Installation Paths
# ============================================================================
set(CMAKE_INSTALL_BINDIR ${WARPLOOM_INSTALL_BIN_DIR} CACHE PATH "Binary installation directory")
set(CMAKE_INSTALL_LIBDIR ${WARPLOOM_INSTALL_LIB_DIR} CACHE PATH "Library installation directory")
set(CMAKE_INSTALL_INCLUDEDIR ${WARPLOOM_INSTALL_INCLUDE_DIR} CACHE PATH "Header installation directory")
set(CMAKE_INSTALL_DATAROOTDIR ${WARPLOOM_INSTALL_DATA_DIR} CACHE PATH "Data installation directory")
set(CMAKE_INSTALL_DOCDIR ${WARPLOOM_INSTALL_DOC_DIR} CACHE PATH "Documentation installation directory")
set(CMAKE_INSTALL_CMAKEDIR ${WARPLOOM_INSTALL_CMAKE_DIR} CACHE PATH "CMake config installation directory")

# ============================================================================
# Assets Installation
# ============================================================================
if(EXISTS ${WARPLOOM_ASSETS_DIR})
    install(DIRECTORY ${WARPLOOM_ASSETS_DIR}/
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/assets
        USE_SOURCE_PERMISSIONS
    )
endif()

# ============================================================================
# Shader Installation
# ============================================================================
# The compiled SPIR-V is what the app actually runs. Install it alongside
# the binary so a relocatable install can find it via
# WARPLOOM_INSTALL_SHADERS_DIR without a build tree present.
if(WARPLOOM_SHADER_OUTPUT_DIR AND EXISTS "${WARPLOOM_SHADER_OUTPUT_DIR}")
    install(DIRECTORY "${WARPLOOM_SHADER_OUTPUT_DIR}/"
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/shaders
        COMPONENT Runtime
    )
endif()

# ============================================================================
# Documentation Installation
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/README.md)
    install(FILES ${CMAKE_SOURCE_DIR}/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
    )
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/LICENSE)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
    )
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/CHANGELOG.md)
    install(FILES ${CMAKE_SOURCE_DIR}/CHANGELOG.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
    )
endif()

# ============================================================================
# Configuration Files Installation
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/config/)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/config
        FILES_MATCHING PATTERN "*.json" PATTERN "*.yaml" PATTERN "*.yml"
    )
endif()

# ============================================================================
# CMake Configuration Installation
# ============================================================================
# The five modules and the headerless aggregate each install their own
# Warploom<Name>Config.cmake + version file from modules/*/CMakeLists.txt.
# There is no root-level target to export: the S5-A aggregate replaced the
# omnicpp_runtime monolith, so the old OmniCppEngine export block is gone.

# ============================================================================
# Platform-Specific Installation
# ============================================================================
if(WARPLOOM_PLATFORM_WINDOWS)
    # Windows-specific installation
    if(EXISTS ${CMAKE_SOURCE_DIR}/assets/DotNameCppLogo.svg)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/DotNameCppLogo.svg
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}
        )
    endif()
elseif(WARPLOOM_PLATFORM_WASM)
    # WASM-specific installation (web files)
    if(EXISTS ${CMAKE_SOURCE_DIR}/assets/ems-mini.html)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/ems-mini.html
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}
        )
    endif()
endif()

# ============================================================================
# Installation Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== Installation Configuration ===")
message(STATUS "Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Bin Dir: ${CMAKE_INSTALL_BINDIR}")
message(STATUS "Lib Dir: ${CMAKE_INSTALL_LIBDIR}")
message(STATUS "Include Dir: ${CMAKE_INSTALL_INCLUDEDIR}")
message(STATUS "Data Dir: ${CMAKE_INSTALL_DATAROOTDIR}")
message(STATUS "Doc Dir: ${CMAKE_INSTALL_DOCDIR}")
message(STATUS "CMake Dir: ${CMAKE_INSTALL_CMAKEDIR}")
message(STATUS "==============================")
message(STATUS "")
