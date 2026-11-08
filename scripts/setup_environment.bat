@echo off
REM Warploom Environment Setup Script
REM Automatically configures VULKAN_SDK and QT_DIR for Windows builds

setlocal enabledelayedexpansion

REM Function to detect Vulkan SDK installation
echo 🔍 Detecting Vulkan SDK installation...
set VULKAN_SDK=

REM Common Vulkan SDK installation paths
set VULKAN_PATHS=
    "C:\VulkanSDK"
    "C:\Program Files\VulkanSDK"
    "C:\Program Files (x86)\VulkanSDK"
    "%LOCALAPPDATA%\VulkanSDK"
    "%PROGRAMDATA%\VulkanSDK"

for %%V in (%VULKAN_PATHS%) do (
    set EXPANDED_PATH=%%V
    set EXPANDED_PATH=!EXPANDED_PATH:~="! 
    set EXPANDED_PATH=!EXPANDED_PATH:^"="! 
    set EXPANDED_PATH=!EXPANDED_PATH:"="! 
    
    if exist "!EXPANDED_PATH!\" (
        for /d %%D in ("!EXPANDED_PATH!\*") do (
            if exist "%%D\Include\vulkan\vulkan.h" (
                set VULKAN_SDK=%%D
                echo ✅ Found Vulkan SDK at: %%D
                goto :VulkanFound
            )
        )
    )
)

:VulkanFound
if not defined VULKAN_SDK (
    echo ⚠️  Vulkan SDK not found. Vulkan-related features will be disabled.
    echo    Please install Vulkan SDK from https://vulkan.lunarg.com/
    set VULKAN_SDK=
) else (
    echo 📋 Setting VULKAN_SDK environment variable...
    setx VULKAN_SDK "%VULKAN_SDK%"
    echo    VULKAN_SDK=%VULKAN_SDK%
)

REM Function to detect Qt installation
echo.
echo 🔍 Detecting Qt installation...
set QT_DIR=

REM Common Qt installation paths
set QT_PATHS=
    "C:\Qt"
    "C:\Program Files\Qt"
    "C:\Program Files (x86)\Qt"
    "%LOCALAPPDATA%\Qt"
    "%PROGRAMDATA%\Qt"

for %%Q in (%QT_PATHS%) do (
    set EXPANDED_PATH=%%Q
    set EXPANDED_PATH=!EXPANDED_PATH:~="! 
    set EXPANDED_PATH=!EXPANDED_PATH:^"="! 
    set EXPANDED_PATH=!EXPANDED_PATH:"="! 
    
    if exist "!EXPANDED_PATH!\" (
        for /d %%D in ("!EXPANDED_PATH!\*") do (
            if exist "%%D\bin\qmake.exe" (
                set QT_DIR=%%D
                echo ✅ Found Qt installation at: %%D
                goto :QtFound
            )
        )
    )
)

:QtFound
if not defined QT_DIR (
    echo ⚠️  Qt not found. Qt-related features will be disabled.
    echo    Please install Qt from https://www.qt.io/
    set QT_DIR=
) else (
    echo 📋 Setting QT_DIR environment variable...
    setx QT_DIR "%QT_DIR%"
    echo    QT_DIR=%QT_DIR%
    
    REM Also set Qt plugin path
    set QT_PLUGIN_PATH=%QT_DIR%\plugins
    setx QT_PLUGIN_PATH "%QT_PLUGIN_PATH%"
    echo    QT_PLUGIN_PATH=%QT_PLUGIN_PATH%
)

REM Function to detect MSVC installation
echo.
echo 🔍 Detecting MSVC installation...
set MSVC_PATH=

REM Try to find MSVC via vswhere
where vswhere >nul 2>&1
if %ERRORLEVEL% equ 0 (
    for /f "usebackq delims=" %%i in (`vswhere -latest -property installationPath`) do (
        set MSVC_PATH=%%i
        echo ✅ Found Visual Studio at: %%i
        goto :MSVCFound
    )
)

:MSVCFound
if not defined MSVC_PATH (
    echo ⚠️  Visual Studio not found. MSVC builds will be disabled.
    echo    Please install Visual Studio with C++ workload
) else (
    echo 📋 Visual Studio detected for MSVC builds
)

REM Function to detect MinGW installation
echo.
echo 🔍 Detecting MinGW installation...
set MINGW_PATH=

REM Common MinGW installation paths
set MINGW_PATHS=
    "C:\mingw64"
    "C:\mingw32"
    "C:\msys64\mingw64"
    "C:\msys64\mingw32"
    "C:\msys64\ucrt64"
    "C:\msys64\clang64"

for %%M in (%MINGW_PATHS%) do (
    set EXPANDED_PATH=%%M
    set EXPANDED_PATH=!EXPANDED_PATH:~="! 
    set EXPANDED_PATH=!EXPANDED_PATH:^"="! 
    set EXPANDED_PATH=!EXPANDED_PATH:"="! 
    
    if exist "!EXPANDED_PATH!\bin\gcc.exe" (
        set MINGW_PATH=%%M
        echo ✅ Found MinGW at: %%M
        goto :MinGWFound
    )
)

:MinGWFound
if not defined MINGW_PATH (
    echo ⚠️  MinGW not found. MinGW builds will be disabled.
    echo    Please install MinGW from https://www.mingw-w64.org/
) else (
    echo 📋 MinGW detected for MinGW builds
)

REM Function to detect Conan installation
echo.
echo 🔍 Detecting Conan installation...
where conan >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo ✅ Conan package manager found
    conan --version
) else (
    echo ⚠️  Conan not found. Package management will be disabled.
    echo    Please install Conan: pip install conan
)

REM Function to detect CMake installation
echo.
echo 🔍 Detecting CMake installation...
where cmake >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo ✅ CMake found
    cmake --version
) else (
    echo ⚠️  CMake not found. Build system will be disabled.
    echo    Please install CMake from https://cmake.org/
)

REM Function to detect Ninja installation
echo.
echo 🔍 Detecting Ninja installation...
where ninja >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo ✅ Ninja build system found
    ninja --version
) else (
    echo ⚠️  Ninja not found. Builds will use default generator.
    echo    Please install Ninja from https://ninja-build.org/
)

REM Update PATH with detected tools
echo.
echo 🔧 Updating system PATH...

set UPDATED_PATH=%PATH%

if defined VULKAN_SDK (
    set UPDATED_PATH=%VULKAN_SDK%\Bin;%UPDATED_PATH%
)

if defined QT_DIR (
    set UPDATED_PATH=%QT_DIR%\bin;%UPDATED_PATH%
)

if defined MINGW_PATH (
    set UPDATED_PATH=%MINGW_PATH%\bin;%UPDATED_PATH%
)

REM Set the updated PATH permanently
setx PATH "%UPDATED_PATH%"

echo.
echo ✅ Environment setup completed!
echo.
echo Current environment variables:
echo    VULKAN_SDK=%VULKAN_SDK%
echo    QT_DIR=%QT_DIR%
echo    QT_PLUGIN_PATH=%QT_PLUGIN_PATH%
echo    MSVC_PATH=%MSVC_PATH%
echo    MINGW_PATH=%MINGW_PATH%

echo.
echo 📝 To use this environment, restart your terminal or run:
echo    call "%~f0"

echo.
echo 🚀 Ready for Warploom development!

endlocal