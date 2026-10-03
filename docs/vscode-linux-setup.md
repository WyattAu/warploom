# VSCode Linux Setup Guide

> **Note**
> Parts of this page describe the pre-0.1 Conan and direnv setup, which has
> been retired. The engine's only third-party dependency is GoogleTest,
> fetched by CPM; `conan/` no longer exists. See the README for the current
> build path.


**Version:** 1.0.0
**Last Updated:** 2026-01-28
**Related ADRs:** [ADR-026: VSCode tasks and launch configuration](../.specs/02_adrs/ADR-026-vscode-tasks-launch-configuration.md), [ADR-032: VSCode Platform-Specific Tasks](../.specs/02_adrs/ADR-032-vscode-platform-specific-tasks.md)
**Related Requirements:** [REQ-003: VSCode Configuration](../.specs/04_future_state/reqs/REQ-003-vscode-configuration.md), [REQ-048: VSCode tasks.json Configuration](../.specs/04_future_state/reqs/REQ-048-vscode-tasks-configuration.md), [REQ-049: VSCode launch.json Configuration](../.specs/04_future_state/reqs/REQ-049-vscode-launch-configuration.md)

---

## Overview

This guide provides comprehensive instructions for setting up Visual Studio Code (VSCode) for C++ development on Linux with the OmniCPP Template project. It covers installation, extensions, configuration, build tasks, debugging, and integration with Nix and CachyOS.

### Why VSCode for Linux C++ Development?

VSCode provides a powerful, extensible development environment for C++ on Linux:

- **IntelliSense:** Advanced code completion and navigation
- **Integrated Debugging:** Native GDB and LLDB support
- **Build Integration:** Seamless CMake and Ninja integration
- **Extension Ecosystem:** Rich C++ extension marketplace
- **Cross-Platform:** Same IDE across Windows, Linux, and macOS
- **Git Integration:** Built-in version control features
- **Terminal Integration:** Integrated shell for build commands

---

## Prerequisites

Before setting up VSCode, ensure you have:

- **Linux Distribution:** Ubuntu/Debian, Fedora/RHEL, Arch Linux, or CachyOS
- **VSCode Installed:** Version 1.80 or later
- **Build Tools:** GCC or Clang, CMake, Ninja
- **Python 3.8+:** For build automation scripts
- **Git:** For version control

### Supported Linux Distributions

| Distribution | Status | Notes |
|--------------|----------|-------|
| Ubuntu 22.04+ | Primary | Fully supported, primary CI environment |
| Debian 12+ | Primary | Fully supported |
| Fedora 38+ | Supported | May require package adjustments |
| Arch Linux | Supported | Primary development platform |
| CachyOS | Primary | Optimized configurations available |

---

## VSCode Installation

### Installing VSCode on Linux

#### Ubuntu/Debian

```bash
# Download and install VSCode .deb package
wget -qO- https://packages.microsoft.com/keys/microsoft.asc | gpg --dearmor -o /usr/share/keyrings/microsoft-archive-keyring.gpg
echo "deb [arch=amd64,arm64,armhf signed-by=/usr/share/keyrings/microsoft-archive-keyring.gpg] https://packages.microsoft.com/repos/code stable main" | sudo tee /etc/apt/sources.list.d/vscode.list

sudo apt update
sudo apt install code
```

#### Fedora/RHEL

```bash
# Import Microsoft GPG key
sudo rpm --import https://packages.microsoft.com/keys/microsoft.asc

# Add VSCode repository
sudo sh -c 'echo -e "[code]\nname=Visual Studio Code\nbaseurl=https://packages.microsoft.com/yumrepos/vscode\nenabled=1\ngpgcheck=1\ngpgkey=https://packages.microsoft.com/keys/microsoft.asc" > /etc/yum.repos.d/vscode.repo'

# Install VSCode
sudo dnf check-update
sudo dnf install code
```

#### Arch Linux/CachyOS

```bash
# Install VSCode from official repositories
sudo pacman -S code

# Or install VSCodium (open-source build without telemetry)
sudo pacman -S vscodium-bin
```

### Verifying Installation

```bash
# Check VSCode version
code --version

# Expected output:
# 1.85.1
# abc123def456
```

### VSCode Settings Location

VSCode stores settings in `~/.config/Code/User/settings.json`. Project-specific settings are in `.vscode/settings.json`.

---

## Essential VSCode Extensions for C++ Development

### Required Extensions

Install these extensions for optimal C++ development experience:

#### C/C++ Extension Pack

**Extension:** ms-vscode.cpptools

This is the core C++ extension providing:

- **IntelliSense:** Code completion and parameter hints
- **Code Navigation:** Go to definition, find references
- **Debugging:** Native GDB and LLDB support
- **Code Formatting:** Clang-format integration
- **Syntax Highlighting:** C++23 syntax support

**Installation:**
```bash
# Install from command line
code --install-extension ms-vscode.cpptools

# Or install via VSCode UI:
# 1. Open Extensions view (Ctrl+Shift+X)
# 2. Search for "C/C++"
# 3. Click "Install" on "C/C++ Extension Pack"
```

#### CMake Tools

**Extension:** ms-vscode.cmake-tools

Provides integrated CMake support:

- **CMake Presets:** Select and configure CMake presets
- **Build Targets:** Quick access to CMake targets
- **Debug Configuration:** Automatic debug config generation
- **CMake Cache:** View and edit CMake cache variables
- **Kit Selection:** Choose compiler toolchains

**Installation:**
```bash
code --install-extension ms-vscode.cmake-tools
```

#### Python Extension

**Extension:** ms-python.python

Required for Python build scripts:

- **Python IntelliSense:** Code completion for Python
- **Linting:** Pylint and flake8 integration
- **Debugging:** Python debugger support
- **Virtual Environments:** Auto-detect Python environments

**Installation:**
```bash
code --install-extension ms-python.python
```

### Recommended Extensions

These extensions enhance the development experience:

#### C/C++ Themes

**Extension:** jeff-hykin.better-cpp-syntax

Improved C++ syntax highlighting with modern C++23 features.

```bash
code --install-extension jeff-hykin.better-cpp-syntax
```

#### CMake Language Support

**Extension:** twxs.cmake

CMake syntax highlighting, snippets, and validation.

```bash
code --install-extension twxs.cmake
```

#### Doxygen Documentation Generator

**Extension:** cschloss.doxygen-docgen

Generate Doxygen documentation from C++ code.

```bash
code --install-extension cschloss.doxygen-docgen
```

#### GitLens

**Extension:** eamodio.gitlens

Enhanced Git integration with blame, code lens, and history.

```bash
code --install-extension eamodio.gitlens
```

#### Error Lens

**Extension:** usernamehw.errorlens

Show compiler errors and warnings inline in the editor.

```bash
code --install-extension usernamehw.errorlens
```

### Installing All Extensions at Once

Create a script to install all extensions:

```bash
#!/bin/bash
# scripts/vscode/install_extensions.sh

echo "Installing VSCode extensions for C++ development..."

# Core extensions
code --install-extension ms-vscode.cpptools
code --install-extension ms-vscode.cmake-tools
code --install-extension ms-python.python

# Recommended extensions
code --install-extension jeff-hykin.better-cpp-syntax
code --install-extension twxs.cmake
code --install-extension cschloss.doxygen-docgen
code --install-extension eamodio.gitlens
code --install-extension usernamehw.errorlens

echo "All extensions installed successfully!"
```

Run the script:
```bash
chmod +x scripts/vscode/install_extensions.sh
./scripts/vscode/install_extensions.sh
```

---

## VSCode C/C++ Configuration

### Creating Workspace Settings

The project includes `.vscode/settings.json` with recommended C++ configuration. This file is automatically loaded when you open the project in VSCode.

### C/C++ IntelliSense Configuration

The C/C++ extension uses IntelliSense mode for code completion. Configure it in `.vscode/c_cpp_properties.json`:

```json
{
  "configurations": [
    {
      "name": "Linux GCC",
      "includePath": [
        "${workspaceFolder}/**",
        "${workspaceFolder}/include/**",
        "${workspaceFolder}/build/include/**",
        "/usr/include/**",
        "/usr/local/include/**"
      ],
      "defines": [],
      "compilerPath": "/usr/bin/gcc",
      "cStandard": "c17",
      "cppStandard": "c++23",
      "intelliSenseMode": "linux-gcc-x64",
      "compileCommands": "${workspaceFolder}/build/compile_commands.json"
    },
    {
      "name": "Linux Clang",
      "includePath": [
        "${workspaceFolder}/**",
        "${workspaceFolder}/include/**",
        "${workspaceFolder}/build/include/**",
        "/usr/include/**",
        "/usr/local/include/**"
      ],
      "defines": [],
      "compilerPath": "/usr/bin/clang",
      "cStandard": "c17",
      "cppStandard": "c++23",
      "intelliSenseMode": "linux-clang-x64",
      "compileCommands": "${workspaceFolder}/build/compile_commands.json"
    }
  ],
  "version": 4
}
```

### VSCode Settings for C++

The `.vscode/settings.json` file includes these C++ specific settings:

```json
{
  // C/C++ Configuration
  "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools",
  "C_Cpp.default.intelliSenseMode": "linux-gcc-x64",
  "C_Cpp.default.compilerPath": "/usr/bin/gcc",
  "C_Cpp.default.includePath": [
    "${workspaceFolder}/include",
    "${workspaceFolder}/build/include"
  ],
  "C_Cpp.default.defines": [],
  "C_Cpp.errorSquiggles": "enabled",
  "C_Cpp.autocomplete": "default",
  "C_Cpp.formatting": "clangFormat",
  "C_Cpp.clang_format_style": "file",
  "C_Cpp.clang_format_fallbackStyle": "llvm",
  "C_Cpp.clang_format_sortIncludes": true,
  "C_Cpp.inlayHints.autoDeclarationTypes.enabled": true,
  "C_Cpp.inlayHints.parameterNames.enabled": true,
  "C_Cpp.inlayHints.referenceOperator.enabled": true,
  "C_Cpp.inlayHints.enabled": true,

  // Editor Configuration
  "editor.formatOnSave": true,
  "editor.formatOnPaste": true,
  "editor.formatOnType": false,
  "editor.rulers": [100],
  "editor.tabSize": 4,
  "editor.insertSpaces": true,
  "editor.detectIndentation": false,
  "editor.suggest.snippetsPreventQuickSuggestions": false,
  "editor.wordBasedSuggestions": false,

  // File Associations
  "files.associations": {
    "*.cmake": "cmake",
    "CMakeLists.txt": "cmake",
    "*.h": "cpp",
    "*.hpp": "cpp",
    "*.cpp": "cpp",
    "*.cc": "cpp",
    "*.cxx": "cpp",
    "*.c": "c"
  },

  // CMake Tools Configuration
  "cmake.configureOnOpen": true,
  "cmake.sourceDirectory": "${workspaceFolder}",
  "cmake.buildDirectory": "${workspaceFolder}/build",
  "cmake.generator": "Ninja",
  "cmake.defaultVariants": {
    "buildType": {
      "default": "debug",
      "description": "The build type",
      "choices": {
        "debug": {
          "short": "Debug",
          "long": "Emit debug information",
          "buildType": "Debug"
        },
        "release": {
          "short": "Release",
          "long": "Optimize for speed",
          "buildType": "Release"
        }
      }
    }
  }
}
```

### Clang-Format Integration

The project uses `.clang-format` for code formatting. VSCode automatically uses this file when `C_Cpp.clang_format_style` is set to `"file"`.

**Verify clang-format is working:**

1. Open a C++ file
2. Press `Shift+Alt+F` to format
3. Verify code follows project style

**Manual format check:**

```bash
# Check formatting without modifying files
clang-format --dry-run --Werror src/*.cpp

# Format files
clang-format -i src/*.cpp include/*.hpp
```

---

## VSCode CMake Tools Configuration

### CMake Presets Integration

The project uses CMake Presets defined in [`CMakePresets.json`](../CMakePresets.json:1). CMake Tools automatically detects and uses these presets.

#### Available Presets

| Preset | Compiler | Build Type | Description |
|---------|-----------|-------------|-------------|
| `linux-gcc-debug` | GCC 13 | Debug | GCC debug build with symbols |
| `linux-gcc-release` | GCC 13 | Release | GCC release build with optimizations |
| `linux-clang-debug` | Clang 19 | Debug | Clang debug build with symbols |
| `linux-clang-release` | Clang 19 | Release | Clang release build with optimizations |
| `cachyos-gcc-debug` | GCC 13 | Debug | CachyOS GCC debug build |
| `cachyos-gcc-release` | GCC 13 | Release | CachyOS GCC release build with optimizations |
| `cachyos-clang-debug` | Clang 19 | Debug | CachyOS Clang debug build |
| `cachyos-clang-release` | Clang 19 | Release | CachyOS Clang release build with optimizations |

### Selecting CMake Preset

1. **Command Palette:** `Ctrl+Shift+P`
2. **Type:** "CMake: Select a Kit"
3. **Select:** Your preferred compiler (GCC or Clang)
4. **Command Palette:** `Ctrl+Shift+P`
5. **Type:** "CMake: Select Variant"
6. **Select:** Debug or Release

### Building with CMake Tools

#### Quick Build

1. **Status Bar:** Click "Build" button in status bar
2. **Or:** `Ctrl+Shift+B` to open build tasks
3. **Select:** "Build Engine (Linux GCC - Debug)" or similar

#### Building Specific Targets

1. **Command Palette:** `Ctrl+Shift+P`
2. **Type:** "CMake: Build Target"
3. **Select:** Target (engine, game, standalone, tests)

### CMake Tools Status Bar

The CMake Tools status bar shows:

- **Kit:** Currently selected compiler
- **Variant:** Debug or Release
- **Build:** Current build status
- **Debug:** Debugging status
- **Tests:** Test status

### CMake Cache Variables

View and edit CMake cache variables:

1. **Command Palette:** `Ctrl+Shift+P`
2. **Type:** "CMake: Edit Cache"
3. **Edit:** Variables in JSON editor
4. **Save:** Changes take effect on next configure

---

## VSCode Build Tasks

### Task Configuration

The project includes [`.vscode/tasks.json`](../.vscode/tasks.json:1) with comprehensive build tasks for Linux.

### Available Linux Build Tasks

#### Configure Tasks

| Task | Description | Command |
|-------|-------------|----------|
| Configure Build (Linux GCC - Debug) | Configure GCC debug build | `python OmniCppController.py configure --compiler gcc --build-type Debug` |
| Configure Build (Linux GCC - Release) | Configure GCC release build | `python OmniCppController.py configure --compiler gcc --build-type Release` |
| Configure Build (Linux Clang - Debug) | Configure Clang debug build | `python OmniCppController.py configure --compiler clang --build-type Debug` |
| Configure Build (Linux Clang - Release) | Configure Clang release build | `python OmniCppController.py configure --compiler clang --build-type Release` |

#### Build Tasks

| Task | Description | Command |
|-------|-------------|----------|
| Build Engine (Linux GCC - Debug) | Build engine with GCC debug | `python OmniCppController.py build engine default linux-gcc-debug debug` |
| Build Engine (Linux GCC - Release) | Build engine with GCC release | `python OmniCppController.py build engine default linux-gcc-release release` |
| Build Engine (Linux Clang - Debug) | Build engine with Clang debug | `python OmniCppController.py build engine default linux-clang-debug debug` |
| Build Engine (Linux Clang - Release) | Build engine with Clang release | `python OmniCppController.py build engine default linux-clang-release release` |
| Build Game (Linux GCC - Debug) | Build game with GCC debug | `python OmniCppController.py build game default linux-gcc-debug debug` |
| Build Game (Linux GCC - Release) | Build game with GCC release | `python OmniCppController.py build game default linux-gcc-release release` |
| Build Game (Linux Clang - Debug) | Build game with Clang debug | `python OmniCppController.py build game default linux-clang-debug debug` |
| Build Game (Linux Clang - Release) | Build game with Clang release | `python OmniCppController.py build game default linux-clang-release release` |

#### Utility Tasks

| Task | Description | Command |
|-------|-------------|----------|
| Clean Build | Clean build artifacts | `python OmniCppController.py clean` |
| Clean All | Clean all artifacts including dependencies | `python OmniCppController.py clean --all` |
| Format Code | Format code with clang-format | `python OmniCppController.py format --fix` |
| Format Check | Check code formatting | `python OmniCppController.py format --check` |
| Lint Code | Run linters | `python OmniCppController.py lint` |
| Lint Fix | Lint and fix issues | `python OmniCppController.py lint --fix` |
| Run Tests | Execute unit tests | `python OmniCppController.py test` |

### Running Build Tasks

#### From Command Palette

1. **Open Command Palette:** `Ctrl+Shift+P`
2. **Type:** "Tasks: Run Task"
3. **Select:** Task from list
4. **View Output:** Tasks output in terminal panel

#### From Keyboard Shortcut

1. **Press:** `Ctrl+Shift+B`
2. **Select:** Task from list
3. **View Output:** Tasks output in terminal panel

#### From Status Bar

1. **Click:** Build icon in status bar
2. **Select:** Task from dropdown
3. **View Output:** Tasks output in terminal panel

### Custom Build Tasks

Create custom build tasks in `.vscode/tasks.json`:

```json
{
  "label": "Build Custom Target",
  "type": "shell",
  "command": "python",
  "args": [
    "OmniCppController.py",
    "build",
    "engine",
    "default",
    "linux-gcc-debug",
    "debug"
  ],
  "options": {
    "cwd": "${workspaceFolder}"
  },
  "problemMatcher": "$gcc",
  "group": {
    "kind": "build",
    "isDefault": false
  },
  "detail": "Build custom target with GCC debug"
}
```

---

## VSCode Debug Configurations

### Launch Configuration

The project includes [`.vscode/launch.json`](../.vscode/launch.json:1) with debug configurations for Linux.

### Available Debug Configurations

#### GDB Configurations

| Configuration | Type | Debugger | Program |
|--------------|--------|-----------|----------|
| Debug Engine (Linux GCC - Debug) | cppdbg | GDB | `${workspaceFolder}/build/bin/Debug/OmniCppEngine` |
| Debug Engine (Linux GCC - Release) | cppdbg | GDB | `${workspaceFolder}/build/bin/Release/OmniCppEngine` |
| Debug Game (Linux GCC - Debug) | cppdbg | GDB | `${workspaceFolder}/build/bin/Debug/OmniCppGame` |
| Debug Game (Linux GCC - Release) | cppdbg | GDB | `${workspaceFolder}/build/bin/Release/OmniCppGame` |
| Debug Standalone (Linux GCC - Debug) | cppdbg | GDB | `${workspaceFolder}/build/bin/Debug/OmniCppStandalone` |
| Debug Standalone (Linux GCC - Release) | cppdbg | GDB | `${workspaceFolder}/build/bin/Release/OmniCppStandalone` |

#### LLDB Configurations

| Configuration | Type | Debugger | Program |
|--------------|--------|-----------|----------|
| Debug Engine (Linux Clang - Debug) | cppdbg | LLDB | `${workspaceFolder}/build/bin/Debug/OmniCppEngine` |
| Debug Engine (Linux Clang - Release) | cppdbg | LLDB | `${workspaceFolder}/build/bin/Release/OmniCppEngine` |
| Debug Game (Linux Clang - Debug) | cppdbg | LLDB | `${workspaceFolder}/build/bin/Debug/OmniCppGame` |
| Debug Game (Linux Clang - Release) | cppdbg | LLDB | `${workspaceFolder}/build/bin/Release/OmniCppGame` |
| Debug Standalone (Linux Clang - Debug) | cppdbg | LLDB | `${workspaceFolder}/build/bin/Debug/OmniCppStandalone` |
| Debug Standalone (Linux Clang - Release) | cppdbg | LLDB | `${workspaceFolder}/build/bin/Release/OmniCppStandalone` |

### Starting Debug Session

#### From Run and Debug View

1. **Open Run and Debug View:** `Ctrl+Shift+D`
2. **Select:** Configuration from dropdown
3. **Click:** Green play button or press `F5`
4. **Set Breakpoints:** Click in editor gutter or press `F9`

#### From Command Palette

1. **Open Command Palette:** `Ctrl+Shift+P`
2. **Type:** "Debug: Start Debugging"
3. **Select:** Configuration from list

#### From Status Bar

1. **Click:** Debug icon in status bar
2. **Select:** Configuration from dropdown
3. **Start:** Debug session

### Debugging Features

#### Breakpoints

- **Set Breakpoint:** Click in editor gutter or press `F9`
- **Conditional Breakpoint:** Right-click breakpoint, select "Edit Breakpoint"
- **Logpoint:** Right-click breakpoint, select "Add Logpoint"
- **Disable Breakpoint:** Click breakpoint icon
- **Remove Breakpoint:** Right-click breakpoint, select "Remove Breakpoint"

#### Stepping

- **Step Over:** `F10` - Execute current line
- **Step Into:** `F11` - Step into function
- **Step Out:** `Shift+F11` - Step out of function
- **Continue:** `F5` - Continue execution
- **Pause:** `Pause` button - Pause execution

#### Variables and Watches

- **Variables View:** Automatically shows local variables
- **Watch View:** Add expressions to watch
- **Call Stack:** View function call stack
- **Hover:** Hover over variables to see values

#### Memory Inspection

- **Memory View:** View raw memory at addresses
- **Disassembly View:** View assembly code
- **Registers View:** View CPU registers

### GDB Configuration

GDB debug configurations include these setup commands:

```json
{
  "setupCommands": [
    {
      "description": "Enable pretty-printing for gdb",
      "text": "-enable-pretty-printing",
      "ignoreFailures": true
    },
    {
      "description": "Set Disassembly Flavor to Intel",
      "text": "-gdb-set disassembly-flavor intel",
      "ignoreFailures": true
    },
    {
      "description": "Set Python Path for GDB",
      "text": "-gdb-set python print-stack full",
      "ignoreFailures": true
    }
  ]
}
```

### LLDB Configuration

LLDB debug configurations include these setup commands:

```json
{
  "setupCommands": [
    {
      "description": "Enable pretty-printing for lldb",
      "text": "-enable-pretty-printing",
      "ignoreFailures": true
    }
  ]
}
```

---

## VSCode Nix Shell Integration

### Loading Nix Shell in VSCode

The project supports Nix development environments via [`flake.nix`](../flake.nix:1). VSCode can use these environments for consistent development.

#### Method 1: Nix Shell Task

The project includes a "Load Nix Shell" task in [`.vscode/tasks.json`](../.vscode/tasks.json:1):

```json
{
  "label": "Load Nix Shell",
  "type": "shell",
  "command": "nix",
  "args": ["develop"],
  "options": {
    "cwd": "${workspaceFolder}"
  },
  "problemMatcher": [],
  "group": {
    "kind": "build",
    "isDefault": false
  },
  "detail": "Load Nix development environment",
  "presentation": {
    "reveal": "always",
    "panel": "new"
  }
}
```

**Usage:**

1. **Open Command Palette:** `Ctrl+Shift+P`
2. **Type:** "Tasks: Run Task"
3. **Select:** "Load Nix Shell"
4. **Terminal:** Opens in Nix environment

#### Method 2: Direnv Integration

You can add an `.envrc` for automatic environment loading with direnv:

```bash
# .envrc content
use flake
```

**Setting up direnv:**

```bash
# Install direnv
sudo pacman -S direnv  # Arch/CachyOS
sudo apt install direnv  # Ubuntu/Debian
sudo dnf install direnv  # Fedora

# Add direnv hook to shell
echo 'eval "$(direnv hook bash)"' >> ~/.bashrc
source ~/.bashrc

# Allow .envrc in project directory
direnv allow
```

**Usage:**

- **Automatic:** Nix environment loads when you open project in VSCode
- **Terminal:** VSCode integrated terminal automatically has Nix environment
- **Status:** Direnv status shows in terminal prompt

#### Method 3: VSCode Nix Extension

Install the Nix extension for VSCode:

```bash
code --install-extension arrterian.nix-ide
```

This extension provides:

- **Nix Syntax Highlighting:** For `.nix` files
- **Nix Language Server:** IntelliSense for Nix
- **Flake Support:** Better flake.nix editing
- **Nix Shell Integration:** Quick access to Nix environments

### Nix Environment Variables in VSCode

When using Nix shell, these environment variables are available:

```bash
# Qt6 configuration
export QT_QPA_PLATFORM=wayland
export QT_PLUGIN_PATH=/nix/store/.../lib/qt-6/plugins

# Vulkan configuration
export VK_LAYER_PATH=/nix/store/.../share/vulkan/explicit_layer.d
export VK_ICD_FILENAMES=/nix/store/.../share/vulkan/icd.d/intel_icd.x86_64.json

# CMake configuration
export CMAKE_GENERATOR="Ninja"
export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)
```

### Building with Nix in VSCode

Once Nix shell is loaded:

1. **Select CMake Kit:** Choose "GCC 13" or "Clang 19" from Nix environment
2. **Configure:** `Ctrl+Shift+P` → "CMake: Configure"
3. **Build:** `Ctrl+Shift+B` → Select build task
4. **Debug:** `F5` → Select debug configuration

All tools (CMake, Ninja, compilers) come from Nix environment, ensuring reproducibility.

---

## VSCode CachyOS Setup

### CachyOS-Specific Configurations

CachyOS is an Arch Linux derivative with performance optimizations. The project includes CachyOS-specific VSCode configurations.

### CachyOS Build Tasks

The project includes CachyOS-optimized build tasks:

| Task | Description | Command |
|-------|-------------|----------|
| Configure Build (CachyOS GCC - Debug) | Configure CachyOS GCC debug | `python OmniCppController.py configure --compiler gcc --build-type Debug --cachyos` |
| Configure Build (CachyOS GCC - Release) | Configure CachyOS GCC release | `python OmniCppController.py configure --compiler gcc --build-type Release --cachyos` |
| Configure Build (CachyOS Clang - Debug) | Configure CachyOS Clang debug | `python OmniCppController.py configure --compiler clang --build-type Debug --cachyos` |
| Configure Build (CachyOS Clang - Release) | Configure CachyOS Clang release | `python OmniCppController.py configure --compiler clang --build-type Release --cachyos` |

### CachyOS Compiler Flags

CachyOS builds use optimized compiler flags:

```bash
# CachyOS-specific optimization flags
export CFLAGS="-march=native -O3 -flto -DNDEBUG"
export CXXFLAGS="-march=native -O3 -flto -DNDEBUG"

# CachyOS security hardening flags (addresses TM-LX-001)
export CFLAGS="$CFLAGS -fstack-protector-strong -D_FORTIFY_SOURCE=2"
export CXXFLAGS="$CXXFLAGS -fstack-protector-strong -D_FORTIFY_SOURCE=2"

# Linker flags for security
export LDFLAGS="-Wl,--as-needed -Wl,--no-undefined"
```

### CachyOS Debug Configurations

The project includes CachyOS-specific debug configurations:

```json
{
  "name": "Debug Engine (CachyOS GCC - Debug)",
  "type": "cppdbg",
  "request": "launch",
  "program": "${workspaceFolder}/build/bin/Debug/OmniCppEngine",
  "args": [],
  "stopAtEntry": false,
  "cwd": "${workspaceFolder}",
  "environment": [
    {
      "name": "CFLAGS",
      "value": "-march=native -O3 -flto -DNDEBUG"
    },
    {
      "name": "CXXFLAGS",
      "value": "-march=native -O3 -flto -DNDEBUG"
    }
  ],
  "externalConsole": false,
  "MIMode": "gdb",
  "miDebuggerPath": "/usr/bin/gdb",
  "setupCommands": [
    {
      "description": "Enable pretty-printing for gdb",
      "text": "-enable-pretty-printing",
      "ignoreFailures": true
    },
    {
      "description": "Set Disassembly Flavor to Intel",
      "text": "-gdb-set disassembly-flavor intel",
      "ignoreFailures": true
    }
  ],
  "preLaunchTask": "Build Engine (CachyOS GCC - Debug)"
}
```

### CachyOS Performance Profiling

CachyOS builds support performance profiling with `perf`:

```json
{
  "name": "Profile Engine (CachyOS)",
  "type": "cppdbg",
  "request": "launch",
  "program": "/usr/bin/perf",
  "args": [
    "record",
    "--call-graph=dwarf",
    "${workspaceFolder}/build/bin/Release/OmniCppEngine"
  ],
  "cwd": "${workspaceFolder}",
  "externalConsole": false,
  "preLaunchTask": "Build Engine (CachyOS GCC - Release)"
}
```

### CachyOS Validation Task

The project includes a CachyOS validation task:

```json
{
  "label": "Validate CachyOS Environment",
  "type": "shell",
  "command": "python",
  "args": [
    "OmniCppController.py",
    "validate",
    "--platform",
    "linux"
  ],
  "options": {
    "cwd": "${workspaceFolder}"
  },
  "problemMatcher": [],
  "group": {
    "kind": "test",
    "isDefault": false
  },
  "detail": "Validate CachyOS build environment"
}
```

Run this task to verify CachyOS environment setup:

1. **Command Palette:** `Ctrl+Shift+P`
2. **Type:** "Tasks: Run Task"
3. **Select:** "Validate CachyOS Environment"

---

## VSCode Troubleshooting

### Common Issues

#### Issue 1: IntelliSense Not Working

**Symptoms:** No code completion, red squiggles on valid code

**Solutions:**

1. **Check compile_commands.json:**
   ```bash
   # Verify compile_commands.json exists
   ls -la build/compile_commands.json
   ```

2. **Regenerate compile_commands.json:**
   ```bash
   # Reconfigure CMake
   cmake --preset linux-gcc-debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
   ```

3. **Reload IntelliSense:**
   - Command Palette: `Ctrl+Shift+P`
   - Type: "C/C++: Reset IntelliSense Database"
   - Select: Reset

4. **Check C_Cpp configuration:**
   - Command Palette: `Ctrl+Shift+P`
   - Type: "C/C++: Select a Configuration Provider"
   - Select: "CMake Tools"

#### Issue 2: Build Task Not Found

**Symptoms:** "Task not found" error when running build task

**Solutions:**

1. **Verify tasks.json exists:**
   ```bash
   ls -la .vscode/tasks.json
   ```

2. **Check JSON syntax:**
   ```bash
   # Validate JSON
   python -m json.tool .vscode/tasks.json
   ```

3. **Reload VSCode:**
   - Command Palette: `Ctrl+Shift+P`
   - Type: "Developer: Reload Window"
   - Select: Reload

#### Issue 3: Debugger Not Starting

**Symptoms:** "Failed to launch debugger" error

**Solutions:**

1. **Verify GDB/LLDB installed:**
   ```bash
   which gdb
   which lldb
   ```

2. **Check debugger path in launch.json:**
   ```json
   "miDebuggerPath": "/usr/bin/gdb"  // Ensure correct path
   ```

3. **Check program path:**
   ```json
   "program": "${workspaceFolder}/build/bin/Debug/OmniCppEngine"  // Ensure executable exists
   ```

4. **Build debug target first:**
   - Run build task before debugging
   - Verify executable exists in build directory

#### Issue 4: CMake Not Finding Kit

**Symptoms:** "No CMake kits found" error

**Solutions:**

1. **Scan for kits:**
   - Command Palette: `Ctrl+Shift+P`
   - Type: "CMake: Scan for Kits"
   - Wait for scan to complete

2. **Manual kit selection:**
   - Command Palette: `Ctrl+Shift+P`
   - Type: "CMake: Select a Kit"
   - Select: GCC or Clang

3. **Check CMake installation:**
   ```bash
   cmake --version
   which cmake
   ```

#### Issue 5: Nix Shell Not Loading

**Symptoms:** "nix: command not found" error

**Solutions:**

1. **Verify Nix installed:**
   ```bash
   nix --version
   ```

2. **Check Nix environment:**
   ```bash
   # Check if Nix is in PATH
   echo $PATH | grep nix
   ```

3. **Reload shell:**
   ```bash
   # Log out and log back in
   # Or reload VSCode
   ```

4. **Enable flakes:**
   ```bash
   # Check flakes enabled
   cat ~/.config/nix/nix.conf | grep flakes
   ```

#### Issue 6: Format on Save Not Working

**Symptoms:** Code not formatted when saving

**Solutions:**

1. **Check clang-format installed:**
   ```bash
   which clang-format
   clang-format --version
   ```

2. **Verify .clang-format exists:**
   ```bash
   ls -la .clang-format
   ```

3. **Check VSCode settings:**
   ```json
   {
     "editor.formatOnSave": true,
     "C_Cpp.formatting": "clangFormat",
     "C_Cpp.clang_format_style": "file"
   }
   ```

4. **Manual format:**
   - Press `Shift+Alt+F`
   - Verify formatting works

### Debugging VSCode Issues

#### Enable Developer Tools

1. **Help Menu:** Help → Toggle Developer Tools
2. **Console:** Check for errors in console
3. **Network Tab:** Check for failed requests

#### Check Extension Logs

1. **Output Panel:** View → Output
2. **Select Extension:** Choose extension from dropdown
3. **View Logs:** Check for errors and warnings

#### VSCode Logs Location

Linux logs are in:

```
~/.config/Code/logs/
├── [timestamp]-renderer1.log
├── [timestamp]-main.log
├── [timestamp]-exthost1.log
└── [timestamp]-sharedprocess1.log
```

### Performance Issues

#### Slow IntelliSense

**Solutions:**

1. **Exclude directories:**
   ```json
   {
     "files.exclude": {
       "**/build": true,
       "**/.git": true,
       "**/node_modules": true
     }
   }
   ```

2. **Reduce IntelliSense mode:**
   ```json
   {
     "C_Cpp.intelliSenseEngine": "default"
   }
   ```

3. **Disable unused features:**
   ```json
   {
     "C_Cpp.autocomplete": "default",
     "C_Cpp.errorSquiggles": "enabled"
   }
   ```

#### Slow Builds

**Solutions:**

1. **Enable ccache:**
   ```bash
   # Install ccache
   sudo pacman -S ccache  # Arch/CachyOS
   sudo apt install ccache  # Ubuntu/Debian
   sudo dnf install ccache  # Fedora

   # Configure CMake to use ccache
   export CC="ccache gcc"
   export CXX="ccache g++"
   ```

2. **Increase parallel jobs:**
   ```json
   {
     "cmake.buildJobs": 8
   }
   ```

3. **Use Ninja:**
   - Ensure CMake generator is set to Ninja
   - Faster than Make for large projects

---

## VSCode Best Practices

### Workflow Recommendations

#### Daily Development Workflow

1. **Open Project:** Open folder in VSCode
2. **Load Environment:** Run "Load Nix Shell" task (if using Nix)
3. **Select Kit:** Choose CMake kit (GCC or Clang)
4. **Select Variant:** Choose Debug or Release
5. **Configure:** Run CMake configure task
6. **Build:** Run build task
7. **Develop:** Write code with IntelliSense
8. **Debug:** Set breakpoints and run debug configuration
9. **Test:** Run test task
10. **Format:** Format code before committing

#### Code Review Workflow

1. **Open PR:** Open pull request in VSCode
2. **Review Changes:** Use GitLens for diff view
3. **Comment:** Add inline comments
4. **Build Locally:** Build changes before approving
5. **Run Tests:** Execute tests to verify changes

### Keyboard Shortcuts

#### Essential Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+Shift+P` | Command Palette |
| `Ctrl+P` | Quick Open File |
| `Ctrl+Shift+N` | New Window |
| `Ctrl+Shift+W` | Close Window |
| `Ctrl+B` | Toggle Sidebar |
| `Ctrl+Shift+B` | Run Build Task |
| `F5` | Start Debugging |
| `Shift+F5` | Stop Debugging |
| `F9` | Toggle Breakpoint |
| `F10` | Step Over |
| `F11` | Step Into |
| `Shift+F11` | Step Out |
| `Ctrl+Shift+F` | Find in Files |
| `Ctrl+Shift+H` | Replace in Files |
| `Ctrl+`` ` | Toggle Terminal |
| `Ctrl+Shift+`` ` | Create New Terminal |
| `Ctrl+K Ctrl+S` | Save All |
| `Ctrl+K Z` | Zen Mode |
| `Ctrl+Shift+M` | Show Problems |
| `Ctrl+Shift+X` | Show Extensions |

#### Custom Shortcuts

Add custom shortcuts in `keybindings.json`:

```json
[
  {
    "key": "ctrl+shift+r",
    "command": "workbench.action.tasks.runTask",
    "args": "Run Tests"
  },
  {
    "key": "ctrl+shift+f",
    "command": "editor.action.formatDocument",
    "when": "editorHasDocumentFormattingProvider && editorTextFocus && !editorReadonly"
  }
]
```

### Workspace Management

#### Multi-Root Workspaces

Open multiple projects in one workspace:

1. **File Menu:** File → Add Folder to Workspace
2. **Select:** Project directories
3. **Save:** File → Save Workspace As
4. **Open:** File → Open Workspace

#### Workspace Settings

Workspace-specific settings in `.vscode/settings.json` override user settings:

```json
{
  "C_Cpp.default.compilerPath": "/usr/bin/gcc",
  "cmake.configureOnOpen": true
}
```

### Git Integration

#### Git Source Control

VSCode provides integrated Git support:

- **Changes View:** View staged and unstaged changes
- **Diff View:** Compare changes side-by-side
- **Blame View:** View line-by-line blame with GitLens
- **Branch Management:** Create, checkout, and merge branches
- **Stash:** Save work in progress

#### Git Commands

| Command | Action |
|----------|--------|
| `git init` | Initialize repository |
| `git clone` | Clone repository |
| `git add` | Stage changes |
| `git commit` | Commit changes |
| `git push` | Push to remote |
| `git pull` | Pull from remote |
| `git branch` | List branches |
| `git checkout` | Switch branches |
| `git merge` | Merge branches |

### Code Quality

#### Linting

The project uses clang-tidy for static analysis:

```bash
# Run clang-tidy
python OmniCppController.py lint

# Fix issues automatically
python OmniCppController.py lint --fix
```

#### Formatting

The project uses clang-format for code formatting:

```bash
# Format all files
python OmniCppController.py format --fix

# Check formatting
python OmniCppController.py format --check
```

#### Pre-commit Hooks

Configure pre-commit hooks to automatically check code:

```bash
# Install pre-commit
pip install pre-commit

# Install hooks
pre-commit install

# Run hooks manually
pre-commit run --all-files
```

### Security Considerations

#### VSCode Security Settings

Configure VSCode security settings in `settings.json`:

```json
{
  // Security settings
  "security.workspace.trust.enabled": true,
  "security.workspace.trust.banner": "always",
  "security.workspace.trust.untrustedFiles": "open",
  
  // Disable telemetry (optional)
  "telemetry.enableTelemetry": false,
  
  // Disable crash reporting (optional)
  "telemetry.enableCrashReporter": false
}
```

#### Environment Variable Protection

Avoid exposing sensitive environment variables in VSCode:

```json
{
  "terminal.integrated.env.linux": {
    "PATH": "${env:PATH}"
  }
}
```

#### Task Security

Validate task configurations to prevent injection attacks (addresses TM-LX-005):

```json
{
  "tasks": [
    {
      "label": "Secure Build Task",
      "type": "shell",
      "command": "python",
      "args": [
        "OmniCppController.py",
        "build"
      ],
      "options": {
        "cwd": "${workspaceFolder}",
        "env": {
          "PATH": "${env:PATH}"
        }
      }
    }
  ]
}
```

---

## Additional Resources

### Official Documentation

- [VSCode Documentation](https://code.visualstudio.com/docs)
- [C/C++ Extension Documentation](https://code.visualstudio.com/docs/cpp/cpp-ide-debugging)
- [CMake Tools Documentation](https://code.visualstudio.com/docs/cmake/cpp-cmake-tools)
- [Python Extension Documentation](https://code.visualstudio.com/docs/python/python-tutorial)

### Project Documentation

- [Linux Builds Guide](linux-builds.md)
- [Nix Development Guide](nix-development.md)
- [CachyOS Builds Guide](cachyos-builds.md)
- [Troubleshooting Guide](troubleshooting-guide.md)

### Related ADRs

- [ADR-026: VSCode tasks and launch configuration](../.specs/02_adrs/ADR-026-vscode-tasks-launch-configuration.md)
- [ADR-032: VSCode Platform-Specific Tasks](../.specs/02_adrs/ADR-032-vscode-platform-specific-tasks.md)

### Related Requirements

- [REQ-003: VSCode Configuration](../.specs/04_future_state/reqs/REQ-003-vscode-configuration.md)
- [REQ-048: VSCode tasks.json Configuration](../.specs/04_future_state/reqs/REQ-048-vscode-tasks-configuration.md)
- [REQ-049: VSCode launch.json Configuration](../.specs/04_future_state/reqs/REQ-049-vscode-launch-configuration.md)

---

## Summary

This guide provides comprehensive VSCode setup for Linux C++ development with the OmniCPP Template project. Key points:

- **Installation:** Install VSCode and required extensions
- **Configuration:** Configure C/C++ IntelliSense and CMake Tools
- **Build Tasks:** Use predefined tasks for building with GCC and Clang
- **Debugging:** Use GDB and LLDB configurations for debugging
- **Nix Integration:** Load Nix shell for reproducible builds
- **CachyOS Setup:** Use CachyOS-optimized configurations
- **Troubleshooting:** Resolve common VSCode issues
- **Best Practices:** Follow recommended workflows and shortcuts

For additional help, refer to the project documentation or open an issue on GitHub.
