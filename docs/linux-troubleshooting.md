# Linux Troubleshooting Guide

**Version:** 1.0.0
**Last Updated:** 2026-01-28
**Project:** OmniCPP-Template
**References:** [ADR-028](../.specs/02_adrs/ADR-028-cachyos-primary-linux-target.md) | [REQ-008-003](../.specs/04_future_state/reqs/REQ-008-documentation.md)

---

## Table of Contents

1. [Introduction to Linux Troubleshooting](#introduction-to-linux-troubleshooting)
2. [Common Linux Issues](#common-linux-issues)
3. [GCC Troubleshooting](#gcc-troubleshooting)
4. [Clang Troubleshooting](#clang-troubleshooting)
5. [CMake Troubleshooting](#cmake-troubleshooting)
6. [Conan Troubleshooting](#conan-troubleshooting)
7. [Nix Troubleshooting](#nix-troubleshooting)
8. [CachyOS Troubleshooting](#cachyos-troubleshooting)
9. [Qt6/Vulkan Troubleshooting](#qt6vulkan-troubleshooting)
10. [VSCode Troubleshooting](#vscode-troubleshooting)
11. [Debugging Techniques](#debugging-techniques)
12. [Getting Help](#getting-help)

---

## Introduction to Linux Troubleshooting

This guide provides comprehensive troubleshooting assistance for Linux development with the OmniCPP Template. Linux development presents unique challenges due to distribution fragmentation, package manager diversity, and system configuration variations.

### Primary Linux Target

According to [ADR-028](../.specs/02_adrs/ADR-028-cachyos-primary-linux-target.md), **CachyOS is the primary Linux target** for this project. While the build system supports other distributions, CachyOS-specific optimizations and configurations are prioritized.

### Troubleshooting Philosophy

Effective Linux troubleshooting follows these principles:

1. **Isolate the Problem:** Identify whether the issue is environment-specific, code-specific, or configuration-related
2. **Validate Environment:** Use the provided validation script to verify your setup
3. **Check Dependencies:** Ensure all required packages and libraries are installed
4. **Review Logs:** Examine build logs, system logs, and error messages carefully
5. **Reproduce Consistently:** Create a minimal reproduction case to verify the issue
6. **Document Findings:** Record what you tried and the results for future reference

### Security Considerations

When troubleshooting, be aware of security threats outlined in the [Threat Model](../.specs/03_threat_model/analysis.md):

- **TM-LX-001:** Nix package manager security risks
- **TM-LX-002:** Direnv environment variable injection
- **TM-LX-003:** CachyOS-specific security considerations
- **TM-LX-004:** Linux build system security risks

Always validate scripts and commands before running them, especially those from untrusted sources.

---

## Common Linux Issues

### Distribution Detection Fails

**Symptoms:**
- Build system cannot detect Linux distribution
- Error: "Unable to detect Linux distribution"
- Platform-specific configurations not applied

**Diagnosis:**

```bash
# Check if /etc/os-release exists
ls -la /etc/os-release

# View distribution information
cat /etc/os-release

# Check for CachyOS-specific files
ls -la /etc/cachyos-release
ls -la /usr/lib/cachyos-release
```

**Solutions:**

1. **Ensure os-release is present:**
   ```bash
   # Reinstall os-release package (Debian/Ubuntu)
   sudo apt install --reinstall base-files

   # Reinstall os-release package (Arch/CachyOS)
   sudo pacman -S filesystem
   ```

2. **Manual distribution specification:**
   ```bash
   # Set distribution manually for build system
   export WARPLOOM_LINUX_DISTRO="cachyos"
   export WARPLOOM_LINUX_FAMILY="arch"
   ```

3. **Run validation script:**
   ```bash
   ./scripts/linux/validate_environment.sh --verbose
   ```

### Permission Denied Errors

**Symptoms:**
- Permission denied when running build scripts
- Cannot write to build directories
- Cannot install packages

**Solutions:**

1. **Check file permissions:**
   ```bash
   # Check directory permissions
   ls -la build/

   # Fix permissions for build directory
   chmod -R u+rw build/
   ```

2. **Use sudo for system operations:**
   ```bash
   # Install system packages
   sudo pacman -S gcc cmake ninja

   # Do NOT use sudo for project builds
   # Build in user directory without sudo
   cmake --build build/
   ```

3. **Fix ownership issues:**
   ```bash
   # If build directory was created with sudo
   sudo chown -R $USER:$USER build/
   ```

### PATH Issues

**Symptoms:**
- Commands not found
- Wrong version of tool being used
- Scripts cannot locate executables

**Diagnosis:**

```bash
# Check current PATH
echo $PATH

# Find where a command is located
which gcc
which cmake
which conan

# Check all versions in PATH
type -a gcc
```

**Solutions:**

1. **Update PATH in shell configuration:**
   ```bash
   # Add to ~/.bashrc or ~/.zshrc
   export PATH="$HOME/.local/bin:$PATH"
   export PATH="$HOME/.conan2/bin:$PATH"

   # Reload shell configuration
   source ~/.bashrc
   ```

2. **Use absolute paths:**
   ```bash
   # Specify full path to tool
   /usr/bin/gcc --version
   /usr/local/bin/cmake --version
   ```

3. **Check for conflicting installations:**
   ```bash
   # Find all instances of a tool
   find /usr -name "gcc" 2>/dev/null
   find /usr/local -name "gcc" 2>/dev/null
   find "$HOME" -name "gcc" 2>/dev/null
   ```

### Library Linking Errors

**Symptoms:**
- Undefined reference errors during linking
- Shared library not found at runtime
- `ldd` shows missing libraries

**Diagnosis:**

```bash
# Check library dependencies of executable
ldd ./build/debug/default/bin/WarploomStandalone

# Check if library is installed
ldconfig -p | grep library_name

# Search for library files
find /usr -name "lib*.so*" 2>/dev/null | grep library_name
```

**Solutions:**

1. **Install missing development packages:**
   ```bash
   # Ubuntu/Debian
   sudo apt install libssl-dev libpq-dev

   # Arch/CachyOS
   sudo pacman -S openssl postgresql-libs

   # Fedora/RHEL
   sudo dnf install openssl-devel postgresql-devel
   ```

2. **Update library cache:**
   ```bash
   # Update dynamic linker cache
   sudo ldconfig

   # Verify library is now found
   ldconfig -p | grep library_name
   ```

3. **Set library path:**
   ```bash
   # Add to LD_LIBRARY_PATH for custom libraries
   export LD_LIBRARY_PATH="$HOME/.local/lib:$LD_LIBRARY_PATH"

   # Add to /etc/ld.so.conf.d/ for system-wide
   echo "/opt/custom/lib" | sudo tee /etc/ld.so.conf.d/custom.conf
   sudo ldconfig
   ```

---

## GCC Troubleshooting

### GCC Not Found

**Symptoms:**
- Error: "gcc: command not found"
- Build system cannot locate GCC

**Diagnosis:**

```bash
# Check if GCC is installed
which gcc
gcc --version

# Check for GCC packages
dpkg -l | grep gcc  # Debian/Ubuntu
pacman -Qs gcc       # Arch/CachyOS
rpm -qa | grep gcc   # Fedora/RHEL
```

**Solutions:**

1. **Install GCC:**
   ```bash
   # Ubuntu/Debian
   sudo apt update
   sudo apt install build-essential gcc g++

   # Arch/CachyOS
   sudo pacman -S gcc

   # Fedora/RHEL
   sudo dnf install gcc gcc-c++
   ```

2. **Verify installation:**
   ```bash
   gcc --version
   # Expected: gcc (GCC) 13.x.x or higher

   g++ --version
   # Expected: g++ (GCC) 13.x.x or higher
   ```

3. **Update PATH if needed:**
   ```bash
   # Add to ~/.bashrc if GCC in non-standard location
   export PATH="/usr/local/gcc/bin:$PATH"
   source ~/.bashrc
   ```

### GCC Version Too Old

**Symptoms:**
- Error: "GCC version too old"
- C++23 features not supported
- Build fails with compiler errors

**Diagnosis:**

```bash
# Check GCC version
gcc --version | head -n1

# Check required version in project
grep -r "MIN_GCC" scripts/linux/validate_environment.sh
```

**Solutions:**

1. **Install newer GCC:**
   ```bash
   # Arch/CachyOS (latest version in repos)
   sudo pacman -S gcc

   # Ubuntu (use newer PPA if needed)
   sudo add-apt-repository ppa:ubuntu-toolchain-r/test
   sudo apt update
   sudo apt install gcc-13 g++-13

   # Set as default
   sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100
   sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100
   ```

2. **Build GCC from source (last resort):**
   ```bash
   # Download and build GCC 13+
   wget https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.xz
   tar xf gcc-13.2.0.tar.xz
   cd gcc-13.2.0
   ./contrib/download_prerequisites
   mkdir build && cd build
   ../configure --prefix=/usr/local/gcc-13 --enable-languages=c,c++
   make -j$(nproc)
   sudo make install
   ```

### GCC Compilation Errors

**Symptoms:**
- Compilation fails with GCC-specific errors
- Internal compiler error
- Segmentation fault during compilation

**Common Issues and Solutions:**

1. **Memory issues:**
   ```bash
   # Reduce parallel jobs if running out of memory
   cmake --build build/ -j2  # Use fewer jobs

   # Increase swap space if needed
   sudo fallocate -l 4G /swapfile
   sudo chmod 600 /swapfile
   sudo mkswap /swapfile
   sudo swapon /swapfile
   ```

2. **Flag issues:**
   ```bash
   # Test with minimal flags
   cmake -DCMAKE_C_FLAGS="" -DCMAKE_CXX_FLAGS="" ../..

   # Check for conflicting optimization flags
   grep -r "O3\|Ofast" CMakeLists.txt
   ```

3. **Include path issues:**
   ```bash
   # Check include paths
   gcc -E -v -x c++ /dev/null 2>&1 | grep include

   # Add missing include paths
   cmake -DCMAKE_INCLUDE_PATH="/path/to/includes" ../..
   ```

### CachyOS GCC-Specific Issues

**Symptoms:**
- CachyOS-specific compiler flags not applied
- Performance optimizations not working
- `-march=native` warnings

**Solutions:**

1. **Verify CachyOS detection:**
   ```bash
   # Check if CachyOS is detected
   grep -c "cachyos" /etc/os-release

   # Run validation script
   ./scripts/linux/validate_environment.sh --verbose
   ```

2. **Apply CachyOS flags manually:**
   ```bash
   # Use CachyOS-optimized flags
   export CFLAGS="-march=native -O3 -flto -DNDEBUG"
   export CXXFLAGS="-march=native -O3 -flto -DNDEBUG"
   export LDFLAGS="-Wl,--as-needed -Wl,--no-undefined -flto"

   # Build with flags
   cmake -DCMAKE_C_FLAGS="$CFLAGS" \
         -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
         -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
         ../..
   ```

3. **Use CachyOS CMake preset:**
   ```bash
   # Configure with CachyOS preset
   cmake --preset cachyos-gcc-release

   # Build with preset
   cmake --build --preset cachyos-gcc-release
   ```

---

## Clang Troubleshooting

### Clang Not Found

**Symptoms:**
- Error: "clang: command not found"
- Build system cannot locate Clang

**Diagnosis:**

```bash
# Check if Clang is installed
which clang
clang --version

# Check for Clang packages
dpkg -l | grep clang  # Debian/Ubuntu
pacman -Qs clang       # Arch/CachyOS
rpm -qa | grep clang   # Fedora/RHEL
```

**Solutions:**

1. **Install Clang:**
   ```bash
   # Ubuntu/Debian
   sudo apt update
   sudo apt install clang clang++ libc++-dev libc++abi-dev

   # Arch/CachyOS
   sudo pacman -S clang

   # Fedora/RHEL
   sudo dnf install clang clang-tools-extra
   ```

2. **Verify installation:**
   ```bash
   clang --version
   # Expected: clang version 19.x.x or higher

   clang++ --version
   # Expected: clang version 19.x.x or higher
   ```

### Clang Version Too Old

**Symptoms:**
- Error: "Clang version too old"
- C++23 features not supported
- Build fails with compiler errors

**Solutions:**

1. **Install newer Clang:**
   ```bash
   # Arch/CachyOS (latest version in repos)
   sudo pacman -S clang

   # Ubuntu (use newer PPA if needed)
   wget https://apt.llvm.org/llvm.sh
   chmod +x llvm.sh
   sudo ./llvm.sh 19

   # Fedora (latest version in repos)
   sudo dnf install clang clang-tools-extra
   ```

2. **Build Clang from source (last resort):**
   ```bash
   # Download and build Clang 19+
   git clone https://github.com/llvm/llvm-project.git
   cd llvm-project
   git checkout llvmorg-19.1.0
   mkdir build && cd build
   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DLLVM_ENABLE_PROJECTS="clang" \
         -DCMAKE_INSTALL_PREFIX=/usr/local/clang-19 \
         ../llvm
   ninja
   sudo ninja install
   ```

### Clang Compilation Errors

**Symptoms:**
- Compilation fails with Clang-specific errors
- Different behavior than GCC
- Linking issues with libc++

**Common Issues and Solutions:**

1. **libc++ linking issues:**
   ```bash
   # Use libc++ explicitly
   cmake -DCMAKE_CXX_COMPILER=clang++ \
         -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
         -DCMAKE_EXE_LINKER_FLAGS="-lc++ -lc++abi" \
         ../..

   # Or use libstdc++ (more compatible)
   cmake -DCMAKE_CXX_COMPILER=clang++ \
         -DCMAKE_CXX_FLAGS="-stdlib=libstdc++" \
         ../..
   ```

2. **Include path issues:**
   ```bash
   # Check Clang include paths
   clang -E -v -x c++ /dev/null 2>&1 | grep include

   # Add missing include paths
   cmake -DCMAKE_INCLUDE_PATH="/path/to/includes" ../..
   ```

3. **Warning suppression:**
   ```bash
   # Suppress specific Clang warnings if needed
   cmake -DCMAKE_CXX_FLAGS="-Wno-unknown-warning-option" ../..
   ```

### CachyOS Clang-Specific Issues

**Symptoms:**
- CachyOS Clang optimizations not applied
- libc++ vs libstdc++ conflicts

**Solutions:**

1. **Use CachyOS Clang preset:**
   ```bash
   # Configure with CachyOS Clang preset
   cmake --preset cachyos-clang-release

   # Build with preset
   cmake --build --preset cachyos-clang-release
   ```

2. **Apply CachyOS flags manually:**
   ```bash
   # Use CachyOS-optimized Clang flags
   export CFLAGS="-march=native -O3 -flto -DNDEBUG"
   export CXXFLAGS="-march=native -O3 -flto -DNDEBUG -stdlib=libc++"
   export LDFLAGS="-Wl,--as-needed -Wl,--no-undefined -flto -lc++ -lc++abi"

   # Build with flags
   cmake -DCMAKE_C_COMPILER=clang \
         -DCMAKE_CXX_COMPILER=clang++ \
         -DCMAKE_C_FLAGS="$CFLAGS" \
         -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
         -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
         ../..
   ```

---

## CMake Troubleshooting

### CMake Not Found

**Symptoms:**
- Error: "cmake: command not found"
- Build system cannot locate CMake

**Diagnosis:**

```bash
# Check if CMake is installed
which cmake
cmake --version

# Check for CMake packages
dpkg -l | grep cmake  # Debian/Ubuntu
pacman -Qs cmake       # Arch/CachyOS
rpm -qa | grep cmake   # Fedora/RHEL
```

**Solutions:**

1. **Install CMake:**
   ```bash
   # Ubuntu/Debian
   sudo apt update
   sudo apt install cmake ninja-build

   # Arch/CachyOS
   sudo pacman -S cmake ninja

   # Fedora/RHEL
   sudo dnf install cmake ninja-build
   ```

2. **Verify installation:**
   ```bash
   cmake --version
   # Expected: cmake version 3.20.x or higher

   ninja --version
   # Expected: 1.10.x or higher
   ```

### CMake Version Too Old

**Symptoms:**
- Error: "CMake version too old"
- CMake features not available
- Presets not working

**Solutions:**

1. **Install newer CMake:**
   ```bash
   # Arch/CachyOS (latest version in repos)
   sudo pacman -S cmake

   # Ubuntu (use newer PPA if needed)
   sudo add-apt-repository ppa:kitware/cmake
   sudo apt update
   sudo apt install cmake

   # Install from official binary (universal)
   wget https://github.com/Kitware/CMake/releases/download/v3.31.0/cmake-3.31.0-linux-x86_64.sh
   chmod +x cmake-3.31.0-linux-x86_64.sh
   sudo ./cmake-3.31.0-linux-x86_64.sh --prefix=/usr/local
   ```

2. **Update PATH:**
   ```bash
   # Add to ~/.bashrc if CMake in non-standard location
   export PATH="/usr/local/bin:$PATH"
   source ~/.bashrc
   ```

### CMake Configuration Fails

**Symptoms:**
- CMake configure fails with errors
- Missing dependencies detected
- Generator not found

**Common Issues and Solutions:**

1. **Generator not found:**
   ```bash
   # Check available generators
   cmake --help | grep -i generator

   # Install Ninja if preferred
   sudo pacman -S ninja  # Arch/CachyOS
   sudo apt install ninja-build  # Ubuntu/Debian
   sudo dnf install ninja-build  # Fedora/RHEL

   # Use Ninja generator
   cmake -G Ninja ../..
   ```

2. **Missing dependencies:**
   ```bash
   # Run CMake with verbose output
   cmake -DCMAKE_MESSAGE_LOG_LEVEL=STATUS ../.. 2>&1 | tee cmake.log

   # Check CMake error log
   cat build/CMakeFiles/CMakeError.log
   cat build/CMakeFiles/CMakeOutput.log
   ```

3. **Cache issues:**
   ```bash
   # Clear CMake cache
   rm -rf build/CMakeCache.txt
   rm -rf build/CMakeFiles/

   # Reconfigure
   cmake ../..
   ```

### CMake Preset Issues

**Symptoms:**
- Preset not found
- Preset configuration fails
- Preset not working as expected

**Solutions:**

1. **List available presets:**
   ```bash
   # List all presets
   cmake --list-presets

   # List build presets
   cmake --list-presets --build

   # List test presets
   cmake --list-presets --test
   ```

2. **Check preset configuration:**
   ```bash
   # View preset details
   cat CMakePresets.json | jq '.buildPresets[] | select(.name=="cachyos-gcc-release")'

   # Verify preset exists
   cmake --list-presets | grep cachyos
   ```

3. **Use preset manually:**
   ```bash
   # Configure with preset
   cmake --preset cachyos-gcc-release

   # Build with preset
   cmake --build --preset cachyos-gcc-release

   # Clean with preset
   cmake --build --preset cachyos-gcc-release --target clean
   ```

---

## Conan Troubleshooting

### Conan Not Found

**Symptoms:**
- Error: "conan: command not found"
- Build system cannot locate Conan

**Diagnosis:**

```bash
# Check if Conan is installed
which conan
conan --version

# Check Python packages
pip list | grep conan
```

**Solutions:**

1. **Install Conan:**
   ```bash
   # Install via pip (recommended)
   pip install conan

   # Install specific version
   pip install conan==2.0.17

   # Verify installation
   conan --version
   # Expected: Conan version 2.0.x
   ```

2. **Update PATH:**
   ```bash
   # Add to ~/.bashrc if needed
   export PATH="$HOME/.local/bin:$PATH"
   source ~/.bashrc
   ```

### Conan Version Too Old

**Symptoms:**
- Error: "Conan version too old"
- Conan 2.0 features not available
- Profile issues

**Solutions:**

1. **Update Conan:**
   ```bash
   # Update to latest version
   pip install --upgrade conan

   # Verify version
   conan --version
   ```

2. **Reinstall Conan:**
   ```bash
   # Uninstall old version
   pip uninstall conan

   # Install new version
   pip install conan==2.0.17
   ```

### Conan Profile Issues

**Symptoms:**
- Profile not found
- Profile configuration errors
- Wrong profile being used

**Solutions:**

1. **List available profiles:**
   ```bash
   # List local profiles
   conan profile list

   # Show default profile
   conan profile show default

   # Show specific profile
   conan profile show cachyos
   ```

2. **Detect system profile:**
   ```bash
   # Auto-detect and create default profile
   conan profile detect

   # View detected profile
   conan profile show default
   ```

3. **Use project profile:**
   ```bash
   # Use CachyOS profile
   conan install . --profile cachyos

   # Use CachyOS Clang profile
   conan install . --profile cachyos-clang

   # Use debug profile
   conan install . --profile cachyos-debug
   ```

### Conan Dependency Issues

**Symptoms:**
- Package not found
- Dependency conflicts
- Build fails during Conan install

**Solutions:**

1. **Clear Conan cache:**
   ```bash
   # Clear all caches
   conan remove "*" -c
   conan cache clean

   # Clear specific package
   conan remove fmt/* -c
   ```

2. **Update remotes:**
   ```bash
   # List remotes
   conan remote list

   # Add ConanCenter if missing
   conan remote add conancenter https://center.conan.io

   # Update remotes
   conan remote update
   ```

3. **Build from source:**
   ```bash
   # Build missing packages from source
   conan install . --build=missing

   # Build all packages from source
   conan install . --build=*

   # Build specific package from source
   conan install . --build=fmt
   ```

### CachyOS Conan Profile Issues

**Symptoms:**
- CachyOS profile not found
- Wrong compiler settings
- CachyOS-specific flags not applied

**Solutions:**

1. **Verify CachyOS profile exists:**
   ```bash
   # Check for CachyOS profiles
   ls conan/profiles/cachyos*

   # View profile content
   cat conan/profiles/cachyos
   ```

2. **Create CachyOS profile:**
   ```bash
   # Copy from project profile
   cp conan/profiles/cachyos ~/.conan2/profiles/cachyos

   # Or use profile directly
   conan install . --profile:build=conan/profiles/cachyos
   ```

3. **Verify profile settings:**
   ```bash
   # Show profile details
   conan profile show cachyos

   # Check compiler version
   conan profile show cachyos | grep compiler.version
   ```

---

## Nix Troubleshooting

### Nix Not Found

**Symptoms:**
- Error: "nix: command not found"
- Cannot use Nix development environment

**Diagnosis:**

```bash
# Check if Nix is installed
which nix
nix --version

# Check for Nix store
ls -la /nix
ls -la ~/.nix-profile
```

**Solutions:**

1. **Install Nix:**
   ```bash
   # Install Nix (single-user)
   sh <(curl -L https://nixos.org/nix/install) --no-daemon

   # Install Nix (multi-user, requires sudo)
   sh <(curl -L https://nixos.org/nix/install) --daemon

   # Source Nix environment
   . ~/.nix-profile/etc/profile.d/nix.sh
   ```

2. **Verify installation:**
   ```bash
   nix --version
   # Expected: nix (Nix) 2.x.x or higher
   ```

### Nix Flakes Not Enabled

**Symptoms:**
- Error: "experimental feature 'flakes' is disabled"
- Cannot use flake.nix
- Flake commands not available

**Solutions:**

1. **Enable flakes:**
   ```bash
   # Create Nix configuration directory
   mkdir -p ~/.config/nix

   # Enable flakes in nix.conf
   echo "experimental-features = nix-command flakes" > ~/.config/nix/nix.conf

   # Reload Nix environment
   source ~/.nix-profile/etc/profile.d/nix.sh
   ```

2. **Verify flakes are enabled:**
   ```bash
   # Check Nix configuration
   cat ~/.config/nix/nix.conf

   # Test flake command
   nix flake show
   ```

### Nix Build Fails

**Symptoms:**
- Nix build fails with errors
- Dependencies not found
- Hash mismatch errors

**Solutions:**

1. **Clear Nix store:**
   ```bash
   # Remove old garbage
   nix-collect-garbage -d

   # Optimize Nix store
   nix-store --optimize
   ```

2. **Update flake inputs:**
   ```bash
   # Update all inputs
   nix flake update

   # Update specific input
   nix flake update nixpkgs

   # Lock flake
   nix flake lock
   ```

3. **Build with verbose output:**
   ```bash
   # Build with verbose output
   nix build .#default --verbose

   # Build with show-trace
   nix build .#default --show-trace
   ```

### Nix Security Issues (TM-LX-001)

**Symptoms:**
- Suspicious Nix expressions
- Untrusted packages
- Channel hijacking concerns

**Solutions:**

1. **Pin Nix channels:**
   ```bash
   # Use pinned nixpkgs in flake.nix
   inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable?rev=a1b2c3d4e5f6";

   # Verify pinned commit
   nix flake metadata
   ```

2. **Verify Nix store:**
   ```bash
   # Verify Nix store integrity
   nix-store --verify --check-contents

   # Verify specific package
   nix-store --verify-path /nix/store/abc123...-gcc-13.2.0
   ```

3. **Use signed flakes:**
   ```bash
   # Verify flake signature
   nix flake check --signing-key /path/to/key.pem

   # Use signed flakes only
   nix flake metadata --json | jq '.signed'
   ```

---

## CachyOS Troubleshooting

### CachyOS Detection Fails

**Symptoms:**
- Build system does not detect CachyOS
- CachyOS-specific optimizations not applied
- Generic Linux configuration used

**Diagnosis:**

```bash
# Check /etc/os-release
cat /etc/os-release | grep -i cachyos

# Check for CachyOS-specific files
ls -la /etc/cachyos-release
ls -la /usr/lib/cachyos-release

# Run validation script
./scripts/linux/validate_environment.sh --verbose
```

**Solutions:**

1. **Verify CachyOS installation:**
   ```bash
   # Check CachyOS kernel
   pacman -Qs cachyos-kernel

   # Check CachyOS-specific packages
   pacman -Qs cachyos
   ```

2. **Manual CachyOS specification:**
   ```bash
   # Set distribution manually
   export WARPLOOM_LINUX_DISTRO="cachyos"
   export WARPLOOM_LINUX_FAMILY="arch"
   ```

3. **Reinstall CachyOS release files:**
   ```bash
   # Reinstall filesystem package
   sudo pacman -S filesystem

   # Verify files are present
   ls -la /etc/cachyos-release
   ```

### CachyOS Package Issues

**Symptoms:**
- Pacman fails to install packages
- Package conflicts
- Database errors

**Solutions:**

1. **Update system:**
   ```bash
   # Update package database
   sudo pacman -Sy

   # Update all packages
   sudo pacman -Syu

   # Update only specific package
   sudo pacman -S gcc
   ```

2. **Fix database errors:**
   ```bash
   # Remove lock files
   sudo rm /var/lib/pacman/db.lck

   # Refresh databases
   sudo pacman -Sy

   # Update system
   sudo pacman -Su
   ```

3. **Resolve conflicts:**
   ```bash
   # Remove conflicting package
   sudo pacman -R conflicting-package

   # Install required package
   sudo pacman -S required-package

   # Or use --overwrite flag (use with caution)
   sudo pacman -S --overwrite '*' package-name
   ```

### CachyOS Kernel Issues

**Symptoms:**
- Kernel-related build failures
- Performance issues
- Hardware compatibility problems

**Solutions:**

1. **Check kernel version:**
   ```bash
   # Check current kernel
   uname -r

   # Check CachyOS kernel package
   pacman -Qs cachyos-kernel
   ```

2. **Update kernel:**
   ```bash
   # Update CachyOS kernel
   sudo pacman -Syu cachyos-kernel

   # Reboot to apply changes
   sudo reboot
   ```

3. **Check kernel logs:**
   ```bash
   # View kernel messages
   dmesg | tail -n 100

   # Check for errors
   dmesg | grep -i error
   ```

### CachyOS Security Issues (TM-LX-003)

**Symptoms:**
- Pacman cache poisoning
- AUR package vulnerabilities
- CachyOS-specific exploits

**Solutions:**

1. **Verify Pacman cache:**
   ```bash
   # Verify package integrity
   pacman -Qkk

   # Verify specific package
   pacman -Qkk package-name

   # Clean cache regularly
   sudo pacman -Sc
   ```

2. **Validate AUR packages:**
   ```bash
   # Review PKGBUILD before installing
   cat PKGBUILD

   # Check for suspicious patterns
   grep -E 'curl|wget|eval|exec' PKGBUILD

   # Use trusted AUR helpers
   yay -S package-name  # yay has built-in security checks
   ```

3. **Enable kernel hardening:**
   ```bash
   # Enable CachyOS kernel hardening
   # /etc/sysctl.d/99-security.conf
   echo "kernel.kptr_restrict=2" | sudo tee /etc/sysctl.d/99-security.conf
   echo "kernel.dmesg_restrict=1" | sudo tee -a /etc/sysctl.d/99-security.conf
   echo "kernel.kexec_load_disabled=1" | sudo tee -a /etc/sysctl.d/99-security.conf
   echo "kernel.modules_disabled=1" | sudo tee -a /etc/sysctl.d/99-security.conf
   echo "kernel.randomize_va_space=2" | sudo tee -a /etc/sysctl.d/99-security.conf

   # Apply settings
   sudo sysctl --system
   ```

---

## Qt6/Vulkan Troubleshooting

### Qt6 Not Found

**Symptoms:**
- Error: "Qt6 not found"
- CMake cannot find Qt6 components
- Qt6 development files missing

**Diagnosis:**

```bash
# Check for Qt6 packages
dpkg -l | grep qt6  # Debian/Ubuntu
pacman -Qs qt6       # Arch/CachyOS
rpm -qa | grep qt6   # Fedora/RHEL

# Check for qmake
which qmake6
which qmake

# Check Qt6 version
qmake6 -v 2>&1 | grep -oP 'Qt version \K[0-9.]+'
```

**Solutions:**

1. **Install Qt6:**
   ```bash
   # Ubuntu/Debian
   sudo apt update
   sudo apt install qt6-base-dev qt6-tools-dev qt6-declarative-dev

   # Arch/CachyOS
   sudo pacman -S qt6-base qt6-tools qt6-declarative

   # Fedora/RHEL
   sudo dnf install qt6-qtbase-devel qt6-qttools-devel qt6-qtdeclarative-devel
   ```

2. **Set Qt6 path:**
   ```bash
   # Add to ~/.bashrc if Qt6 in non-standard location
   export Qt6_DIR="/path/to/qt6/lib/cmake/Qt6"
   export CMAKE_PREFIX_PATH="/path/to/qt6:$CMAKE_PREFIX_PATH"
   source ~/.bashrc
   ```

### Vulkan Not Found

**Symptoms:**
- Error: "Vulkan SDK not found"
- Qt/Vulkan builds fail
- Vulkan headers missing

**Diagnosis:**

```bash
# Check for Vulkan packages
dpkg -l | grep vulkan  # Debian/Ubuntu
pacman -Qs vulkan     # Arch/CachyOS
rpm -qa | grep vulkan # Fedora/RHEL

# Check for Vulkan tools
which vulkaninfo
vulkaninfo 2>&1 | head -n 20

# Check Vulkan SDK
echo $VULKAN_SDK
ls -la $VULKAN_SDK 2>/dev/null || echo "VULKAN_SDK not set"
```

**Solutions:**

1. **Install Vulkan:**
   ```bash
   # Ubuntu/Debian
   sudo apt update
   sudo apt install vulkan-tools libvulkan-dev vulkan-validationlayers-dev

   # Arch/CachyOS
   sudo pacman -S vulkan-icd-loader vulkan-headers vulkan-tools

   # Fedora/RHEL
   sudo dnf install vulkan-loader-devel vulkan-tools
   ```

2. **Install Vulkan SDK (LunarG):**
   ```bash
   # Download Vulkan SDK
   wget https://sdk.lunarg.com/sdk/download/latest/linux/vulkan-sdk.tar.xz

   # Extract and install
   tar xf vulkan-sdk.tar.xz
   cd vulkan-sdk
   ./vulkansdk install

   # Source environment
   source /path/to/vulkan-sdk/setup-env.sh

   # Add to ~/.bashrc
   echo "source /path/to/vulkan-sdk/setup-env.sh" >> ~/.bashrc
   ```

3. **Set Vulkan environment:**
   ```bash
   # Set Vulkan SDK path
   export VULKAN_SDK="/path/to/VulkanSDK"
   export VK_LAYER_PATH="$VULKAN_SDK/etc/vulkan/explicit_layer.d"
   export LD_LIBRARY_PATH="$VULKAN_SDK/lib:$LD_LIBRARY_PATH"
   export PATH="$VULKAN_SDK/bin:$PATH"
   ```

### Qt6/Vulkan Integration Issues

**Symptoms:**
- Qt6 cannot find Vulkan
- Vulkan surface creation fails
- Rendering issues

**Solutions:**

1. **Verify Qt6 Vulkan support:**
   ```bash
   # Check for Qt6 Vulkan module
   ls /usr/lib/qt6/plugins/platforms/libqvulkan.so  # Arch/CachyOS
   ls /usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqvulkan.so  # Ubuntu/Debian

   # Check Qt6 configuration
   qmake6 -query QT_INSTALL_PLUGINS
   ```

2. **Set Qt platform:**
   ```bash
   # Use Wayland platform (CachyOS default)
   export QT_QPA_PLATFORM=wayland

   # Use X11 platform if needed
   export QT_QPA_PLATFORM=xcb

   # Let Qt auto-detect
   unset QT_QPA_PLATFORM
   ```

3. **Enable Vulkan validation layers:**
   ```bash
   # Set validation layer path
   export VK_LAYER_PATH="$VULKAN_SDK/etc/vulkan/explicit_layer.d"

   # Enable validation layers
   export VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation"

   # Check available layers
   vk-layers
   ```

### Wayland/X11 Issues

**Symptoms:**
- Display server issues
- Window creation fails
- Input problems

**Solutions:**

1. **Check display server:**
   ```bash
   # Check if Wayland is running
   echo $XDG_SESSION_TYPE

   # Check Wayland compositor
   ps aux | grep -E 'weston|kwin|gnome-shell'

   # Check X11 server
   ps aux | grep Xorg
   ```

2. **Set Qt platform explicitly:**
   ```bash
   # Force Wayland
   export QT_QPA_PLATFORM=wayland

   # Force X11
   export QT_QPA_PLATFORM=xcb

   # Test with different platform
   QT_QPA_PLATFORM=wayland ./your_app
   QT_QPA_PLATFORM=xcb ./your_app
   ```

3. **Install required libraries:**
   ```bash
   # Wayland support
   sudo pacman -S wayland wayland-protocols  # Arch/CachyOS
   sudo apt install libwayland-dev  # Ubuntu/Debian

   # X11 support
   sudo pacman -S libx11 libxcb  # Arch/CachyOS
   sudo apt install libx11-dev libxcb-xinerama0  # Ubuntu/Debian
   ```

---

## VSCode Troubleshooting

### Language Server Not Working

**Symptoms:**
- No IntelliSense or code completion
- clangd/ccls not functioning
- Red squiggles under code

**Solutions:**

1. **Restart language servers:**
   ```
   Ctrl+Shift+P → "clangd: Restart language server"
   Ctrl+Shift+P → "ccls: Restart"
   ```

2. **Clear language server caches:**
   ```bash
   # Clear clangd cache
   rm -rf ~/.cache/clangd

   # Clear ccls cache
   rm -rf ~/.cache/ccls-cache

   # Clear VSCode C/C++ cache
   rm -rf ~/.config/Code/User/globalStorage/ms-vscode.cpptools
   ```

3. **Check compilation database:**
   ```bash
   # Ensure compile_commands.json exists
   ls build/*/compile_commands.json

   # Regenerate if missing
   cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../..
   ```

4. **Verify clangd configuration:**
   ```json
   // .vscode/settings.json
   {
     "clangd.path": "/usr/bin/clangd",
     "clangd.arguments": [
       "--background-index",
       "--clang-tidy",
       "--header-insertion=iwyu",
       "--completion-style=detailed",
       "--function-arg-placeholders",
       "--fallback-style=llvm"
     ]
   }
   ```

### VSCode Tasks Not Working

**Symptoms:**
- VSCode tasks fail to run
- Controller script errors
- Build tasks not executing

**Solutions:**

1. **Check Python environment:**
   ```bash
   # Verify Python version
   python --version
   # Expected: Python 3.8+

   # Install required packages
   pip install -r requirements.txt
   ```

2. **Validate VSCode settings:**
   ```json
   // .vscode/settings.json
   {
     "python.pythonPath": "python",
     "cmake.configureOnOpen": false,
     "cmake.buildDirectory": "${workspaceFolder}/build/${buildType}",
     "cmake.sourceDirectory": "${workspaceFolder}"
   }
   ```

3. **Check tasks.json:**
   ```json
   // .vscode/tasks.json
   {
     "version": "2.0.0",
     "tasks": [
       {
         "label": "Build Standalone Debug",
         "type": "shell",
         "command": "python",
         "args": [
           "WarploomController.py",
           "build",
           "standalone",
           "Zero to Build",
           "default",
           "Debug"
         ],
         "group": {
           "kind": "build",
           "isDefault": true
         },
         "problemMatcher": ["$gcc"]
       }
     ]
   }
   ```

### VSCode Debugging Issues

**Symptoms:**
- Debugger cannot start
- Breakpoints not hit
- Cannot inspect variables

**Solutions:**

1. **Check launch.json:**
   ```json
   // .vscode/launch.json
   {
     "version": "0.2.0",
     "configurations": [
       {
         "name": "Debug Standalone (GDB)",
         "type": "cppdbg",
         "request": "launch",
         "program": "${workspaceFolder}/build/debug/default/bin/WarploomStandalone",
         "args": [],
         "stopAtEntry": false,
         "cwd": "${workspaceFolder}",
         "environment": [],
         "externalConsole": false,
         "MIMode": "gdb",
         "setupCommands": [
           {
             "description": "Enable pretty-printing for gdb",
             "text": "-enable-pretty-printing",
             "ignoreFailures": true
           }
         ]
       }
     ]
   }
   ```

2. **Install GDB/LLDB:**
   ```bash
   # Install GDB
   sudo pacman -S gdb  # Arch/CachyOS
   sudo apt install gdb  # Ubuntu/Debian

   # Install LLDB
   sudo pacman -S lldb  # Arch/CachyOS
   sudo apt install lldb  # Ubuntu/Debian
   ```

3. **Verify debug symbols:**
   ```bash
   # Check if executable has debug symbols
   file build/debug/default/bin/WarploomStandalone

   # Check with readelf
   readelf -S build/debug/default/bin/WarploomStandalone | grep debug

   # Rebuild with debug info if needed
   cmake -DCMAKE_BUILD_TYPE=Debug ../..
   ```

### CachyOS VSCode Issues

**Symptoms:**
- CachyOS-specific configurations not applied
- Wrong compiler selected
- Platform-specific issues

**Solutions:**

1. **Use CachyOS-specific tasks:**
   ```json
   // .vscode/tasks.json - CachyOS GCC
   {
     "label": "Build CachyOS GCC Release",
     "type": "shell",
     "command": "cmake",
     "args": [
       "--build",
       "--preset",
       "cachyos-gcc-release"
     ],
     "problemMatcher": ["$gcc"]
   }
   ```

2. **Set environment variables:**
   ```json
   // .vscode/tasks.json - With CachyOS flags
   {
     "label": "Build CachyOS Optimized",
     "type": "shell",
     "command": "cmake",
     "args": [
       "--build",
       "--preset",
       "cachyos-gcc-release"
     ],
     "options": {
       "env": {
         "CFLAGS": "-march=native -O3 -flto -DNDEBUG",
         "CXXFLAGS": "-march=native -O3 -flto -DNDEBUG"
       }
     },
     "problemMatcher": ["$gcc"]
   }
   ```

3. **Verify clangd is using correct compiler:**
   ```json
   // .vscode/settings.json - CachyOS clangd
   {
     "clangd.path": "/usr/bin/clangd",
     "clangd.arguments": [
       "--compile-commands-dir=${workspaceFolder}/build/cachyos-gcc/debug",
       "--query-driver=/usr/bin/gcc",
       "--query-driver=/usr/bin/clang"
     ]
   }
   ```

---

## Debugging Techniques

### Enable Verbose Output

**CMake Verbose Build:**
```bash
# Build with verbose output
cmake --build build/ --verbose

# Or set environment variable
export VERBOSE=1
cmake --build build/
```

**Conan Verbose Install:**
```bash
# Install with verbose output
conan install . --build=missing -v

# Or set environment variable
export CONAN_VERBOSE=1
conan install .
```

**Compiler Verbose Output:**
```bash
# GCC verbose
gcc -v -c file.cpp

# Clang verbose
clang -v -c file.cpp

# Show preprocessing
gcc -E file.cpp
```

### Check Logs

**CMake Logs:**
```bash
# View CMake output log
cat build/CMakeFiles/CMakeOutput.log

# View CMake error log
cat build/CMakeFiles/CMakeError.log

# View CMake cache
cat build/CMakeCache.txt | grep -i error
```

**Build Logs:**
```bash
# View build output
cat build/build.ninja

# Check for errors
grep -i error build/build.ninja

# Check for warnings
grep -i warning build/build.ninja
```

**System Logs:**
```bash
# View kernel messages
dmesg | tail -n 100

# View journal logs
journalctl -xe

# View specific service logs
journalctl -u systemd-journald
```

### Use Debugging Tools

**GDB (GNU Debugger):**
```bash
# Start GDB with executable
gdb ./build/debug/default/bin/WarploomStandalone

# Common GDB commands
(gdb) break main          # Set breakpoint
(gdb) run                # Start program
(gdb) next               # Step over
(gdb) step               # Step into
(gdb) print variable     # Print variable
(gdb) backtrace          # Show stack trace
(gdb) continue           # Continue execution
(gdb) quit               # Exit GDB
```

**LLDB (LLVM Debugger):**
```bash
# Start LLDB with executable
lldb ./build/debug/default/bin/WarploomStandalone

# Common LLDB commands
(lldb) breakpoint set --name main  # Set breakpoint
(lldb) run                      # Start program
(lldb) next                     # Step over
(lldb) step                     # Step into
(lldb) frame variable            # Print variable
(lldb) bt                       # Show stack trace
(lldb) continue                  # Continue execution
(lldb) quit                     # Exit LLDB
```

**Valgrind (Memory Debugger):**
```bash
# Check for memory leaks
valgrind --leak-check=full ./build/debug/default/bin/WarploomStandalone

# Check for memory errors
valgrind --tool=memcheck ./build/debug/default/bin/WarploomStandalone

# Generate suppression file
valgrind --gen-suppressions=all ./build/debug/default/bin/WarploomStandalone
```

**AddressSanitizer (ASan):**
```bash
# Configure with ASan
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
      -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
      ../..

# Build and run
cmake --build build/
./build/debug/default/bin/WarploomStandalone
```

**UndefinedBehaviorSanitizer (UBSan):**
```bash
# Configure with UBSan
cmake -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -g" \
      -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -g" \
      ../..

# Build and run
cmake --build build/
./build/debug/default/bin/WarploomStandalone
```

### Isolate Issues

**Minimal Reproduction:**
```bash
# Create minimal test case
cat > test_minimal.cpp << 'EOF'
#include <iostream>
int main() {
    std::cout << "Hello, World!" << std::endl;
    return 0;
}
EOF

# Compile minimal case
g++ -o test_minimal test_minimal.cpp

# Run minimal case
./test_minimal
```

**Binary Search:**
```bash
# Comment out half the code
# Build and test
# If error persists, issue is in uncommented half
# If error gone, issue is in commented half
# Repeat until issue is isolated
```

**Clean Build:**
```bash
# Remove build directory
rm -rf build/

# Rebuild from scratch
cmake --preset cachyos-gcc-debug
cmake --build --preset cachyos-gcc-debug
```

### Performance Profiling

**perf (Linux Performance Tool):**
```bash
# Record performance data
perf record -g ./build/release/default/bin/WarploomStandalone

# Analyze performance data
perf report

# Annotate source code
perf annotate
```

**gprof (GNU Profiler):**
```bash
# Configure with profiling
cmake -DCMAKE_CXX_FLAGS="-pg" -DCMAKE_C_FLAGS="-pg" ../..

# Build and run
cmake --build build/
./build/release/default/bin/WarploomStandalone

# Analyze profile
gprof ./build/release/default/bin/WarploomStandalone gmon.out > analysis.txt
```

---

## Getting Help

### Before Asking for Help

Before seeking help, gather the following information:

1. **System Information:**
   ```bash
   # Linux distribution
   cat /etc/os-release

   # Kernel version
   uname -r

   # Architecture
   uname -m

   # CachyOS detection
   grep -i cachyos /etc/os-release || echo "Not CachyOS"
   ```

2. **Tool Versions:**
   ```bash
   # GCC version
   gcc --version | head -n1

   # Clang version
   clang --version | head -n1

   # CMake version
   cmake --version | head -n1

   # Conan version
   conan --version

   # Nix version (if applicable)
   nix --version 2>/dev/null || echo "Nix not installed"
   ```

3. **Build Configuration:**
   ```bash
   # CMake configuration
   cat build/CMakeCache.txt | grep -E "CMAKE_BUILD_TYPE|CMAKE_C_COMPILER|CMAKE_CXX_COMPILER"

   # Conan profile
   conan profile show 2>/dev/null || echo "Conan not configured"

   # Environment variables
   env | grep -E "CMAKE|CONAN|CC|CXX|LD_LIBRARY"
   ```

4. **Error Messages:**
   - Full error output (not just last line)
   - Backtrace if available
   - Log files (CMakeError.log, build.log)

5. **Steps to Reproduce:**
   - Exact commands used
   - Expected behavior
   - Actual behavior
   - Minimal reproduction case

### Where to Get Help

1. **Documentation:**
   - [Linux Builds Guide](linux-builds.md)
   - [CachyOS Builds Guide](cachyos-builds.md)
   - [General Troubleshooting Guide](troubleshooting.md)
   - [Developer Guide](developer-guide.md)

2. **Project Issues:**
   - Search existing issues: https://github.com/your-org/OmniCPP-template/issues
   - Create new issue with gathered information
   - Include logs, error messages, and reproduction steps

3. **Community Resources:**
   - Discord server (if available)
   - Reddit community
   - Stack Overflow (tag with project name)

4. **External Resources:**
   - [CMake Documentation](https://cmake.org/documentation/)
   - [Conan Documentation](https://docs.conan.io/)
   - [GCC Documentation](https://gcc.gnu.org/onlinedocs/)
   - [Clang Documentation](https://clang.llvm.org/docs/)
   - [Arch Linux Wiki](https://wiki.archlinux.org/)
   - [CachyOS Wiki](https://wiki.cachyos.org/)

### Reporting Bugs

When reporting bugs, use this template:

```markdown
## Bug Report

### System Information
- **OS:** [e.g., CachyOS 2024.01]
- **Kernel:** [e.g., 6.6.8-cachyos]
- **Architecture:** [e.g., x86_64]
- **Compiler:** [e.g., GCC 13.2.0]
- **CMake:** [e.g., 3.31.0]
- **Conan:** [e.g., 2.0.17]

### Expected Behavior
[Describe what should happen]

### Actual Behavior
[Describe what actually happens]

### Steps to Reproduce
1. [First step]
2. [Second step]
3. [Third step]

### Error Messages
```bash
[Paste full error output here]
```

### Additional Context
- [Any additional context or screenshots]
- [Logs attached]
```

### Security Issues

If you discover a security vulnerability:

1. **Do NOT** create a public issue
2. **Do** follow the project's security disclosure policy
3. **Do** provide detailed information about the vulnerability
4. **Do** include steps to reproduce (if safe)
5. **Do** wait for confirmation before disclosing publicly

Refer to the [Threat Model](../.specs/03_threat_model/analysis.md) for known security considerations.

---

## Quick Reference

### Common Commands

```bash
# Validate environment
./scripts/linux/validate_environment.sh --verbose

# Clean build
rm -rf build/

# Configure with preset
cmake --preset cachyos-gcc-release

# Build with preset
cmake --build --preset cachyos-gcc-release

# Run tests
ctest --preset cachyos-gcc-debug

# Install dependencies
conan install . --profile cachyos

# Update system (CachyOS)
sudo pacman -Syu

# Check library dependencies
ldd ./build/debug/default/bin/WarploomStandalone

# View kernel messages
dmesg | tail -n 50

# Check system logs
journalctl -xe
```

### Environment Variables

```bash
# CachyOS optimizations
export CFLAGS="-march=native -O3 -flto -DNDEBUG"
export CXXFLAGS="-march=native -O3 -flto -DNDEBUG"
export LDFLAGS="-Wl,--as-needed -Wl,--no-undefined -flto"

# Qt6 platform
export QT_QPA_PLATFORM=wayland  # or xcb

# Vulkan SDK
export VULKAN_SDK="/path/to/VulkanSDK"
export VK_LAYER_PATH="$VULKAN_SDK/etc/vulkan/explicit_layer.d"
export LD_LIBRARY_PATH="$VULKAN_SDK/lib:$LD_LIBRARY_PATH"

# Conan
export CONAN_HOME="$HOME/.conan2"

# Nix
export NIX_PATH="nixpkgs=/path/to/nixpkgs"
```

### Useful Paths

```bash
# CMake cache
build/CMakeCache.txt

# CMake logs
build/CMakeFiles/CMakeError.log
build/CMakeFiles/CMakeOutput.log

# Conan cache
~/.conan2/cache

# Conan profiles
~/.conan2/profiles/
conan/profiles/

# Nix store
/nix/store

# Nix configuration
~/.config/nix/nix.conf

# CachyOS release files
/etc/cachyos-release
/usr/lib/cachyos-release

# Qt6 plugins
/usr/lib/qt6/plugins/  # Arch/CachyOS
/usr/lib/x86_64-linux-gnu/qt6/plugins/  # Ubuntu/Debian

# Vulkan SDK
$VULKAN_SDK
```

---

## Document Control

| Version | Date | Author | Changes |
|---------|------|---------|---------|
| 1.0 | 2026-01-28 | Initial version |

---

## References

- [ADR-028: CachyOS as Primary Linux Target](../.specs/02_adrs/ADR-028-cachyos-primary-linux-target.md)
- [ADR-027: Nix Package Manager Integration](../.specs/02_adrs/ADR-027-nix-package-manager-integration.md)
- [ADR-029: Direnv for Environment Management](../.specs/02_adrs/ADR-029-direnv-environment-management.md)
- [Threat Model Analysis](../.specs/03_threat_model/analysis.md)
- [Coding Standards](../.specs/01_standards/coding_standards.md)
- [Linux Builds](linux-builds.md)
- [CachyOS Builds](cachyos-builds.md)
- [General Troubleshooting](troubleshooting.md)
- [CachyOS Website](https://cachyos.org/)
- [CachyOS Wiki](https://wiki.cachyos.org/)
- [Arch Linux Wiki](https://wiki.archlinux.org/)
