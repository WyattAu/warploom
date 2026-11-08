# Compiler Detection System Documentation

**Version:** 1.0.0  
**Date:** 2026-01-06  
**Status:** Production Ready

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
3. [Compiler Detectors](#3-compiler-detectors)
4. [Terminal Detectors](#4-terminal-detectors)
5. [Cross-Compiler Detection](#5-cross-compiler-detection)
6. [Utility Classes](#6-utility-classes)
7. [Usage Examples](#7-usage-examples)
8. [Troubleshooting Guide](#8-troubleshooting-guide)
9. [API Reference](#9-api-reference)
10. [Configuration](#10-configuration)

---

## 1. Overview

### 1.1 Purpose

The Compiler Detection System provides a unified, extensible framework for detecting, validating, and managing C++ compilers on Windows. It supports multiple compiler families (MSVC, MinGW-GCC, MinGW-Clang, MSVC-Clang), terminal environments, and cross-compilation toolchains.

### 1.2 Key Features

- **Multi-Compiler Support:** Detects MSVC, MSVC-Clang, MinGW-GCC, and MinGW-Clang compilers
- **Automatic Detection:** Discovers compilers via multiple methods (vswhere, registry, package managers)
- **Version Detection:** Extracts and validates compiler versions
- **Capability Detection:** Identifies C++ standard support (C++14, C++17, C++20, C++23)
- **Terminal Integration:** Maps compilers to appropriate terminal environments
- **Cross-Compilation:** Supports Linux, WASM, and Android cross-compilation
- **Environment Setup:** Automatically configures build environments
- **Caching:** Optimizes performance with intelligent caching
- **Error Handling:** Comprehensive error handling with recovery suggestions

### 1.3 Supported Compilers

| Compiler Type | Description                   | Supported Architectures |
| ------------- | ----------------------------- | ----------------------- |
| MSVC          | Microsoft Visual C++ Compiler | x64, x86, arm, arm64    |
| MSVC-Clang    | Clang with MSVC libraries     | x64, x86                |
| MinGW-GCC     | GCC for Windows (MinGW)       | x64, x86                |
| MinGW-Clang   | Clang for Windows (MinGW)     | x64, x86                |

### 1.4 Supported Terminals

| Terminal Type                 | Description                 | Recommended For        |
| ----------------------------- | --------------------------- | ---------------------- |
| MSVC Developer Command Prompt | VS Developer Command Prompt | MSVC, MSVC-Clang       |
| MSVC x64 Native               | Native x64 Command Prompt   | MSVC x64 builds        |
| MSVC x86 Native               | Native x86 Command Prompt   | MSVC x86 builds        |
| MSYS2 UCRT64                  | UCRT64 Shell                | MinGW-GCC, MinGW-Clang |
| MSYS2 MINGW64                 | MINGW64 Shell               | MinGW-GCC, MinGW-Clang |
| MSYS2 MINGW32                 | MINGW32 Shell               | MinGW-GCC 32-bit       |
| MSYS2 CLANG64                 | CLANG64 Shell               | MinGW-Clang            |

---

## 2. Architecture

### 2.1 System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Compiler Detection System                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │              Compiler Detection Layer                    │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐   │  │
│  │  │ MSVC Detector│  │MinGW Detector│  │LLVM Detector │   │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘   │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │              Compiler Factory Layer                       │  │
│  │  ┌──────────────────────────────────────────────────┐   │  │
│  │  │         CompilerFactory                          │   │  │
│  │  │  - create_compiler()                             │   │  │
│  │  │  - get_available_compilers()                     │   │  │
│  │  │  - select_best_compiler()                        │   │  │
│  │  └──────────────────────────────────────────────────┘   │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │              Compiler Manager Layer                      │  │
│  │  ┌──────────────────────────────────────────────────┐   │  │
│  │  │         CompilerManager                          │   │  │
│  │  │  - detect_all_compilers()                        │   │  │
│  │  │  - get_compiler_info()                          │   │  │
│  │  │  - validate_compiler()                          │   │  │
│  │  └──────────────────────────────────────────────────┘   │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │              Terminal Detection Layer                   │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐   │  │
│  │  │MSVC Terminal │  │MSYS2 Terminal│  │Generic Term. │   │  │
│  │  │  Detector    │  │   Detector   │  │   Detector   │   │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘   │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │              Cross-Compilation Layer                    │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐   │  │
│  │  │Linux Cross   │  │WASM Cross    │  │Android Cross │   │  │
│  │  │Compiler      │  │Compiler      │  │Compiler      │   │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘   │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Layer Responsibilities

| Layer              | Responsibility                                | Key Classes                                                        |
| ------------------ | --------------------------------------------- | ------------------------------------------------------------------ |
| Compiler Detection | Discover compilers in all standard locations  | MSVCDetector, MinGWDetector, MSVCClangDetector, MinGWClangDetector |
| Compiler Factory   | Create and manage compiler instances          | CompilerFactory                                                    |
| Compiler Manager   | Coordinate detection and validation           | CompilerManager                                                    |
| Terminal Detection | Discover developer terminals                  | MSVCTerminalDetector, MSYS2TerminalDetector                        |
| Terminal Invoker   | Execute commands in terminals                 | TerminalInvoker                                                    |
| Cross-Compilation  | Detect and setup cross-compilation toolchains | LinuxCrossCompiler, WASMCrossCompiler, AndroidCrossCompiler        |
| Integration        | Map compilers to terminals                    | CompilerTerminalMapper                                             |

### 2.3 Design Principles

1. **Separation of Concerns:** Each layer has a single, well-defined responsibility
2. **Dependency Inversion:** High-level modules don't depend on low-level modules
3. **Open/Closed:** Open for extension, closed for modification
4. **Interface Segregation:** Clients don't depend on interfaces they don't use
5. **Single Responsibility:** Each class has one reason to change

---

## 3. Compiler Detectors

### 3.1 MSVC Detector

#### Overview

The MSVC Detector discovers Microsoft Visual C++ compilers installed on the system. It uses multiple detection methods to ensure comprehensive coverage.

#### Detection Methods

1. **vswhere Detection:** Uses Visual Studio's `vswhere.exe` utility to discover installations
2. **Standard Paths Detection:** Searches standard installation directories
3. **Registry Detection:** Queries Windows Registry for installation information
4. **Package Manager Detection:** Checks Chocolatey, Scoop, and winget installations

#### Supported Versions

- Visual Studio 2022 (MSVC 19.3x)
- Visual Studio 2019 (MSVC 19.2x)
- Visual Studio 2017 (MSVC 19.1x)

#### Architecture Support

- **x64:** 64-bit native compilation
- **x86:** 32-bit native compilation
- **arm:** ARM cross-compilation from x64
- **arm64:** ARM64 cross-compilation from x64

#### Detection Process

```python
# Example: Detect MSVC compilers
from warploom_scripts.compilers.msvc_detector import MSVCDetector

detector = MSVCDetector()
compilers = detector.detect()

for compiler in compilers:
    print(f"Found MSVC {compiler.version} at {compiler.path}")
    print(f"  Architecture: {compiler.architecture}")
    print(f"  Capabilities: {compiler.capabilities}")
```

#### Version Detection

The detector extracts version information from:

- Compiler executable (`cl.exe`)
- Visual Studio installation metadata
- Windows SDK version

#### Capability Detection

Automatically detects support for:

- C++23 modules
- C++20 concepts, ranges, coroutines
- C++17 structured bindings, if constexpr
- C++14 generic lambdas

#### Validation

The detector validates:

- Executable existence and accessibility
- Version compatibility
- Windows SDK availability
- Architecture support

### 3.2 MSVC-Clang Detector

#### Overview

The MSVC-Clang Detector discovers Clang compilers that use MSVC libraries and headers. This provides the Clang frontend with MSVC's standard library.

#### Detection Methods

1. **Bundled LLVM Detection:** Detects LLVM bundled with Visual Studio
2. **Standalone LLVM Detection:** Detects standalone LLVM installations
3. **Package Manager Detection:** Checks Chocolatey, Scoop, and winget

#### Supported Versions

- Clang 18.x
- Clang 17.x
- Clang 16.x
- Clang 15.x

#### Architecture Support

- **x64:** 64-bit native compilation
- **x86:** 32-bit native compilation

#### Detection Process

```python
# Example: Detect MSVC-Clang compilers
from warploom_scripts.compilers.msvc_clang_detector import MSVCClangDetector

detector = MSVCClangDetector()
compilers = detector.detect()

for compiler in compilers:
    print(f"Found MSVC-Clang {compiler.version} at {compiler.path}")
    print(f"  Architecture: {compiler.architecture}")
    print(f"  MSVC Compatibility: {compiler.capabilities.msvc_compatibility}")
```

#### Version Detection

Extracts version from:

- `clang.exe --version` output
- LLVM installation metadata

#### Capability Detection

Detects:

- C++23 modules
- C++20 concepts, ranges, coroutines
- MSVC standard library compatibility
- MSVC ABI compatibility

### 3.3 MinGW-GCC Detector

#### Overview

The MinGW-GCC Detector discovers GCC compilers for Windows via MinGW distributions.

#### Detection Methods

1. **MSYS2 Detection:** Detects GCC in MSYS2 environments (UCRT64, MINGW64, MINGW32)
2. **Standalone Detection:** Detects standalone MinGW installations
3. **TDM-GCC Detection:** Detects TDM-GCC distributions
4. **Package Manager Detection:** Checks Chocolatey, Scoop, and winget

#### Supported Versions

- GCC 14.x
- GCC 13.x
- GCC 12.x
- GCC 11.x

#### Architecture Support

- **x64:** 64-bit native compilation
- **x86:** 32-bit native compilation

#### Environment Support

- **UCRT64:** Universal C Runtime (recommended)
- **MINGW64:** MSVCRT 64-bit
- **MINGW32:** MSVCRT 32-bit

#### Detection Process

```python
# Example: Detect MinGW-GCC compilers
from warploom_scripts.compilers.mingw_gcc_detector import MinGWDetector

detector = MinGWDetector()
compilers = detector.detect()

for compiler in compilers:
    print(f"Found MinGW-GCC {compiler.version} at {compiler.path}")
    print(f"  Architecture: {compiler.architecture}")
    print(f"  Environment: {compiler.metadata.get('environment')}")
    print(f"  MSYS2 Path: {compiler.metadata.get('msys2_path')}")
```

#### Version Detection

Extracts version from:

- `gcc.exe --version` output
- MSYS2 package metadata

#### Capability Detection

Detects:

- C++23 modules
- C++20 concepts, ranges, coroutines
- C++17 structured bindings
- MinGW-specific features

### 3.4 MinGW-Clang Detector

#### Overview

The MinGW-Clang Detector discovers Clang compilers for Windows via MinGW distributions.

#### Detection Methods

1. **MSYS2 Detection:** Detects Clang in MSYS2 environments (CLANG64, UCRT64)
2. **Standalone Detection:** Detects standalone MinGW-Clang installations
3. **Package Manager Detection:** Checks Chocolatey, Scoop, and winget

#### Supported Versions

- Clang 18.x
- Clang 17.x
- Clang 16.x

#### Architecture Support

- **x64:** 64-bit native compilation
- **x86:** 32-bit native compilation

#### Environment Support

- **CLANG64:** Clang 64-bit (recommended)
- **UCRT64:** Universal C Runtime

#### Detection Process

```python
# Example: Detect MinGW-Clang compilers
from warploom_scripts.compilers.mingw_clang_detector import MinGWClangDetector

detector = MinGWClangDetector()
compilers = detector.detect()

for compiler in compilers:
    print(f"Found MinGW-Clang {compiler.version} at {compiler.path}")
    print(f"  Architecture: {compiler.architecture}")
    print(f"  Environment: {compiler.metadata.get('environment')}")
```

#### Version Detection

Extracts version from:

- `clang.exe --version` output
- MSYS2 package metadata

#### Capability Detection

Detects:

- C++23 modules
- C++20 concepts, ranges, coroutines
- MinGW standard library compatibility
- POSIX compatibility layer

---

## 4. Terminal Detectors

### 4.1 MSVC Terminal Detector

#### Overview

The MSVC Terminal Detector discovers Visual Studio Developer Command Prompts and native tool command prompts.

#### Detected Terminals

| Terminal ID     | Name                                 | Description                  | Architecture |
| --------------- | ------------------------------------ | ---------------------------- | ------------ |
| developer_cmd   | Developer Command Prompt             | VS Developer Command Prompt  | All          |
| x64_native      | x64 Native Tools Command Prompt      | Native x64 build environment | x64          |
| x86_native      | x86 Native Tools Command Prompt      | Native x86 build environment | x86          |
| x86_x64_cross   | x86_x64 Cross Tools Command Prompt   | Cross-compile x64 from x86   | x64          |
| x64_x86_cross   | x64_x86 Cross Tools Command Prompt   | Cross-compile x86 from x64   | x86          |
| x64_arm_cross   | x64_arm Cross Tools Command Prompt   | Cross-compile ARM from x64   | arm          |
| x64_arm64_cross | x64_arm64 Cross Tools Command Prompt | Cross-compile ARM64 from x64 | arm64        |

#### Detection Process

```python
# Example: Detect MSVC terminals
from warploom_scripts.compilers.msvc_terminal_detector import MSVCTerminalDetector

detector = MSVCTerminalDetector()
terminals = detector.detect()

for terminal in terminals:
    print(f"Found {terminal.name}")
    print(f"  Executable: {terminal.executable}")
    print(f"  Architecture: {terminal.architecture}")
    print(f"  Recommended: {terminal.recommended}")
```

#### Terminal Invocation

The detector provides terminal information for:

- Executable path (`cmd.exe` or `powershell.exe`)
- Arguments (vcvarsall.bat architecture)
- Environment setup requirements

### 4.2 MSYS2 Terminal Detector

#### Overview

The MSYS2 Terminal Detector discovers MSYS2 shell environments for MinGW compilers.

#### Detected Terminals

| Terminal ID | Name          | Description                | Environment |
| ----------- | ------------- | -------------------------- | ----------- |
| ucrt64      | UCRT64 Shell  | Universal C Runtime 64-bit | UCRT64      |
| mingw64     | MINGW64 Shell | MSVCRT 64-bit              | MINGW64     |
| mingw32     | MINGW32 Shell | MSVCRT 32-bit              | MINGW32     |
| msys        | MSYS Shell    | POSIX compatibility        | MSYS        |
| clang64     | CLANG64 Shell | Clang 64-bit               | CLANG64     |

#### Detection Process

```python
# Example: Detect MSYS2 terminals
from warploom_scripts.compilers.msys2_terminal_detector import MSYS2TerminalDetector

detector = MSYS2TerminalDetector()
terminals = detector.detect()

for terminal in terminals:
    print(f"Found {terminal.name}")
    print(f"  Executable: {terminal.executable}")
    print(f"  Environment: {terminal.environment}")
    print(f"  MSYS2 Path: {terminal.metadata.get('msys2_path')}")
```

#### Environment Variables

The detector sets up:

- `MSYSTEM`: Environment type (UCRT64, MINGW64, etc.)
- `MINGW_PREFIX`: Prefix path for MinGW
- `PATH`: Updated with appropriate bin directories

---

## 5. Cross-Compiler Detection

### 5.1 Linux Cross-Compiler

#### Overview

The Linux Cross-Compiler detects toolchains for cross-compiling to Linux from Windows.

#### Supported Architectures

- **x86_64-linux-gnu:** 64-bit Linux
- **aarch64-linux-gnu:** ARM64 Linux

#### Detection Process

```python
# Example: Detect Linux cross-compiler
from warploom_scripts.compilers.linux_cross_compiler import LinuxCrossCompiler

cross_compiler = LinuxCrossCompiler("x86_64-linux-gnu")
info = cross_compiler.detect()

if info:
    print(f"Found Linux cross-compiler")
    print(f"  Target: {info.target_platform} {info.target_architecture}")
    print(f"  Toolchain: {info.toolchain_path}")
    print(f"  Sysroot: {info.sysroot}")
```

#### Toolchain Components

Detects:

- `x86_64-linux-gnu-gcc.exe` - C compiler
- `x86_64-linux-gnu-g++.exe` - C++ compiler
- `x86_64-linux-gnu-gcc-ar.exe` - Archiver
- `x86_64-linux-gnu-strip.exe` - Strip utility

#### CMake Integration

Sets up CMake for cross-compilation:

```cmake
CMAKE_SYSTEM_NAME=Linux
CMAKE_SYSTEM_PROCESSOR=x86_64
CMAKE_C_COMPILER=x86_64-linux-gnu-gcc
CMAKE_CXX_COMPILER=x86_64-linux-gnu-g++
CMAKE_GENERATOR=Ninja
```

### 5.2 WASM Cross-Compiler

#### Overview

The WASM Cross-Compiler detects Emscripten for WebAssembly compilation.

#### Detection Process

```python
# Example: Detect WASM cross-compiler
from warploom_scripts.compilers.wasm_cross_compiler import WASMCrossCompiler

cross_compiler = WASMCrossCompiler()
info = cross_compiler.detect()

if info:
    print(f"Found Emscripten")
    print(f"  Version: {info.metadata.get('emscripten_version')}")
    print(f"  Toolchain: {info.toolchain_path}")
```

#### Toolchain Components

Detects:

- `emcc` - Emscripten C compiler
- `em++` - Emscripten C++ compiler
- `emar` - Emscripten archiver

#### CMake Integration

Sets up CMake for WASM compilation:

```cmake
CMAKE_SYSTEM_NAME=Emscripten
CMAKE_C_COMPILER=emcc
CMAKE_CXX_COMPILER=em++
CMAKE_GENERATOR=Ninja
```

### 5.3 Android Cross-Compiler

#### Overview

The Android Cross-Compiler detects Android NDK for Android compilation.

#### Supported Architectures

- **arm64-v8a:** ARM64
- **armeabi-v7a:** ARM 32-bit
- **x86_64:** x86_64
- **x86:** x86

#### Detection Process

```python
# Example: Detect Android cross-compiler
from warploom_scripts.compilers.android_cross_compiler import AndroidCrossCompiler

cross_compiler = AndroidCrossCompiler("arm64-v8a")
info = cross_compiler.detect()

if info:
    print(f"Found Android NDK")
    print(f"  Version: {info.metadata.get('ndk_version')}")
    print(f"  Toolchain: {info.toolchain_path}")
```

#### Toolchain Components

Detects:

- `aarch64-linux-android-clang` - C compiler
- `aarch64-linux-android-clang++` - C++ compiler
- `llvm-ar` - Archiver

#### CMake Integration

Sets up CMake for Android compilation:

```cmake
CMAKE_SYSTEM_NAME=Android
CMAKE_SYSTEM_VERSION=21
CMAKE_ANDROID_ARCH_ABI=arm64-v8a
CMAKE_ANDROID_NDK=/path/to/ndk
CMAKE_C_COMPILER=aarch64-linux-android-clang
CMAKE_CXX_COMPILER=aarch64-linux-android-clang++
```

---

## 6. Utility Classes

### 6.1 Compiler Factory

#### Overview

The Compiler Factory creates and manages compiler instances based on type and architecture requirements.

#### Key Methods

```python
class CompilerFactory:
    def register_detector(compiler_type: str, detector: ICompilerDetector) -> None
    def create_compiler(compiler_type: str, architecture: str) -> Optional[CompilerInfo]
    def get_available_compilers() -> Dict[str, List[CompilerInfo]]
    def select_best_compiler(requirements: CompilerRequirements) -> Optional[CompilerInfo]
```

#### Usage Example

```python
from warploom_scripts.compilers.factory import CompilerFactory
from warploom_scripts.compilers.msvc_detector import MSVCDetector
from warploom_scripts.compilers.mingw_gcc_detector import MinGWDetector

# Create factory
factory = CompilerFactory()

# Register detectors
factory.register_detector("msvc", MSVCDetector())
factory.register_detector("mingw_gcc", MinGWDetector())

# Create compiler
compiler = factory.create_compiler("msvc", "x64")
if compiler:
    print(f"Created compiler: {compiler.compiler_type} {compiler.version}")
```

#### Compiler Selection

The factory selects the best compiler based on:

- Version (highest preferred)
- Architecture match
- Capability requirements
- User preferences

### 6.2 Compiler Manager

#### Overview

The Compiler Manager coordinates compiler detection, validation, and selection.

#### Key Methods

```python
class CompilerManager:
    def detect_all_compilers() -> Dict[str, List[CompilerInfo]]
    def get_compiler(compiler_type: str, architecture: str) -> Optional[CompilerInfo]
    def get_all_compilers() -> Dict[str, List[CompilerInfo]]
    def validate_compiler(compiler_info: CompilerInfo) -> ValidationResult
    def refresh_detection() -> Dict[str, List[CompilerInfo]]
```

#### Usage Example

```python
from warploom_scripts.compilers.manager import CompilerManager
from warploom_scripts.compilers.factory import CompilerFactory

# Create manager
factory = CompilerFactory()
manager = CompilerManager(factory)

# Detect all compilers
compilers = manager.detect_all_compilers()

# Get specific compiler
msvc_x64 = manager.get_compiler("msvc", "x64")
if msvc_x64:
    print(f"MSVC x64: {msvc_x64.version}")

# Validate compiler
validation = manager.validate_compiler(msvc_x64)
if validation.is_valid:
    print("Compiler is valid")
else:
    print(f"Errors: {validation.errors}")
```

### 6.3 Cache Manager

#### Overview

The Cache Manager provides intelligent caching for compiler detection results to improve performance.

#### Key Methods

```python
class CacheManager:
    def get(key: str) -> Optional[Any]
    def set(key: str, value: Any) -> None
    def invalidate(key: Optional[str] = None) -> None
    def generate_key(*args: Any) -> str
```

#### Usage Example

```python
from warploom_scripts.compilers.cache import CacheManager

# Create cache manager
cache = CacheManager(cache_file=".compiler_cache.json", ttl=3600)

# Get from cache
result = cache.get("msvc_x64_detection")
if result is None:
    # Perform detection
    result = detect_msvc_x64()
    # Cache result
    cache.set("msvc_x64_detection", result)

# Invalidate cache
cache.invalidate("msvc_x64_detection")
```

#### Cache Features

- **TTL Support:** Automatic expiration after specified time
- **File Persistence:** Cache persists across sessions
- **Key Generation:** Automatic key generation from arguments
- **Selective Invalidation:** Invalidate specific entries or entire cache

### 6.4 Retry Handler

#### Overview

The Retry Handler provides automatic retry logic for transient errors.

#### Key Methods

```python
class RetryHandler:
    def retry(func: Callable[..., T], *args, retry_on: Optional[Callable[[Exception], bool]] = None, **kwargs) -> T
```

#### Usage Example

```python
from warploom_scripts.compilers.retry import RetryHandler

# Create retry handler
retry_handler = RetryHandler(max_retries=3, retry_delay=1.0, backoff_factor=2.0)

# Retry function
def detect_compiler():
    # Detection logic that might fail
    pass

result = retry_handler.retry(detect_compiler)
```

#### Retry Strategy

- **Max Retries:** Configurable maximum retry attempts
- **Retry Delay:** Initial delay between retries
- **Backoff Factor:** Exponential backoff multiplier
- **Conditional Retry:** Retry only on specific exceptions

### 6.5 Fallback Handler

#### Overview

The Fallback Handler provides fallback mechanisms when primary methods fail.

#### Key Methods

```python
class FallbackHandler:
    def try_with_fallbacks(methods: List[Callable[..., T]], *args, component: str = "unknown", **kwargs) -> Optional[T]
```

#### Usage Example

```python
from warploom_scripts.compilers.fallback import FallbackHandler
from warploom_scripts.compilers.error_handler import ErrorHandler

# Create fallback handler
error_handler = ErrorHandler()
fallback_handler = FallbackHandler(error_handler)

# Try multiple detection methods
methods = [
    detect_via_vswhere,
    detect_via_registry,
    detect_via_standard_paths
]

result = fallback_handler.try_with_fallbacks(methods, component="msvc_detection")
```

#### Fallback Strategy

- **Sequential Execution:** Try methods in order
- **Error Handling:** Log errors for each failed method
- **First Success:** Return result from first successful method
- **All Fail:** Return None if all methods fail

### 6.6 Error Handler

#### Overview

The Error Handler provides comprehensive error handling and reporting for compiler detection.

#### Key Methods

```python
class ErrorHandler:
    def handle_detection_error(component: str, error_code: str, message: str, exception: Optional[Exception] = None, details: Optional[Dict[str, Any]] = None, suggestion: Optional[str] = None) -> None
    def handle_validation_error(component: str, error_code: str, message: str, details: Optional[Dict[str, Any]] = None) -> None
    def handle_execution_error(component: str, error_code: str, message: str, exception: Optional[Exception] = None, details: Optional[Dict[str, Any]] = None) -> None
    def get_errors() -> List[CompilerDetectionError]
    def get_errors_by_severity(severity: ErrorSeverity) -> List[CompilerDetectionError]
    def has_critical_errors() -> bool
```

#### Usage Example

```python
from warploom_scripts.compilers.error_handler import ErrorHandler, ErrorSeverity

# Create error handler
error_handler = ErrorHandler()

# Handle detection error
error_handler.handle_detection_error(
    component="msvc_detection",
    error_code="VSWHERE_NOT_FOUND",
    message="vswhere.exe not found",
    suggestion="Install Visual Studio Build Tools"
)

# Check for critical errors
if error_handler.has_critical_errors():
    print("Critical errors detected")
    for error in error_handler.get_errors_by_severity(ErrorSeverity.CRITICAL):
        print(f"  {error.message}")
```

#### Error Categories

- **Detection Error:** Compiler detection failures
- **Validation Error:** Compiler validation failures
- **Execution Error:** Command execution failures
- **Configuration Error:** Configuration errors
- **Environment Error:** Environment setup errors
- **Compatibility Error:** Compatibility issues

### 6.7 Logging Integration

#### Overview

The Logging Integration provides unified logging for all compiler detection operations.

#### Key Methods

```python
class CompilerDetectionLogger:
    def debug(message: str) -> None
    def info(message: str) -> None
    def warning(message: str) -> None
    def error(message: str, exception: Exception | None = None) -> None
    def log_detection_start(component: str) -> None
    def log_detection_complete(component: str, count: int, duration: float) -> None
    def log_compiler_selected(compiler_type: str, architecture: str, version: str) -> None
```

#### Usage Example

```python
from warploom_scripts.compilers.logging_integration import CompilerDetectionLogger

# Create logger
logger = CompilerDetectionLogger(level="INFO", verbose=True)

# Log operations
logger.log_detection_start("msvc")
# ... detection logic ...
logger.log_detection_complete("msvc", count=3, duration=1.5)
logger.log_compiler_selected("msvc", "x64", "19.40.0")
```

#### Log Levels

- **DEBUG:** Detailed diagnostic information
- **INFO:** General informational messages
- **WARNING:** Warning messages for potential issues
- **ERROR:** Error messages for failures

---

## 7. Usage Examples

### 7.1 Basic Compiler Detection

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem

# Create detection system
cds = CompilerDetectionSystem()

# Detect all compilers
result = cds.detect_all()

# Print detected compilers
for compiler_type, compilers in result.compilers.items():
    print(f"\n{compiler_type.upper()} Compilers:")
    for compiler in compilers:
        print(f"  - {compiler.version} ({compiler.architecture})")
        print(f"    Path: {compiler.path}")
```

### 7.2 Selecting a Compiler

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem

# Create detection system
cds = CompilerDetectionSystem()

# Get MSVC x64 compiler
compiler = cds.get_compiler("msvc", "x64")
if compiler:
    print(f"Selected: {compiler.compiler_type} {compiler.version}")
    print(f"Path: {compiler.path}")
    print(f"Capabilities: {compiler.capabilities.to_dict()}")
```

### 7.3 Setting Up Environment

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem

# Create detection system
cds = CompilerDetectionSystem()

# Setup environment for MSVC x64
env = cds.setup_environment("msvc", "x64")

# Print environment variables
print("Environment Variables:")
for key, value in env.items():
    print(f"  {key}={value}")
```

### 7.4 Executing Commands

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem

# Create detection system
cds = CompilerDetectionSystem()

# Execute command in MSVC x64 environment
result = cds.execute_command("msvc", "x64", "cmake --version")

if result.success:
    print(f"Output:\n{result.stdout}")
else:
    print(f"Error:\n{result.stderr}")
```

### 7.5 Cross-Compilation Setup

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem

# Create detection system
cds = CompilerDetectionSystem()

# Setup Linux cross-compilation
env = cds.setup_cross_compilation("linux", "x86_64")

# Print environment variables
print("Cross-Compilation Environment:")
for key, value in env.items():
    print(f"  {key}={value}")
```

### 7.6 Compiler Selection with Requirements

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem
from warploom_scripts.compilers.data_structures import CompilerRequirements

# Create detection system
cds = CompilerDetectionSystem()

# Define requirements
requirements = CompilerRequirements(
    compiler_type="msvc",
    architecture="x64",
    cpp_standard="cpp23",
    required_capabilities=["modules", "concepts", "coroutines"]
)

# Select best compiler
factory = cds._compiler_manager._factory
compiler = factory.select_best_compiler(requirements)

if compiler:
    print(f"Selected compiler: {compiler.compiler_type} {compiler.version}")
    print(f"Path: {compiler.path}")
```

### 7.7 Terminal Detection

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem

# Create detection system
cds = CompilerDetectionSystem()

# Get terminal for MSVC x64
terminal = cds.get_terminal("msvc", "x64")
if terminal:
    print(f"Terminal: {terminal.name}")
    print(f"Executable: {terminal.executable}")
    print(f"Arguments: {terminal.arguments}")
```

### 7.8 Cache Management

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem

# Create detection system
cds = CompilerDetectionSystem()

# Get cache info
cache_info = cds._compiler_manager._factory._cache_manager.get_cache_info()
print(f"Cache entries: {cache_info['entries']}")
print(f"Cache size: {cache_info['size']} bytes")

# Clear cache
cds._compiler_manager._factory._cache_manager.clear_cache()
print("Cache cleared")
```

### 7.9 Error Handling

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem
from warploom_scripts.compilers.error_handler import ErrorSeverity

# Create detection system
cds = CompilerDetectionSystem()

# Detect all compilers
result = cds.detect_all()

# Check for errors
if result.errors:
    print("Detection Errors:")
    for error in result.errors:
        print(f"  [{error.severity.value.upper()}] {error.component}: {error.message}")
        if error.suggestion:
            print(f"  Suggestion: {error.suggestion}")
```

### 7.10 Complete Build Workflow

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem

# Create detection system
cds = CompilerDetectionSystem()

# Detect all compilers
result = cds.detect_all()

# Select compiler
compiler = cds.get_compiler("msvc", "x64")
if not compiler:
    print("No MSVC x64 compiler found")
    exit(1)

# Setup environment
env = cds.setup_environment("msvc", "x64")

# Configure CMake
cmake_result = cds.execute_command(
    "msvc", "x64",
    "cmake -S . -B build -G \"Visual Studio 17 2022\" -A x64"
)

if not cmake_result.success:
    print(f"CMake configuration failed:\n{cmake_result.stderr}")
    exit(1)

# Build project
build_result = cds.execute_command(
    "msvc", "x64",
    "cmake --build build --config Release"
)

if not build_result.success:
    print(f"Build failed:\n{build_result.stderr}")
    exit(1)

print("Build succeeded!")
```

---

## 8. Troubleshooting Guide

### 8.1 Common Issues

#### Issue: Compiler Not Detected

**Symptoms:**

- `detect_all()` returns empty list for a compiler type
- `get_compiler()` returns None

**Possible Causes:**

1. Compiler not installed
2. Compiler installed in non-standard location
3. vswhere not available (for MSVC)
4. Registry access denied

**Solutions:**

1. **Verify Installation:**

   ```bash
   # Check MSVC installation
   "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -all

   # Check MinGW installation
   gcc --version
   clang --version
   ```

2. **Check Standard Paths:**

   - MSVC: `C:\Program Files\Microsoft Visual Studio\2022`
   - MinGW: `C:\msys64`, `C:\mingw64`

3. **Verify Registry Access:**

   - Run as administrator
   - Check Windows Registry permissions

4. **Enable Verbose Logging:**
   ```python
   logger = CompilerDetectionLogger(level="DEBUG", verbose=True)
   ```

#### Issue: Version Detection Fails

**Symptoms:**

- Version information is None or incorrect
- Capability detection fails

**Possible Causes:**

1. Compiler executable corrupted
2. Version output format changed
3. Timeout during version detection

**Solutions:**

1. **Verify Compiler Executable:**

   ```bash
   cl.exe
   gcc.exe --version
   clang.exe --version
   ```

2. **Increase Timeout:**

   ```python
   # In detector configuration
   detector.timeout = 30  # seconds
   ```

3. **Check Version Parsing:**
   - Review detector logs for version output
   - Update version regex if format changed

#### Issue: Terminal Not Found

**Symptoms:**

- `get_terminal()` returns None
- Terminal invocation fails

**Possible Causes:**

1. Terminal executable not found
2. vcvarsall.bat not found
3. MSYS2 not installed

**Solutions:**

1. **Verify Terminal Paths:**

   ```bash
   # Check MSVC Developer Command Prompt
   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"

   # Check MSYS2
   C:\msys64\msys2_shell.cmd
   ```

2. **Check Environment Variables:**

   ```bash
   echo %VSINSTALLDIR%
   echo %MSYSTEM%
   ```

3. **Install Missing Components:**
   - Install Visual Studio Build Tools
   - Install MSYS2 from https://www.msys2.org/

#### Issue: Environment Setup Fails

**Symptoms:**

- `setup_environment()` raises exception
- Environment variables not set correctly

**Possible Causes:**

1. vcvarsall.bat execution fails
2. MSYS2 environment variables incorrect
3. Path conflicts

**Solutions:**

1. **Test vcvarsall.bat:**

   ```bash
   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
   ```

2. **Verify MSYS2 Environment:**

   ```bash
   # In MSYS2 shell
   echo $MSYSTEM
   echo $MINGW_PREFIX
   ```

3. **Check PATH Conflicts:**
   - Review PATH variable for conflicts
   - Ensure correct compiler is in PATH

#### Issue: Cross-Compilation Fails

**Symptoms:**

- Cross-compiler not detected
- CMake configuration fails
- Linking errors

**Possible Causes:**

1. Cross-compiler toolchain not installed
2. Sysroot not found
3. CMake generator incorrect

**Solutions:**

1. **Install Cross-Compiler:**

   ```bash
   # Linux cross-compiler (via MSYS2)
   pacman -S mingw-w64-ucrt-x86_64-linux-gnu-gcc

   # Emscripten
   pacman -S emscripten

   # Android NDK
   # Download from https://developer.android.com/ndk
   ```

2. **Verify Toolchain:**

   ```bash
   x86_64-linux-gnu-gcc --version
   emcc --version
   aarch64-linux-android-clang --version
   ```

3. **Check CMake Configuration:**
   ```cmake
   # Verify CMake toolchain file
   cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake ..
   ```

#### Issue: Cache Problems

**Symptoms:**

- Stale detection results
- Cache not invalidating
- Performance degradation

**Possible Causes:**

1. Cache file corrupted
2. TTL too long
3. Cache not cleared after system changes

**Solutions:**

1. **Clear Cache:**

   ```python
   cds._compiler_manager._factory._cache_manager.clear_cache()
   ```

2. **Delete Cache File:**

   ```bash
   rm .compiler_cache.json
   ```

3. **Adjust TTL:**
   ```python
   cache = CacheManager(cache_file=".compiler_cache.json", ttl=1800)  # 30 minutes
   ```

#### Issue: Performance Issues

**Symptoms:**

- Detection takes too long
- System becomes unresponsive

**Possible Causes:**

1. Too many compilers installed
2. Network timeouts
3. Disk I/O bottlenecks

**Solutions:**

1. **Enable Parallel Detection:**

   ```python
   from warploom_scripts.compilers.parallel_detector import ParallelDetector

   parallel_detector = ParallelDetector(max_workers=4)
   results = parallel_detector.detect_parallel(detectors)
   ```

2. **Reduce Detection Scope:**

   ```python
   # Only detect specific compiler types
   factory.register_detector("msvc", MSVCDetector())
   # Don't register other detectors
   ```

3. **Optimize Cache:**
   - Increase TTL for stable systems
   - Use in-memory cache for frequent operations

### 8.2 Debug Mode

Enable debug mode for detailed logging:

```python
from warploom_scripts.compilers.compiler_detection_system import CompilerDetectionSystem
from warploom_scripts.compilers.logging_integration import CompilerDetectionLogger

# Create logger with debug mode
logger = CompilerDetectionLogger(level="DEBUG", verbose=True)

# Create detection system
cds = CompilerDetectionSystem()

# Detect with debug output
result = cds.detect_all()
```

### 8.3 Getting Help

If you encounter issues not covered in this guide:

1. **Check Logs:**

   - Review debug logs for detailed error information
   - Look for error codes and suggestions

2. **Validate Installation:**

   - Verify compiler installations
   - Check terminal availability
   - Test environment setup manually

3. **Report Issues:**
   - Include error messages
   - Provide system information
   - Attach debug logs

---

## 9. API Reference

### 9.1 CompilerDetectionSystem

#### Methods

##### `detect_all() -> DetectionResult`

Detect all compilers, terminals, and cross-compilers.

**Returns:**

- `DetectionResult`: Complete detection result

**Example:**

```python
result = cds.detect_all()
print(f"Found {len(result.compilers)} compiler types")
```

##### `get_compiler(compiler_type: str, architecture: str = "x64") -> Optional[CompilerInfo]`

Get a specific compiler.

**Parameters:**

- `compiler_type`: Type of compiler ("msvc", "mingw_gcc", etc.)
- `architecture`: Target architecture ("x64", "x86", etc.)

**Returns:**

- `CompilerInfo` or `None`: Compiler information or None if not found

**Example:**

```python
compiler = cds.get_compiler("msvc", "x64")
```

##### `get_terminal(compiler_type: str, architecture: str = "x64") -> Optional[TerminalInfo]`

Get terminal for compiler.

**Parameters:**

- `compiler_type`: Type of compiler
- `architecture`: Target architecture

**Returns:**

- `TerminalInfo` or `None`: Terminal information or None if not found

**Example:**

```python
terminal = cds.get_terminal("msvc", "x64")
```

##### `setup_environment(compiler_type: str, architecture: str = "x64") -> Dict[str, str]`

Setup environment for compiler.

**Parameters:**

- `compiler_type`: Type of compiler
- `architecture`: Target architecture

**Returns:**

- `Dict[str, str]`: Environment variables

**Example:**

```python
env = cds.setup_environment("msvc", "x64")
```

##### `execute_command(compiler_type: str, architecture: str, command: str) -> CommandResult`

Execute command in compiler's terminal.

**Parameters:**

- `compiler_type`: Type of compiler
- `architecture`: Target architecture
- `command`: Command to execute

**Returns:**

- `CommandResult`: Command execution result

**Example:**

```python
result = cds.execute_command("msvc", "x64", "cmake --version")
```

##### `setup_cross_compilation(target_platform: str, target_arch: str) -> Dict[str, str]`

Setup cross-compilation environment.

**Parameters:**

- `target_platform`: Target platform ("linux", "wasm", "android")
- `target_arch`: Target architecture

**Returns:**

- `Dict[str, str]`: Environment variables

**Example:**

```python
env = cds.setup_cross_compilation("linux", "x86_64")
```

### 9.2 Data Structures

#### CompilerInfo

```python
@dataclass
class CompilerInfo:
    compiler_type: str  # "msvc", "msvc_clang", "mingw_gcc", "mingw_clang"
    version: VersionInfo
    path: str
    architecture: str  # "x64", "x86", "arm", "arm64"
    capabilities: CapabilityInfo
    environment: EnvironmentInfo
    metadata: Dict[str, str]
```

#### VersionInfo

```python
@dataclass
class VersionInfo:
    major: int
    minor: int
    patch: int
    build: Optional[str] = None
    full_version: str = ""
```

#### CapabilityInfo

```python
@dataclass
class CapabilityInfo:
    cpp23: bool = False
    cpp20: bool = False
    cpp17: bool = False
    cpp14: bool = False
    modules: bool = False
    coroutines: bool = False
    concepts: bool = False
    ranges: bool = False
    std_format: bool = False
    msvc_compatibility: bool = False
    mingw_compatibility: bool = False
```

#### TerminalInfo

```python
@dataclass
class TerminalInfo:
    terminal_id: str
    name: str
    type: TerminalType
    executable: str
    arguments: List[str]
    architecture: str
    environment: str
    capabilities: List[str]
    metadata: Dict[str, str]
    recommended: bool = False
```

#### CommandResult

```python
@dataclass
class CommandResult:
    exit_code: int
    stdout: str
    stderr: str
    environment: Dict[str, str]
    execution_time: float

    @property
    def success(self) -> bool:
        return self.exit_code == 0
```

#### DetectionResult

```python
@dataclass
class DetectionResult:
    compilers: Dict[str, List[CompilerInfo]]
    terminals: Dict[str, List[TerminalInfo]]
    cross_compilers: Dict[str, CrossCompilerInfo]
    errors: List[DetectionError]
    warnings: List[str]
```

---

## 10. Configuration

### 10.1 Configuration File

The compiler detection system uses a JSON configuration file at `config/compilers.json`.

#### Example Configuration

```json
{
  "preferred_compiler": "msvc",
  "preferred_architecture": "x64",
  "cpp_standard": "cpp23",
  "fallback_compilers": ["mingw_gcc", "msvc_clang"],
  "cross_compilation": {
    "linux": {
      "enabled": false,
      "target_architecture": "x86_64-linux-gnu"
    },
    "wasm": {
      "enabled": false,
      "target_architecture": "wasm32"
    },
    "android": {
      "enabled": false,
      "target_architecture": "arm64-v8a"
    }
  },
  "cache": {
    "enabled": true,
    "ttl": 3600,
    "file": ".compiler_cache.json"
  },
  "logging": {
    "level": "INFO",
    "verbose": false,
    "file": "compiler_detection.log"
  }
}
```

### 10.2 Configuration Options

#### Compiler Preferences

- `preferred_compiler`: Default compiler type ("msvc", "mingw_gcc", etc.)
- `preferred_architecture`: Default architecture ("x64", "x86", etc.)
- `cpp_standard`: Default C++ standard ("cpp23", "cpp20", etc.)
- `fallback_compilers`: List of fallback compilers

#### Cross-Compilation

- `cross_compilation.linux.enabled`: Enable Linux cross-compilation
- `cross_compilation.wasm.enabled`: Enable WASM cross-compilation
- `cross_compilation.android.enabled`: Enable Android cross-compilation

#### Cache

- `cache.enabled`: Enable caching
- `cache.ttl`: Cache time-to-live in seconds
- `cache.file`: Cache file path

#### Logging

- `logging.level`: Log level ("DEBUG", "INFO", "WARNING", "ERROR")
- `logging.verbose`: Enable verbose logging
- `logging.file`: Log file path

### 10.3 Programmatic Configuration

```python
from warploom_scripts.compilers.config import ConfigurationIntegration

# Load configuration
config = ConfigurationIntegration("config/compilers.json")

# Get configuration values
preferred_compiler = config.get_preferred_compiler()
preferred_architecture = config.get_preferred_architecture()
cpp_standard = config.get_cpp_standard()

# Set configuration values
config.set("preferred_compiler", "mingw_gcc")
config.save("config/compilers.json")
```

---

## Appendix

### A. Error Codes

| Error Code                                   | Description                    | Severity |
| -------------------------------------------- | ------------------------------ | -------- |
| DETECTION_COMPILER_NOT_FOUND                 | Compiler not found             | HIGH     |
| DETECTION_VSWHERE_NOT_FOUND                  | vswhere.exe not found          | MEDIUM   |
| DETECTION_REGISTRY_ACCESS_FAILED             | Registry access failed         | MEDIUM   |
| VALIDATION_COMPILER_EXECUTABLE_INVALID       | Compiler executable invalid    | HIGH     |
| VALIDATION_COMPILER_VERSION_INVALID          | Compiler version invalid       | MEDIUM   |
| EXECUTION_COMMAND_TIMEOUT                    | Command execution timeout      | MEDIUM   |
| EXECUTION_COMMAND_FAILED                     | Command execution failed       | HIGH     |
| ENVIRONMENT_PATH_NOT_SET                     | PATH not set                   | MEDIUM   |
| COMPATIBILITY_COMPILER_TERMINAL_INCOMPATIBLE | Compiler-terminal incompatible | HIGH     |

### B. Supported C++ Standards

| Standard | MSVC   | MSVC-Clang | MinGW-GCC | MinGW-Clang |
| -------- | ------ | ---------- | --------- | ----------- |
| C++23    | 19.40+ | 17+        | 14+       | 17+         |
| C++20    | 19.28+ | 13+        | 11+       | 13+         |
| C++17    | 19.15+ | 6+         | 8+        | 6+          |
| C++14    | 19.00+ | 3.4+       | 5+        | 3.4+        |

### C. Performance Benchmarks

| Operation           | Target Time | Typical Time |
| ------------------- | ----------- | ------------ |
| Compiler Detection  | < 2s        | 0.5-1.5s     |
| Terminal Detection  | < 500ms     | 100-300ms    |
| Terminal Invocation | < 200ms     | 50-150ms     |
| Environment Setup   | < 1s        | 0.3-0.8s     |
| Cache Lookup        | < 10ms      | 1-5ms        |

### D. System Requirements

#### Minimum Requirements

- Windows 10 or later
- 2 GB RAM
- 100 MB disk space
- Python 3.10+

#### Recommended Requirements

- Windows 11
- 8 GB RAM
- 500 MB disk space
- Python 3.11+
- SSD for better performance

---

**End of Compiler Detection System Documentation**

