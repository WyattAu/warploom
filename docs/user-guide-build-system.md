# Warploom Build System - User Guide

## Table of Contents
- [Introduction](#introduction)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Build Commands](#build-commands)
- [Compiler Support](#compiler-support)
- [Build Types](#build-types)
- [Build Targets](#build-targets)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)

## Introduction

The Warploom Build System is a comprehensive build automation tool designed for C++ projects with support for multiple compilers, platforms, and build configurations. It provides a unified interface for building, testing, and packaging C++ applications.

### Key Features

- **Multi-Compiler Support**: MSVC, MSVC-Clang, MinGW-GCC, MinGW-Clang
- **Cross-Platform**: Windows, Linux, macOS
- **Build Types**: Debug, Release, RelWithDebInfo, MinSizeRel
- **Package Management**: Integration with Conan and vcpkg
- **Performance Monitoring**: Built-in build performance tracking
- **Error Recovery**: Automatic retry and resilience mechanisms

## Installation

### Prerequisites

- Python 3.8 or higher
- CMake 3.20 or higher
- C++ compiler (MSVC, GCC, or Clang)
- Git (for dependency management)

### Setup

1. Clone the repository:
```bash
git clone <repository-url>
cd OmniCPP-template
```

2. Install Python dependencies:
```bash
pip install -r requirements-docs.txt
```

3. Verify installation:
```bash
python WarploomController.py --help
```

## Quick Start

### Basic Build

Build the project with default settings:
```bash
python WarploomController.py build standalone "Build Project" default debug
```

### Clean Build

Perform a clean build from scratch:
```bash
python WarploomController.py build standalone "Clean Build Pipeline" default release
```

### Specify Compiler

Build with a specific compiler:
```bash
python WarploomController.py build standalone "Build Project" default debug --compiler msvc
```

## Build Commands

### Build Command

```bash
python WarploomController.py build <target> <task> <preset> <build_type> [options]
```

#### Parameters

- **target**: Build target (e.g., `standalone`, `targets/qt-vulkan/library`)
- **task**: Build task name (e.g., `Build Project`, `Clean Build Pipeline`)
- **preset**: CMake preset (e.g., `default`)
- **build_type**: Build configuration (`debug`, `release`, `relwithdebinfo`, `minsizerel`)

#### Options

- `--compiler <compiler>`: Specify compiler (`msvc`, `msvc-clang`, `mingw-gcc`, `mingw-clang`)
- `--clean`: Clean build directory before building
- `--verbose`: Enable verbose output
- `--parallel <n>`: Number of parallel jobs

### Examples

Build standalone executable with MSVC:
```bash
python WarploomController.py build standalone "Build Project" default debug --compiler msvc
```

Build Qt-Vulkan library with MinGW-GCC:
```bash
python WarploomController.py build targets/qt-vulkan/library "Build Project" default release --compiler mingw-gcc
```

Clean build with verbose output:
```bash
python WarploomController.py build standalone "Clean Build Pipeline" default release --compiler msvc --verbose
```

## Compiler Support

### MSVC (Microsoft Visual C++)

**Platform**: Windows

**Requirements**:
- Visual Studio 2019 or later
- Windows SDK

**Usage**:
```bash
python WarploomController.py build standalone "Build Project" default debug --compiler msvc
```

### MSVC-Clang

**Platform**: Windows

**Requirements**:
- Visual Studio 2019 or later with Clang toolset
- Windows SDK

**Usage**:
```bash
python WarploomController.py build standalone "Build Project" default debug --compiler msvc-clang
```

### MinGW-GCC

**Platform**: Windows, Linux

**Requirements**:
- MinGW-w64 with GCC
- POSIX-compatible shell (Linux)

**Usage**:
```bash
python WarploomController.py build standalone "Build Project" default debug --compiler mingw-gcc
```

### MinGW-Clang

**Platform**: Windows, Linux

**Requirements**:
- MinGW-w64 with Clang
- POSIX-compatible shell (Linux)

**Usage**:
```bash
python WarploomController.py build standalone "Build Project" default debug --compiler mingw-clang
```

## Build Types

### Debug

- **Purpose**: Development and debugging
- **Optimization**: None (-O0)
- **Symbols**: Full debug symbols
- **Assertions**: Enabled
- **Usage**:
```bash
python WarploomController.py build standalone "Build Project" default debug
```

### Release

- **Purpose**: Production builds
- **Optimization**: Maximum (-O3)
- **Symbols**: Stripped
- **Assertions**: Disabled
- **Usage**:
```bash
python WarploomController.py build standalone "Build Project" default release
```

### RelWithDebInfo

- **Purpose**: Release builds with debug symbols
- **Optimization**: High (-O2)
- **Symbols**: Full debug symbols
- **Assertions**: Disabled
- **Usage**:
```bash
python WarploomController.py build standalone "Build Project" default relwithdebinfo
```

### MinSizeRel

- **Purpose**: Size-optimized builds
- **Optimization**: Size (-Os)
- **Symbols**: Stripped
- **Assertions**: Disabled
- **Usage**:
```bash
python WarploomController.py build standalone "Build Project" default minsizerel
```

## Build Targets

### Standalone

Builds a standalone executable:
```bash
python WarploomController.py build standalone "Build Project" default debug
```

**Output**: `build/debug/warploom.exe` (Windows) or `build/debug/warploom` (Linux/macOS)

### Qt-Vulkan Library

Builds the Qt-Vulkan integration library:
```bash
python WarploomController.py build targets/qt-vulkan/library "Build Project" default debug
```

**Output**: `build/debug/libwarploom_qt_vulkan.dll` (Windows) or `build/debug/libwarploom_qt_vulkan.so` (Linux)

## Configuration

### CMake Presets

CMake presets are defined in `CMakePresets.json` and provide pre-configured build settings.

**Available Presets**:
- `default`: Standard configuration
- `debug`: Debug-optimized configuration
- `release`: Release-optimized configuration
- `coverage`: Code coverage configuration

### Environment Variables

Configure build behavior using environment variables:

- `WARPLOOM_COMPILER`: Default compiler
- `WARPLOOM_BUILD_TYPE`: Default build type
- `WARPLOOM_JOBS`: Number of parallel jobs
- `WARPLOOM_VERBOSE`: Enable verbose output

**Example**:
```bash
export WARPLOOM_COMPILER=msvc
export WARPLOOM_BUILD_TYPE=release
python WarploomController.py build standalone "Build Project" default release
```

### Configuration Files

- `CMakeLists.txt`: Main CMake configuration
- `dependencies.cmake`: Dependency management
- `vcpkg.json`: vcpkg package configuration
- `conanfile.txt`: Conan package configuration

## Troubleshooting

### Common Issues

#### Build Fails with "Compiler Not Found"

**Cause**: Specified compiler is not installed or not in PATH.

**Solution**:
1. Verify compiler installation:
```bash
cl --version  # MSVC
gcc --version  # GCC
clang --version  # Clang
```

2. Add compiler to PATH if necessary
3. Use `--compiler` option to specify correct compiler

#### CMake Configuration Fails

**Cause**: Missing dependencies or incorrect CMake version.

**Solution**:
1. Verify CMake version:
```bash
cmake --version
```

2. Install required dependencies:
```bash
pip install conan
vcpkg install <packages>
```

3. Clean build directory:
```bash
python WarploomController.py build standalone "Clean Build Pipeline" default debug
```

#### Linker Errors

**Cause**: Missing libraries or incorrect library paths.

**Solution**:
1. Verify library installation:
```bash
vcpkg list
conan list
```

2. Check CMake cache for library paths:
```bash
cmake --build build/debug --verbose
```

3. Reconfigure with clean build

#### Performance Issues

**Cause**: Insufficient parallel jobs or system resources.

**Solution**:
1. Adjust parallel jobs:
```bash
python WarploomController.py build standalone "Build Project" default debug --parallel 4
```

2. Use release build for faster compilation:
```bash
python WarploomController.py build standalone "Build Project" default release
```

### Getting Help

For additional help:
- Check the [troubleshooting guide](troubleshooting.md)
- Review [analysis documents](../impl/debug/analysis/)
- Check [error documentation](../impl/debug/errors/identified_errors.md)

### Performance Monitoring

The build system automatically tracks performance metrics:

- Build duration
- Memory usage
- Disk I/O
- Compiler invocation count

View performance metrics:
```bash
python -m json.tool build_performance_*.json
```

## Advanced Usage

### Custom Build Tasks

Define custom build tasks in `warploom_scripts/build.py`:

```python
@task
def custom_build(context: BuildContext) -> bool:
    """Custom build task."""
    # Your build logic here
    return True
```

### Build Hooks

Add pre-build and post-build hooks:

```python
@pre_build_hook
def before_build(context: BuildContext):
    """Execute before build."""
    pass

@post_build_hook
def after_build(context: BuildContext):
    """Execute after build."""
    pass
```

## Best Practices

1. **Always use clean builds for production releases**
2. **Use debug builds during development**
3. **Run tests after each build**
4. **Monitor build performance**
5. **Keep dependencies updated**
6. **Use version control for build artifacts**
7. **Document custom build configurations**

## Support

For issues, questions, or contributions:
- GitHub Issues: [repository-url]/issues
- Documentation: [repository-url]/wiki
- Email: support@example.com
