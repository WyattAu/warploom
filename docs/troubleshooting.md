# Troubleshooting Guide

This guide provides solutions to common issues encountered when building and developing with the Warploom template.

## Table of Contents

- [Build Issues](#build-issues)
- [Qt6 Vulkan Issues](#qt6-vulkan-issues)
- [Cross-Compilation Issues](#cross-compilation-issues)
- [Compiler Issues](#compiler-issues)
- [Dependency Issues](#dependency-issues)
- [VSCode Integration Issues](#vscode-integration-issues)
- [WebAssembly Issues](#webassembly-issues)

## Build Issues

### CMake Configuration Fails

**Symptoms:**
- CMake configuration fails with various errors
- Missing dependencies or toolchain issues

**Solutions:**

1. **Clean Build Directory:**
   ```bash
   # Remove build artifacts
   rm -rf build/
   # Or use controller
   python WarploomController.py build standalone "Clean Build Directory" default Debug
   ```

2. **Regenerate Build Files:**
   ```bash
   # Full clean and rebuild
   python WarploomController.py build standalone "Zero to Build" default Debug
   ```

3. **Check CMake Version:**
   ```bash
   cmake --version  # Should be 3.31+
   ```

### Conan Dependency Installation Fails

**Symptoms:**
- Conan install fails with network or package errors
- Dependency resolution issues

**Solutions:**

1. **Clear Conan Cache:**
   ```bash
   conan remove "*" -c  # Clear all caches
   conan cache clean
   ```

2. **Update Conan:**
   ```bash
   pip install --upgrade conan
   conan --version  # Should be 2.0+
   ```

3. **Check Network Connectivity:**
   ```bash
   conan remote list  # Should show conancenter
   ```

## Qt6 Vulkan Issues

### Vulkan SDK Not Found

**Symptoms:**
- Qt/Vulkan builds fail with "Vulkan SDK not found"
- Error: "Vulkan SDK is required for Qt/Vulkan builds"

**Solutions:**

1. **Install Vulkan SDK:**
   - Download from: https://vulkan.lunarg.com/sdk/home
   - Install to default location (C:\VulkanSDK\ on Windows)
   - Restart command prompt/terminal

2. **Verify Installation:**
   ```bash
   # Windows
   dir "C:\VulkanSDK"

   # Linux/macOS
   ls /usr/local/VulkanSDK/  # or check PATH
   ```

3. **Manual PATH Setup (if needed):**
   ```bash
   # Add to environment variables
   export PATH="$PATH:/path/to/VulkanSDK/bin"
   export VK_SDK_PATH="/path/to/VulkanSDK"
   ```

### Qt6 Build Issues

**Symptoms:**
- Qt6 components fail to build or link
- Missing Qt6 libraries

**Solutions:**

1. **Install Qt6 via vcpkg:**
   ```bash
   vcpkg install qt6-base qt6-widgets
   ```

2. **Check vcpkg Integration:**
   ```bash
   # Ensure vcpkg is in PATH or VCPKG_ROOT is set
   vcpkg integrate install
   ```

## Cross-Compilation Issues

### Toolchain Not Found

**Symptoms:**
- Cross-compilation fails with "toolchain not found"
- Missing gcc-aarch64-linux-gnu or similar

**Solutions:**

1. **Automated Installation (Linux):**
   ```bash
   # ARM64
   python WarploomController.py toolchain install arm64-linux-gnu

   # x86 Linux
   python WarploomController.py toolchain install x86-linux-gnu
   ```

2. **Manual Installation:**

   **Ubuntu/Debian:**
   ```bash
   sudo apt update
   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu  # ARM64
   sudo apt install gcc-x86-64-linux-gnu g++-x86-64-linux-gnu    # x86
   ```

   **Fedora/RHEL:**
   ```bash
   sudo dnf install gcc-aarch64-linux-gnu gcc-c++-aarch64-linux-gnu  # ARM64
   sudo dnf install gcc-x86-64-linux-gnu gcc-c++-x86-64-linux-gnu    # x86
   ```

3. **Verify Toolchain:**
   ```bash
   aarch64-linux-gnu-gcc --version  # ARM64
   x86_64-linux-gnu-gcc --version    # x86
   ```

### Cross-Compilation Build Fails

**Symptoms:**
- Cross-compiled binaries fail to run or link incorrectly

**Solutions:**

1. **Check Target Architecture:**
   ```bash
   # Verify you're building for the correct target
   python WarploomController.py build standalone "Build" arm64-linux-gnu Debug
   ```

2. **Validate Toolchain Paths:**
   ```bash
   which aarch64-linux-gnu-gcc  # Should show valid path
   ```

## Compiler Issues

### Clang-MSVC Build Issues

**Symptoms:**
- Clang with MSVC fails to build
- LLVM/Clang not found

**Solutions:**

1. **Install LLVM (Windows):**
   - Install Visual Studio with LLVM components
   - Or install LLVM separately from https://llvm.org/

2. **Check LLVM Installation:**
   ```bash
   clang-cl --version  # Should work
   ```

3. **Environment Setup:**
   ```bash
   # Ensure setup_clang.bat runs correctly
   python WarploomController.py build standalone "Conan install" clang-msvc Debug
   ```

### GCC MinGW Issues

**Symptoms:**
- MinGW builds fail with missing libraries
- UCRT vs MSVCRT conflicts

**Solutions:**

1. **Use MSYS2 UCRT64:**
   ```bash
   # Install MSYS2 UCRT64
   pacman -S mingw-w64-ucrt-x86_64-gcc
   ```

2. **Check MinGW Setup:**
   ```bash
   gcc --version  # Should show MinGW version
   ```

## Dependency Issues

### vcpkg Package Installation Fails

**Symptoms:**
- vcpkg install fails
- Missing system dependencies

**Solutions:**

1. **Update vcpkg:**
   ```bash
   git pull  # In vcpkg directory
   .\bootstrap-vcpkg.bat  # Windows
   ./bootstrap-vcpkg.sh   # Linux/macOS
   ```

2. **Install System Dependencies:**
   ```bash
   # Ubuntu/Debian
   sudo apt install pkg-config libgl1-mesa-dev libx11-dev libxrandr-dev

   # Fedora/RHEL
   sudo dnf install pkgconf mesa-libGL-devel libX11-devel libXrandr-devel
   ```

### Conan Package Conflicts

**Symptoms:**
- Package version conflicts
- Dependency resolution fails

**Solutions:**

1. **Clear Conan Cache:**
   ```bash
   conan remove "*" -c
   conan cache clean
   ```

2. **Update Package Requirements:**
   ```bash
   # Check conanfile.py for version constraints
   conan install . --build=missing
   ```

## VSCode Integration Issues

### Language Server Not Working

**Symptoms:**
- No IntelliSense or code completion
- clangd/ccls not functioning

**Solutions:**

1. **Restart Language Servers:**
   ```
   Ctrl+Shift+P → "clangd: Restart language server"
   Ctrl+Shift+P → "ccls: Restart"
   ```

2. **Clear Caches:**
   ```bash
   # Clear clangd cache
   rm -rf .cache/clangd

   # Clear ccls cache
   rm -rf ~/.cache/ccls-cache
   ```

3. **Check Compilation Database:**
   ```bash
   # Ensure build directory has compile_commands.json
   ls build/standalone/default/debug/compile_commands.json
   ```

### Task Execution Fails

**Symptoms:**
- VSCode tasks fail to run
- Controller script errors

**Solutions:**

1. **Check Python Environment:**
   ```bash
   python --version  # Should be 3.8+
   pip install -r requirements.txt  # If needed
   ```

2. **Validate VSCode Settings:**
   ```json
   // .vscode/settings.json
   {
     "python.pythonPath": "python",
     "cmake.configureOnOpen": false
   }
   ```

## WebAssembly Issues

### Emscripten Build Fails

**Symptoms:**
- WebAssembly compilation fails
- emcc not found

**Solutions:**

1. **Setup Emscripten Environment:**
   ```bash
   # Navigate to emsdk directory
   cd /path/to/emsdk

   # Install and activate
   ./emsdk install latest
   ./emsdk activate latest
   source ./emsdk_env.sh
   ```

2. **Verify Emscripten:**
   ```bash
   emcc --version  # Should show version
   ```

3. **Check PATH:**
   ```bash
   which emcc  # Should show path
   echo $EMSDK  # Should be set
   ```

### WebAssembly Runtime Issues

**Symptoms:**
- WASM module fails to load in browser
- Runtime errors

**Solutions:**

1. **Check Browser Compatibility:**
   - Use modern browsers (Chrome, Firefox, Safari, Edge)
   - Enable WebAssembly in browser settings

2. **Validate Build Output:**
   ```bash
   # Check generated files
   ls build/targets/qt-vulkan/standalone/emscripten/debug/bin/
   # Should contain .html, .js, .wasm files
   ```

3. **Use Development Server:**
   ```bash
   python WarploomController.py build standalone "Launch WebAssembly Server" emscripten Debug
   ```

## General Debugging Tips

### Enable Verbose Output

1. **CMake Verbose Build:**
   ```bash
   cmake --build build/ -v
   ```

2. **Conan Verbose Install:**
   ```bash
   conan install . -v
   ```

### Check Logs

1. **Controller Logs:**
   ```bash
   cat WarploomController.log
   ```

2. **CMake Logs:**
   ```bash
   cat build/CMakeFiles/CMakeOutput.log
   cat build/CMakeFiles/CMakeError.log
   ```

### System Information

```bash
# Gather system info for bug reports
python -c "import platform; print(platform.platform())"
cmake --version
conan --version
python --version
```

## Getting Help

If these solutions don't resolve your issue:

1. **Check Existing Issues:** Search the GitHub repository for similar problems
2. **Create a Bug Report:** Include:
   - Operating system and version
   - CMake, Conan, Python versions
   - Full error output
   - Steps to reproduce
3. **Community Support:** Join the Discord server for real-time help

## Quick Reference

### Common Commands

```bash
# Clean everything and start fresh
python WarploomController.py build standalone "Zero to Build" default Debug

# Check toolchain availability
python WarploomController.py toolchain check

# Install missing toolchains (Linux)
python WarploomController.py toolchain install arm64-linux-gnu

# Format code
python WarploomController.py build standalone "Format Code" noNeedArch

# Generate documentation
python WarploomController.py build standalone "Generate Documentation" noNeedArch
```

### Environment Variables

```bash
# Vulkan SDK (if not in default location)
export VK_SDK_PATH=/path/to/VulkanSDK
export PATH="$PATH:$VK_SDK_PATH/bin"

# Emscripten
source /path/to/emsdk/emsdk_env.sh

# Conan
export CONAN_HOME=~/.conan
```

This troubleshooting guide covers the most common issues. For project-specific problems, check the issue tracker and documentation.</content>
</xai:function_call">The file docs/troubleshooting.md has been created successfully.