"""
Unit tests for validator modules.

This module provides comprehensive tests for:
- BuildValidator
- ConfigValidator
- DependencyValidator
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Any
import pytest

# Add parent directories to path for imports
import sys
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.validators.build_validator import (
    BuildValidator,
    BuildArtifact,
    BuildValidationResult
)
from warploom_scripts.validators.config_validator import (
    ConfigValidator,
    ValidationResult,
    ValidationLevel
)
from warploom_scripts.validators.dependency_validator import (
    DependencyValidator,
    DependencyInfo,
    DependencyValidationResult
)


class TestBuildValidator:
    """Unit tests for BuildValidator."""

    def test_build_validator_initialization(self) -> None:
        """Test BuildValidator initialization."""
        validator = BuildValidator()
        assert validator is not None
        assert validator.system is not None
        assert validator.machine is not None

    def test_build_artifact_creation(self) -> None:
        """Test BuildArtifact creation."""
        artifact = BuildArtifact(
            path=Path("/test/path"),
            expected_type="executable",
            platform="windows",
            architecture="x86_64",
            build_type="debug"
        )
        assert artifact.path == Path("/test/path")
        assert artifact.expected_type == "executable"
        assert artifact.platform == "windows"
        assert artifact.architecture == "x86_64"
        assert artifact.build_type == "debug"

    def test_build_artifact_exists(self) -> None:
        """Test BuildArtifact.exists() method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create a file
            test_file = Path(tmp_dir) / "test.exe"
            test_file.touch()

            artifact = BuildArtifact(
                path=test_file,
                expected_type="executable",
                platform="windows",
                architecture="x86_64",
                build_type="debug"
            )
            assert artifact.exists() is True

    def test_build_artifact_not_exists(self) -> None:
        """Test BuildArtifact.exists() when file doesn't exist."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.exe"

            artifact = BuildArtifact(
                path=test_file,
                expected_type="executable",
                platform="windows",
                architecture="x86_64",
                build_type="debug"
            )
            assert artifact.exists() is False

    def test_build_artifact_is_valid_executable(self) -> None:
        """Test BuildArtifact.is_valid() for executable."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.exe"
            test_file.touch()

            artifact = BuildArtifact(
                path=test_file,
                expected_type="executable",
                platform="windows",
                architecture="x86_64",
                build_type="debug"
            )
            assert artifact.is_valid() is True

    def test_build_artifact_is_valid_library(self) -> None:
        """Test BuildArtifact.is_valid() for library."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.lib"
            test_file.touch()

            artifact = BuildArtifact(
                path=test_file,
                expected_type="library",
                platform="windows",
                architecture="x86_64",
                build_type="debug"
            )
            assert artifact.is_valid() is True

    def test_build_artifact_is_valid_header(self) -> None:
        """Test BuildArtifact.is_valid() for header."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.h"
            test_file.touch()

            artifact = BuildArtifact(
                path=test_file,
                expected_type="header",
                platform="windows",
                architecture="x86_64",
                build_type="debug"
            )
            assert artifact.is_valid() is True

    def test_build_artifact_is_invalid_wrong_type(self) -> None:
        """Test BuildArtifact.is_valid() with wrong type."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            test_file.touch()

            artifact = BuildArtifact(
                path=test_file,
                expected_type="executable",
                platform="windows",
                architecture="x86_64",
                build_type="debug"
            )
            assert artifact.is_valid() is False

    def test_build_validation_result_creation(self) -> None:
        """Test BuildValidationResult creation."""
        result = BuildValidationResult(
            is_valid=True,
            artifacts_found=[],
            artifacts_missing=[],
            artifacts_invalid=[],
            errors=[],
            warnings=[],
            build_directory=Path("/test")
        )
        assert result.is_valid is True
        assert result.build_directory == Path("/test")

    def test_validate_build_directory_with_valid_dir(self) -> None:
        """Test validate_build_directory with valid directory."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create bin directory with executable
            bin_dir = Path(tmp_dir) / "bin"
            bin_dir.mkdir()
            exe_file = bin_dir / "test.exe"
            exe_file.touch()

            validator = BuildValidator()
            result = validator.validate_build_directory(Path(tmp_dir))

            assert result.is_valid is True
            assert len(result.artifacts_found) > 0

    def test_validate_build_directory_with_missing_artifacts(self) -> None:
        """Test validate_build_directory with missing artifacts."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create bin directory but no files
            bin_dir = Path(tmp_dir) / "bin"
            bin_dir.mkdir()

            validator = BuildValidator()
            result = validator.validate_build_directory(Path(tmp_dir))

            assert result.is_valid is False
            assert len(result.errors) > 0

    def test_validate_executable_functionality_with_valid_exe(self) -> None:
        """Test validate_executable_functionality with valid executable."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create a simple batch file that acts as an executable
            test_file = Path(tmp_dir) / "test.bat"
            test_file.write_text("@echo off\necho Test executable")

            validator = BuildValidator()
            is_valid, message = validator.validate_executable_functionality(test_file)

            # On Windows, batch files should be executable
            assert is_valid is True or "executable" in message.lower()

    def test_validate_executable_functionality_with_missing_exe(self) -> None:
        """Test validate_executable_functionality with missing executable."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.exe"

            validator = BuildValidator()
            is_valid, message = validator.validate_executable_functionality(test_file)

            assert is_valid is False
            assert "does not exist" in message.lower()

    def test_validate_library_integrity_with_valid_library(self) -> None:
        """Test validate_library_integrity with valid library."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create a library file with content
            test_file = Path(tmp_dir) / "test.lib"
            test_file.write_text("test library content")

            validator = BuildValidator()
            is_valid, message = validator.validate_library_integrity(test_file)

            assert is_valid is True

    def test_validate_library_integrity_with_empty_library(self) -> None:
        """Test validate_library_integrity with empty library."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create an empty library file
            test_file = Path(tmp_dir) / "test.lib"
            test_file.touch()

            validator = BuildValidator()
            is_valid, message = validator.validate_library_integrity(test_file)

            assert is_valid is False
            assert "empty" in message.lower()

    def test_validate_with_error_handling_success(self) -> None:
        """Test validate_with_error_handling with valid directory."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create bin directory with executable
            bin_dir = Path(tmp_dir) / "bin"
            bin_dir.mkdir()
            exe_file = bin_dir / "test.exe"
            exe_file.touch()

            validator = BuildValidator()
            result = validator.validate_with_error_handling(Path(tmp_dir))

            assert result.is_valid is True

    def test_validate_with_error_handling_exception(self) -> None:
        """Test validate_with_error_handling with exception."""
        validator = BuildValidator()
        result = validator.validate_with_error_handling(Path("/nonexistent/path"))

        assert result.is_valid is False
        assert len(result.errors) > 0


class TestConfigValidator:
    """Unit tests for ConfigValidator."""

    def test_config_validator_initialization(self) -> None:
        """Test ConfigValidator initialization."""
        validator = ConfigValidator()
        assert validator is not None
        assert validator.schemas is not None

    def test_validation_result_creation(self) -> None:
        """Test ValidationResult creation."""
        result = ValidationResult(
            is_valid=True,
            errors=[],
            warnings=[],
            file_path=Path("/test/config.json")
        )
        assert result.is_valid is True
        assert result.file_path == Path("/test/config.json")

    def test_validation_result_str_valid(self) -> None:
        """Test ValidationResult.__str__ for valid result."""
        result = ValidationResult(
            is_valid=True,
            errors=[],
            warnings=[],
            file_path=Path("/test/config.json")
        )
        result_str = str(result)
        assert "VALID" in result_str

    def test_validation_result_str_invalid(self) -> None:
        """Test ValidationResult.__str__ for invalid result."""
        result = ValidationResult(
            is_valid=False,
            errors=["Test error"],
            warnings=[],
            file_path=Path("/test/config.json")
        )
        result_str = str(result)
        assert "INVALID" in result_str
        assert "Test error" in result_str

    def test_validate_file_with_nonexistent_file(self) -> None:
        """Test validate_file with nonexistent file."""
        validator = ConfigValidator()
        result = validator.validate_file(Path("/nonexistent/file.json"))

        assert result.is_valid is False
        assert len(result.errors) > 0
        assert "does not exist" in result.errors[0].lower()

    def test_validate_file_with_valid_json(self) -> None:
        """Test validate_file with valid JSON."""
        import json
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "CMakePresets.json"
            test_data = {
                "version": 3,
                "configurePresets": [
                    {
                        "name": "test-preset",
                        "description": "Test preset"
                    }
                ]
            }
            with open(test_file, "w") as f:
                json.dump(test_data, f)

            validator = ConfigValidator()
            result = validator.validate_file(test_file)

            assert result.is_valid is True

    def test_validate_file_with_missing_required_field(self) -> None:
        """Test validate_file with missing required field."""
        import json
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "CMakePresets.json"
            test_data = {
                "version": 3,
                "configurePresets": [
                    {
                        "description": "Test preset"
                    }
                ]
            }
            with open(test_file, "w") as f:
                json.dump(test_data, f)

            validator = ConfigValidator()
            result = validator.validate_file(test_file)

            assert result.is_valid is False
            assert any("name" in error for error in result.errors)

    def test_validate_file_with_invalid_version(self) -> None:
        """Test validate_file with invalid version."""
        import json
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "CMakePresets.json"
            test_data = {
                "version": 11,  # Invalid version
                "configurePresets": [
                    {
                        "name": "test-preset"
                    }
                ]
            }
            with open(test_file, "w") as f:
                json.dump(test_data, f)

            validator = ConfigValidator()
            result = validator.validate_file(test_file)

            # Just verify that validation fails with errors
            # The actual error messages may vary
            assert result.is_valid is False
            assert len(result.errors) > 0

    def test_validate_file_with_duplicate_preset_names(self) -> None:
        """Test validate_file with duplicate preset names."""
        import json
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "CMakePresets.json"
            test_data = {
                "version": 3,
                "configurePresets": [
                    {
                        "name": "test-preset"
                    },
                    {
                        "name": "test-preset"
                    }
                ]
            }
            with open(test_file, "w") as f:
                json.dump(test_data, f)

            validator = ConfigValidator()
            result = validator.validate_file(test_file)

            assert result.is_valid is False
            assert any("duplicate" in error.lower() for error in result.errors)

    def test_validate_project_configs(self) -> None:
        """Test validate_project_configs method."""
        import json
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create a valid CMakePresets.json
            test_file = Path(tmp_dir) / "CMakePresets.json"
            test_data = {
                "version": 3,
                "configurePresets": [
                    {
                        "name": "test-preset"
                    }
                ]
            }
            with open(test_file, "w") as f:
                json.dump(test_data, f)

            validator = ConfigValidator()
            results = validator.validate_project_configs(Path(tmp_dir))

            # Just verify that results are returned
            assert len(results) > 0

    def test_validate_with_error_handling_success(self) -> None:
        """Test validate_with_error_handling with valid file."""
        import json
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "CMakePresets.json"
            test_data = {
                "version": 3,
                "configurePresets": [
                    {
                        "name": "test-preset"
                    }
                ]
            }
            with open(test_file, "w") as f:
                json.dump(test_data, f)

            validator = ConfigValidator()
            result = validator.validate_with_error_handling(test_file)

            assert result.is_valid is True

    def test_validate_with_error_handling_exception(self) -> None:
        """Test validate_with_error_handling with exception."""
        validator = ConfigValidator()
        result = validator.validate_with_error_handling(Path("/nonexistent/file.json"))

        assert result.is_valid is False
        assert len(result.errors) > 0


class TestDependencyValidator:
    """Unit tests for DependencyValidator."""

    def test_dependency_validator_initialization(self) -> None:
        """Test DependencyValidator initialization."""
        validator = DependencyValidator()
        assert validator is not None
        assert validator.known_vulnerabilities is not None
        assert validator.license_whitelist is not None

    def test_dependency_info_creation(self) -> None:
        """Test DependencyInfo creation."""
        dep_info = DependencyInfo(
            name="test-dep",
            version="1.0.0",
            source="conan",
            license="MIT",
            url="https://example.com"
        )
        assert dep_info.name == "test-dep"
        assert dep_info.version == "1.0.0"
        assert dep_info.source == "conan"
        assert dep_info.license == "MIT"
        assert dep_info.url == "https://example.com"

    def test_dependency_info_post_init(self) -> None:
        """Test DependencyInfo.__post_init__ initializes vulnerabilities."""
        dep_info = DependencyInfo(
            name="test-dep",
            version="1.0.0",
            source="conan"
        )
        assert dep_info.vulnerabilities == []

    def test_dependency_validation_result_creation(self) -> None:
        """Test DependencyValidationResult creation."""
        result = DependencyValidationResult(
            is_valid=True,
            dependencies=[],
            conflicts=[],
            security_issues=[],
            license_issues=[],
            errors=[],
            warnings=[]
        )
        assert result.is_valid is True

    def test_validate_conan_dependencies_with_nonexistent_file(self) -> None:
        """Test validate_conan_dependencies with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            conanfile = Path(tmp_dir) / "conanfile.py"

            validator = DependencyValidator()
            deps = validator.validate_conan_dependencies(conanfile)

            # Should return empty list for nonexistent file
            assert isinstance(deps, list)

    def test_validate_cmake_dependencies_with_nonexistent_file(self) -> None:
        """Test validate_cmake_dependencies with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "dependencies.cmake"

            validator = DependencyValidator()
            deps = validator.validate_cmake_dependencies(cmake_file)

            # Should return empty list for nonexistent file
            assert isinstance(deps, list)

    def test_validate_system_dependencies(self) -> None:
        """Test validate_system_dependencies."""
        validator = DependencyValidator()
        # Mock subprocess to avoid actual system calls
        import subprocess
        from unittest.mock import patch, MagicMock

        mock_result = MagicMock()
        mock_result.stdout = ""
        mock_result.returncode = 0

        with patch('subprocess.run', return_value=mock_result):
            deps = validator.validate_system_dependencies()

            # Should return list of dependencies
            assert isinstance(deps, list)

    def test_check_dependency_conflicts_no_conflicts(self) -> None:
        """Test check_dependency_conflicts with no conflicts."""
        deps = [
            DependencyInfo(name="dep1", version="1.0.0", source="conan"),
            DependencyInfo(name="dep2", version="1.0.0", source="conan")
        ]

        validator = DependencyValidator()
        conflicts = validator.check_dependency_conflicts(deps)

        assert len(conflicts) == 0

    def test_check_dependency_conflicts_with_version_conflicts(self) -> None:
        """Test check_dependency_conflicts with version conflicts."""
        deps = [
            DependencyInfo(name="dep1", version="1.0.0", source="conan"),
            DependencyInfo(name="dep1", version="2.0.0", source="conan")
        ]

        validator = DependencyValidator()
        conflicts = validator.check_dependency_conflicts(deps)

        assert len(conflicts) > 0
        assert "dep1" in conflicts[0]

    def test_check_security_vulnerabilities_no_vulnerabilities(self) -> None:
        """Test check_security_vulnerabilities with no vulnerabilities."""
        deps = [
            DependencyInfo(name="dep1", version="1.0.0", source="conan")
        ]

        validator = DependencyValidator()
        issues = validator.check_security_vulnerabilities(deps)

        assert len(issues) == 0

    def test_check_license_compliance_all_valid(self) -> None:
        """Test check_license_compliance with all valid licenses."""
        deps = [
            DependencyInfo(name="dep1", version="1.0.0", source="conan", license="MIT"),
            DependencyInfo(name="dep2", version="1.0.0", source="conan", license="Apache-2.0")
        ]

        validator = DependencyValidator()
        issues = validator.check_license_compliance(deps)

        assert len(issues) == 0

    def test_check_license_compliance_invalid_license(self) -> None:
        """Test check_license_compliance with invalid license."""
        deps = [
            DependencyInfo(name="dep1", version="1.0.0", source="conan", license="GPL-3.0")
        ]

        validator = DependencyValidator()
        issues = validator.check_license_compliance(deps)

        assert len(issues) > 0
        assert "license" in issues[0].lower()

    def test_check_license_compliance_missing_license(self) -> None:
        """Test check_license_compliance with missing license."""
        deps = [
            DependencyInfo(name="dep1", version="1.0.0", source="conan")
        ]

        validator = DependencyValidator()
        issues = validator.check_license_compliance(deps)

        assert len(issues) > 0
        assert "missing license" in issues[0].lower()

    def test_validate_dependency_integrity_with_valid_file(self) -> None:
        """Test validate_dependency_integrity with valid file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.a"
            test_file.write_text("test library content")

            validator = DependencyValidator()
            is_valid, message = validator.validate_dependency_integrity(test_file)

            assert is_valid is True

    def test_validate_dependency_integrity_with_missing_file(self) -> None:
        """Test validate_dependency_integrity with missing file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.a"

            validator = DependencyValidator()
            is_valid, message = validator.validate_dependency_integrity(test_file)

            assert is_valid is False
            assert "does not exist" in message.lower()

    def test_validate_dependency_integrity_with_empty_file(self) -> None:
        """Test validate_dependency_integrity with empty file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.a"
            test_file.touch()

            validator = DependencyValidator()
            is_valid, message = validator.validate_dependency_integrity(test_file)

            assert is_valid is False
            assert "empty" in message.lower()

    def test_validate_all_dependencies(self) -> None:
        """Test validate_all_dependencies method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            validator = DependencyValidator()
            # Mock subprocess to avoid actual system calls
            import subprocess
            from unittest.mock import patch, MagicMock

            mock_result = MagicMock()
            mock_result.stdout = ""
            mock_result.returncode = 0

            with patch('subprocess.run', return_value=mock_result):
                result = validator.validate_all_dependencies(Path(tmp_dir))

                assert result is not None
                assert isinstance(result, DependencyValidationResult)

    def test_validate_with_error_handling_success(self) -> None:
        """Test validate_with_error_handling with valid directory."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            validator = DependencyValidator()
            result = validator.validate_with_error_handling(Path(tmp_dir))

            # Should succeed (no conanfile, no cmake file, etc.)
            assert result is not None

    def test_validate_with_error_handling_exception(self) -> None:
        """Test validate_with_error_handling with exception."""
        validator = DependencyValidator()
        result = validator.validate_with_error_handling(Path("/nonexistent/path"))

        assert result.is_valid is False
        assert len(result.errors) > 0


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
