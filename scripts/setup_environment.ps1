<#
Warploom Environment Setup Script
Automatically configures VULKAN_SDK and QT_DIR for Windows builds
#>

function Write-Color {
    param (
        [string]$Text,
        [string]$Color = "White"
    )
    
    $colors = @{
        "Red" = "[91m"
        "Green" = "[92m"
        "Yellow" = "[93m"
        "Blue" = "[94m"
        "Magenta" = "[95m"
        "Cyan" = "[96m"
        "White" = "[97m"
        "Reset" = "[0m"
    }
    
    if ($colors.ContainsKey($Color)) {
        Write-Host "$($colors[$Color])$Text$($colors["Reset"])" -NoNewline
    } else {
        Write-Host $Text -NoNewline
    }
}

function Write-Success {
    param ([string]$Text)
    Write-Color "✅ " -Color "Green"
    Write-Color $Text -Color "White"
    Write-Host
}

function Write-Warning {
    param ([string]$Text)
    Write-Color "⚠️  " -Color "Yellow"
    Write-Color $Text -Color "White"
    Write-Host
}

function Write-ErrorMsg {
    param ([string]$Text)
    Write-Color "❌ " -Color "Red"
    Write-Color $Text -Color "White"
    Write-Host
}

function Write-Info {
    param ([string]$Text)
    Write-Color "📋 " -Color "Blue"
    Write-Color $Text -Color "White"
    Write-Host
}

# Function to detect Vulkan SDK installation
Write-Color "🔍 Detecting Vulkan SDK installation..." -Color "Cyan"
Write-Host

$vulkanPaths = @(
    "C:\VulkanSDK",
    "C:\Program Files\VulkanSDK",
    "C:\Program Files (x86)\VulkanSDK",
    "$env:LOCALAPPDATA\VulkanSDK",
    "$env:PROGRAMDATA\VulkanSDK"
)

$vulkanSdk = $null

foreach ($path in $vulkanPaths) {
    if (Test-Path $path) {
        $versions = Get-ChildItem -Directory $path
        foreach ($version in $versions) {
            $vulkanInclude = Join-Path $version.FullName "Include\vulkan\vulkan.h"
            if (Test-Path $vulkanInclude) {
                $vulkanSdk = $version.FullName
                Write-Success "Found Vulkan SDK at: $vulkanSdk"
                break
            }
        }
        if ($vulkanSdk) { break }
    }
}

if (-not $vulkanSdk) {
    Write-Warning "Vulkan SDK not found. Vulkan-related features will be disabled."
    Write-Warning "Please install Vulkan SDK from https://vulkan.lunarg.com/"
} else {
    Write-Info "Setting VULKAN_SDK environment variable..."
    [Environment]::SetEnvironmentVariable("VULKAN_SDK", $vulkanSdk, "User")
    $env:VULKAN_SDK = $vulkanSdk
    Write-Host "    VULKAN_SDK=$vulkanSdk"
}

# Function to detect Qt installation
Write-Host
Write-Color "🔍 Detecting Qt installation..." -Color "Cyan"
Write-Host

$qtPaths = @(
    "C:\Qt",
    "C:\Program Files\Qt",
    "C:\Program Files (x86)\Qt",
    "$env:LOCALAPPDATA\Qt",
    "$env:PROGRAMDATA\Qt"
)

$qtDir = $null

foreach ($path in $qtPaths) {
    if (Test-Path $path) {
        $versions = Get-ChildItem -Directory $path
        foreach ($version in $versions) {
            $qmakePath = Join-Path $version.FullName "bin\qmake.exe"
            if (Test-Path $qmakePath) {
                $qtDir = $version.FullName
                Write-Success "Found Qt installation at: $qtDir"
                break
            }
        }
        if ($qtDir) { break }
    }
}

if (-not $qtDir) {
    Write-Warning "Qt not found. Qt-related features will be disabled."
    Write-Warning "Please install Qt from https://www.qt.io/"
} else {
    Write-Info "Setting QT_DIR environment variable..."
    [Environment]::SetEnvironmentVariable("QT_DIR", $qtDir, "User")
    $env:QT_DIR = $qtDir
    Write-Host "    QT_DIR=$qtDir"
    
    # Also set Qt plugin path
    $qtPluginPath = Join-Path $qtDir "plugins"
    [Environment]::SetEnvironmentVariable("QT_PLUGIN_PATH", $qtPluginPath, "User")
    $env:QT_PLUGIN_PATH = $qtPluginPath
    Write-Host "    QT_PLUGIN_PATH=$qtPluginPath"
}

# Function to detect MSVC installation
Write-Host
Write-Color "🔍 Detecting MSVC installation..." -Color "Cyan"
Write-Host

$msvcPath = $null

try {
    $vsWhere = Get-Command vswhere -ErrorAction SilentlyContinue
    if ($vsWhere) {
        $vsInstallPath = & vswhere -latest -property installationPath
        if ($vsInstallPath) {
            $msvcPath = $vsInstallPath
            Write-Success "Found Visual Studio at: $msvcPath"
        }
    }
} catch {
    # vswhere not found
}

if (-not $msvcPath) {
    Write-Warning "Visual Studio not found. MSVC builds will be disabled."
    Write-Warning "Please install Visual Studio with C++ workload"
} else {
    Write-Info "Visual Studio detected for MSVC builds"
}

# Function to detect MinGW installation
Write-Host
Write-Color "🔍 Detecting MinGW installation..." -Color "Cyan"
Write-Host

$mingwPaths = @(
    "C:\mingw64",
    "C:\mingw32",
    "C:\msys64\mingw64",
    "C:\msys64\mingw32",
    "C:\msys64\ucrt64",
    "C:\msys64\clang64"
)

$mingwPath = $null

foreach ($path in $mingwPaths) {
    $gccPath = Join-Path $path "bin\gcc.exe"
    if (Test-Path $gccPath) {
        $mingwPath = $path
        Write-Success "Found MinGW at: $mingwPath"
        break
    }
}

if (-not $mingwPath) {
    Write-Warning "MinGW not found. MinGW builds will be disabled."
    Write-Warning "Please install MinGW from https://www.mingw-w64.org/"
} else {
    Write-Info "MinGW detected for MinGW builds"
}

# Function to detect Conan installation
Write-Host
Write-Color "🔍 Detecting Conan installation..." -Color "Cyan"
Write-Host

try {
    $conanPath = Get-Command conan -ErrorAction SilentlyContinue
    if ($conanPath) {
        Write-Success "Conan package manager found"
        $conanVersion = & conan --version
        Write-Host "    $conanVersion"
    } else {
        Write-Warning "Conan not found. Package management will be disabled."
        Write-Warning "Please install Conan: pip install conan"
    }
} catch {
    Write-Warning "Conan not found. Package management will be disabled."
    Write-Warning "Please install Conan: pip install conan"
}

# Function to detect CMake installation
Write-Host
Write-Color "🔍 Detecting CMake installation..." -Color "Cyan"
Write-Host

try {
    $cmakePath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakePath) {
        Write-Success "CMake found"
        $cmakeVersion = & cmake --version | Select-Object -First 1
        Write-Host "    $cmakeVersion"
    } else {
        Write-Warning "CMake not found. Build system will be disabled."
        Write-Warning "Please install CMake from https://cmake.org/"
    }
} catch {
    Write-Warning "CMake not found. Build system will be disabled."
    Write-Warning "Please install CMake from https://cmake.org/"
}

# Function to detect Ninja installation
Write-Host
Write-Color "🔍 Detecting Ninja installation..." -Color "Cyan"
Write-Host

try {
    $ninjaPath = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninjaPath) {
        Write-Success "Ninja build system found"
        $ninjaVersion = & ninja --version
        Write-Host "    $ninjaVersion"
    } else {
        Write-Warning "Ninja not found. Builds will use default generator."
        Write-Warning "Please install Ninja from https://ninja-build.org/"
    }
} catch {
    Write-Warning "Ninja not found. Builds will use default generator."
    Write-Warning "Please install Ninja from https://ninja-build.org/"
}

# Update PATH with detected tools
Write-Host
Write-Color "🔧 Updating system PATH..." -Color "Cyan"
Write-Host

$currentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
$updatedPath = $currentPath

if ($vulkanSdk) {
    $vulkanBin = Join-Path $vulkanSdk "Bin"
    if (-not ($updatedPath -like "*$vulkanBin*")) {
        $updatedPath = "$vulkanBin;$updatedPath"
    }
}

if ($qtDir) {
    $qtBin = Join-Path $qtDir "bin"
    if (-not ($updatedPath -like "*$qtBin*")) {
        $updatedPath = "$qtBin;$updatedPath"
    }
}

if ($mingwPath) {
    $mingwBin = Join-Path $mingwPath "bin"
    if (-not ($updatedPath -like "*$mingwBin*")) {
        $updatedPath = "$mingwBin;$updatedPath"
    }
}

[Environment]::SetEnvironmentVariable("PATH", $updatedPath, "User")

Write-Host
Write-Success "Environment setup completed!"
Write-Host
Write-Color "Current environment variables:" -Color "Cyan"
Write-Host
Write-Host "    VULKAN_SDK=$env:VULKAN_SDK"
Write-Host "    QT_DIR=$env:QT_DIR"
Write-Host "    QT_PLUGIN_PATH=$env:QT_PLUGIN_PATH"
Write-Host "    MSVC_PATH=$msvcPath"
Write-Host "    MINGW_PATH=$mingwPath"

Write-Host
Write-Color "📝 To use this environment, restart your terminal or run:" -Color "Cyan"
Write-Host
Write-Color "    . \"$($MyInvocation.MyCommand.Path)\"" -Color "Magenta"

Write-Host
Write-Success "🚀 Ready for Warploom development!"

# Return success
return 0