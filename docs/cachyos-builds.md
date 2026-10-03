# CachyOS Builds

**Version:** 1.0.0  
**Last Updated:** 2026-01-28  
**Project:** OmniCPP-Template  
**References:** [ADR-032](../.specs/02_adrs/ADR-032-cachyos-support.md) | [REQ-008-002](../.specs/04_future_state/reqs/REQ-008-documentation.md)

---

## Table of Contents

1. [Introduction to CachyOS](#introduction-to-cachyos)
2. [CachyOS Installation](#cachyos-installation)
3. [CachyOS-Specific Compiler Flags](#cachyos-specific-compiler-flags)
4. [CachyOS GCC Build Instructions](#cachyos-gcc-build-instructions)
5. [CachyOS Clang Build Instructions](#cachyos-clang-build-instructions)
6. [CachyOS Conan Profiles](#cachyos-conan-profiles)
7. [CachyOS CMake Presets](#cachyos-cmake-presets)
8. [CachyOS Performance Optimizations](#cachyos-performance-optimizations)
9. [CachyOS Troubleshooting](#cachyos-troubleshooting)
10. [CachyOS Best Practices](#cachyos-best-practices)
11. [Security Considerations](#security-considerations)

---

## Introduction to CachyOS

CachyOS is an Arch Linux derivative optimized for performance. It features a custom Linux kernel with performance patches, optimized compiler toolchains, and system-level optimizations designed for gaming and high-performance computing.

### Key Features

- **Performance-Optimized Kernel:** Custom kernel with performance patches for gaming and workloads
- **GCC 13 & Clang 19:** Latest compiler versions with C++23 support
- **Native Architecture Optimization:** Default `-march=native` for host-specific optimizations
- **Link-Time Optimization (LTO):** Enabled by default for better performance
- **Rolling Release Model:** Always up-to-date with latest packages

### Why CachyOS for OmniCPP?

CachyOS provides an ideal development environment for C++ game engine development:

1. **Modern C++ Support:** Full C++23 support with latest GCC and Clang
2. **Performance Focus:** System-level optimizations translate to better build performance
3. **Gaming-Optimized:** Kernel patches improve real-time performance for game engines
4. **Arch Linux Ecosystem:** Access to AUR and extensive package repositories
5. **Reproducible Builds:** Consistent toolchain across the system

---

## CachyOS Installation

### Prerequisites

Before setting up CachyOS for OmniCPP development, ensure you have:

- CachyOS installed (minimum version: 2024.01)
- Root/sudo access for package installation
- At least 20GB of free disk space
- 8GB RAM minimum (16GB recommended for large projects)

### Installing Required Packages

Use the provided setup script for automated configuration:

```bash
# Run the CachyOS setup script
./scripts/linux/setup_cachyos.sh

# Preview changes without applying
./scripts/linux/setup_cachyos.sh --dry-run

# Enable debug output
./scripts/linux/setup_cachyos.sh --debug
```

### Manual Package Installation

If you prefer manual installation, install the required packages:

```bash
# Update system packages
sudo pacman -Syu

# Install build tools
sudo pacman -S --needed gcc g++ clang cmake ninja

# Install development libraries
sudo pacman -S --needed vulkan-headers vulkan-loader qt6-base

# Install additional tools
sudo pacman -S --needed git python python-pip ccache
```

### Verifying Installation

After installation, verify your toolchain:

```bash
# Check GCC version
gcc --version
# Expected: gcc (GCC) 13.x.x

# Check Clang version
clang --version
# Expected: clang version 19.x.x

# Check CMake version
cmake --version
# Expected: cmake version 3.20.x or higher

# Check Ninja version
ninja --version
```

---

## CachyOS-Specific Compiler Flags

CachyOS uses specific compiler flags optimized for performance. These flags are applied automatically when using CachyOS profiles or presets.

### Release Build Flags

#### GCC Release Flags

```bash
# Compiler flags
CFLAGS="-march=native -O3 -flto -DNDEBUG"
CXXFLAGS="-march=native -O3 -flto -DNDEBUG"

# Linker flags
LDFLAGS="-Wl,--as-needed -Wl,--no-undefined -flto"
```

#### Clang Release Flags

```bash
# Compiler flags
CFLAGS="-march=native -O3 -flto -DNDEBUG"
CXXFLAGS="-march=native -O3 -flto -DNDEBUG"

# Linker flags
LDFLAGS="-Wl,--as-needed -Wl,--no-undefined -flto"
```

### Debug Build Flags

#### GCC Debug Flags

```bash
# Compiler flags
CFLAGS="-march=native -g -O0 -DDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2"
CXXFLAGS="-march=native -g -O0 -DDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2"

# Linker flags
LDFLAGS="-Wl,--as-needed -Wl,--no-undefined -Wl,-z,relro -Wl,-z,now"
```

#### Clang Debug Flags

```bash
# Compiler flags
CFLAGS="-march=native -g -O0 -DDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2"
CXXFLAGS="-march=native -g -O0 -DDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2"

# Linker flags
LDFLAGS="-Wl,--as-needed -Wl,--no-undefined -Wl,-z,relro -Wl,-z,now"
```

### Flag Explanations

| Flag | Purpose | Effect |
|-------|---------|--------|
| `-march=native` | Optimize for host CPU | Generates code optimized for your specific CPU architecture |
| `-O3` | Maximum optimization | Enables all optimization flags including auto-vectorization |
| `-flto` | Link-Time Optimization | Optimizes across translation units during linking |
| `-DNDEBUG` | Disable assertions | Removes `assert()` calls for release builds |
| `-g` | Generate debug info | Includes debugging symbols in binary |
| `-O0` | No optimization | Disables optimizations for better debugging |
| `-DDEBUG` | Enable debug code | Activates debug-specific code paths |
| `-fstack-protector-strong` | Stack protection | Adds stack canaries to prevent buffer overflows |
| `-D_FORTIFY_SOURCE=2` | Runtime buffer checks | Adds runtime checks for buffer operations |
| `--as-needed` | Link only used libraries | Reduces binary size by removing unused dependencies |
| `--no-undefined` | Fail on undefined symbols | Ensures all symbols are resolved at link time |
| `-z,relro` | Relocation read-only | Makes some sections read-only after relocation |
| `-z,now` | Bind symbols immediately | Binds all symbols at load time (no lazy binding) |

---

## CachyOS GCC Build Instructions

### Quick Start

The fastest way to build with GCC on CachyOS is using CMake presets:

```bash
# Configure with CachyOS GCC release preset
cmake --preset gcc-release

# Build the project
cmake --build --preset gcc-release

# Run tests (if enabled)
ctest --preset gcc-release
```

### Manual Configuration

For more control, configure manually:

```bash
# Create build directory
mkdir -p build/cachyos-gcc/release
cd build/cachyos-gcc/release

# Configure with CMake
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_STANDARD=23 \
  -DCMAKE_C_FLAGS="-march=native -O3 -flto -DNDEBUG" \
  -DCMAKE_CXX_FLAGS="-march=native -O3 -flto -DNDEBUG" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--no-undefined -flto" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--as-needed -Wl,--no-undefined -flto" \
  -DOMNICPP_BUILD_ENGINE=ON \
  -DOMNICPP_BUILD_GAME=ON \
  ../..

# Build
ninja

# Install (optional)
ninja install
```

### Debug Build

```bash
# Create debug build directory
mkdir -p build/cachyos-gcc/debug
cd build/cachyos-gcc/debug

# Configure for debug
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_STANDARD=23 \
  -DCMAKE_C_FLAGS="-march=native -g -O0 -DDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2" \
  -DCMAKE_CXX_FLAGS="-march=native -g -O0 -DDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--no-undefined -Wl,-z,relro -Wl,-z,now" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--as-needed -Wl,--no-undefined -Wl,-z,relro -Wl,-z,now" \
  -DOMNICPP_BUILD_ENGINE=ON \
  -DOMNICPP_BUILD_GAME=ON \
  -DOMNICPP_BUILD_TESTS=ON \
  ../..

# Build
ninja
```

### Using ccache for Faster Rebuilds

Enable ccache to speed up rebuilds:

```bash
# Install ccache
sudo pacman -S ccache

# Enable ccache for GCC
export CC="ccache gcc"
export CXX="ccache g++"

# Configure and build as usual
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ../..
ninja
```

---

## CachyOS Clang Build Instructions

### Quick Start

Build with Clang using CMake presets:

```bash
# Configure with CachyOS Clang release preset
cmake --preset clang-release

# Build the project
cmake --build --preset clang-release

# Run tests (if enabled)
ctest --preset clang-release
```

### Manual Configuration

```bash
# Create build directory
mkdir -p build/cachyos-clang/release
cd build/cachyos-clang/release

# Configure with CMake
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_STANDARD=23 \
  -DCMAKE_C_FLAGS="-march=native -O3 -flto -DNDEBUG" \
  -DCMAKE_CXX_FLAGS="-march=native -O3 -flto -DNDEBUG" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--no-undefined -flto" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--as-needed -Wl,--no-undefined -flto" \
  -DOMNICPP_BUILD_ENGINE=ON \
  -DOMNICPP_BUILD_GAME=ON \
  ../..

# Build
ninja
```

### Debug Build

```bash
# Create debug build directory
mkdir -p build/cachyos-clang/debug
cd build/cachyos-clang/debug

# Configure for debug
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_STANDARD=23 \
  -DCMAKE_C_FLAGS="-march=native -g -O0 -DDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2" \
  -DCMAKE_CXX_FLAGS="-march=native -g -O0 -DDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed -Wl,--no-undefined -Wl,-z,relro -Wl,-z,now" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--as-needed -Wl,--no-undefined -Wl,-z,relro -Wl,-z,now" \
  -DOMNICPP_BUILD_ENGINE=ON \
  -DOMNICPP_BUILD_GAME=ON \
  -DOMNICPP_BUILD_TESTS=ON \
  ../..

# Build
ninja
```

### Using libc++ with Clang

CachyOS Clang builds use `libc++` (LLVM's C++ standard library) by default:

```bash
# Configure with libc++
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
  -DCMAKE_EXE_LINKER_FLAGS="-lc++ -lc++abi" \
  ../..
```

---

## Package management on CachyOS

Warploom has no package-manager step. Its only third-party dependency is
GoogleTest, fetched by CPM from a shared source cache
(`~/.cache/warploom/cpm`), and Vulkan, `libxcb` and a SPIR-V compiler come
from the system.

The `conan/` tree that this page used to document has been deleted: it was
Conan 1 syntax driven by Conan 2 flags, declared thirty packages the engine
never included, and defaulted to off. `conan-linux-profiles.md` went with it.

CachyOS needs nothing special. Install the toolchain and build:

```bash
sudo pacman -S base-devel cmake ninja vulkan-headers libxcb glslang
cmake --preset headless-debug
cmake --build build/headless-debug -j$(nproc)
```

For the windowed viewport you also want a Vulkan driver
(`vulkan-radeon`, `vulkan-intel` or `nvidia-dkms`) and `libxcb`.

## CachyOS CMake Presets

The `cachyos-*` presets were byte-identical duplicates of the `gcc-*` /
`clang-*` presets and have been deleted; use those directly. Inside a Nix
shell the `nix-cachyos-*` presets still exist.

### Available Presets

| Preset | Compiler | Build Type | Description |
|--------|-----------|-------------|-------------|
| `gcc-debug` | GCC 13 | Debug | Debug build with GCC |
| `gcc-release` | GCC 13 | Release | Optimized release build with GCC |
| `gcc-relwithdebinfo` | GCC 13 | RelWithDebInfo | Release with debug info |
| `cachyos-gcc-minsizerel` | GCC 13 | MinSizeRel | Minimum size release |
| `clang-debug` | Clang 19 | Debug | Debug build with Clang |
| `clang-release` | Clang 19 | Release | Optimized release build with Clang |
| `clang-relwithdebinfo` | Clang 19 | RelWithDebInfo | Release with debug info |
| `cachyos-clang-minsizerel` | Clang 19 | MinSizeRel | Minimum size release |

### Using CMake Presets

```bash
# List all available presets
cmake --list-presets

# Configure with CachyOS GCC release preset
cmake --preset gcc-release

# Configure with CachyOS Clang release preset
cmake --preset clang-release

# Build with preset
cmake --build --preset gcc-release

# Clean build
cmake --build --preset gcc-release --target clean

# Run tests
ctest --preset gcc-debug
```

### Preset Configuration Details

#### gcc-release Preset

```json
{
  "name": "gcc-release",
  "displayName": "CachyOS-GCC Release",
  "description": "Release build with CachyOS GCC 13 compiler with performance optimizations",
  "binaryDir": "${sourceDir}/build/cachyos-gcc/release",
  "generator": "Ninja",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_C_COMPILER": "gcc",
    "CMAKE_CXX_COMPILER": "g++",
    "CMAKE_CXX_STANDARD": "23",
    "CMAKE_CXX_FLAGS": "-march=native -O3 -flto -DNDEBUG -Wall -Wextra -Wpedantic",
    "CMAKE_C_FLAGS": "-march=native -O3 -flto -DNDEBUG -Wall -Wextra -Wpedantic",
    "CMAKE_EXE_LINKER_FLAGS": "-Wl,--as-needed -Wl,--no-undefined -flto",
    "CMAKE_SHARED_LINKER_FLAGS": "-Wl,--as-needed -Wl,--no-undefined -flto",
    "OMNICPP_BUILD_ENGINE": "ON",
    "OMNICPP_BUILD_GAME": "ON",
    "OMNICPP_BUILD_TESTS": "OFF",
    "OMNICPP_BUILD_EXAMPLES": "OFF"
  }
}
```

#### clang-release Preset

```json
{
  "name": "clang-release",
  "displayName": "CachyOS-Clang Release",
  "description": "Release build with CachyOS Clang 19 compiler with performance optimizations",
  "binaryDir": "${sourceDir}/build/cachyos-clang/release",
  "generator": "Ninja",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_C_COMPILER": "clang",
    "CMAKE_CXX_COMPILER": "clang++",
    "CMAKE_CXX_STANDARD": "23",
    "CMAKE_CXX_FLAGS": "-march=native -O3 -flto -DNDEBUG -Wall -Wextra -Wpedantic",
    "CMAKE_C_FLAGS": "-march=native -O3 -flto -DNDEBUG -Wall -Wextra -Wpedantic",
    "CMAKE_EXE_LINKER_FLAGS": "-Wl,--as-needed -Wl,--no-undefined -flto",
    "CMAKE_SHARED_LINKER_FLAGS": "-Wl,--as-needed -Wl,--no-undefined -flto",
    "OMNICPP_BUILD_ENGINE": "ON",
    "OMNICPP_BUILD_GAME": "ON",
    "OMNICPP_BUILD_TESTS": "OFF",
    "OMNICPP_BUILD_EXAMPLES": "OFF"
  }
}
```

---

## CachyOS Performance Optimizations

CachyOS provides several performance optimizations that benefit C++ development.

### Link-Time Optimization (LTO)

LTO performs optimizations across translation units during the linking phase, resulting in better performance.

```bash
# Enable LTO in CMake
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# Or via command line
cmake -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON ..
```

### Native Architecture Optimization

The `-march=native` flag generates code optimized for your specific CPU:

```bash
# Check your CPU features
cat /proc/cpuinfo | grep flags

# View optimized instruction set
gcc -march=native -v -E - < /dev/null 2>&1 | grep cc1
```

### Unity Builds

Unity builds combine multiple source files into a single compilation unit, reducing build time:

```bash
# Enable unity builds in CMake
set(CMAKE_UNITY_BUILD ON)

# Or via command line
cmake -DCMAKE_UNITY_BUILD=ON ..
```

### Parallel Compilation

Use all available CPU cores for faster compilation:

```bash
# Build with all cores
ninja -j$(nproc)

# Or specify specific number of jobs
ninja -j8
```

### ccache Integration

Cache compilation artifacts to speed up rebuilds:

```bash
# Install ccache
sudo pacman -S ccache

# Enable ccache in CMake
cmake -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      ..

# Check cache statistics
ccache -s
```

### Build Performance Tips

1. **Use Ninja over Make:** Ninja is faster for incremental builds
2. **Enable ccache:** Reduces rebuild time significantly
3. **Use SSD storage:** Faster I/O improves build speed
4. **Increase swap:** Prevents out-of-memory errors during large builds
5. **Disable tests for release builds:** Reduces build time

---

## CachyOS Troubleshooting

### Common Issues and Solutions

#### Issue: GCC version not found

**Error:**
```
CMake Error: Could not find CMAKE_C_COMPILER
```

**Solution:**
```bash
# Install GCC
sudo pacman -S gcc

# Verify installation
gcc --version

# Set environment variables
export CC=gcc
export CXX=g++
```

#### Issue: LTO linking errors

**Error:**
```
undefined reference to 'symbol'
```

**Solution:**
```bash
# Disable LTO temporarily
cmake -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF ..

# Or use thin LTO (faster linking)
cmake -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=THIN ..
```

#### Issue: ccache not working

**Error:**
```
ccache: command not found
```

**Solution:**
```bash
# Install ccache
sudo pacman -S ccache

# Verify installation
ccache --version

# Enable in CMake
cmake -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      ..
```

#### Issue: Vulkan not found

**Error:**
```
Could not find Vulkan
```

**Solution:**
```bash
# Install Vulkan headers and loader
sudo pacman -S vulkan-headers vulkan-loader

# Verify Vulkan installation
vulkaninfo | less
```

#### Issue: Out of memory during build

**Error:**
```
cc1plus: out of memory allocating
```

**Solution:**
```bash
# Increase swap space
sudo fallocate -l 4G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile

# Reduce parallel jobs
ninja -j2
```

#### Issue: Permission denied on build directory

**Error:**
```
CMake Error: Could not create build directory
```

**Solution:**
```bash
# Fix directory permissions
sudo chown -R $USER:$USER build/

# Or use a different build directory
mkdir -p ~/build/omnicpp
cd ~/build/omnicpp
cmake /path/to/project
```

### Debugging Build Issues

#### Enable CMake Debug Output

```bash
# Enable verbose output
cmake --preset gcc-debug --trace-expand

# Or with environment variable
VERBOSE=1 cmake --build --preset gcc-debug
```

#### Check Compiler Flags

```bash
# Show actual compiler flags
cmake --preset gcc-release --trace 2>&1 | grep CMAKE_CXX_FLAGS

# Verify flags in build directory
cat build/cachyos-gcc/release/CMakeCache.txt | grep FLAGS
```

#### Test Simple Compilation

```bash
# Create a simple test file
cat > test.cpp << 'EOF'
#include <iostream>
int main() {
    std::cout << "Hello from CachyOS!" << std::endl;
    return 0;
}
EOF

# Compile with CachyOS flags
g++ -march=native -O3 -flto -DNDEBUG test.cpp -o test

# Run the test
./test
```

### Getting Help

If you encounter issues not covered here:

1. Check the [troubleshooting guide](troubleshooting.md)
2. Review the [CMake documentation](https://cmake.org/documentation/)
3. Search [CachyOS forums](https://wiki.cachyos.org/)
4. Open an issue on the project repository

---

## CachyOS Best Practices

### Development Workflow

1. **Use CMake Presets:** Leverage pre-configured presets for consistency
2. **Separate Build Directories:** Keep debug and release builds separate
3. **Version Control:** Commit `CMakeCache.txt` and `conan.lock` files
4. **Regular Updates:** Keep system packages updated for security patches
5. **Clean Builds:** Periodically clean build directories to avoid stale artifacts

### Code Quality

1. **Enable Warnings:** Always build with `-Wall -Wextra -Wpedantic`
2. **Treat Warnings as Errors:** Use `-Werror` for strict builds
3. **Static Analysis:** Run `clang-tidy` and `cppcheck` regularly
4. **Code Formatting:** Use `clang-format` for consistent code style
5. **Unit Testing:** Write and run tests for all new features

### Performance Optimization

1. **Profile Before Optimizing:** Use profilers to identify bottlenecks
2. **Benchmark Changes:** Measure performance impact of optimizations
3. **Use Release Builds:** Profile with release builds, not debug builds
4. **Consider Trade-offs:** Balance performance vs. code maintainability
5. **Document Optimizations:** Explain why specific optimizations were chosen

### Security Practices

1. **Enable Hardening:** Use security flags even in debug builds
2. **Validate Inputs:** Always validate user input and external data
3. **Use Safe APIs:** Prefer safe string and memory functions
4. **Regular Audits:** Review code for security vulnerabilities
5. **Keep Dependencies Updated:** Update third-party libraries regularly

### Environment Configuration

```bash
# Set up environment file
cat > ~/.cachyos_dev.env << 'EOF'
# CachyOS development environment
export CC=gcc
export CXX=g++
export CFLAGS="-march=native -O3 -flto -DNDEBUG"
export CXXFLAGS="-march=native -O3 -flto -DNDEBUG"
export LDFLAGS="-Wl,--as-needed -Wl,--no-undefined -flto"
export PATH="$HOME/.local/bin:$PATH"
EOF

# Source the environment
source ~/.cachyos_dev.env
```

### IDE Integration

#### VSCode Configuration

Create `.vscode/settings.json`:

```json
{
  "cmake.configureOnOpen": true,
  "cmake.defaultVariants": {
    "buildType": "Release",
    "compiler": "gcc"
  },
  "C_Cpp.default.compilerPath": "/usr/bin/gcc",
  "C_Cpp.default.cppStandard": "c++23",
  "C_Cpp.default.cStandard": "c17",
  "C_Cpp.default.intelliSenseMode": "linux-gcc-x64"
}
```

#### CLion Configuration

1. Open project in CLion
2. Go to File → Settings → Build, Execution, Deployment → CMake
3. Add new profile: "CachyOS GCC Release"
4. Set build type to Release
5. Add CMake options: `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`

---

## Security Considerations

### Threat Model

CachyOS builds should consider the following security aspects (see [threat model analysis](../.specs/03_threat_model/analysis.md)):

- **TM-LX-001:** CachyOS Security Considerations
- **TM-LX-002:** Distribution-Specific Vulnerabilities
- **TM-LX-005:** Linux Script Security Risks

### Security Hardening Flags

CachyOS profiles include security hardening flags:

```bash
# Stack protection
-fstack-protector-strong

# Buffer overflow detection
-D_FORTIFY_SOURCE=2

# Position-independent code
-fPIC -fPIE

# Read-only relocations
-Wl,-z,relro

# Immediate binding
-Wl,-z,now

# No undefined symbols
-Wl,--no-undefined
```

### Dependency Security

1. **Verify Package Signatures:** Always verify package signatures
2. **Pin Versions:** Use exact versions in `conanfile.txt`
3. **Regular Updates:** Keep dependencies updated
4. **Security Scanning:** Use tools like `snyk` or `trivy`
5. **Private Repositories:** Use private Conan repositories for sensitive projects

### Build Security

1. **Reproducible Builds:** Ensure builds are reproducible
2. **Secure Storage:** Store build artifacts securely
3. **Access Control:** Restrict access to build servers
4. **Audit Trails:** Log all build activities
5. **Code Signing:** Sign release binaries

### Environment Security

```bash
# Don't commit sensitive files
echo ".cachyos.env" >> .gitignore
echo "build/" >> .gitignore

# Secure environment variables
chmod 600 ~/.cachyos_dev.env

# Use secure package sources
sudo pacman-key --refresh-keys
```

---

## Additional Resources

### Documentation

- [OmniCPP Documentation](index.md)
- [Linux Builds Guide](linux-builds.md)
- [Developer Guide](developer-guide.md)
- [Troubleshooting Guide](troubleshooting.md)

### External Resources

- [CachyOS Wiki](https://wiki.cachyos.org/)
- [CMake Documentation](https://cmake.org/documentation/)
- [Conan Documentation](https://docs.conan.io/)
- [GCC Documentation](https://gcc.gnu.org/onlinedocs/)
- [Clang Documentation](https://clang.llvm.org/docs/)

### Community

- [CachyOS Forum](https://forum.cachyos.org/)
- [CachyOS Discord](https://discord.gg/cachyos)
- [OmniCPP GitHub](https://github.com/your-org/OmniCPP-template)

---

## Changelog

### Version 1.0.0 (2026-01-28)

- Initial documentation for CachyOS builds
- Added GCC and Clang build instructions
- Added Conan profiles documentation
- Added CMake presets documentation
- Added performance optimization guide
- Added troubleshooting section
- Added security considerations

---

**Document Status:** Complete  
**Last Reviewed:** 2026-01-28  
**Next Review:** 2026-07-28
