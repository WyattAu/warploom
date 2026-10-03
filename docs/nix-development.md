# Nix Development Environment

> **Note**
> Parts of this page describe the pre-0.1 Conan and direnv setup, which has
> been retired. The engine's only third-party dependency is GoogleTest,
> fetched by CPM; `conan/` no longer exists. See the README for the current
> build path.


**Version:** 1.0.0
**Last Updated:** 2026-01-28
**Related ADRs:** [ADR-027: Nix Package Manager Integration](../.specs/02_adrs/ADR-027-nix-package-manager-integration.md)
**Related Requirements:** [REQ-006-001: Create nix-development.md](../.specs/04_future_state/reqs/REQ-006-documentation.md)

---

## Overview

This document describes how to set up and use the Nix development environment for the OmniCPP Template project on Linux. Nix provides reproducible, declarative development environments that ensure all developers have identical toolchains and dependencies.

### What is Nix?

Nix is a purely functional package manager that provides:

- **Reproducibility:** Exact same environment across all developer machines
- **Declarative Configuration:** All dependencies specified in a single [`flake.nix`](../flake.nix:1) file
- **Isolation:** No interference with system packages
- **Version Pinning:** [`flake.lock`](../flake.lock:1) ensures exact dependency versions
- **Rollback:** Ability to revert to previous environment versions

### Why Use Nix?

Traditional Linux package management has several limitations:

1. **Environment Drift:** System package updates can break build environments
2. **Reproducibility Issues:** Different developers may have different toolchain versions
3. **Dependency Conflicts:** System packages may conflict with project dependencies
4. **Setup Complexity:** Developers must manually install and configure multiple tools
5. **Version Pinning Difficulty:** Hard to ensure exact versions across all dependencies

Nix addresses all these issues by providing a completely isolated, reproducible development environment.

### Project Nix Configuration

The OmniCPP Template provides multiple Nix development shells:

- **default:** Full environment with GCC 13, Clang 19, Qt6, Vulkan, and all tools
- **gcc:** GCC 13-focused environment
- **clang:** Clang 19-focused environment
- **cachyos-gcc:** CachyOS-optimized GCC 13 environment
- **cachyos-clang:** CachyOS-optimized Clang 19 environment

---

## Prerequisites

Before setting up Nix, ensure you have:

- **Linux System:** Preferably CachyOS or Arch Linux derivative
- **Git:** For cloning the repository
- **Internet Connection:** For downloading Nix packages
- **Disk Space:** At least 10 GB free space for Nix store
- **Basic Shell Knowledge:** Familiarity with bash shell

### Supported Platforms

- **Primary:** x86_64-linux (CachyOS, Arch Linux)
- **Secondary:** Other Linux distributions (may require additional configuration)
- **Not Supported:** Windows (use WSL2), macOS (not tested)

---

## Installation

### Installing Nix

The project provides an automated setup script at [`scripts/linux/setup_nix.sh`](../scripts/linux/setup_nix.sh:1) that handles Nix installation and configuration.

#### Automated Installation

```bash
# Run the setup script
./scripts/linux/setup_nix.sh
```

The script supports several options:

```bash
# Preview changes without making them
./scripts/linux/setup_nix.sh --dry-run

# Force Nix installation even if already installed
./scripts/linux/setup_nix.sh --install

# Skip flakes configuration
./scripts/linux/setup_nix.sh --no-flakes

# Install in multi-user mode (requires sudo)
./scripts/linux/setup_nix.sh --multi-user

# Enable debug output
./scripts/linux/setup_nix.sh --debug
```

#### Manual Installation

If you prefer manual installation, follow these steps:

1. **Download and Run Nix Installer:**

```bash
# Single-user mode (no sudo required)
curl -L https://nixos.org/nix/install | sh

# Multi-user mode (requires sudo)
curl -L https://nixos.org/nix/install | sh -s -- --daemon
```

2. **Log Out and Log Back In:**

Nix requires you to log out and log back in to load the environment variables.

3. **Verify Installation:**

```bash
nix --version
# Expected output: nix (Nix) 2.18.0 or later
```

### Minimum Nix Version

The project requires Nix 2.4.0 or later. The setup script automatically validates the version:

```bash
# Check Nix version
nix --version

# Compare against minimum required (2.4.0)
```

### Enabling Nix Flakes

Nix flakes are an experimental feature that must be enabled:

```bash
# Create Nix configuration directory
mkdir -p ~/.config/nix

# Enable flakes in nix.conf
echo "experimental-features = nix-command flakes" >> ~/.config/nix/nix.conf
```

The setup script automatically enables flakes for you.

---

## Configuration

### flake.nix Structure

The [`flake.nix`](../flake.nix:1) file defines all development dependencies and configurations. Here's the structure:

```nix
{
  description = "OmniCPP C++ Development Environment for CachyOS with Qt6, Vulkan, GCC 13, and Clang 19";

  inputs = {
    # Use nixos-unstable for latest packages (pinned via flake.lock for security)
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      # System architecture targeting CachyOS (Arch Linux derivative)
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      # Development shells - provide reproducible environments
      devShells.${system}.default = pkgs.mkShell {
        # ... configuration
      };
    };
}
```

### Available Development Shells

#### Default Shell

The default shell includes all toolchains and dependencies:

- **Compilers:** GCC 13, Clang 19
- **Build System:** CMake, Ninja, ccache
- **Package Managers:** Conan, Python with pip
- **Graphics:** Qt6 (qtbase, qtdeclarative, qttools, qtwayland, qtsvg, qtimageformats)
- **Vulkan:** Headers, loader, tools, validation layers, extension layer
- **Shader Tools:** glslang, shaderc, spirv-tools
- **Development Tools:** clang-tools, doxygen, graphviz
- **Debugging:** GDB, LLDB, Valgrind
- **Performance:** perf

#### GCC Shell

GCC-focused environment with GCC 13 and essential tools:

```bash
nix develop .#gcc
```

#### Clang Shell

Clang-focused environment with Clang 19 and essential tools:

```bash
nix develop .#clang
```

#### CachyOS GCC Shell

CachyOS-optimized GCC 13 environment with performance flags:

```bash
nix develop .#cachyos-gcc
```

This shell includes CachyOS-specific optimizations:

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

#### CachyOS Clang Shell

CachyOS-optimized Clang 19 environment with performance flags:

```bash
nix develop .#cachyos-clang
```

### flake.lock Usage

The [`flake.lock`](../flake.lock:1) file pins exact versions of all dependencies, ensuring reproducibility:

```bash
# Update flake.lock to latest versions
nix flake update

# Revert to specific lockfile commit
nix flake update --commit <commit-hash>
```

**Security Note:** Always commit [`flake.lock`](../flake.lock:1) to version control to ensure all developers use the same dependency versions. This addresses TM-LX-001: Nix Package Manager Security Risks.

### Environment Variables

Each Nix shell sets up the following environment variables:

#### Qt6 Configuration

```bash
# Qt6 platform configuration
export QT_QPA_PLATFORM=wayland
export QT_PLUGIN_PATH=/nix/store/.../lib/qt-6/plugins
export QMAKE=/nix/store/.../bin/qmake
```

#### Vulkan Configuration

```bash
# Vulkan validation layers
export VK_LAYER_PATH=/nix/store/.../share/vulkan/explicit_layer.d

# Vulkan ICD files (Intel and AMD)
export VK_ICD_FILENAMES=/nix/store/.../share/vulkan/icd.d/intel_icd.x86_64.json:/nix/store/.../share/vulkan/icd.d/radeon_icd.x86_64.json
```

#### CMake Configuration

```bash
# Use Ninja generator
export CMAKE_GENERATOR="Ninja"

# Parallel builds
export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)

# Add Nix paths to CMake prefix
export CMAKE_PREFIX_PATH="/nix/store/.../qtbase:/nix/store/.../vulkan-loader:$CMAKE_PREFIX_PATH"

# Generate compile_commands.json for IDE support
export CMAKE_EXPORT_COMPILE_COMMANDS=ON
```

#### Compiler Configuration

```bash
# Default to Clang for better diagnostics (in default shell)
export CC=clang
export CXX=clang++
export CMAKE_C_COMPILER=clang
export CMAKE_CXX_COMPILER=clang++
```

#### Compiler Cache

```bash
# ccache directory
export CCACHE_DIR=$PWD/.ccache
```

### Direnv Integration

You can add an `.envrc` for automatic environment loading with direnv:

```bash
# .envrc content
use flake
```

#### Setting Up Direnv

```bash
# Install direnv (Arch Linux/CachyOS)
sudo pacman -S direnv

# Install direnv hook for your shell
# For bash
echo 'eval "$(direnv hook bash)"' >> ~/.bashrc

# For zsh
echo 'eval "$(direnv hook zsh)"' >> ~/.zshrc

# Reload shell configuration
source ~/.bashrc  # or ~/.zshrc

# Allow .envrc in project directory
direnv allow
```

Direnv automatically loads the Nix environment when you enter the project directory and unloads it when you leave.

---

## Usage

### Loading Nix Shell

#### Default Shell

```bash
# Enter default Nix development environment
nix develop

# Or explicitly specify default shell
nix develop .#default
```

You should see output like:

```
>> Loaded OmniCPP C++ Development Environment (CachyOS)
>> GCC 13 and Clang 19 toolchains available
>> Qt6 and Vulkan graphics libraries loaded
>> Conan package manager and Python environment ready
```

#### Specific Toolchain Shells

```bash
# GCC 13 environment
nix develop .#gcc

# Clang 19 environment
nix develop .#clang

# CachyOS GCC 13 environment
nix develop .#cachyos-gcc

# CachyOS Clang 19 environment
nix develop .#cachyos-clang
```

#### Running Commands in Nix Shell

You can run single commands without entering the shell:

```bash
# Run a command in Nix environment
nix develop --command cmake --version

# Build the project
nix develop --command cmake --preset=linux-clang-debug
nix develop --command cmake --build --preset=linux-clang-debug

# Run tests
nix develop --command ctest --preset=linux-clang-debug
```

### Building the Project

Once in the Nix shell, use CMake to build the project:

```bash
# Configure build
cmake --preset=linux-clang-debug

# Build
cmake --build --preset=linux-clang-debug

# Run tests
ctest --preset=linux-clang-debug
```

### Using Conan in Nix Shell

Conan is pre-installed in the Nix shell:

```bash
# Install Conan dependencies
conan install . --build=missing --profile=conan/profiles/clang-linux

# Build with Conan toolchain
cmake --preset=linux-clang-debug -DCMAKE_TOOLCHAIN_FILE=conan/conan_toolchain.cmake
```

### Using Python in Nix Shell

Python 3 with pip is available in the Nix shell:

```bash
# Check Python version
python --version

# Install Python packages
pip install -r requirements.txt

# Run Python scripts
python OmniCppController.py --help
```

### Checking Available Tools

You can check which tools are available in the Nix shell:

```bash
# Check compiler versions
gcc --version
clang --version

# Check CMake version
cmake --version
ninja --version

# Check Qt6 version
qmake --version

# Check Vulkan tools
vulkaninfo --version
glslang --version
```

---

## Nix Package Management

### Understanding Nix Store

Nix stores all packages in the `/nix/store` directory with unique hash-based paths:

```
/nix/store/<hash>-<package-name>-<version>
```

This ensures:

- **No conflicts:** Multiple versions of the same package can coexist
- **Garbage collection:** Unused packages can be safely removed
- **Reproducibility:** Same hash always produces the same package

### Adding Dependencies

To add new dependencies to [`flake.nix`](../flake.nix:1):

1. **Find the Package:**

```bash
# Search for a package in nixpkgs
nix search nixpkgs <package-name>
```

2. **Add to buildInputs:**

```nix
devShells.${system}.default = pkgs.mkShell {
  buildInputs = with pkgs; [
    # Add new package here
    <new-package>

    # Existing packages
    gcc13
    clang
    # ...
  ];
};
```

3. **Update the Shell:**

```bash
# Exit current shell and re-enter
exit
nix develop
```

### Updating Dependencies

To update all dependencies to latest versions:

```bash
# Update flake.lock
nix flake update

# Enter updated shell
nix develop
```

To update specific inputs:

```bash
# Update specific input
nix flake lock --update-input nixpkgs
```

### Garbage Collection

Nix stores all packages even after they're no longer used. Clean up unused packages:

```bash
# Remove old generations
nix-collect-garbage -d

# Remove all but current generation
nix-collect-garbage

# Optimize nix store (deduplicate files)
nix-store --optimize
```

### Checking Disk Usage

Check how much space the Nix store is using:

```bash
# Check nix store size
du -sh /nix/store

# List largest packages
nix-store --query --requisites /nix/var/nix/profiles/default | \
  xargs -I {} du -sh {} | sort -hr | head -20
```

---

## Nix Integration with CMake

### CMake Toolchain File

The setup script creates a CMake toolchain file at [`cmake/toolchains/nix.cmake`](../cmake/toolchains/nix.cmake:1) for Nix environments:

```cmake
# CMake toolchain file for Nix on Linux
cmake_minimum_required(3.20)

# Detect compiler from Nix environment
if(DEFINED ENV{CC})
    set(CMAKE_C_COMPILER $ENV{CC})
endif()

if(DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER $ENV{CXX})
endif()

# Set compiler flags for optimization
set(CMAKE_C_FLAGS_INIT "-march=native")
set(CMAKE_CXX_FLAGS_INIT "-march=native")

# Enable position independent code
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler-specific flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -DNDEBUG")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
endif()

# Enable warnings
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# Nix-specific settings
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

### Using Nix with CMake Presets

The project includes CMake presets configured for Nix environments:

```bash
# Configure with Nix preset
cmake --preset=linux-clang-debug

# Build with Nix preset
cmake --build --preset=linux-clang-debug
```

### Finding Nix Packages in CMake

CMake automatically finds packages installed by Nix:

```cmake
# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# Find Vulkan
find_package(Vulkan REQUIRED)

# Find Conan packages
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
```

---

## Nix Integration with Conan

### Conan Profile for Nix

The retired setup script used to create a Conan profile at `conan/profiles/nix`:

```ini
# Conan profile for Nix on Linux

[settings]
os=Linux
arch=x86_64
compiler=gcc
compiler.version=13
compiler.libcxx=libstdc++11
build_type=Release

[conf]
# Nix provides tools in specific paths
tools.build:compiler_executables={"c": "gcc", "cpp": "g++"}

[buildenv]
# Use Nix-provided compilers
CC=gcc
CXX=g++
```

### Using Conan in Nix Shell

```bash
# Install Conan dependencies
conan install . --build=missing --profile=conan/profiles/nix

# Build with Conan toolchain
cmake --preset=linux-clang-debug \
  -DCMAKE_TOOLCHAIN_FILE=conan/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug
```

### Conan and Nix Compatibility

When using Conan with Nix:

1. **Use Conan for C++ libraries:** fmt, spdlog, nlohmann_json, etc.
2. **Use Nix for system tools:** compilers, CMake, Python, etc.
3. **Avoid conflicts:** Don't install the same package via both Conan and Nix

---

## Nix Troubleshooting

### Common Issues

#### Issue: Nix command not found

**Symptom:** `nix: command not found`

**Solution:**

```bash
# Check if Nix is installed
which nix

# If not found, re-source shell configuration
source ~/.bashrc  # or ~/.zshrc

# If still not found, reinstall Nix
curl -L https://nixos.org/nix/install | sh
```

#### Issue: Flakes not enabled

**Symptom:** `error: experimental feature 'flakes' is disabled`

**Solution:**

```bash
# Enable flakes in nix.conf
mkdir -p ~/.config/nix
echo "experimental-features = nix-command flakes" >> ~/.config/nix/nix.conf

# Reload shell
source ~/.bashrc  # or ~/.zshrc
```

#### Issue: flake.nix not found

**Symptom:** `error: flake 'git+file:///path/to/project' does not provide attribute 'devShells.x86_64-linux.default'`

**Solution:**

```bash
# Check if flake.nix exists in project root
ls -la flake.nix

# If missing, ensure you're in the correct directory
cd /path/to/project

# Check flake.nix syntax
nix flake check
```

#### Issue: Build fails with missing dependencies

**Symptom:** CMake cannot find Qt6 or Vulkan headers

**Solution:**

```bash
# Check if packages are installed in Nix shell
nix develop --command pkg-config --modversion Qt6Core

# Check Qt6 paths
echo $QT_PLUGIN_PATH
echo $CMAKE_PREFIX_PATH

# Rebuild Nix shell
nix develop --rebuild
```

#### Issue: Out of disk space

**Symptom:** `error: write: No space left on device`

**Solution:**

```bash
# Run garbage collection
nix-collect-garbage -d

# Optimize nix store
nix-store --optimize

# Check disk usage
df -h /nix
```

#### Issue: Slow builds

**Symptom:** First-time builds are very slow

**Solution:**

```bash
# Use ccache for compiler caching
export CCACHE_DIR=$PWD/.ccache

# Increase parallel builds
export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)

# Use binary cache (if available)
# Add to ~/.config/nix/nix.conf:
# substituters = https://cache.nixos.org https://your-cache.example.com
```

#### Issue: Vulkan validation layers not found

**Symptom:** Vulkan applications fail with validation layer errors

**Solution:**

```bash
# Check VK_LAYER_PATH
echo $VK_LAYER_PATH

# Manually set layer path
export VK_LAYER_PATH=/nix/store/.../share/vulkan/explicit_layer.d

# Verify layers are available
ls $VK_LAYER_PATH
```

#### Issue: Qt6 applications fail to start

**Symptom:** Qt6 applications crash or fail to display

**Solution:**

```bash
# Check Qt6 platform
echo $QT_QPA_PLATFORM

# Set to wayland or xcb
export QT_QPA_PLATFORM=wayland

# Check Qt6 plugin path
echo $QT_PLUGIN_PATH

# Verify plugins exist
ls $QT_PLUGIN_PATH
```

### Debugging Nix Issues

#### Enable Debug Output

```bash
# Enable Nix debug output
export NIX_DEBUG=1

# Run Nix command with verbose output
nix develop -vv
```

#### Check Nix Logs

```bash
# View Nix daemon logs
journalctl -u nix-daemon -f

# View recent Nix errors
journalctl -u nix-daemon --since "1 hour ago" | grep error
```

#### Verify Nix Store Integrity

```bash
# Verify Nix store
nix-store --verify --check-contents

# Repair corrupted store (requires sudo)
sudo nix-store --verify --check-contents --repair
```

#### Test Nix Shell

```bash
# Test basic shell
nix develop --command echo "Nix shell works"

# Test compiler
nix develop --command gcc --version

# Test CMake
nix develop --command cmake --version
```

---

## Nix Best Practices

### Security Best Practices

#### 1. Pin Exact Versions

Always commit [`flake.lock`](../flake.lock:1) to version control:

```bash
# Update lockfile
nix flake update

# Commit to git
git add flake.lock
git commit -m "Update Nix dependencies"
```

This addresses **TM-LX-001: Nix Package Manager Security Risks** by preventing dependency hijacking.

#### 2. Verify Flake Inputs

Only use trusted flake inputs:

```nix
inputs = {
  # Use official nixpkgs
  nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  # Avoid untrusted flake inputs
  # untrusted-flake.url = "github:unknown/repo";
};
```

#### 3. Use Binary Caches

Configure trusted binary caches:

```bash
# Add to ~/.config/nix/nix.conf
substituters = https://cache.nixos.org https://cache.nixos.cachyos.org
trusted-public-keys = cache.nixos.org-1:6NCHdD59X431o0gNypC9Uh5TS5CwU7vAz5bCjJd7nU8=
```

#### 4. Regular Security Updates

Keep Nix and dependencies updated:

```bash
# Update Nix
nix-channel --update nixpkgs

# Update flake.lock
nix flake update

# Review changes before committing
git diff flake.lock
```

### Development Best Practices

#### 1. Use Direnv for Automatic Loading

Set up direnv for automatic environment loading:

```bash
# Install direnv
sudo pacman -S direnv

# Hook into shell
echo 'eval "$(direnv hook bash)"' >> ~/.bashrc

# Allow .envrc
direnv allow
```

#### 2. Separate Shells for Different Tasks

Use specific shells for different tasks:

```bash
# Use default shell for full development
nix develop .#default

# Use gcc shell for GCC-specific testing
nix develop .#gcc

# Use clang shell for Clang-specific testing
nix develop .#clang
```

#### 3. Keep flake.nix Organized

Organize [`flake.nix`](../flake.nix:1) with clear sections:

```nix
{
  description = "...";

  inputs = {
    # Input declarations
  };

  outputs = { self, nixpkgs }:
    let
      # Local definitions
    in
    {
      # Development shells
      devShells.${system}.default = pkgs.mkShell {
        # Core compilers
        buildInputs = with pkgs; [
          gcc13
          llvmPackages_19.clang
        ];

        # Build system
        buildInputs = with pkgs; [
          cmake
          ninja
        ];

        # Graphics
        buildInputs = with pkgs; [
          qt6.qtbase
          vulkan-loader
        ];
      };
    };
}
```

#### 4. Document Custom Packages

Document any custom packages in [`flake.nix`](../flake.nix:1):

```nix
# Custom package: example-library
# Description: Example C++ library for demonstration
# Source: https://github.com/example/example-library
pkgs.stdenv.mkDerivation {
  pname = "example-library";
  version = "1.0.0";
  src = fetchFromGitHub {
    owner = "example";
    repo = "example-library";
    rev = "v1.0.0";
    sha256 = "sha256-...";
  };
  buildInputs = [ cmake ];
}
```

#### 5. Use Ccache for Faster Builds

Enable ccache in Nix shell:

```nix
shellHook = ''
  # Compiler cache configuration
  export CCACHE_DIR=$PWD/.ccache
  export CC="ccache gcc"
  export CXX="ccache g++"
'';
```

### Performance Best Practices

#### 1. Optimize Nix Store

Regularly optimize the Nix store:

```bash
# Deduplicate files
nix-store --optimize

# Remove old generations
nix-collect-garbage -d
```

#### 2. Use Binary Caches

Configure binary caches to avoid building from source:

```bash
# Add to ~/.config/nix/nix.conf
substituters = https://cache.nixos.org https://cache.nixos.cachyos.org
```

#### 3. Parallel Builds

Enable parallel builds:

```nix
shellHook = ''
  # Parallel builds
  export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)
'';
```

#### 4. Profile Build Performance

Profile Nix builds to identify bottlenecks:

```bash
# Profile Nix build
nix build --show-trace

# Profile CMake build
cmake --build . -- --verbose
```

### Collaboration Best Practices

#### 1. Commit flake.lock

Always commit [`flake.lock`](../flake.lock:1) to ensure reproducibility:

```bash
git add flake.nix flake.lock
git commit -m "Update Nix dependencies"
```

#### 2. Document Nix Changes

Document changes to Nix configuration in commit messages:

```bash
git commit -m "Add fmt library to Nix shell

- Added fmt/10.2.1 to buildInputs
- Updated flake.lock
- Tested with nix develop"
```

#### 3. Review Dependency Updates

Review dependency updates before merging:

```bash
# Check what changed
nix flake update
git diff flake.lock

# Test updated dependencies
nix develop
cmake --preset=linux-clang-debug
cmake --build --preset=linux-clang-debug
```

#### 4. Use CI/CD with Nix

Configure CI/CD to use Nix:

```yaml
# Example GitHub Actions workflow
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - uses: cachix/install-nix-action@v20
        with:
          nix_path: nixpkgs=channel:nixos-unstable
      - run: nix develop --command cmake --preset=linux-clang-debug
      - run: nix develop --command cmake --build --preset=linux-clang-debug
```

---

## References

### Official Documentation

- [Nix Manual](https://nixos.org/manual/nix/stable/)
- [Nix Flakes](https://nixos.wiki/wiki/Flakes)
- [Nix Pills](https://nixos.org/guides/nix-pills/)
- [NixOS Wiki](https://nixos.wiki/)

### Project Documentation

- [ADR-027: Nix Package Manager Integration](../.specs/02_adrs/ADR-027-nix-package-manager-integration.md)
- [REQ-006-001: Create nix-development.md](../.specs/04_future_state/reqs/REQ-006-documentation.md)
- [setup_nix.sh Script](../scripts/linux/setup_nix.sh:1)
- [Threat Model: TM-LX-001](../.specs/03_threat_model/analysis.md:498)

### Related Documentation

- [CachyOS Builds](cachyos-builds.md)
- [Linux Builds](linux-builds.md)
- [Conan Linux Profiles](conan-linux-profiles.md)
- [VSCode Linux Setup](vscode-linux-setup.md)

### External Resources

- [CachyOS Nix Guide](https://wiki.cachyos.org/nix)
- [Reproducible Builds with Nix](https://reproducible-builds.org/docs/nix/)
- [Direnv Documentation](https://direnv.net/)
- [Nixpkgs Manual](https://nixos.org/manual/nixpkgs/stable/)

---

## Appendix

### A. Nix Command Reference

#### Common Commands

```bash
# Enter Nix shell
nix develop

# Enter specific shell
nix develop .#gcc

# Run command in shell
nix develop --command <command>

# Update flake.lock
nix flake update

# Check flake
nix flake check

# Search for packages
nix search nixpkgs <package>

# Show package info
nix show nixpkgs#<package>

# Garbage collection
nix-collect-garbage -d

# Optimize store
nix-store --optimize
```

#### Flake Commands

```bash
# Initialize new flake
nix flake init

# Update specific input
nix flake lock --update-input nixpkgs

# Show flake metadata
nix flake metadata

# Show flake info
nix flake info
```

### B. Environment Variables

#### Nix-Specific Variables

```bash
# Nix configuration directory
export NIX_CONF_DIR=~/.config/nix

# Nix state directory
export NIX_STATE_DIR=/nix/var/nix

# Nix store directory
export NIX_STORE_DIR=/nix/store

# Nix log directory
export NIX_LOG_DIR=/nix/var/log/nix
```

#### Project-Specific Variables (Set by Nix Shell)

```bash
# Qt6
export QT_QPA_PLATFORM=wayland
export QT_PLUGIN_PATH=/nix/store/.../lib/qt-6/plugins
export QMAKE=/nix/store/.../bin/qmake

# Vulkan
export VK_LAYER_PATH=/nix/store/.../share/vulkan/explicit_layer.d
export VK_ICD_FILENAMES=/nix/store/.../share/vulkan/icd.d/intel_icd.x86_64.json

# CMake
export CMAKE_GENERATOR="Ninja"
export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)
export CMAKE_PREFIX_PATH=/nix/store/.../qtbase:/nix/store/.../vulkan-loader
export CMAKE_EXPORT_COMPILE_COMMANDS=ON

# Compiler
export CC=clang
export CXX=clang++
export CMAKE_C_COMPILER=clang
export CMAKE_CXX_COMPILER=clang++

# Compiler cache
export CCACHE_DIR=$PWD/.ccache
```

### C. Troubleshooting Checklist

Before reporting issues, check:

- [ ] Nix version is 2.4.0 or later
- [ ] Flakes are enabled in `~/.config/nix/nix.conf`
- [ ] `flake.nix` exists in project root
- [ ] `flake.lock` is committed to version control
- [ ] Sufficient disk space (>10 GB free)
- [ ] Internet connection is working
- [ ] No conflicting system packages
- [ ] Direnv is properly configured (if using)
- [ ] Environment variables are set correctly
- [ ] Nix store is not corrupted

---

**Document Control**

| Version | Date | Author | Changes |
|---------|------|---------|---------|
| 1.0.0 | 2026-01-28 | Technical Writer | Initial version |
