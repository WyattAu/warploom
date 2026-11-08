# warploom_scripts/utils/command_utils.py
"""
Command execution utility functions for OmniCPP project.

Provides command execution with retry logic and error handling.
"""

from __future__ import annotations

import subprocess
import time
from typing import Dict, Optional

from .exceptions import CommandExecutionError
from .logging_utils import log_error, log_info, log_warning


def execute_command(
    cmd: str,
    env: Optional[Dict[str, str]] = None,
    retries: int = 1,
    timeout: int = 300,
) -> None:
    """Execute a shell command with retry logic and error handling.

    This function executes a shell command with configurable retry attempts
    and timeout. It logs the command execution and provides detailed error
    messages on failure.

    Args:
        cmd: The shell command to execute.
        env: Optional dictionary of environment variables for command.
        retries: Number of retry attempts if command fails (default: 1).
        timeout: Timeout in seconds for command execution (default: 300).

    Raises:
        subprocess.TimeoutExpired: If command times out.
        CommandExecutionError: If command fails after all retries.
        Exception: For any other unexpected errors during execution.
    """
    log_info(f"Executing: {cmd}")

    # Check if MSYS2 environment is set up
    import os
    # Check for MSYS2_PATH or PATH containing MSYS2 paths
    msys2_path = os.environ.get("MSYS2_PATH", "")
    path = os.environ.get("PATH", "")
    if (msys2_path or "/msys64" in path or "/ucrt64" in path) and not env:
        # Use MSYS2 environment if available
        env = os.environ.copy()
        # Convert Windows paths to POSIX paths for MSYS2
        # MSYS2 expects POSIX paths with forward slashes (e.g., /c/msys64/ucrt64/bin:/c/msys64/usr/bin)
        # instead of Windows paths with backslashes (e.g., C:\msys64\ucrt64\bin;C:\msys64\usr\bin)
        if "PATH" in env:
            # Convert backslashes to forward slashes
            posix_path = env["PATH"].replace("\\", "/")
            # Convert semicolons to colons (Windows path separator to POSIX path separator)
            # Do this BEFORE converting drive letters to avoid corrupting the path
            posix_path = posix_path.replace(";", ":")
            # Convert drive letters (e.g., C: to /c)
            # Use a more specific regex to only match drive letters at the start of a path component
            # Convert drive letter to lowercase (e.g., C: to /c)
            import re
            posix_path = re.sub(r"(^|:)([A-Za-z]):", lambda m: f"{m.group(1)}/{m.group(2).lower()}", posix_path)
            env["PATH"] = posix_path
            log_info(f"Converted MSYS2 PATH: {posix_path[:100]}...")
        else:
            log_info(f"Using MSYS2 environment: PATH={path[:100]}...")

    last_error: Optional[Exception] = None
    last_output: Optional[str] = None

    for attempt in range(retries):
        try:
            result: subprocess.CompletedProcess[str] = subprocess.run(
                cmd,
                shell=True,
                env=env,
                timeout=timeout,
                capture_output=True,
                text=True,
            )

            if result.returncode == 0:
                log_info(f"Command completed: {cmd}")
                return 0

            # Command failed
            error_msg: str = result.stderr.strip() if result.stderr else "Unknown error"
            last_output = result.stdout + result.stderr
            log_error(
                f"Command failed (attempt {attempt + 1}/{retries}): {error_msg}"
            )

            if attempt < retries - 1:
                log_warning("Retrying in 2 seconds...")
                time.sleep(2)
            else:
                last_error = CommandExecutionError(
                    f"Command failed after {retries} attempts: {cmd}",
                    command=cmd,
                    return_code=result.returncode,
                    output=last_output,
                )

        except subprocess.TimeoutExpired as e:
            log_error(f"Command timed out after {timeout}s: {cmd}")
            raise subprocess.TimeoutExpired(
                cmd, timeout, output=e.output, stderr=e.stderr
            ) from e
        except subprocess.CalledProcessError as e:
            log_error(f"Command execution failed: {e}")
            last_error = e
            if attempt < retries - 1:
                log_warning("Retrying in 2 seconds...")
                time.sleep(2)
        except FileNotFoundError as e:
            log_error(f"Command not found: {cmd}")
            raise FileNotFoundError(
                f"Command executable not found in PATH: {cmd.split()[0]}"
            ) from e
        except PermissionError as e:
            log_error(f"Permission denied executing command: {cmd}")
            raise PermissionError(
                f"Permission denied executing command: {cmd}"
            ) from e
        except Exception as e:
            log_error(f"Unexpected error: {e}")
            raise

    # If we get here, all retries failed
    if last_error:
        raise last_error


__all__ = [
    'execute_command',
]
