# warploom_scripts/validators/build_validator.py
"""
Build output validation for OmniCPP project.

This module validates:
- Build artifacts existence and integrity
- Library/executable functionality
- Build configuration consistency
- Cross-platform compatibility
"""

import logging
import platform
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from ..error_handler import ValidationError, ErrorSeverity, create_error_context

logger = logging.getLogger(__name__)


@dataclass
class BuildArtifact:
    """Represents a build artifact"""
    path: Path
    expected_type: str  # 'library', 'executable', 'header', 'config'
    platform: str
    architecture: str
    build_type: str  # 'debug', 'release'

    def exists(self) -> bool:
        return self.path.exists()

    def is_valid(self) -> bool:
        """Basic validation of the artifact"""
        if not self.exists():
            return False

        if self.expected_type == 'executable':
            return self._is_executable()
        elif self.expected_type == 'library':
            return self._is_library()
        elif self.expected_type == 'header':
            return self._is_header()

        return True

    def _is_executable(self) -> bool:
        """Check if file is a valid executable"""
        if platform.system() == 'Windows':
            return self.path.suffix.lower() in ['.exe', '.dll']
        else:
            # On Unix-like systems, check if file is executable
            return self.path.is_file() and bool(self.path.stat().st_mode & 0o111)

    def _is_library(self) -> bool:
        """Check if file is a valid library"""
        if platform.system() == 'Windows':
            return self.path.suffix.lower() in ['.lib', '.dll', '.a']
        else:
            return self.path.suffix in ['.so', '.dylib', '.a']

    def _is_header(self) -> bool:
        """Check if file is a valid header"""
        return self.path.suffix.lower() in ['.h', '.hpp', '.hxx']


@dataclass
class BuildValidationResult:
    """Result of build validation"""
    is_valid: bool
    artifacts_found: List[BuildArtifact]
    artifacts_missing: List[BuildArtifact]
    artifacts_invalid: List[BuildArtifact]
    errors: List[str]
    warnings: List[str]
    build_directory: Path


class BuildValidator:
    """Build output validator"""

    def __init__(self) -> None:
        self.system = platform.system().lower()
        self.machine = platform.machine().lower()

    def validate_build_directory(self, build_dir: Path, expected_artifacts: Optional[List[BuildArtifact]] = None) -> BuildValidationResult:
        """Validate a build directory"""
        if expected_artifacts is None:
            expected_artifacts = self._discover_expected_artifacts(build_dir)

        found_artifacts: List[BuildArtifact] = []
        missing_artifacts: List[BuildArtifact] = []
        invalid_artifacts: List[BuildArtifact] = []
        errors: List[str] = []
        warnings: List[str] = []

        for artifact in expected_artifacts:
            if not artifact.exists():
                missing_artifacts.append(artifact)
                errors.append(f"Missing artifact: {artifact.path}")
            elif not artifact.is_valid():
                invalid_artifacts.append(artifact)
                errors.append(f"Invalid artifact: {artifact.path}")
            else:
                found_artifacts.append(artifact)

        # Additional validations
        self._validate_build_consistency(build_dir, warnings, errors)
        self._validate_cross_platform_compatibility(build_dir, found_artifacts, warnings)

        return BuildValidationResult(
            is_valid=len(errors) == 0,
            artifacts_found=found_artifacts,
            artifacts_missing=missing_artifacts,
            artifacts_invalid=invalid_artifacts,
            errors=errors,
            warnings=warnings,
            build_directory=build_dir
        )

    def _discover_expected_artifacts(self, build_dir: Path) -> List[BuildArtifact]:
        """Discover expected artifacts based on build directory structure"""
        artifacts = []

        # Try to determine platform, architecture, and build type from path
        path_parts = build_dir.parts
        platform_name = "unknown"
        arch = "unknown"
        build_type = "unknown"

        # Parse build directory path (e.g., build/release/library/x86_64/debug)
        for part in path_parts:
            if part in ['windows', 'linux', 'darwin', 'emscripten']:
                platform_name = part
            elif part in ['x86_64', 'x86', 'arm64', 'arm']:
                arch = part
            elif part in ['debug', 'release', 'relwithdebinfo', 'minsizerel']:
                build_type = part

        # Look for common build artifacts
        if (build_dir / "bin").exists():
            bin_dir = build_dir / "bin"
            for item in bin_dir.iterdir():
                if item.is_file():
                    if self._is_executable_file(item):
                        artifacts.append(BuildArtifact(
                            path=item,
                            expected_type='executable',
                            platform=platform_name,
                            architecture=arch,
                            build_type=build_type
                        ))

        if (build_dir / "lib").exists():
            lib_dir = build_dir / "lib"
            for item in lib_dir.iterdir():
                if item.is_file() and self._is_library_file(item):
                    artifacts.append(BuildArtifact(
                        path=item,
                        expected_type='library',
                        platform=platform_name,
                        architecture=arch,
                        build_type=build_type
                    ))

        return artifacts

    def _is_executable_file(self, path: Path) -> bool:
        """Check if a file is an executable"""
        if self.system == 'windows':
            return path.suffix.lower() in ['.exe', '.bat', '.cmd']
        else:
            return path.is_file() and not path.suffix  # Unix executables typically have no extension

    def _is_library_file(self, path: Path) -> bool:
        """Check if a file is a library"""
        if self.system == 'windows':
            return path.suffix.lower() in ['.lib', '.dll', '.a']
        else:
            return path.suffix in ['.so', '.dylib', '.a']

    def _validate_build_consistency(self, build_dir: Path, warnings: List[str], errors: List[str]) -> None:
        """Validate build consistency"""
        # Check for CMake cache
        cmake_cache = build_dir / "CMakeCache.txt"
        if not cmake_cache.exists():
            warnings.append("CMakeCache.txt not found - build may not be properly configured")

        # Check for build artifacts
        has_bin = (build_dir / "bin").exists() and any((build_dir / "bin").iterdir())
        has_lib = (build_dir / "lib").exists() and any((build_dir / "lib").iterdir())

        if not has_bin and not has_lib:
            errors.append("No build artifacts found in bin/ or lib/ directories")

        # Check for compilation database (for clangd)
        compile_commands = build_dir / "compile_commands.json"
        if not compile_commands.exists():
            warnings.append("compile_commands.json not found - may affect IDE integration")

    def _validate_cross_platform_compatibility(self, build_dir: Path, artifacts: List[BuildArtifact], warnings: List[str]) -> None:
        """Validate cross-platform compatibility"""
        # Check if we're building for a different platform
        for artifact in artifacts:
            if artifact.platform != self.system and artifact.platform != "unknown":
                warnings.append(f"Cross-compiling for {artifact.platform} on {self.system} host")

    def validate_executable_functionality(self, executable_path: Path) -> Tuple[bool, str]:
        """Test basic functionality of an executable"""
        if not executable_path.exists():
            return False, "Executable does not exist"

        try:
            # Try to run the executable with --help or -h flag
            result = subprocess.run(
                [str(executable_path), "--help"],
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                return True, "Executable runs successfully with --help"
            else:
                # Try -h flag
                result = subprocess.run(
                    [str(executable_path), "-h"],
                    capture_output=True,
                    text=True,
                    timeout=10
                )

                if result.returncode == 0:
                    return True, "Executable runs successfully with -h"
                else:
                    return False, f"Executable failed to run: {result.stderr.strip()}"

        except subprocess.TimeoutExpired:
            return False, "Executable timed out"
        except Exception as e:
            return False, f"Failed to execute: {str(e)}"

    def validate_library_integrity(self, library_path: Path) -> Tuple[bool, str]:
        """Validate library file integrity"""
        if not library_path.exists():
            return False, "Library does not exist"

        # Basic size check
        if library_path.stat().st_size == 0:
            return False, "Library file is empty"

        # Platform-specific validation
        if self.system == 'windows':
            # On Windows, we could check PE file format, but for now just check extension
            if library_path.suffix.lower() not in ['.lib', '.dll', '.a']:
                return False, f"Unexpected library extension: {library_path.suffix}"
        else:
            # On Unix-like systems, check if it's a valid ELF/shared library
            if library_path.suffix not in ['.so', '.dylib', '.a']:
                return False, f"Unexpected library extension: {library_path.suffix}"

        return True, "Library appears valid"

    def validate_with_error_handling(self, build_dir: Path) -> BuildValidationResult:
        """Validate build directory with proper error handling"""
        try:
            result = self.validate_build_directory(build_dir)

            if not result.is_valid:
                error = ValidationError(
                    f"Build validation failed for {build_dir}",
                    severity=ErrorSeverity.HIGH,
                    context=create_error_context(
                        build_directory=str(build_dir),
                        errors=result.errors,
                        warnings=result.warnings,
                        missing_artifacts=[str(a.path) for a in result.artifacts_missing],
                        invalid_artifacts=[str(a.path) for a in result.artifacts_invalid]
                    )
                )
                logger.error(str(error))

            return result

        except Exception as e:
            error = ValidationError(
                f"Unexpected error during build validation: {str(e)}",
                severity=ErrorSeverity.MEDIUM,
                context=create_error_context(build_directory=str(build_dir))
            )
            logger.error(str(error))

            return BuildValidationResult(
                is_valid=False,
                artifacts_found=[],
                artifacts_missing=[],
                artifacts_invalid=[],
                errors=[str(e)],
                warnings=[],
                build_directory=build_dir
            )
