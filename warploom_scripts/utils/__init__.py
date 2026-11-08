# warploom_scripts/utils/__init__.py
"""
Shared utilities for OmniCPP project.

This package provides common utilities used across all scripts:
- Exception classes
- Logging functions
- Command execution
- File operations
- System detection
- Path manipulation
"""

# Import exception classes
import subprocess

from .exceptions import (
    CommandExecutionError,
    NotADirectoryError,
    PathValidationError,
)

# Import logging functions
from .logging_utils import (
    log_error,
    log_info,
    log_success,
    log_warning,
)

# Import command execution
from .command_utils import execute_command
from .system_utils import SystemUtils

# Backward-compatible utility functions used by the legacy test suite.
run_command = SystemUtils.run_command

def validate_compiler(compiler: str) -> bool:
    """Return whether a compiler executable is available."""
    return SystemUtils.is_command_available(compiler)

def detect_platform() -> str:
    """Return the normalized host platform name."""
    return SystemUtils.get_platform_info()["system"]

def get_compiler_path(compiler: str):
    """Return the compiler path or None when it is unavailable."""
    import shutil
    return shutil.which(compiler)

def format_duration(seconds: float) -> str:
    """Format seconds using the legacy human-readable representation."""
    total = int(seconds)
    hours, remainder = divmod(total, 3600)
    minutes, secs = divmod(remainder, 60)
    if hours:
        return f"{hours}h {minutes}m {secs}s"
    if minutes:
        return f"{minutes}m {secs}s"
    return f"{secs}s"

def sanitize_filename(filename: str) -> str:
    """Sanitize a filename using the shared path utility."""
    return PathUtils.sanitize_filename(filename)

# Import platform detection
from .platform_utils import (
    get_workspace_dir,
    get_system_platform,
    is_windows,
    is_linux,
    is_macos,
    get_executable_extension,
    get_library_extension,
)

# Import utility classes
from .file_utils import FileUtils
from .path_utils import PathUtils
from .terminal_utils import (
    TerminalEnvironment,
    TerminalSetupError,
    execute_with_terminal_setup,
    setup_terminal_environment,
)

__all__ = [
    # Exceptions
    'CommandExecutionError',
    'NotADirectoryError',
    'PathValidationError',
    # Logging
    'log_error',
    'log_info',
    'log_success',
    'log_warning',
    # Command execution
    'execute_command',
    'run_command',
    'validate_compiler',
    'detect_platform',
    'get_compiler_path',
    'format_duration',
    'sanitize_filename',
    # Platform detection
    'get_workspace_dir',
    'get_system_platform',
    'is_windows',
    'is_linux',
    'is_macos',
    'get_executable_extension',
    'get_library_extension',
    # Utility classes
    'FileUtils',
    'PathUtils',
    'SystemUtils',
    # Terminal utilities
    'TerminalEnvironment',
    'TerminalSetupError',
    'execute_with_terminal_setup',
    'setup_terminal_environment',
]
