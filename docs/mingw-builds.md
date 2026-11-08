# Windows GCC (MinGW) Builds

This document outlines the requirements and expected behavior for Windows GCC (MinGW) builds in the Warploom project.

## Prerequisites

### MinGW Installation

MinGW installation is **required** for Windows GCC builds. The project uses MSYS2 UCRT64 environment for MinGW support.

#### Recommended Installation: MSYS2 UCRT64

1. **Download and Install MSYS2:**
   - Download from: https://www.msys2.org/
   - Install to default location (C:\msys64)

2. **Update MSYS2:**
   ```bash
   pacman -Syu
   ```

3. **Install UCRT64 Toolchain:**
   ```bash
   pacman -S mingw-w64-ucrt-x86_64-gcc \
           mingw-w64-ucrt-x86_64-gdb \
           mingw-w64-ucrt-x64-64-make \
           mingw-w64-ucrt-x86_64-ninja \
           mingw-w64-ucrt-x86_64-cmake
   ```

4. **Verify Installation:**
   ```bash
   gcc --version
   g++ --version
   ```

#### Alternative: Standalone MinGW-w64

If you prefer standalone MinGW-w64 over MSYS2:

1. Download from: https://www.mingw-w64.org/downloads/
2. Choose UCRT runtime for better Windows compatibility
3. Add `bin` directory to system PATH

### Additional Requirements

- **Vulkan SDK:** Required for Qt/Vulkan builds
  - Download from: https://vulkan.lunarg.com/sdk/home
  - Install to default location (C:\VulkanSDK\...)

- **Python 3.8+:** For build automation scripts
- **CMake 3.31+:** Build system
- **Conan 2.0+:** Package management

## Build Pipeline Behavior

When MinGW is available, the build pipeline follows these steps:

### 1. Environment Setup

The build system automatically detects and configures the MinGW environment:

- **Detection:** Checks for MSYS2 UCRT64 GCC at `C:\msys64\ucrt64\bin\gcc.exe`
- **Fallback:** Searches for GCC in system PATH
- **Setup Script:** `conan/setup_gcc_mingw_ucrt.bat` configures:
  - MSYS2 UCRT64 PATH
  - Vulkan SDK environment variables
  - Compiler variables (CC=gcc.exe, CXX=g++.exe)

### 2. Conan Profile Configuration

Uses the `gcc-mingw-ucrt` Conan profile:

```ini
[settings]
os=Windows
arch=x86_64
compiler=gcc
compiler.version=11
compiler.libcxx=libstdc++11
compiler.cppstd=23
build_type=Release

[buildenv]
msys2_root=C:/msys64
PATH+={{msys2_root}}/ucrt64/bin
```

### 3. CMake Configuration

MinGW builds use these CMake presets:

- **gcc-mingw-ucrt-debug:** Debug configuration
- **gcc-mingw-ucrt-release:** Release configuration

Key settings:
- **Generator:** Ninja
- **Toolchain:** Conan toolchain (automatic)
- **Build Directory:** `build/debug/gcc-mingw-ucrt` or `build/release/gcc-mingw-ucrt`
- **Compiler Flags:** Standard GCC flags with MinGW-specific adjustments

### 4. Build Process

The build pipeline executes:

1. **Dependency Installation:** Conan installs required packages
2. **CMake Generation:** Configures build system with MinGW toolchain
3. **Compilation:** Ninja builds all targets
4. **Testing:** Runs unit tests (if enabled)
5. **Packaging:** Creates distribution packages

## Expected Build Outputs

### Directory Structure

```
build/
├── debug/gcc-mingw-ucrt/
│   ├── bin/                    # Executables
│   │   ├── WarploomStandalone.exe
│   │   └── test executables
│   ├── lib/                    # Libraries
│   │   ├── WarploomLib.dll
│   │   └── static libraries
│   ├── CMakeFiles/             # Build artifacts
│   ├── compile_commands.json   # LSP database
│   └── conan/                  # Conan artifacts
└── release/gcc-mingw-ucrt/
    └── [same structure]
```

### Build Artifacts

- **Executables:** Windows PE executables (.exe)
- **Libraries:** Dynamic libraries (.dll) and static libraries (.a)
- **Debug Symbols:** PDB files for debugging
- **Packages:** TGZ/ZIP archives for distribution

### Runtime Dependencies

MinGW builds produce executables that depend on:
- **UCRT runtime:** `ucrtbase.dll` (modern Windows runtime)
- **GCC runtime:** `libgcc_s_seh-1.dll`, `libstdc++-6.dll`
- **Vulkan:** `vulkan-1.dll` (for Qt/Vulkan builds)

## MinGW-Specific Configuration

### Static Runtime Linking

When `USE_STATIC_RUNTIME=ON`, MinGW builds use full static linking:

```cmake
if(WIN32)
    target_link_options(${TARGET_NAME} PRIVATE -static)
endif()
```

This bundles runtime libraries into the executable, eliminating external dependencies.

### Compiler Flags

MinGW builds inherit standard GCC flags with Windows-specific additions:
- **Exception Handling:** SEH (Structured Exception Handling)
- **Threading:** POSIX threads via winpthreads
- **Optimization:** Standard GCC optimization levels

### Limitations

1. **Coverage Analysis:** Not supported for MinGW builds (native Windows only)
2. **Qt/Vulkan:** Requires Vulkan SDK installation
3. **Path Length:** Windows path length limitations may affect deep directory structures
4. **Unicode:** UCRT provides better UTF-8 support than legacy MinGW

## Build Commands

### VSCode Keyboard Workflow

1. **Setup:** Shift+F7 → "Zero to Hero" (complete setup)
2. **Build:** F7 (quick rebuild)
3. **Debug:** F5 (start debugging)
4. **Run:** Ctrl+Alt+R (execute standalone)

### Python Controller

```bash
# Complete MinGW build
python WarploomController.py build both "Zero to Hero" gcc-mingw-ucrt Debug

# Individual steps
python WarploomController.py build standalone "Conan install" gcc-mingw-ucrt Debug
python WarploomController.py build standalone "CMake configure" gcc-mingw-ucrt Debug
python WarploomController.py build standalone "Build" gcc-mingw-ucrt Debug
```

### CMake Presets

```bash
# Configure
cmake --preset gcc-mingw-ucrt-debug

# Build
cmake --build --preset gcc-mingw-ucrt-debug

# Test
ctest --preset gcc-mingw-ucrt-debug
```

### Makefile

```bash
# Complete build
make ARCH=gcc-mingw-ucrt build

# Individual targets
make conan-install
make cmake-configure
make build-only
```

## Troubleshooting

### Common Issues

#### 1. "MinGW not found" Error

**Symptoms:** Build fails with "MSYS2 UCRT64 GCC not found"

**Solutions:**
- Verify MSYS2 installation: `C:\msys64\ucrt64\bin\gcc.exe` exists
- Run setup script manually: `conan\setup_gcc_mingw_ucrt.bat`
- Add MinGW to PATH if using standalone installation

#### 2. Vulkan SDK Missing

**Symptoms:** Qt/Vulkan builds fail with Vulkan-related errors

**Solutions:**
- Install Vulkan SDK from https://vulkan.lunarg.com/sdk/home
- Ensure Vulkan binaries are in PATH
- Verify `VK_SDK_PATH` environment variable

#### 3. Conan Profile Issues

**Symptoms:** Dependency installation fails

**Solutions:**
- Update Conan: `pip install --upgrade conan`
- Clear Conan cache: `conan remove "*" -f`
- Verify profile: `conan profile show gcc-mingw-ucrt`

#### 4. Linker Errors

**Symptoms:** Undefined references or linking failures

**Solutions:**
- Enable static runtime: `-DUSE_STATIC_RUNTIME=ON`
- Check library paths in Conan profile
- Verify all dependencies are installed

#### 5. Runtime DLL Issues

**Symptoms:** Executable fails to start with missing DLL errors

**Solutions:**
- Copy required DLLs to executable directory
- Use static linking for runtime libraries
- Install Visual C++ Redistributables (for MSVC compatibility)

### Debug Information

Enable verbose output for troubleshooting:

```bash
# CMake verbose build
cmake --build --preset gcc-mingw-ucrt-debug --verbose

# Conan verbose install
conan install . --profile:host=conan/profiles/gcc-mingw-ucrt --build=missing -v
```

### Environment Verification

Run this script to verify MinGW environment:

```batch
@echo off
echo MinGW Environment Check
echo =======================

echo Checking MSYS2 UCRT64...
if exist "C:\msys64\ucrt64\bin\gcc.exe" (
    echo ✓ MSYS2 UCRT64 GCC found
) else (
    echo ✗ MSYS2 UCRT64 GCC not found
)

echo Checking Vulkan SDK...
if defined VULKAN_SDK (
    echo ✓ Vulkan SDK found: %VULKAN_SDK%
) else (
    echo ✗ Vulkan SDK not found
)

echo Checking Conan...
conan --version >nul 2>&1
if %errorlevel% equ 0 (
    echo ✓ Conan found
) else (
    echo ✗ Conan not found
)
```

## Performance Considerations

### Build Performance

- **Ninja Generator:** Fast parallel builds
- **ccache:** Compiler cache support (when enabled)
- **Unity Builds:** Reduced compilation time for large projects

### Runtime Performance

- **UCRT Runtime:** Better performance than legacy MSVCRT
- **GCC Optimizations:** Standard optimization flags
- **Static Linking:** Reduced startup time (when enabled)

## Integration with CI/CD

MinGW builds are supported in GitHub Actions:

```yaml
- name: MinGW Build
  run: |
    choco install msys2
    # MSYS2 setup commands
    python WarploomController.py build both "Zero to Hero" gcc-mingw-ucrt Release
```

## Migration from Other Toolchains

### From MSVC

- MinGW produces native Windows executables
- Different ABI (GNU vs MSVC)
- Cannot mix MinGW and MSVC object files

### From Linux Cross-Compilation

- Native Windows development environment
- Direct Windows API access
- Better Windows-specific optimizations

## Best Practices

1. **Use MSYS2 UCRT64:** Preferred over standalone MinGW for better compatibility
2. **Enable Static Runtime:** For distribution to systems without MinGW installed
3. **Test on Target Systems:** Verify executables run on clean Windows installations
4. **Use Conan Profiles:** Ensure consistent dependency versions
5. **Monitor Dependencies:** Keep track of DLL requirements for distribution

## Support

For MinGW-specific issues:

1. Check MSYS2 documentation: https://www.msys2.org/
2. Review Conan MinGW documentation
3. Verify Vulkan SDK installation
4. Test with minimal example to isolate issues

The build system provides comprehensive logging to help diagnose MinGW-specific problems.