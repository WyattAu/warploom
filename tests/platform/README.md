# Platform-Specific Tests

This directory contains platform-specific test files for the Warploom project. These tests are organized by platform and compiler to facilitate targeted testing across different operating systems and compiler configurations.

## Directory Structure

```
tests/platform/
├── test_linux_gcc.cpp      # Linux GCC platform test
├── test_linux_clang.cpp    # Linux Clang platform test
├── test_windows_msvc.cpp    # Windows MSVC platform test
└── test_windows_mingw.cpp   # Windows MinGW platform test
```

## Test Files

### Linux Tests

- **test_linux_gcc.cpp**: Tests basic C++ compilation on Linux with GCC compiler
- **test_linux_clang.cpp**: Tests basic C++ compilation on Linux with Clang compiler

### Windows Tests

- **test_windows_msvc.cpp**: Tests basic C++ compilation on Windows with MSVC (Microsoft Visual C++)
- **test_windows_mingw.cpp**: Tests basic C++ compilation on Windows with MinGW (GCC/Clang)

## Purpose

These platform-specific tests serve the following purposes:

1. **Compiler Verification**: Verify that the compiler toolchain is correctly configured and can compile basic C++ code
2. **Platform Compatibility**: Ensure that the codebase works across different platforms (Linux, Windows)
3. **Build System Validation**: Validate that the build system (CMake, Conan, etc.) correctly handles platform-specific configurations
4. **Quick Smoke Tests**: Provide quick compilation tests to verify environment setup

## Running Platform Tests

### Linux

```bash
# Compile with GCC
g++ -std=c++23 tests/platform/test_linux_gcc.cpp -o test_linux_gcc
./test_linux_gcc

# Compile with Clang
clang++ -std=c++23 tests/platform/test_linux_clang.cpp -o test_linux_clang
./test_linux_clang
```

### Windows (MSVC)

```cmd
cl /std:c++23 tests\platform\test_windows_msvc.cpp
test_windows_msvc.exe
```

### Windows (MinGW)

```cmd
g++ -std=c++23 tests\platform\test_windows_mingw.cpp -o test_windows_mingw.exe
test_windows_mingw.exe
```

## Integration with Build System

These test files are referenced by the build system configuration:

- **CMakeLists.txt**: Platform-specific test targets can be added here
- **CMakePresets.json**: Platform-specific build configurations
- **Conan Profiles**: Platform-specific compiler profiles in `conan/profiles/`

## References

- **ADR-028**: Linux Platform Support - Defines Linux platform support strategy
- **REQ-009**: Repository Cleanup - Defines test file organization requirements
- **Coding Standards**: [`.specs/01_standards/coding_standards.md`](../../.specs/01_standards/coding_standards.md)
- **Linux Expansion Manifest**: [`.specs/04_future_state/linux_expansion_manifest.md`](../../.specs/04_future_state/linux_expansion_manifest.md)

## Maintenance

When adding new platform support:

1. Create a new test file following the naming convention: `test_<platform>_<compiler>.cpp`
2. Update this README with the new test file
3. Update the build system configuration if needed
4. Reference the relevant ADR and requirement documents

## Notes

- These are basic compilation tests, not comprehensive unit tests
- For comprehensive testing, see the `tests/unit/` and `tests/integration/` directories
- Platform-specific build configurations are managed in `cmake/toolchains/`
- Compiler-specific profiles are in `conan/profiles/`
