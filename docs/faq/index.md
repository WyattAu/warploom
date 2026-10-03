# Frequently Asked Questions

Welcome to the OmniCPP-template FAQ. This section provides quick answers to common questions about the project, build system, and development workflow.

## Quick Navigation

- [Common Questions](common-questions.md) - General questions about the project
- [Build Questions](build-questions.md) - Build system and compilation issues
- [Performance Questions](performance-questions.md) - Performance optimization and profiling

## Getting Started

### What is OmniCPP-template?

**TL;DR:** OmniCPP-template is a C++23 best practice template with a game engine and game example monorepo.

**The Deep Dive:**
OmniCPP-template is a comprehensive C++23 project template that demonstrates modern C++ best practices, including:
- A modular game engine architecture with ECS (Entity Component System)
- Cross-platform build system supporting Windows, Linux, and WebAssembly
- Multiple compiler support (MSVC, MSVC-Clang, MinGW-GCC, MinGW-Clang, GCC, Clang)
- Integrated package management (Conan, vcpkg, CPM.cmake)
- Code quality tools (clang-format, clang-tidy, pylint, mypy)

**The Analogy:**
Think of OmniCPP-template as a "starter kit" for C++ game development—like a pre-assembled LEGO set where all the pieces are designed to work together, but you can still customize and extend it however you want.

### How do I get started?

**TL;DR:** Run `python OmniCppController.py build standalone "Clean Build Pipeline" default release` to build the project.

**The Deep Dive:**
The project uses [`OmniCppController.py`](../../OmniCppController.py:1) as the main entry point for all build operations. The controller manages:
- Compiler detection and validation
- Dependency installation via Conan/vcpkg
- CMake configuration and building
- Code formatting and linting

**Quick Start Commands:**
```bash
# Build standalone executable
python OmniCppController.py build standalone "Clean Build Pipeline" default release

# Build engine library
python OmniCppController.py build engine "Clean Build Pipeline" default release

# Build everything
python OmniCppController.py build all "Clean Build Pipeline" default release
```

### What are the system requirements?

**TL;DR:** Python 3.8+, CMake 3.20+, and a C++ compiler (MSVC, GCC, or Clang).

**The Deep Dive:**

| Requirement | Minimum Version | Recommended |
|-------------|-----------------|--------------|
| Python | 3.8 | 3.10+ |
| CMake | 3.20 | 3.25+ |
| C++ Compiler | C++17 support | C++23 support |
| Git | 2.0 | Latest |

**Platform-Specific Requirements:**

**Windows:**
- Visual Studio 2019+ (for MSVC)
- Windows SDK 10+
- Optional: MSYS2 (for MinGW compilers)

**Linux:**
- GCC 9+ or Clang 10+
- POSIX-compliant shell
- Build-essential packages

**macOS:**
- Xcode Command Line Tools
- Homebrew (for dependencies)

### Where can I find more help?

- [Troubleshooting Guide](../troubleshooting.md) - Detailed solutions to common issues
- [User Guide - Build System](../user-guide-build-system.md) - Comprehensive build system documentation
- [Developer Guide](../developer-guide.md) - Development workflow and contribution guidelines
- [GitHub Issues](https://github.com/your-repo/issues) - Report bugs and request features
