# ============================================================================
# OmniCpp Template - Package Configuration
# ============================================================================
# Configures CPack for distribution package creation
# ============================================================================

# Include CPack
include(CPack)

# ============================================================================
# Package Information
# ============================================================================
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION ${PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_VENDOR "OmniCpp")
set(CPACK_PACKAGE_CONTACT "maintainers@github.com")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/omnicpp/omnicpp-template")
set(CPACK_PACKAGE_LICENSE_FILE "${CMAKE_SOURCE_DIR}/LICENSE")

# ============================================================================
# Package Generators
# ============================================================================
if(WARPLOOM_PLATFORM_WINDOWS)
    # Windows package generators
    set(CPACK_GENERATOR "ZIP;NSIS;WIX")
    set(CPACK_SOURCE_GENERATOR "ZIP")
elseif(WARPLOOM_PLATFORM_LINUX)
    # Linux package generators
    set(CPACK_GENERATOR "DEB;RPM;TGZ")
    set(CPACK_SOURCE_GENERATOR "TGZ")
elseif(WARPLOOM_PLATFORM_MACOS)
    # macOS package generators
    set(CPACK_GENERATOR "DragNDrop;TGZ")
    set(CPACK_SOURCE_GENERATOR "TGZ")
elseif(WARPLOOM_PLATFORM_WASM)
    # WASM package generators
    set(CPACK_GENERATOR "ZIP;TGZ")
    set(CPACK_SOURCE_GENERATOR "TGZ")
else()
    # Default package generators
    set(CPACK_GENERATOR "TGZ;ZIP")
    set(CPACK_SOURCE_GENERATOR "TGZ")
endif()

# ============================================================================
# Package File Names
# ============================================================================
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-src")

# ============================================================================
# Package Components
# ============================================================================
set(CPACK_COMPONENTS_ALL "Runtime;Development;Documentation;Data")

# Runtime component (executables and libraries)
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Runtime")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Runtime files (executables and libraries)")
set(CPACK_COMPONENT_RUNTIME_REQUIRED TRUE)

# Development component (headers and CMake files)
set(CPACK_COMPONENT_DEVELOPMENT_DISPLAY_NAME "Development")
set(CPACK_COMPONENT_DEVELOPMENT_DESCRIPTION "Development files (headers and CMake config)")
set(CPACK_COMPONENT_DEVELOPMENT_DEPENDS Runtime)

# Documentation component (docs and examples)
set(CPACK_COMPONENT_DOCUMENTATION_DISPLAY_NAME "Documentation")
set(CPACK_COMPONENT_DOCUMENTATION_DESCRIPTION "Documentation and examples")

# Data component (assets and config files)
set(CPACK_COMPONENT_DATA_DISPLAY_NAME "Data")
set(CPACK_COMPONENT_DATA_DESCRIPTION "Data files (assets and configuration)")

# ============================================================================
# Platform-Specific Package Configuration
# ============================================================================
if(WARPLOOM_PLATFORM_WINDOWS)
    # Windows-specific configuration
    set(CPACK_NSIS_DISPLAY_NAME "${PROJECT_NAME}")
    set(CPACK_NSIS_PACKAGE_NAME "${PROJECT_NAME}")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_NSIS_MODIFY_PATH ON)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

    # WIX configuration
    # A real upgrade GUID is required; the template placeholder
    # "YOUR-GUID-HERE" is not a GUID and WiX refused to generate.
    set(CPACK_WIX_UPGRADE_GUID "6f2b8c14-9a3d-4f27-9b6e-1c0a5d3e7f48")
    # The banner/dialog/license artwork this block referenced
    # (assets/banner.bmp, assets/dialog.bmp, LICENSE.rtf) does not exist in
    # the tree, and CPack treats a missing file as a hard error. Add the
    # assets and these lines back if branded installers are ever wanted.

elseif(WARPLOOM_PLATFORM_LINUX)
    # Linux-specific configuration
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 libstdc++6")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
    set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_RPM_PACKAGE_REQUIRES "glibc libstdc++")

elseif(WARPLOOM_PLATFORM_MACOS)
    # macOS-specific configuration
    set(CPACK_DRAGNDROP_COMPONENTS "Runtime")
    set(CPACK_MACOSX_BUNDLE_NAME "${PROJECT_NAME}.app")
    # No icon: assets/icon.icns is not in the tree.
    # set(CPACK_MACOSX_BUNDLE_ICON "${CMAKE_SOURCE_DIR}/assets/icon.icns")

elseif(WARPLOOM_PLATFORM_WASM)
    # WASM-specific configuration
    set(CPACK_ARCHIVE_COMPONENT_INSTALL "Runtime;Data")
endif()

# ============================================================================
# Package Installation
# ============================================================================
set(CPACK_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})

# ============================================================================
# Package Output Directory
# ============================================================================
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")

# ============================================================================
# Package Checksums
# ============================================================================
set(CPACK_PACKAGE_CHECKSUM SHA256)

# ============================================================================
# Source Package
# ============================================================================
set(CPACK_SOURCE_IGNORE_FILES
    ".git"
    ".gitignore"
    ".vscode"
    "build"
    "packages"
    ".cache"
    "*.pyc"
    "__pycache__"
    ".pytest_cache"
    )

# ============================================================================
# Package Configuration Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== Package Configuration ===")
message(STATUS "Package Name: ${CPACK_PACKAGE_NAME}")
message(STATUS "Package Version: ${CPACK_PACKAGE_VERSION}")
message(STATUS "Generators: ${CPACK_GENERATOR}")
message(STATUS "Source Generator: ${CPACK_SOURCE_GENERATOR}")
message(STATUS "Output Directory: ${CPACK_PACKAGE_DIRECTORY}")
message(STATUS "Components: ${CPACK_COMPONENTS_ALL}")
message(STATUS "Checksum: ${CPACK_PACKAGE_CHECKSUM}")
message(STATUS "==========================")
message(STATUS "")
