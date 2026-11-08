# Warploom Migration Guide

This guide helps you migrate from the old build system to the new integrated system with logging, platform detection, compiler detection, and terminal setup.

## Overview

The Warploom build system has been significantly enhanced with the following new components:

1. **Comprehensive Logging System** - Structured logging with multiple handlers and formatters
2. **Platform Detection** - Automatic OS and architecture detection
3. **Compiler Detection** - Automatic compiler detection with C++23 validation
4. **Terminal Environment Setup** - Automatic terminal setup for different compilers

These components are now integrated into the main controller and work together seamlessly.

## Migration Steps

### Step 1: Update Your Code

If you have custom scripts or code that uses the old logging functions, update them to use the new logging system:

**Old API:**
```python
from warploom_scripts.utils import log_info, log_warning, log_error, log_success

log_info("Message")
log_warning("Warning")
log_error("Error")
log_success("Success")
```

**New API:**
```python
from warploom_scripts.logging import get_logger, setup_logging

# Initialize logging once at startup
setup_logging()

# Get logger for your module
logger = get_logger(__name__)

# Use logger methods
logger.info("Message")
logger.warning("Warning")
logger.error("Error")
# Note: Use logger.info() for success messages
```

### Step 2: Update Build Scripts

If you have custom build scripts, update them to use the new integrated components:

**Old Approach:**
```python
# Manual compiler detection
if is_windows():
    compiler = "msvc"
elif is_linux():
    compiler = "gcc"
```

**New Approach:**
```python
from warploom_scripts.logging import get_logger, setup_logging
from warploom_scripts.platform.detector import detect_platform, PlatformInfo
from warploom_scripts.compilers.detector import detect_compiler, validate_cpp23_support
from warploom_scripts.utils.terminal_utils import execute_with_terminal_setup

# Initialize logging
setup_logging()
logger = get_logger(__name__)

# Detect platform
platform_info = detect_platform()
logger.info(f"Platform: {platform_info.os} {platform_info.architecture}")

# Detect compiler
compiler_info = detect_compiler(platform_info=platform_info)
if compiler_info:
    logger.info(f"Compiler: {compiler_info.name} {compiler_info.version}")

    # Validate C++23 support
    validation = validate_cpp23_support(compiler_info)
    if not validation.valid:
        logger.warning(f"Compiler does not support C++23, falling back to {validation.fallback}")

# Use terminal setup for MinGW builds
if compiler_info and compiler_info.name.lower() in ["mingw-clang", "mingw-gcc"]:
    result = execute_with_terminal_setup(
        'cmake --build .',
        compiler=compiler_info.name.lower(),
        cwd=str(project_root)
    )
```

### Step 3: Update Configuration Files

The new logging system uses JSON configuration files. Update your configuration if needed:

**Configuration Files:**
- `config/logging_python.json` - Python logging configuration
- `config/logging_cpp.json` - C++ logging configuration
- `config/compilers.json` - Compiler detection configuration

**Example Configuration:**
```json
{
  "level": "INFO",
  "console_handler_enabled": true,
  "colored_output": true,
  "file_handler_enabled": true,
  "file_path": "logs/warploom.log",
  "max_bytes": 10485760,
  "backup_count": 5,
  "format": "%(asctime)s - %(name)s - %(levelname)s - %(message)s",
  "datefmt": "%Y-%m-%d %H:%M:%S"
}
```

### Step 4: Update CI/CD Workflows

Update your GitHub Actions workflows to use the new integrated components:

**Old Workflow:**
```yaml
- name: Build
  run: |
    python WarploomController.py build engine "Clean Build Pipeline" default release
```

**New Workflow:**
```yaml
- name: Build
  run: |
    python WarploomController.py build engine "Clean Build Pipeline" default release
  env:
    # Set log level for CI
    WARPLOOM_LOG_LEVEL: INFO
```

### Step 5: Update Documentation

Update your documentation to reflect the new architecture:

**Key Changes:**
- Added comprehensive logging system documentation
- Added platform detection documentation
- Added compiler detection documentation
- Added terminal setup documentation
- Updated build commands to show new features

## New Features

### Logging System

The new logging system provides:

1. **Structured Logging** - Consistent format across all components
2. **Multiple Handlers** - Console and file handlers with rotation
3. **Custom Formatters** - Colored console output, JSON logging
4. **Configuration-Driven** - JSON-based configuration with environment overrides
5. **Dynamic Log Levels** - Runtime log level changes
6. **Backward Compatibility** - Legacy logging functions supported

**Usage:**
```python
from warploom_scripts.logging import setup_logging, get_logger

# Initialize logging
setup_logging()

# Get logger
logger = get_logger(__name__)

# Log messages
logger.info("Info message")
logger.warning("Warning message")
logger.error("Error message")
logger.debug("Debug message")
logger.critical("Critical message")
```

### Platform Detection

The platform detection system provides:

1. **OS Detection** - Windows, Linux, macOS
2. **Architecture Detection** - x86_64, ARM64, x86
3. **Platform Information** - Comprehensive platform details
4. **Cross-Platform Support** - Platform-specific code paths

**Usage:**
```python
from warploom_scripts.platform.detector import detect_platform, PlatformInfo

# Detect platform
platform_info = detect_platform()

# Access platform information
print(f"OS: {platform_info.os}")
print(f"Architecture: {platform_info.architecture}")
print(f"64-bit: {platform_info.is_64bit}")
```

### Compiler Detection

The compiler detection system provides:

1. **Multi-Compiler Support** - MSVC, MSVC-Clang, MinGW-GCC, MinGW-Clang, GCC, Clang
2. **C++23 Validation** - Automatic validation with fallback to C++20
3. **Version Detection** - Automatic version detection for all compilers
4. **Platform-Specific Detection** - Windows (MSVC, MinGW), Linux (GCC, Clang)
5. **Compiler Information** - Path, version, capabilities

**Usage:**
```python
from warploom_scripts.compilers.detector import detect_compiler, validate_cpp23_support

# Detect compiler
compiler_info = detect_compiler()

if compiler_info:
    print(f"Compiler: {compiler_info.name}")
    print(f"Version: {compiler_info.version}")
    print(f"Path: {compiler_info.path}")
    print(f"C++23 Support: {compiler_info.supports_cpp23}")

    # Validate C++23 support
    validation = validate_cpp23_support(compiler_info)
    if not validation.valid:
        print(f"Warning: {validation.fallback}")
        for warning in validation.warnings:
            print(f"Warning: {warning}")
```

### Terminal Environment Setup

The terminal setup system provides:

1. **VS Dev Prompt** - Automatic Visual Studio Developer Command Prompt setup
2. **MSYS2 Integration** - Automatic MSYS2 UCRT64 environment setup
3. **Linux Environment** - Automatic environment setup for Linux compilers
4. **Path Conversion** - Automatic Windows to MSYS2 path conversion
5. **Environment Validation** - Verification of terminal environment

**Usage:**
```python
from warploom_scripts.utils.terminal_utils import execute_with_terminal_setup

# Execute command with terminal setup
result = execute_with_terminal_setup(
    'cmake --build .',
    compiler='mingw-gcc',
    cwd=str(project_root)
)

# Terminal environment is automatically set up
# - VS Dev Prompt for MSVC
# - MSYS2 for MinGW
# - Default for Linux
```

## Common Migration Issues

### Issue 1: Import Errors

**Problem:**
```
ImportError: cannot import name 'log_info' from 'warploom_scripts.utils'
```

**Solution:**
Update imports to use the new logging system:
```python
# Old
from warploom_scripts.utils import log_info

# New
from warploom_scripts.logging import get_logger

logger = get_logger(__name__)
logger.info("Message")
```

### Issue 2: Missing Compiler Detection

**Problem:**
```
Build fails because compiler is not detected
```

**Solution:**
The new system automatically detects compilers. If you need to specify a compiler, use the new compiler names:
- `msvc` - Microsoft Visual C++
- `msvc-clang` - Clang with MSVC ABI
- `mingw-gcc` - MinGW with GCC
- `mingw-clang` - MinGW with Clang
- `gcc` - GCC on Linux
- `clang` - Clang on Linux

### Issue 3: Terminal Setup Failures

**Problem:**
```
MinGW builds fail with PATH errors
```

**Solution:**
The new terminal setup system automatically handles:
- MSYS2 environment variables
- Path conversion between Windows and MSYS2 formats
- Working directory preservation

No manual terminal setup is required.

### Issue 4: Logging Not Working

**Problem:**
```
Log messages are not appearing
```

**Solution:**
Ensure logging is initialized:
```python
from warploom_scripts.logging import setup_logging

# Call once at startup
setup_logging()
```

## Testing Your Migration

After completing the migration, test your setup:

### 1. Test Logging

```bash
# Test logging initialization
python -c "from warploom_scripts.logging import setup_logging, get_logger; setup_logging(); logger = get_logger('test'); logger.info('Logging test')"

# Test log levels
python -c "from warploom_scripts.logging import setup_logging, get_logger; setup_logging(); logger = get_logger('test'); logger.debug('Debug message'); logger.info('Info message'); logger.warning('Warning message'); logger.error('Error message')"
```

### 2. Test Platform Detection

```bash
# Test platform detection
python -c "from warploom_scripts.platform.detector import detect_platform; info = detect_platform(); print(f'OS: {info.os}, Arch: {info.architecture}')"
```

### 3. Test Compiler Detection

```bash
# Test compiler detection
python -c "from warploom_scripts.compilers.detector import detect_compiler; info = detect_compiler(); print(f'Compiler: {info.name if info else None}')"
```

### 4. Test Terminal Setup

```bash
# Test terminal setup
python WarploomController.py build engine "Clean Build Pipeline" default release --compiler mingw-gcc
```

### 5. Test Full Integration

```bash
# Run comprehensive integration tests
python impl/tests/test_full_integration.py
```

## Rollback Procedure

If you encounter issues and need to rollback:

1. **Restore Old Imports:**
   ```python
   from warploom_scripts.utils import log_info, log_warning, log_error, log_success
   ```

2. **Restore Old Build Scripts:**
   - Revert any custom build scripts
   - Use original WarploomController.py methods

3. **Restore Configuration:**
   - Revert any changes to configuration files
   - Use original logging configuration

## Best Practices

### 1. Initialize Logging Early

Always initialize logging at the start of your application:
```python
import sys
from warploom_scripts.logging import setup_logging

def main():
    # Initialize logging first
    setup_logging()

    # Rest of your code
    ...
```

### 2. Use Structured Logging

Use the new logging system for better log management:
```python
logger = get_logger(__name__)

# Good
logger.info("Processing file: %s", filename)

# Better
logger.info("Processing file", extra={"filename": filename})
```

### 3. Let System Detect Platform

Don't manually detect platform - use the integrated system:
```python
# Bad
import platform
if platform.system() == "Windows":
    compiler = "msvc"

# Good
from warploom_scripts.platform.detector import detect_platform
platform_info = detect_platform()
compiler = detect_compiler(platform_info=platform_info)
```

### 4. Use Compiler Detection

Let the system detect and validate compilers:
```python
# Bad
compiler = "msvc"  # Hardcoded

# Good
compiler_info = detect_compiler()
if compiler_info:
    validation = validate_cpp23_support(compiler_info)
    if not validation.valid:
        # Handle fallback
        cpp_standard = validation.fallback
```

### 5. Use Terminal Setup

Let the system handle terminal environments:
```python
# Bad
import subprocess
subprocess.run(['cmd', '/c', 'vcvars64.bat', '&&', 'cmake', '--build', '.'])

# Good
from warploom_scripts.utils.terminal_utils import execute_with_terminal_setup
result = execute_with_terminal_setup('cmake --build .', compiler='msvc', cwd=str(project_root))
```

## Support

If you encounter issues during migration:

1. **Check Documentation** - Review this guide and other documentation
2. **Check Logs** - Review log files for error messages
3. **Run Tests** - Use the comprehensive integration test suite
4. **Report Issues** - Create GitHub issues with detailed information

## Summary

The new integrated system provides:

- ✅ **Better Logging** - Structured, configurable, with multiple handlers
- ✅ **Platform Detection** - Automatic, cross-platform, with architecture detection
- ✅ **Compiler Detection** - Automatic, with C++23 validation and fallback
- ✅ **Terminal Setup** - Automatic, with environment validation
- ✅ **Integration** - All components work together seamlessly

Migration should be straightforward if you follow this guide. The new system is backward compatible and provides better functionality out of the box.
