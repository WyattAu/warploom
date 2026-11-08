# warploom_scripts/utils/system_utils.py
"""
System utility functions for Warploom project.

Provides system detection, command execution, and platform-specific operations.
"""

import ctypes
import logging
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    import psutil
except ImportError:
    psutil = None  # type: ignore[assignment]

logger = logging.getLogger(__name__)


class SystemUtils:
    """Utility class for system operations"""

    @staticmethod
    def get_platform_info() -> Dict[str, str]:
        """Get comprehensive platform information"""
        return {
            'system': platform.system().lower(),
            'machine': platform.machine().lower(),
            'processor': platform.processor(),
            'python_version': sys.version.split()[0],
            'python_executable': sys.executable,
            'cwd': str(Path.cwd()),
            'home': str(Path.home()),
            'is_windows': 'true' if platform.system() == 'Windows' else 'false',
            'is_linux': 'true' if platform.system() == 'Linux' else 'false',
            'is_macos': 'true' if platform.system() == 'Darwin' else 'false',
        }

    @staticmethod
    def is_command_available(command: str) -> bool:
        """Check if a command is available in PATH"""
        return shutil.which(command) is not None

    @staticmethod
    def get_command_version(command: str) -> Optional[str]:
        """Get version of a command"""
        try:
            result = subprocess.run(
                [command, '--version'],
                capture_output=True,
                text=True,
                timeout=10
            )
            if result.returncode == 0:
                # Extract version from first line
                first_line = result.stdout.split('\n')[0].strip()
                # Try to extract version number
                import re
                version_match = re.search(r'(\d+\.\d+(?:\.\d+)*)', first_line)
                return version_match.group(1) if version_match else first_line
        except (subprocess.SubprocessError, subprocess.TimeoutExpired):
            pass
        return None

    @staticmethod
    def run_command(cmd: List[str], cwd: Optional[Path] = None,
                   env: Optional[Dict[str, str]] = None,
                   timeout: int = 300) -> Tuple[int, str, str]:
        """
        Run a command and return (returncode, stdout, stderr)

        Args:
            cmd: Command as list of strings
            cwd: Working directory
            env: Environment variables
            timeout: Timeout in seconds

        Returns:
            Tuple of (return_code, stdout, stderr)
        """
        try:
            logger.debug(f"Running command: {' '.join(cmd)}")
            if cwd:
                logger.debug(f"Working directory: {cwd}")

            result = subprocess.run(
                cmd,
                cwd=str(cwd) if cwd else None,
                env=env,
                capture_output=True,
                text=True,
                timeout=timeout
            )

            return result.returncode, result.stdout, result.stderr

        except subprocess.TimeoutExpired:
            logger.error(f"Command timed out after {timeout}s: {' '.join(cmd)}")
            return -1, "", f"Command timed out after {timeout} seconds"
        except Exception as e:
            logger.error(f"Failed to run command {' '.join(cmd)}: {e}")
            return -1, "", str(e)

    @staticmethod
    def run_command_interactive(cmd: List[str], cwd: Optional[Path] = None,
                               env: Optional[Dict[str, str]] = None) -> int:
        """
        Run a command interactively (shows output in real-time)

        Returns:
            Return code
        """
        try:
            logger.debug(f"Running interactive command: {' '.join(cmd)}")

            result = subprocess.run(
                cmd,
                cwd=str(cwd) if cwd else None,
                env=env
            )

            return result.returncode

        except Exception as e:
            logger.error(f"Failed to run interactive command {' '.join(cmd)}: {e}")
            return -1

    @staticmethod
    def find_visual_studio_installation() -> Optional[Path]:
        """Find Visual Studio installation on Windows"""
        if platform.system() != 'Windows':
            return None

        # Common Visual Studio installation paths
        possible_paths = [
            Path("C:/Program Files/Microsoft Visual Studio/2022"),
            Path("C:/Program Files (x86)/Microsoft Visual Studio/2019"),
        ]

        editions = ["Enterprise", "Professional", "Community"]

        for base_path in possible_paths:
            if base_path.exists():
                for edition in editions:
                    vs_path = base_path / edition
                    if vs_path.exists():
                        return vs_path

        return None

    @staticmethod
    def get_visual_studio_dev_cmd_path() -> Optional[Path]:
        """Get path to Visual Studio Developer Command Prompt"""
        vs_install = SystemUtils.find_visual_studio_installation()
        if vs_install:
            dev_cmd = vs_install / "Common7" / "Tools" / "VsDevCmd.bat"
            if dev_cmd.exists():
                return dev_cmd
        return None

    @staticmethod
    def setup_visual_studio_environment() -> Optional[Dict[str, str]]:
        """Setup Visual Studio environment variables"""
        dev_cmd = SystemUtils.get_visual_studio_dev_cmd_path()
        if not dev_cmd:
            return None

        try:
            # Run VsDevCmd.bat and capture environment
            cmd = f'"{dev_cmd}" && set'
            result = subprocess.run(
                cmd,
                shell=True,
                capture_output=True,
                text=True
            )

            if result.returncode == 0:
                env: Dict[str, str] = {}
                for line in result.stdout.splitlines():
                    if '=' in line:
                        key, value = line.split('=', 1)
                        env[key] = value
                return env

        except Exception as e:
            logger.error(f"Failed to setup Visual Studio environment: {e}")

        return None

    @staticmethod
    def get_system_memory_gb() -> float:
        """Get system memory in GB"""
        if psutil is not None:
            return psutil.virtual_memory().total / (1024**3)

        # Fallback for systems without psutil
        if platform.system() == 'Windows':
            try:
                result = subprocess.run(
                    ['wmic', 'ComputerSystem', 'get', 'TotalPhysicalMemory'],
                    capture_output=True,
                    text=True
                )
                if result.returncode == 0:
                    # Parse memory size
                    for line in result.stdout.splitlines():
                        line = line.strip()
                        if line.isdigit():
                            return float(int(line) / (1024**3))
            except Exception:
                pass

        elif platform.system() == 'Linux':
            try:
                with open('/proc/meminfo', 'r') as f:
                    for line in f:
                        if line.startswith('MemTotal:'):
                            # Extract memory in kB and convert to GB
                            mem_kb = int(line.split()[1])
                            return float(mem_kb / (1024**2))
            except Exception:
                pass

        return 0.0
    @staticmethod
    def get_cpu_count() -> int:
        """Get number of CPU cores"""
        return os.cpu_count() or 1

    @staticmethod
    def is_admin() -> bool:
        """Check if running with administrator privileges"""
        try:
            if platform.system() == 'Windows':
                return bool(ctypes.windll.shell32.IsUserAnAdmin() != 0)
            else:
                # os.geteuid() is only available on Unix-like systems
                if hasattr(os, 'geteuid'):
                    return bool(getattr(os, 'geteuid', lambda: 1)() == 0)
                else:
                    # Fallback for systems without geteuid
                    return False
        except Exception:
            return False

    @staticmethod
    def get_environment_info() -> Dict[str, str]:
        """Get relevant environment information"""
        info = SystemUtils.get_platform_info()

        # Add command availability
        commands_to_check = [
            'cmake', 'ninja', 'conan', 'gcc', 'clang', 'python3',
            'git', 'make', 'doxygen', 'clang-tidy', 'clang-format'
        ]

        for cmd in commands_to_check:
            version = SystemUtils.get_command_version(cmd)
            info[f'{cmd}_available'] = 'yes' if version else 'no'
            if version:
                info[f'{cmd}_version'] = version

        # Add system resources
        info['cpu_count'] = str(SystemUtils.get_cpu_count())
        info['memory_gb'] = f"{SystemUtils.get_system_memory_gb():.1f}"
        info['is_admin'] = 'yes' if SystemUtils.is_admin() else 'no'

        return info
