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
# Engine Installation
# ============================================================================
if(TARGET OmniCppEngine)
    # Install engine library
    install(TARGETS OmniCppEngine
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Install engine headers
    install(DIRECTORY ${WARPLOOM_INCLUDE_DIR}/engine/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/engine
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
    )

    # Install engine headers (OmniCppLib)
    install(DIRECTORY ${WARPLOOM_INCLUDE_DIR}/OmniCppLib/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/OmniCppLib
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
    )
endif()

# ============================================================================
# Game Installation
# ============================================================================
if(TARGET OmniCppGame)
    # Install game executable
    install(TARGETS OmniCppGame
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # Install game headers
    install(DIRECTORY ${WARPLOOM_INCLUDE_DIR}/game/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/game
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
    )
endif()

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
install(FILES
    ${CMAKE_SOURCE_DIR}/cmake/OmniCppEngineConfig.cmake.in
    DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
)

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
elseif(WARPLOOM_PLATFORM_LINUX)
    # Linux-specific installation
    install(CODE "
        execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/applications\")
        execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/256x256/apps\")
    ")
elseif(WARPLOOM_PLATFORM_WASM)
    # WASM-specific installation (web files)
    if(EXISTS ${CMAKE_SOURCE_DIR}/assets/ems-mini.html)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/ems-mini.html
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}
        )
    endif()
endif()

# ============================================================================
# Export Configuration
# ============================================================================
if(TARGET OmniCppEngine)
    # Export engine targets
    install(EXPORT OmniCppEngineTargets
        FILE OmniCppEngineTargets.cmake
        NAMESPACE OmniCpp::
        DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
    )

    # Create config file
    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_SOURCE_DIR}/cmake/OmniCppEngineConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/OmniCppEngineConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
    )

    # Create version file
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/OmniCppEngineConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    # Install config and version files
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/OmniCppEngineConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/OmniCppEngineConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
    )
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
