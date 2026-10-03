# Linux Builds

> **Note**
> Parts of this page describe the pre-0.1 Conan and direnv setup, which has
> been retired. The engine's only third-party dependency is GoogleTest,
> fetched by CPM; `conan/` no longer exists. See the README for the current
> build path.


This document outlines requirements and expected behavior for Linux builds in OmniCpp project.

## Overview

Linux builds have been significantly enhanced with the following new features:

- **Nix Package Manager Integration** - Reproducible development environments with [`flake.nix`](../flake.nix:1) and [`flake.lock`](../flake.lock:1)
- **CachyOS Primary Target** - Performance-optimized builds for CachyOS with custom compiler flags
- **Direnv Integration** - Automatic environment loading with an `.envrc` you create yourself
- **Enhanced Conan Profiles** - Linux-specific profiles for GCC 13 and Clang 19
- **CMake Presets** - Linux-specific CMake presets for GCC and Clang
- **VSCode Linux Configuration** - Platform-specific tasks and debug configurations

> **Note:** CachyOS is the primary Linux target with optimized compiler flags and performance tuning. See [`docs/cachyos-builds.md`](cachyos-builds.md:1) for CachyOS-specific instructions. See [`docs/nix-development.md`](nix-development.md:1) for Nix environment setup. See [`docs/linux-troubleshooting.md`](linux-troubleshooting.md:1) for troubleshooting. See [`docs/vscode-linux-setup.md`](vscode-linux-setup.md:1) for VSCode configuration.

## Prerequisites

### Linux Environment

Linux environment is **required** for Linux builds. The project supports native Linux development with GCC and Clang compilers.

#### Supported Distributions

The build system works on most modern Linux distributions:

- **Ubuntu/Debian:** Primary development and CI environment
- **Fedora/RHEL/CentOS:** Supported with package manager adjustments
- **Arch Linux:** Supported with pacman package manager
- **Other distributions:** May work with equivalent packages

#### Essential Tools

Install required development tools:

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install build-essential cmake ninja-build python3 python3-pip git

# Fedora/RHEL
sudo dnf install gcc gcc-c++ cmake ninja-build python3 python3-pip git

# Arch Linux
sudo pacman -S gcc cmake ninja python python-pip git
```

#### Compiler Installation

Choose one or both compilers:

##### GCC Installation

```bash
# Ubuntu/Debian
sudo apt install gcc g++

# Fedora/RHEL
sudo dnf install gcc gcc-c++

# Arch Linux
sudo pacman -S gcc
```

##### Clang Installation

```bash
# Ubuntu/Debian
sudo apt install clang clang++

# Fedora/RHEL
sudo dnf install clang clang++

# Arch Linux
sudo pacman -S clang
```

#### Additional Requirements

- **Vulkan SDK:** Required for Qt/Vulkan builds
  - Ubuntu/Debian: `sudo apt install vulkan-tools libvulkan-dev vulkan-validationlayers-dev`
  - Fedora: `sudo dnf install vulkan-tools vulkan-loader-devel`
  - Arch: `sudo pacman -S vulkan-icd-loader vulkan-headers`

- **Qt6 Development:** Required for Qt/Vulkan builds
  - Ubuntu/Debian: `sudo apt install qt6-base-dev qt6-tools-dev`
  - Fedora: `sudo dnf install qt6-qtbase-devel qt6-qttools-devel`
  - Arch: `sudo pacman -S qt6-base qt6-tools`

- **Python 3.8+:** For build automation scripts
- **CMake 3.31+:** Build system
- **Conan 2.0+:** Package management (`pip install conan`)
- **Ninja:** Fast build tool

## Build Pipeline Behavior

When running on Linux, build pipeline follows these steps:

### 1. Environment Setup

The build system automatically detects Linux environment:

- **Detection:** Identifies Linux platform via `platform.system() == 'Linux'`
- **Compiler Detection:** Searches for GCC/Clang in PATH
- **Default Configuration:** Uses system compilers when no specific toolchain is specified
- **Conan Environment:** Sources `conanbuild.sh` for dependency environment setup

### 2. Conan Profile Configuration

For native Linux builds, Conan uses default profiles or system detection:

```ini
[settings]
os=Linux
arch=x86_64  # or arm64 for ARM systems
compiler=gcc  # or clang
compiler.version=11  # system compiler version
compiler.libcxx=libstdc++11
build_type=Release
```

### 3. CMake Configuration

Linux builds use native CMake configuration:

- **Generator:** Ninja (preferred) or Make
- **Toolchain:** System compilers (no cross-compilation toolchain needed)
- **Build Directory:** `build/debug/default` or `build/release/default`
- **Compiler Flags:** Standard GCC/Clang flags with Linux-specific optimizations

### 4. Build Process

The build pipeline executes:

1. **Dependency Installation:** Conan installs required packages using system compilers
2. **CMake Generation:** Configures build system with native toolchain
3. **Compilation:** Ninja/Make builds all targets
4. **Testing:** Runs unit tests (if enabled)
5. **Packaging:** Creates distribution packages (DEB/RPM/TGZ)

## Expected Build Outputs

### Directory Structure

```
build/
├── debug/default/
│   ├── bin/                    # Executables
│   │   ├── OmniCppStandalone
│   │   └── test executables
│   ├── lib/                    # Libraries
│   │   ├── libOmniCppLib.so
│   │   └── static libraries (.a)
│   ├── CMakeFiles/             # Build artifacts
│   ├── compile_commands.json   # LSP database
│   └── conan/                  # Conan artifacts
└── release/default/
    └── [same structure]
```

### Build Artifacts

- **Executables:** ELF executables (no extension)
- **Libraries:** Shared libraries (.so) and static libraries (.a)
- **Debug Symbols:** DWARF debug information
- **Packages:** DEB/RPM/TGZ archives for distribution

### Runtime Dependencies

Linux builds produce executables that depend on:
- **System libraries:** glibc, libstdc++, libgcc
- **Qt libraries:** For Qt/Vulkan builds
- **Vulkan libraries:** For Vulkan-enabled builds
- **X11 libraries:** For GUI applications

## Linux-Specific Configuration

### Compiler Selection

The build system automatically selects the best available compiler:

- **Primary:** GCC (most compatible with system libraries)
- **Alternative:** Clang (better diagnostics, faster compilation)
- **Fallback:** Any available compiler in PATH

### Distribution-Specific Packaging

Linux builds support native package formats:

- **DEB packages:** For Debian/Ubuntu-based systems
- **RPM packages:** For Fedora/RHEL-based systems
- **TGZ archives:** Universal distribution format

### Library Linking

Linux builds use standard dynamic linking:
- **Shared libraries (.so):** Default for runtime flexibility
- **Static libraries (.a):** Available when `BUILD_SHARED_LIBS=OFF`
- **RPATH configuration:** Automatic library search path setup

## Build Commands

### VSCode Keyboard Workflow

1. **Setup:** Shift+F7 → "Zero to Hero" (complete setup)
2. **Build:** F7 (quick rebuild)
3. **Debug:** F5 (start debugging)
4. **Run:** Ctrl+Alt+R (execute standalone)

### Python Controller

```bash
# Complete Linux build
python OmniCppController.py build both "Zero to Hero" default Debug

# Individual steps
python OmniCppController.py build standalone "Conan install" default Debug
python OmniCppController.py build standalone "CMake configure" default Debug
python OmniCppController.py build standalone "Build" default Debug
```

### CMake Direct

```bash
# Configure
cmake --preset generic-linux-x86_64-gcc-15-debug -DBUILD_LIBRARY=ON -DBUILD_STANDALONE=ON

# Build
cmake --build build/debug/default -j $(nproc)

# Test
ctest --test-dir build/debug/default
```

### Makefile

```bash
# Complete build
make zero-to-hero

# Individual targets
make conan-install
make cmake-configure
make build-only
make run-tests
```

## Vulkan SDK Integration

For Qt/Vulkan builds on Linux:

### Vulkan SDK Installation

```bash
# Ubuntu/Debian
wget -qO- https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo apt-key add -
sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan-1.3.250-focal.list https://packages.lunarg.com/vulkan/1.3.250/lunarg-vulkan-1.3.250-focal.list
sudo apt update
sudo apt install vulkan-sdk

# Fedora
sudo dnf install vulkan-tools vulkan-loader-devel

# Arch Linux
sudo pacman -S vulkan-icd-loader vulkan-headers
```

### Environment Variables

The build system automatically sets:
- `VULKAN_SDK`: Vulkan SDK installation path
- `VK_LAYER_PATH`: Validation layer search path
- `LD_LIBRARY_PATH`: Vulkan library search path

## Qt6 Integration

For Qt/Vulkan builds:

### Qt6 Installation

```bash
# Ubuntu/Debian
sudo apt install qt6-base-dev qt6-tools-dev qt6-declarative-dev

# Fedora
sudo dnf install qt6-qtbase-devel qt6-qttools-devel qt6-qtdeclarative-devel

# Arch Linux
sudo pacman -S qt6-base qt6-tools qt6-declarative
```

### Qt-Specific Configuration

- **CMake Integration:** Automatic Qt6 discovery via `find_package(Qt6)`
- **MOC/UIC/RCC:** Automatic Qt meta-object compilation
- **Plugin System:** Proper Qt plugin deployment
- **QML Support:** Declarative UI compilation (when enabled)

## Troubleshooting

### Common Issues

#### 1. "Compiler not found" Error

**Symptoms:** Build fails with "gcc/g++ not found"

**Solutions:**
- Install build-essential: `sudo apt install build-essential`
- Verify PATH: `which gcc && which g++`
- Check compiler versions: `gcc --version`

#### 2. Vulkan SDK Missing

**Symptoms:** Qt/Vulkan builds fail with Vulkan-related errors

**Solutions:**
- Install Vulkan SDK as described above
- Verify Vulkan installation: `vulkaninfo`
- Check environment variables: `echo $VULKAN_SDK`

#### 3. Qt6 Development Libraries Missing

**Symptoms:** CMake fails to find Qt6 components

**Solutions:**
- Install Qt6 dev packages: `sudo apt install qt6-base-dev`
- Verify Qt6 installation: `pkg-config --modversion Qt6Core`
- Clear CMake cache and reconfigure

#### 4. Conan Profile Issues

**Symptoms:** Dependency installation fails

**Solutions:**
- Update Conan: `pip install --upgrade conan`
- Detect system: `conan profile detect`
- Clear cache: `conan remove "*" -f`

#### 5. Library Linking Errors

**Symptoms:** Undefined references during linking

**Solutions:**
- Check library dependencies: `ldd executable`
- Install missing dev packages
- Verify Conan installation completed successfully

### Debug Information

Enable verbose output for troubleshooting:

```bash
# CMake verbose build
cmake --build build/debug/default --verbose

# Conan verbose install
conan install . --build=missing -v
```

### Environment Verification

Run this script to verify Linux build environment:

```bash
#!/bin/bash
echo "Linux Build Environment Check"
echo "=============================="

echo "Checking Linux distribution..."
if command -v lsb_release &> /dev/null; then
    lsb_release -a
else
    cat /etc/os-release
fi

echo -e "\nChecking compilers..."
if command -v gcc &> /dev/null; then
    echo "✓ GCC found: $(gcc --version | head -n1)"
else
    echo "✗ GCC not found"
fi

if command -v clang &> /dev/null; then
    echo "✓ Clang found: $(clang --version | head -n1)"
else
    echo "✗ Clang not found"
fi

echo -e "\nChecking build tools..."
for tool in cmake ninja python3 conan; do
    if command -v $tool &> /dev/null; then
        echo "✓ $tool found: $($tool --version | head -n1)"
    else
        echo "✗ $tool not found"
    fi
done

echo -e "\nChecking Vulkan..."
if command -v vulkaninfo &> /dev/null; then
    echo "✓ Vulkan tools found"
else
    echo "✗ Vulkan tools not found"
fi

echo -e "\nChecking Qt6..."
if pkg-config --exists Qt6Core; then
    echo "✓ Qt6 found: $(pkg-config --modversion Qt6Core)"
else
    echo "✗ Qt6 not found"
fi
```

## Performance Considerations

### Build Performance

- **Ninja Generator:** Fast parallel builds with `-j $(nproc)`
- **ccache:** Compiler cache support (when enabled)
- **Unity Builds:** Reduced compilation time for large projects
- **Precompiled Headers:** Faster recompilation

### Runtime Performance

- **Native Compilation:** Optimized for target architecture
- **System Libraries:** Best performance with system libc/libstdc++
- **Link-Time Optimization:** Available when `ENABLE_IPO=ON`

## Integration with CI/CD

Linux builds are supported in GitHub Actions and other CI systems:

```yaml
- name: Linux Build
  run: |
    sudo apt update
    sudo apt install build-essential cmake ninja-build python3 python3-pip
    pip install conan
    conan profile detect
    python OmniCppController.py build both "Zero to Hero" default Release
```

## Cross-Platform Compatibility

### From Windows Development

- Linux uses different path separators and library naming
- Executables are ELF format vs Windows PE
- Different system calls and library availability

### From macOS Development

- Similar Unix-like environment but different library versions
- Linux uses glibc vs macOS libc
- Different package managers and installation paths

## Best Practices

1. **Use System Packages:** Prefer distribution packages for better integration
2. **Enable ccache:** `ENABLE_CCACHE=ON` for faster rebuilds
3. **Use Ninja:** Faster than Make for large projects
4. **Test on Target Distributions:** Verify builds on intended deployment systems
5. **Monitor Dependencies:** Track shared library requirements for distribution

## Support

For Linux-specific issues:

1. Check distribution documentation for package installation
2. Review Conan Linux documentation
3. Verify Vulkan/Qt6 installation guides
4. Test with minimal example to isolate issues
5. Check system logs for missing dependencies

The build system provides comprehensive logging to help diagnose Linux-specific problems.
