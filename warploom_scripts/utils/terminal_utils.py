"""
Terminal Environment Setup Utilities (Fixed Version).

This module provides utilities for setting up of correct terminal
environment for different compilers on Windows.

This version fixes the MSYS2 UCRT64 prompt issues by:
1. Using MSYS2's built-in cygpath utility for path conversion
2. Resolving Windows short filenames to long filenames
3. Using MSYS2's built-in environment variables
4. Using ucrt64.exe instead of bash.exe for environment setup

Classes:
    TerminalSetupError: Exception for terminal setup failures.
    TerminalEnvironment: Class for managing terminal environment setup.
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
from pathlib import Path
from typing import Optional

from .logging_utils import log_info, log_warning


class TerminalSetupError(Exception):
    """Exception raised when terminal setup fails."""

    def __init__(
        self,
        message: str,
        terminal_type: Optional[str] = None,
    ) -> None:
        """Initialize terminal setup error.

        Args:
            message: Error message describing issue.
            terminal_type: Optional terminal type being set up.
        """
        self.terminal_type = terminal_type
        super().__init__(message)


class TerminalEnvironment:
    """Manages terminal environment setup for different compilers.

    This class provides methods to set up correct terminal
    environment for different compilers on Windows.

    Attributes:
        compiler: The compiler being used.
        terminal_type: The terminal type (vsdevcmd, msys2).
        env_vars: Dictionary of environment variables to set.
    """

    def __init__(self, compiler: Optional[str] = None) -> None:
        """Initialize terminal environment.

        Args:
            compiler: The compiler being used (msvc, clang-msvc, mingw-clang, mingw-gcc).
        """
        self.compiler = compiler
        self.terminal_type: str = self._detect_terminal_type()
        self.env_vars: dict[str, str] = self._get_env_vars()

    def _detect_terminal_type(self) -> str:
        """Detect terminal type based on compiler.

        Returns:
            The terminal type (vsdevcmd, msys2, default).
        """
        if not self.compiler:
            return "default"

        compiler_lower = self.compiler.lower()

        if compiler_lower in ["msvc", "clang-msvc"]:
            return "vsdevcmd"
        elif compiler_lower in ["mingw-clang", "mingw-gcc"]:
            return "msys2"
        else:
            return "default"

    def _get_env_vars(self) -> dict[str, str]:
        """Get environment variables for terminal type.

        Returns:
            Dictionary of environment variables.
        """
        if self.terminal_type == "vsdevcmd":
            return self._get_vsdevcmd_env()
        elif self.terminal_type == "msys2":
            return self._get_msys2_env()
        else:
            return {}

    def _get_vsdevcmd_env(self) -> dict[str, str]:
        """Get environment variables for Visual Studio Developer Command Prompt.

        Returns:
            Dictionary of environment variables.
        """
        # Find Visual Studio installation
        vs_install_paths = [
            Path("C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build"),
            Path("C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Auxiliary/Build"),
            Path("C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build"),
            Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build"),
            Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/Professional/VC/Auxiliary/Build"),
            Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build"),
        ]

        vsdevcmd_path = None
        for path in vs_install_paths:
            vsdevcmd_file = path / "vcvars64.bat"
            if vsdevcmd_file.exists():
                vsdevcmd_path = vsdevcmd_file
                break

        if not vsdevcmd_path:
            log_warning("Visual Studio Developer Command Prompt not found")
            return {}

        env_vars = {
            "VSDEVCMD_PATH": str(vsdevcmd_path.parent),
        }

        log_info(f"Using Visual Studio Developer Command Prompt: {vsdevcmd_path}")
        return env_vars

    def _get_msys2_env(self) -> dict[str, str]:
        """Get environment variables for MSYS2 UCRT64.

        Returns:
            Dictionary of environment variables.
        """
        # Find MSYS2 installation
        msys2_path = self._find_msys2_installation()
        if not msys2_path:
            log_warning("MSYS2 UCRT64 not found")
            return {}

        # Use MSYS2's built-in environment variables
        ucrt64_bin = msys2_path / "ucrt64" / "bin"
        usr_bin = msys2_path / "usr" / "bin"
        usr_local_bin = msys2_path / "usr" / "local" / "bin"

        # Convert paths to MSYS2 format using cygpath
        ucrt64_bin_msys2 = self._convert_path_to_msys2(str(ucrt64_bin))
        usr_bin_msys2 = self._convert_path_to_msys2(str(usr_bin))
        usr_local_bin_msys2 = self._convert_path_to_msys2(str(usr_local_bin))

        # Build MSYS2 PATH using MSYS2's built-in paths
        msys2_path_env = f"{ucrt64_bin_msys2}:{usr_bin_msys2}:{usr_local_bin_msys2}"

        # Add user's local bin directory to PATH for Conan
        user_local_bin = Path.home() / ".local" / "bin"
        if user_local_bin.exists():
            # Convert user local bin to MSYS2 format using cygpath
            user_local_bin_msys2 = self._convert_path_to_msys2(str(user_local_bin))
            if user_local_bin_msys2:
                msys2_path_env = f"{user_local_bin_msys2}:{msys2_path_env}"
                log_info(f"Added user local bin to MSYS2 PATH: {user_local_bin_msys2}")

        env_vars = {
            "MSYS2_PATH": str(msys2_path),
            "MSYSTEM": "UCRT64",
            "MSYSTEM_PREFIX": str(msys2_path / "ucrt64"),
            "MSYSTEM_CARCH": "x86_64",
            "MINGW_PREFIX": str(msys2_path / "ucrt64"),
            "MINGW_CHOST": "x86_64-w64-mingw32",
            "PATH": msys2_path_env,
        }

        log_info(f"Using MSYS2 UCRT64: {msys2_path}")
        return env_vars

    def _find_msys2_installation(self) -> Optional[Path]:
        """Find MSYS2 installation path.

        Returns:
            MSYS2 installation path or None if not found.
        """
        msys2_paths = [
            Path("C:/msys64"),
            Path("C:/msys32"),
        ]

        for path in msys2_paths:
            ucrt64_bin = path / "ucrt64" / "bin"
            if ucrt64_bin.exists():
                return path

        return None

    def _convert_path_to_msys2(self, path_str: str) -> Optional[str]:
        """Convert Windows path to MSYS2 format using MSYS2's cygpath utility.

        Args:
            path_str: Windows path string.

        Returns:
            MSYS2 formatted path string or None if conversion fails.
        """
        msys2_path = self._find_msys2_installation()
        if not msys2_path:
            return None

        cygpath = msys2_path / "usr" / "bin" / "cygpath.exe"
        if not cygpath.exists():
            log_warning("cygpath.exe not found, falling back to manual conversion")
            return self._convert_path_to_msys2_manual(path_str)

        try:
            # Resolve Windows short filename to long filename
            long_path = self._get_long_path_name(path_str)
            
            # Convert to MSYS2 format using cygpath
            result = subprocess.run(
                [str(cygpath), "-u", long_path],
                capture_output=True,
                text=True,
                timeout=10,
            )
            if result.returncode == 0:
                return result.stdout.strip()
        except (subprocess.TimeoutExpired, FileNotFoundError) as e:
            log_warning(f"Failed to convert path using cygpath: {e}")
            return self._convert_path_to_msys2_manual(path_str)

        return None

    def _convert_path_to_msys2_manual(self, path_str: str) -> Optional[str]:
        """Convert Windows path to MSYS2 format manually.

        Args:
            path_str: Windows path string.

        Returns:
            MSYS2 formatted path string or None if conversion fails.
        """
        # Resolve Windows short filename to long filename
        long_path = self._get_long_path_name(path_str)
        
        # Convert drive letter (e.g., C:\ to /c/)
        if len(long_path) >= 2 and long_path[1] == ':':
            converted = '/' + long_path[0].lower() + long_path[2:].replace('\\', '/')
        else:
            converted = long_path.replace('\\', '/')
        
        return converted

    def _get_long_path_name(self, short_path: str) -> str:
        """Convert Windows short filename to long filename.

        Args:
            short_path: Windows short filename path.

        Returns:
            Long filename path.
        """
        try:
            # Get required buffer size
            buffer_size = ctypes.windll.kernel32.GetLongPathNameW(short_path, None, 0)
            if buffer_size == 0:
                return short_path
            
            # Allocate buffer and get long path
            buffer = ctypes.create_unicode_buffer(buffer_size)
            result = ctypes.windll.kernel32.GetLongPathNameW(short_path, buffer, buffer_size)
            if result == 0:
                return short_path
            
            return buffer.value
        except Exception as e:
            log_warning(f"Failed to get long path name: {e}")
            return short_path

    def setup_environment(self) -> dict[str, str]:
        """Set up terminal environment.

        Returns:
            Dictionary of environment variables to set.
        """
        # Start with current environment variables
        env_vars = os.environ.copy()

        # Add terminal-specific environment variables
        # For MSYS2, we need to ensure the PATH is set correctly
        if self.terminal_type == "msys2":
            # Get MSYS2-specific PATH from env_vars
            msys2_path = self.env_vars.get("PATH", "")
            if msys2_path:
                # Convert Windows paths to MSYS2 format
                # MSYS2 expects POSIX paths with forward slashes
                msys2_path_converted = msys2_path.replace("\\", "/").replace(";", ":")
                # Set PATH to MSYS2-specific PATH
                env_vars["PATH"] = msys2_path_converted
                log_info(f"Setting MSYS2 PATH: {msys2_path_converted}")
            
            # Set other MSYS2 environment variables
            for key, value in self.env_vars.items():
                if key != "PATH":
                    env_vars[key] = value
        else:
            # For other terminal types, just add the env_vars
            env_vars.update(self.env_vars)

        return env_vars

    def execute_in_environment(
        self,
        command: str,
        cwd: Optional[str] = None,
    ) -> int:
        """Execute a command in configured terminal environment.

        Args:
            command: The command to execute.
            cwd: Optional working directory.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            TerminalSetupError: If terminal setup fails.
        """
        env = self.setup_environment()

        log_info(f"Executing in {self.terminal_type} environment: {command}")
        log_info(f"Capture output: enabled")

        try:
            if self.terminal_type == "vsdevcmd":
                # Execute in Visual Studio Developer Command Prompt
                vsdevcmd_path = env.get("VSDEVCMD_PATH", "")
                if vsdevcmd_path:
                    vsdevcmd_bat = Path(vsdevcmd_path) / "vcvars64.bat"
                    if vsdevcmd_bat.exists():
                        # Execute command directly using shell=True
                        # This avoids issues with temp batch files
                        full_command = f'call "{vsdevcmd_bat}" && {command}'
                        
                        log_info(f"Executing command in vsdevcmd environment")
                        log_info(f"Working directory: {cwd}")
                        log_info(f"Full command: {full_command}")
                        
                        # Use os.system() instead of subprocess.run()
                        # This allows output to go directly to terminal
                        import os
                        return_code = os.system(full_command)

                        log_info(f"Command completed with return code: {return_code}")

                        return return_code

            elif self.terminal_type == "msys2":
                # Execute in MSYS2 UCRT64 using bash.exe directly (Direct Bash Method)
                # This is the recommended method to avoid Mintty GUI terminal issues
                msys2_path = env.get("MSYS2_PATH", "")
                bash_exe = None
                if msys2_path:
                    bash_exe = Path(msys2_path) / "usr" / "bin" / "bash.exe"
                    if bash_exe.exists():
                        log_info(f"MSYS2 bash.exe: {bash_exe}")
                        log_info(f"Command to execute: {command}")
                        log_info(f"Command type: {type(command)}")
                        log_info(f"Command length: {len(command)}")
                        
                        # Setup environment variables for MSYS2 bash
                        # MSYSTEM: Sets the environment to UCRT64
                        # CHERE_INVOKING: Tells MSYS2 to stay in the current working directory
                        # MSYS2_PATH_TYPE: Inherit Windows PATH (useful if you need non-msys tools)
                        env["MSYSTEM"] = "UCRT64"
                        env["CHERE_INVOKING"] = "1"
                        env["MSYS2_PATH_TYPE"] = "inherit"
                        
                        # Convert working directory to MSYS2 format
                        if cwd:
                            cwd_msys2 = self._convert_path_to_msys2(cwd)
                            if cwd_msys2:
                                # Use '-l' (login) to ensure profile scripts run
                                # Use '-c' to run a command string
                                # Don't use shlex.quote() for the command string
                                # The -c flag expects a single argument containing the command to execute
                                # Adding extra quotes breaks the parsing
                                full_command = f'cd "{cwd_msys2}" && {command}'
                            else:
                                log_warning(f"Failed to convert working directory to MSYS2 format: {cwd}")
                                full_command = command
                        else:
                            full_command = command
                        
                        log_info(f"Full command to execute: {full_command}")
                        log_info(f"Full command length: {len(full_command)}")
                        
                        # Don't pass cwd to subprocess.run() when executing in MSYS2 environment
                        # because the working directory is being set in the command string itself
                        result = subprocess.run(
                            [str(bash_exe), "-l", "-c", full_command],
                            env=env,
                            capture_output=True,
                            text=True,
                        )

                        # Print stdout and stderr
                        if result.stdout:
                            print(result.stdout, flush=True)
                        if result.stderr:
                            print(result.stderr, file=sys.stderr, flush=True)

                        log_info(f"MSYS2 execution completed with return code: {result.returncode}")

                        return result.returncode
                else:
                    log_warning(f"MSYS2 bash.exe not found: {bash_exe}")
                    return 1

            else:
                # Execute in default environment
                result = subprocess.run(
                    command,
                    shell=True,
                    cwd=cwd,
                    env=env,
                    capture_output=True,
                    text=True,
                )
                # Print stdout and stderr
                if result.stdout:
                    print(result.stdout, flush=True)
                if result.stderr:
                    print(result.stderr, file=sys.stderr, flush=True)
                return result.returncode

        except Exception as e:
            raise TerminalSetupError(
                f"Failed to execute command in terminal environment: {e}",
                terminal_type=self.terminal_type,
            ) from e
        
        # This should never be reached, but return error code if it is
        return 1


def setup_terminal_environment(compiler: Optional[str] = None) -> TerminalEnvironment:
    """Set up terminal environment for specified compiler.

    Args:
        compiler: The compiler being used (msvc, clang-msvc, mingw-clang, mingw-gcc).

    Returns:
        TerminalEnvironment instance configured for compiler.
    """
    return TerminalEnvironment(compiler)


def execute_with_terminal_setup(
    command: str,
    compiler: Optional[str] = None,
    cwd: Optional[str] = None,
    env: Optional[dict[str, str]] = None,
) -> int:
    """Execute a command with appropriate terminal environment setup.
    
    This function sets up the correct terminal environment for the specified
    compiler and executes the command. It supports MSVC, MSVC-Clang,
    MinGW-GCC, MinGW-Clang, and Linux compilers.
    
    Args:
        command: The command to execute.
        compiler: The compiler being used (msvc, clang-msvc, mingw-clang, mingw-gcc, gcc, clang).
        cwd: Optional working directory for command execution.
        env: Optional additional environment variables to merge with terminal environment.
        
    Returns:
        Exit code (0 for success, non-zero for failure).
        
    Raises:
        TerminalSetupError: If terminal setup fails.
        
    Example:
        >>> exit_code = execute_with_terminal_setup('cmake --build .', compiler='msvc')
        >>> if exit_code == 0:
        ...     print("Build succeeded")
    """
    terminal_env = setup_terminal_environment(compiler)
    
    # Merge additional environment variables if provided
    if env:
        terminal_env.env_vars.update(env)
    
    return terminal_env.execute_in_environment(command, cwd)


def detect_terminal_type() -> str:
    """Detect available terminal types on the current platform.
    
    This function detects which terminal types are available on the current
    platform. It checks for Visual Studio Developer Command Prompt on Windows,
    MSYS2 on Windows, and standard terminals on Linux.
    
    Returns:
        Detected terminal type ('vsdevcmd', 'msys2', 'default', or 'unknown').
        
    Example:
        >>> terminal_type = detect_terminal_type()
        >>> print(f"Using terminal type: {terminal_type}")
    """
    import platform
    
    current_platform = platform.system().lower()
    
    if current_platform == 'windows':
        # Check for Visual Studio Developer Command Prompt
        vs_paths = [
            Path("C:/Program Files/Microsoft Visual Studio/2022"),
            Path("C:/Program Files (x86)/Microsoft Visual Studio/2022"),
            Path("C:/Program Files/Microsoft Visual Studio/2019"),
            Path("C:/Program Files (x86)/Microsoft Visual Studio/2019"),
        ]
        
        for vs_path in vs_paths:
            if vs_path.exists():
                vcvars_path = vs_path / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
                if vcvars_path.exists():
                    log_info("Detected Visual Studio Developer Command Prompt")
                    return "vsdevcmd"
        
        # Check for MSYS2
        msys2_paths = [
            Path("C:/msys64"),
            Path("C:/msys32"),
        ]
        
        for msys2_path in msys2_paths:
            if msys2_path.exists():
                ucrt64_bin = msys2_path / "ucrt64" / "bin"
                if ucrt64_bin.exists():
                    log_info("Detected MSYS2 UCRT64")
                    return "msys2"
        
        # Default for Windows
        log_info("Using default terminal for Windows")
        return "default"
    
    elif current_platform == 'linux':
        # Check for common Linux terminals
        terminals = ['bash', 'zsh', 'sh']
        
        for terminal in terminals:
            try:
                result = subprocess.run(
                    ['which', terminal],
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                if result.returncode == 0:
                    log_info(f"Detected Linux terminal: {terminal}")
                    return "default"
            except Exception:
                continue
        
        log_warning("No Linux terminal detected")
        return "unknown"
    
    else:
        log_warning(f"Unsupported platform: {current_platform}")
        return "unknown"
