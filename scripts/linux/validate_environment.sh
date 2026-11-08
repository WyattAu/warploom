#!/usr/bin/env bash
#
# Description: Validation script for Linux build environment
# Usage: ./scripts/linux/validate_environment.sh [options]
#
# Options:
#   --help        Show this help message
#   --version     Show script version
#   --verbose     Enable verbose output
#   --strict       Fail on warnings (treat warnings as errors)
#
# References:
#   - ADR-035: Linux Setup Script Architecture
#   - REQ-005-006: Create validate_environment.sh
#   - TM-LX-005: Linux Script Security Risks
#

set -euo pipefail

# Script metadata
readonly SCRIPT_VERSION="1.0.0"
readonly SCRIPT_NAME="$(basename "${BASH_SOURCE[0]}")"

# Configuration
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Minimum version requirements
readonly MIN_GCC_MAJOR=13
readonly MIN_GCC_MINOR=0
readonly MIN_CLANG_MAJOR=19
readonly MIN_CLANG_MINOR=0
readonly MIN_CMAKE_MAJOR=3
readonly MIN_CMAKE_MINOR=20
readonly MIN_CONAN_MAJOR=2
readonly MIN_CONAN_MINOR=0
readonly MIN_QT_MAJOR=6
readonly MIN_QT_MINOR=0
readonly MIN_VULKAN_MAJOR=1
readonly MIN_VULKAN_MINOR=3

# Validation counters
VALIDATION_PASSED=0
VALIDATION_FAILED=0
VALIDATION_WARNINGS=0

# Colors for output (use tput for compatibility)
if command -v tput >/dev/null 2>&1 && tput colors >/dev/null 2>&1; then
    RED="$(tput setaf 1)"
    GREEN="$(tput setaf 2)"
    YELLOW="$(tput setaf 3)"
    BLUE="$(tput setaf 4)"
    CYAN="$(tput setaf 6)"
    NC="$(tput sgr0)"
else
    RED=""
    GREEN=""
    YELLOW=""
    BLUE=""
    CYAN=""
    NC=""
fi

# Logging functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1" >&2
    ((VALIDATION_WARNINGS++))
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
    ((VALIDATION_FAILED++))
}

log_debug() {
    if [[ "${VERBOSE:-0}" == "1" ]]; then
        echo -e "${BLUE}[DEBUG]${NC} $1" >&2
    fi
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((VALIDATION_PASSED++))
}

log_section() {
    echo ""
    echo -e "${CYAN}=== $1 ===${NC}"
}

# Display usage information
show_usage() {
    cat << EOF
Usage: $SCRIPT_NAME [options]

Validate the Linux build environment for Warploom.

This script checks for:
  - Linux distribution detection
  - CachyOS detection
  - Nix environment detection
  - GCC/Clang compiler availability and version
  - Qt6 availability and version
  - Vulkan availability and version
  - CMake availability and version
  - Conan availability and version
  - Environment variables
  - CMake toolchains
  - Conan profiles

Options:
  --help        Show this help message
  --version     Show script version
  --verbose     Enable verbose output
  --strict       Fail on warnings (treat warnings as errors)

Exit Codes:
  0    All validations passed
  1    One or more validations failed
  2    Script error occurred

Examples:
  $SCRIPT_NAME                    # Run validation
  $SCRIPT_NAME --verbose          # Enable verbose output
  $SCRIPT_NAME --strict           # Treat warnings as errors

EOF
}

# Show script version
show_version() {
    echo "$SCRIPT_NAME version $SCRIPT_VERSION"
}

# Detect Linux distribution
detect_distro() {
    if [[ -f /etc/os-release ]]; then
        # Use grep to avoid sourcing potentially malicious content (TM-LX-005)
        local distro_id
        distro_id=$(grep -E '^ID=' /etc/os-release | cut -d= -f2 | tr -d '"')
        echo "$distro_id"
    else
        echo "unknown"
    fi
}

# Detect CachyOS specifically
detect_cachyos() {
    local distro
    distro=$(detect_distro)
    
    # Check for CachyOS ID
    if [[ "$distro" == "cachyos" ]]; then
        echo "true"
        return 0
    fi
    
    # Check for CachyOS in ID_LIKE
    if [[ -f /etc/os-release ]]; then
        local id_like
        id_like=$(grep -E '^ID_LIKE=' /etc/os-release | cut -d= -f2 | tr -d '"')
        if [[ "$id_like" == *"cachyos"* ]]; then
            echo "true"
            return 0
        fi
    fi
    
    # Check for CachyOS-specific files
    if [[ -f /etc/cachyos-release ]] || [[ -f /usr/lib/cachyos-release ]]; then
        echo "true"
        return 0
    fi
    
    echo "false"
    return 0
}

# Detect Nix environment
detect_nix() {
    # Check if Nix is installed
    if command -v nix >/dev/null 2>&1; then
        echo "true"
        return 0
    fi
    
    # Check for Nix store
    if [[ -d "/nix" ]] || [[ -d "$HOME/.nix-profile" ]]; then
        echo "true"
        return 0
    fi
    
    echo "false"
    return 0
}

# Parse version string into major and minor components
parse_version() {
    local version="$1"
    local major minor
    major=$(echo "$version" | cut -d. -f1)
    minor=$(echo "$version" | cut -d. -f2)
    echo "$major $minor"
}

# Compare versions (returns: 0=equal, 1=greater, 2=less)
compare_versions() {
    local v1="$1"
    local v2="$2"
    local i1 i2

    # Split by dots and compare each component
    IFS='.' read -ra i1 <<< "$v1"
    IFS='.' read -ra i2 <<< "$v2"

    for ((i=0; i<${#i1[@]} || i<${#i2[@]}; i++)); do
        local n1="${i1[i]:-0}"
        local n2="${i2[i]:-0}"
        if ((n1 > n2)); then
            return 1
        elif ((n1 < n2)); then
            return 2
        fi
    done
    return 0
}

# Validate version meets minimum requirements
validate_version() {
    local component="$1"
    local version="$2"
    local min_major="$3"
    local min_minor="$4"

    # Handle "unknown" or "not found" versions
    if [[ "$version" == "unknown" || "$version" == "not found" ]]; then
        log_warn "$component version cannot be determined"
        return 1
    fi

    log_debug "Validating $component version: $version (minimum required: $min_major.$min_minor.0)"

    local current_major current_minor
    read -r current_major current_minor <<< "$(parse_version "$version")"

    # Check major version
    if ((current_major < min_major)); then
        log_error "$component version $version is older than minimum required $min_major.$min_minor.0"
        return 1
    fi

    # Check minor version if major version matches
    if ((current_major == min_major)) && ((current_minor < min_minor)); then
        log_error "$component version $version is older than minimum required $min_major.$min_minor.0"
        return 1
    fi

    log_debug "$component version $version meets minimum requirements"
    return 0
}

# Validate Linux distribution
validate_distro() {
    log_section "Linux Distribution"
    
    local distro
    distro=$(detect_distro)
    
    if [[ "$distro" == "unknown" ]]; then
        log_error "Unable to detect Linux distribution"
        log_warn "/etc/os-release not found or unreadable"
        return 1
    fi
    
    log_pass "Detected distribution: $distro"
    
    # Display additional distribution info if available
    if [[ -f /etc/os-release ]]; then
        local pretty_name
        pretty_name=$(grep -E '^PRETTY_NAME=' /etc/os-release | cut -d= -f2 | tr -d '"')
        log_info "Distribution name: $pretty_name"
        
        local version_id
        version_id=$(grep -E '^VERSION_ID=' /etc/os-release | cut -d= -f2 | tr -d '"')
        if [[ -n "$version_id" ]]; then
            log_info "Distribution version: $version_id"
        fi
    fi
    
    return 0
}

# Validate CachyOS detection
validate_cachyos() {
    log_section "CachyOS Detection"
    
    local is_cachyos
    is_cachyos=$(detect_cachyos)
    
    if [[ "$is_cachyos" == "true" ]]; then
        log_pass "CachyOS detected"
        log_info "CachyOS-specific optimizations are available"
        
        # Check for CachyOS-specific packages
        if command -v pacman >/dev/null 2>&1; then
            local cachyos_packages
            cachyos_packages=$(pacman -Q cachyos-kernel 2>/dev/null || echo "")
            if [[ -n "$cachyos_packages" ]]; then
                log_info "CachyOS kernel installed: $cachyos_packages"
            fi
        fi
    else
        log_info "Not running on CachyOS"
        log_debug "CachyOS-specific optimizations will not be applied"
    fi
    
    return 0
}

# Validate Nix environment
validate_nix() {
    log_section "Nix Environment"
    
    local is_nix
    is_nix=$(detect_nix)
    
    if [[ "$is_nix" == "true" ]]; then
        log_pass "Nix environment detected"
        
        # Check Nix version
        if command -v nix >/dev/null 2>&1; then
            local nix_version
            nix_version=$(nix --version 2>/dev/null | grep -oP 'nix \(Nix\) \K[0-9.]+' || echo "unknown")
            log_info "Nix version: $nix_version"
            
            if [[ "$nix_version" != "unknown" ]]; then
                if validate_version "Nix" "$nix_version" "$MIN_NIX_MAJOR" "$MIN_NIX_MINOR"; then
                    log_pass "Nix version meets minimum requirements"
                else
                    log_error "Nix version does not meet minimum requirements"
                fi
            fi
        fi
        
        # Check for Nix flakes
        if [[ -f "$PROJECT_ROOT/flake.nix" ]]; then
            log_pass "flake.nix found in project root"
        else
            log_warn "flake.nix not found in project root"
        fi
        
        # Check Nix configuration
        local nix_conf_dir="${XDG_CONFIG_HOME:-$HOME/.config}/nix"
        if [[ -f "$nix_conf_dir/nix.conf" ]]; then
            log_pass "Nix configuration file found: $nix_conf_dir/nix.conf"
            
            # Check for flakes configuration
            if grep -q "experimental-features.*flakes" "$nix_conf_dir/nix.conf" 2>/dev/null; then
                log_pass "Nix flakes are enabled"
            else
                log_warn "Nix flakes may not be enabled in configuration"
            fi
        else
            log_warn "Nix configuration file not found: $nix_conf_dir/nix.conf"
        fi
    else
        log_info "Nix environment not detected"
        log_debug "Nix-specific features will not be available"
    fi
    
    return 0
}

# Validate GCC compiler
validate_gcc() {
    log_section "GCC Compiler"
    
    if ! command -v gcc >/dev/null 2>&1; then
        log_error "GCC not found in PATH"
        return 1
    fi
    
    # Get GCC version
    local gcc_version
    gcc_version=$(gcc --version | head -n1 | awk '{print $NF}')
    log_info "GCC version: $gcc_version"
    
    # Validate GCC version
    if validate_version "GCC" "$gcc_version" "$MIN_GCC_MAJOR" "$MIN_GCC_MINOR"; then
        log_pass "GCC version meets minimum requirements"
    else
        log_error "GCC version does not meet minimum requirements"
        return 1
    fi
    
    # Check g++
    if command -v g++ >/dev/null 2>&1; then
        local gxx_version
        gxx_version=$(g++ --version | head -n1 | awk '{print $NF}')
        log_pass "G++ found: $gxx_version"
    else
        log_error "G++ not found in PATH"
        return 1
    fi
    
    # Test compilation
    local test_file="/tmp/gcc_test_$$.c"
    local test_output="/tmp/gcc_test_$$.out"
    
    echo '#include <stdio.h>' > "$test_file"
    echo 'int main() { printf("Hello from GCC\\n"); return 0; }' >> "$test_file"
    
    if gcc "$test_file" -o "$test_output" 2>/dev/null; then
        log_pass "GCC can compile C programs"
        rm -f "$test_file" "$test_output"
    else
        log_error "GCC compilation test failed"
        rm -f "$test_file" "$test_output"
        return 1
    fi
    
    return 0
}

# Validate Clang compiler
validate_clang() {
    log_section "Clang Compiler"
    
    if ! command -v clang >/dev/null 2>&1; then
        log_warn "Clang not found in PATH (optional)"
        return 0
    fi
    
    # Get Clang version
    local clang_version
    clang_version=$(clang --version | head -n1 | sed -n 's/.*version \([0-9.]*\).*/\1/p')
    log_info "Clang version: $clang_version"
    
    # Validate Clang version
    if [[ "$clang_version" != "unknown" ]]; then
        if validate_version "Clang" "$clang_version" "$MIN_CLANG_MAJOR" "$MIN_CLANG_MINOR"; then
            log_pass "Clang version meets minimum requirements"
        else
            log_error "Clang version does not meet minimum requirements"
            return 1
        fi
    fi
    
    # Check clang++
    if command -v clang++ >/dev/null 2>&1; then
        local clangxx_version
        clangxx_version=$(clang++ --version | head -n1 | sed -n 's/.*version \([0-9.]*\).*/\1/p')
        log_pass "Clang++ found: $clangxx_version"
    else
        log_warn "Clang++ not found in PATH"
    fi
    
    # Test compilation
    local test_file="/tmp/clang_test_$$.c"
    local test_output="/tmp/clang_test_$$.out"
    
    echo '#include <stdio.h>' > "$test_file"
    echo 'int main() { printf("Hello from Clang\\n"); return 0; }' >> "$test_file"
    
    if clang "$test_file" -o "$test_output" 2>/dev/null; then
        log_pass "Clang can compile C programs"
        rm -f "$test_file" "$test_output"
    else
        log_error "Clang compilation test failed"
        rm -f "$test_file" "$test_output"
        return 1
    fi
    
    return 0
}

# Validate Qt6 installation
validate_qt6() {
    log_section "Qt6 Installation"
    
    local qmake_cmd=""
    if command -v qmake6 >/dev/null 2>&1; then
        qmake_cmd="qmake6"
    elif command -v qmake >/dev/null 2>&1; then
        qmake_cmd="qmake"
    fi
    
    if [[ -z "$qmake_cmd" ]]; then
        log_warn "Qt6 not found in PATH (optional for non-GUI builds)"
        return 0
    fi
    
    log_pass "Qt6 found: $qmake_cmd"
    
    # Get Qt6 version
    local qt6_version
    qt6_version=$("$qmake_cmd" -v 2>&1 | grep -oP 'Qt version \K[0-9.]+' || echo "unknown")
    log_info "Qt6 version: $qt6_version"
    
    # Validate Qt6 version
    if [[ "$qt6_version" != "unknown" ]]; then
        if validate_version "Qt6" "$qt6_version" "$MIN_QT_MAJOR" "$MIN_QT_MINOR"; then
            log_pass "Qt6 version meets minimum requirements"
        else
            log_error "Qt6 version does not meet minimum requirements"
            return 1
        fi
    fi
    
    # Check Qt6 modules
    local qt6_modules=()
    qt6_modules+=("QtCore")
    qt6_modules+=("QtGui")
    qt6_modules+=("QtWidgets")
    
    for module in "${qt6_modules[@]}"; do
        if "$qmake_cmd" -query QT_INSTALL_LIBS 2>/dev/null | grep -q "$module"; then
            log_debug "Qt6 module found: $module"
        else
            log_warn "Qt6 module may be missing: $module"
        fi
    done
    
    return 0
}

# Validate Vulkan installation
validate_vulkan() {
    log_section "Vulkan Installation"
    
    if ! command -v vulkaninfo >/dev/null 2>&1; then
        log_warn "Vulkan not found in PATH (optional for non-GPU builds)"
        return 0
    fi
    
    log_pass "Vulkan found: vulkaninfo"
    
    # Get Vulkan version
    local vulkan_version
    vulkan_version=$(vulkaninfo --summary 2>/dev/null | grep -oP 'Vulkan Instance Version: \K[0-9.]+' || echo "unknown")
    log_info "Vulkan version: $vulkan_version"
    
    # Validate Vulkan version
    if [[ "$vulkan_version" != "unknown" ]]; then
        if validate_version "Vulkan" "$vulkan_version" "$MIN_VULKAN_MAJOR" "$MIN_VULKAN_MINOR"; then
            log_pass "Vulkan version meets minimum requirements"
        else
            log_error "Vulkan version does not meet minimum requirements"
            return 1
        fi
    fi
    
    # Check Vulkan validation layers
    local vk_layer_path="${VK_LAYER_PATH:-/usr/share/vulkan/explicit_layer.d}"
    if [[ -d "$vk_layer_path" ]]; then
        local layer_count
        layer_count=$(find "$vk_layer_path" -name "*.json" 2>/dev/null | wc -l)
        if ((layer_count > 0)); then
            log_pass "Vulkan validation layers found: $layer_count layers"
        else
            log_warn "No Vulkan validation layers found in $vk_layer_path"
        fi
    else
        log_warn "Vulkan layer path not found: $vk_layer_path"
    fi
    
    return 0
}

# Validate CMake installation
validate_cmake() {
    log_section "CMake Installation"
    
    if ! command -v cmake >/dev/null 2>&1; then
        log_error "CMake not found in PATH"
        return 1
    fi
    
    # Get CMake version
    local cmake_version
    cmake_version=$(cmake --version | grep -oP 'cmake version \K[0-9.]+' || echo "unknown")
    log_info "CMake version: $cmake_version"
    
    # Validate CMake version
    if [[ "$cmake_version" != "unknown" ]]; then
        if validate_version "CMake" "$cmake_version" "$MIN_CMAKE_MAJOR" "$MIN_CMAKE_MINOR"; then
            log_pass "CMake version meets minimum requirements"
        else
            log_error "CMake version does not meet minimum requirements"
            return 1
        fi
    fi
    
    # Check for Ninja generator
    if command -v ninja >/dev/null 2>&1; then
        local ninja_version
        ninja_version=$(ninja --version 2>/dev/null || echo "unknown")
        log_pass "Ninja found: $ninja_version"
    else
        log_warn "Ninja not found in PATH (recommended for faster builds)"
    fi
    
    return 0
}

# Validate Conan installation
validate_conan() {
    log_section "Conan Installation"
    
    if ! command -v conan >/dev/null 2>&1; then
        log_warn "Conan not found in PATH (optional if using other package managers)"
        return 0
    fi
    
    # Get Conan version
    local conan_version
    conan_version=$(conan --version 2>/dev/null | grep -oP 'Conan version \K[0-9.]+' || echo "unknown")
    log_info "Conan version: $conan_version"
    
    # Validate Conan version
    if [[ "$conan_version" != "unknown" ]]; then
        if validate_version "Conan" "$conan_version" "$MIN_CONAN_MAJOR" "$MIN_CONAN_MINOR"; then
            log_pass "Conan version meets minimum requirements"
        else
            log_error "Conan version does not meet minimum requirements"
            return 1
        fi
    fi
    
    # Check Conan home directory
    local conan_home="${CONAN_USER_HOME:-$HOME/.conan2}"
    if [[ -d "$conan_home" ]]; then
        log_pass "Conan home directory found: $conan_home"
    else
        log_warn "Conan home directory not found: $conan_home"
    fi
    
    return 0
}

# Validate environment variables
validate_environment_variables() {
    log_section "Environment Variables"
    
    local env_vars=()
    env_vars+=("CC")
    env_vars+=("CXX")
    env_vars+=("CFLAGS")
    env_vars+=("CXXFLAGS")
    env_vars+=("LDFLAGS")
    
    for var in "${env_vars[@]}"; do
        if [[ -n "${!var:-}" ]]; then
            log_pass "$var is set: ${!var}"
        else
            log_debug "$var is not set (will use defaults)"
        fi
    done
    
    # Check for Qt6 platform
    if [[ -n "${QT_QPA_PLATFORM:-}" ]]; then
        log_pass "QT_QPA_PLATFORM is set: $QT_QPA_PLATFORM"
    else
        log_debug "QT_QPA_PLATFORM is not set (will use default)"
    fi
    
    # Check for Vulkan paths
    if [[ -n "${VK_LAYER_PATH:-}" ]]; then
        log_pass "VK_LAYER_PATH is set: $VK_LAYER_PATH"
    else
        log_debug "VK_LAYER_PATH is not set (will use defaults)"
    fi
    
    if [[ -n "${VK_ICD_FILENAMES:-}" ]]; then
        log_pass "VK_ICD_FILENAMES is set"
    else
        log_debug "VK_ICD_FILENAMES is not set (will use defaults)"
    fi
    
    return 0
}

# Validate CMake toolchains
validate_cmake_toolchains() {
    log_section "CMake Toolchains"
    
    local toolchain_dir="$PROJECT_ROOT/cmake/toolchains"
    
    if [[ ! -d "$toolchain_dir" ]]; then
        log_warn "CMake toolchains directory not found: $toolchain_dir"
        return 0
    fi
    
    log_pass "CMake toolchains directory found: $toolchain_dir"
    
    # Check for specific toolchain files
    local toolchain_files=()
    toolchain_files+=("gcc-linux.cmake")
    toolchain_files+=("clang-linux.cmake")
    toolchain_files+=("cachyos.cmake")
    toolchain_files+=("nix.cmake")
    
    for file in "${toolchain_files[@]}"; do
        local toolchain_file="$toolchain_dir/$file"
        if [[ -f "$toolchain_file" ]]; then
            log_pass "Toolchain file found: $file"
        else
            log_debug "Toolchain file not found: $file (may not be needed)"
        fi
    done
    
    return 0
}

# Validate Conan profiles
validate_conan_profiles() {
    log_section "Conan Profiles"
    
    local profiles_dir="$PROJECT_ROOT/conan/profiles"
    
    if [[ ! -d "$profiles_dir" ]]; then
        log_warn "Conan profiles directory not found: $profiles_dir"
        return 0
    fi
    
    log_pass "Conan profiles directory found: $profiles_dir"
    
    # Check for specific profile files
    local profile_files=()
    profile_files+=("gcc-linux")
    profile_files+=("clang-linux")
    profile_files+=("cachyos")
    profile_files+=("cachyos-clang")
    profile_files+=("nix")
    
    for file in "${profile_files[@]}"; do
        local profile_file="$profiles_dir/$file"
        if [[ -f "$profile_file" ]]; then
            log_pass "Conan profile found: $file"
        else
            log_debug "Conan profile not found: $file (may not be needed)"
        fi
    done
    
    # Check if default profile exists
    if command -v conan >/dev/null 2>&1; then
        if conan profile show default >/dev/null 2>&1; then
            log_pass "Default Conan profile is configured"
        else
            log_warn "Default Conan profile not found"
        fi
    fi
    
    return 0
}

# Display validation summary
display_summary() {
    echo ""
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN}Validation Summary${NC}"
    echo -e "${CYAN}========================================${NC}"
    echo ""
    echo -e "${GREEN}Passed: $VALIDATION_PASSED${NC}"
    echo -e "${YELLOW}Warnings: $VALIDATION_WARNINGS${NC}"
    echo -e "${RED}Failed: $VALIDATION_FAILED${NC}"
    echo ""
    
    if ((VALIDATION_FAILED == 0)); then
        echo -e "${GREEN}All critical validations passed!${NC}"
        if ((VALIDATION_WARNINGS > 0)); then
            echo -e "${YELLOW}There are $VALIDATION_WARNINGS warning(s) that should be reviewed.${NC}"
        fi
        return 0
    else
        echo -e "${RED}Validation failed with $VALIDATION_FAILED error(s)${NC}"
        echo ""
        echo "Please fix the errors above before proceeding with the build."
        return 1
    fi
}

# Main validation function
main() {
    local verbose=false
    local strict=false
    
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --help)
                show_usage
                exit 0
                ;;
            --version)
                show_version
                exit 0
                ;;
            --verbose)
                verbose=true
                VERBOSE=1
                shift
                ;;
            --strict)
                strict=true
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                show_usage
                exit 2
                ;;
        esac
    done
    
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN}Warploom Environment Validation${NC}"
    echo -e "${CYAN}========================================${NC}"
    echo ""
    
    # Run all validations
    validate_distro || true
    validate_cachyos || true
    validate_nix || true
    validate_gcc || true
    validate_clang || true
    validate_qt6 || true
    validate_vulkan || true
    validate_cmake || true
    validate_conan || true
    validate_environment_variables || true
    validate_cmake_toolchains || true
    validate_conan_profiles || true
    
    # Display summary and exit
    display_summary
}

# Run main function
main "$@"
