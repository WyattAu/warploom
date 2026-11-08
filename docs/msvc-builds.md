# MSVC Build Instructions for Warploom

This guide provides comprehensive instructions for building Warploom with Microsoft Visual Studio (MSVC) on Windows.

## Prerequisites

### Required Tools

1. **Visual Studio 2022** with C++ workload
   - Download: [https://visualstudio.microsoft.com/](https://visualstudio.microsoft.com/)
   - Required components:
     - MSVC v143 - VS 2022 C++ x64/x86 build tools
     - Windows 11 SDK
     - C++ CMake tools for Windows

2. **CMake 3.25+**
   - Download: [https://cmake.org/download/](https://cmake.org/download/)
   - Ensure CMake is in your PATH

3. **Ninja Build System**
   - Download: [https://ninja-build.org/](https://ninja-build.org/)
   - Or install via Visual Studio installer

4. **Conan Package Manager**
   ```bash
   pip install conan
   ```

5. **Git**
   - Download: [https://git-scm.com/](https://git-scm.com/)

### Optional Dependencies

1. **Vulkan SDK** (for Vulkan support)
   - Download: [https://vulkan.lunarg.com/](https://vulkan.lunarg.com/)
   - Run the setup_environment script to auto-detect:
     ```bash
     scripts\setup_environment.ps1
     ```

2. **Qt 6** (for Qt GUI applications)
   - Download: [https://www.qt.io/download](https://www.qt.io/download)
   - Run the setup_environment script to auto-detect:
     ```bash
     scripts\setup_environment.ps1
     ```

## Environment Setup

### Automatic Setup

Run the environment setup script to automatically configure VULKAN_SDK and QT_DIR:

```bash
# PowerShell (recommended)
.\scripts\setup_environment.ps1

# Or Command Prompt
call scripts\setup_environment.bat
```

### Manual Setup

If you prefer manual setup, configure these environment variables:

```bash
# Vulkan SDK (if installed)
setx VULKAN_SDK "C:\VulkanSDK\<version>"

# Qt 6 (if installed)
setx QT_DIR "C:\Qt\<version>\msvc2019_64"
setx QT_PLUGIN_PATH "%QT_DIR%\plugins"
```

## Building with MSVC

### Using CMake Presets

Warploom provides pre-configured CMake presets for MSVC builds:

#### Debug Build

```bash
# Configure with MSVC Debug preset
cmake --preset=msvc-debug

# Build the project
cmake --build --preset=msvc-debug

# Run tests
ctest --preset=msvc-debug
```

#### Release Build

```bash
# Configure with MSVC Release preset
cmake --preset=msvc-release

# Build the project
cmake --build --preset=msvc-release

# Run tests
ctest --preset=msvc-release
```

### Custom Build Configuration

For custom configurations, use the following command:

```bash
cmake -B build/debug/msvc \
    -G "Ninja" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=cl \
    -DCMAKE_CXX_COMPILER=cl \
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
    -DCONAN_PROFILE=conan/profiles/msvc-debug \
    -DCMAKE_TOOLCHAIN_FILE=conan/conan_toolchain.cmake
```

### Build Options

| Option | Description | Default |
|--------|-------------|---------|
| `CMAKE_BUILD_TYPE` | Build type (Debug/Release) | Debug |
| `ENABLE_GTESTS` | Enable Google Test integration | ON |
| `ENABLE_CCACHE` | Enable ccache for faster builds | ON |
| `ENABLE_IPO` | Enable Interprocedural Optimization | OFF (Debug), ON (Release) |
| `ENABLE_HARDENING` | Enable security hardening | ON |
| `BUILD_SHARED_LIBS` | Build shared libraries | OFF |

## Cross-Platform Builds with MSVC

### ARM64 Windows

```bash
# Configure for ARM64 Windows
cmake --preset=arm64-windows-msvc-debug

# Build
cmake --build --preset=arm64-windows-msvc-debug
```

### x64 Windows (Default)

```bash
# Configure for x64 Windows
cmake --preset=msvc-debug

# Build
cmake --build --preset=msvc-debug
```

## Using WarploomController.py

For comprehensive builds using the WarploomController:

```bash
# Activate Python environment
python -m venv venv
.\venv\Scripts\activate

# Install required packages
pip install -r requirements.txt

# Run MSVC builds
python WarploomController.py --build-platform msvc --config debug
python WarploomController.py --build-platform msvc --config release
```

## Troubleshooting

### Common Issues

1. **MSVC not found**: Ensure Visual Studio is installed with C++ workload
2. **Missing Conan profile**: Run `conan profile detect --force`
3. **Vulkan not detected**: Install Vulkan SDK and run setup script
4. **Qt not detected**: Install Qt and run setup script

### Debugging Builds

For detailed build debugging:

```bash
# Enable verbose output
cmake --build --preset=msvc-debug --verbose

# Check configuration
cmake --preset=msvc-debug -LH

# Clean build
cmake --build --preset=msvc-debug --clean-first
```

## Advanced Configuration

### Custom Conan Profiles

Create custom Conan profiles in `conan/profiles/`:

```ini
# conan/profiles/msvc-custom
[settings]
os=Windows
arch=x86_64
compiler=msvc
compiler.version=193
compiler.runtime=dynamic
compiler.cppstd=20
build_type=Debug

[conf]
tools.cmake.cmaketoolchain:generator=Ninja Multi-Config
```

### Custom CMake Presets

Add custom presets to `CMakePresets.json`:

```json
{
  "name": "msvc-custom",
  "inherits": ["msvc-base"],
  "displayName": "MSVC Custom",
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/build/custom/msvc",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Debug",
    "CONAN_PROFILE": "conan/profiles/msvc-custom",
    "CUSTOM_OPTION": "ON"
  },
  "toolchainFile": "conan/conan_toolchain.cmake"
}
```

## Performance Optimization

### Parallel Builds

```bash
# Use all available cores
cmake --build --preset=msvc-debug --parallel

# Specify number of jobs
cmake --build --preset=msvc-debug --parallel 8
```

### Build Cache

Enable ccache for faster rebuilds:

```bash
# Configure with ccache
cmake --preset=msvc-debug -DENABLE_CCACHE=ON

# Check cache statistics
ccache -s
```

## Build Artifacts

Build outputs are located in:

```
build\<config>\<platform>\  # Build directory
build\<config>\<platform>\bin\  # Executables
build\<config>\<platform>\lib\  # Libraries
```

## Documentation

Generate documentation with Doxygen:

```bash
# Generate API documentation
doxygen Doxyfile

# Open documentation
start docs\html\index.html