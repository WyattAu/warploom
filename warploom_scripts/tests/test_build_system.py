"""
Unit tests for build system modules.

Tests for cmake, conan, vcpkg, and package_manager modules.
"""

from __future__ import annotations

import json
import tempfile
from pathlib import Path
from typing import Any
from unittest.mock import Mock, patch, MagicMock
import pytest

# Add parent directories to path for imports
import sys
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.build_system.cmake import CMakeError, CMakeWrapper, cmake_configure, cmake_build, cmake_clean
from warploom_scripts.build_system.conan import ConanError, ConanWrapper, conan_install, conan_create_profile
from warploom_scripts.build_system.vcpkg import VcpkgError, VcpkgWrapper, vcpkg_install, vcpkg_integrate
from warploom_scripts.build_system.package_manager import (
    PackageSecurityError,
    PackageManager,
    ConanManager,
    VcpkgManager,
    CPMManager,
    PackageManagerFactory,
    verify_package_security,
)


class TestCMakeError:
    """Unit tests for CMakeError exception."""

    def test_cmake_error_creation(self) -> None:
        """Test CMakeError creation."""
        error = CMakeError("Test error", "cmake command", 1)
        assert str(error) == "Test error"
        assert error.command == "cmake command"
        assert error.exit_code == 1

    def test_cmake_error_default_exit_code(self) -> None:
        """Test CMakeError with default exit code."""
        error = CMakeError("Test error", "cmake command")
        assert error.exit_code == 1


class TestCMakeWrapper:
    """Unit tests for CMakeWrapper class."""

    def test_cmake_wrapper_initialization(self) -> None:
        """Test CMakeWrapper initialization."""
        wrapper = CMakeWrapper()
        assert wrapper.source_dir == Path.cwd()
        assert wrapper.build_dir == Path.cwd() / "build"
        assert wrapper.cmake_path == Path("cmake")

    def test_cmake_wrapper_with_paths(self) -> None:
        """Test CMakeWrapper with custom paths."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "source"
            build = Path(tmp_dir) / "build"
            wrapper = CMakeWrapper(source_dir=source, build_dir=build)
            assert wrapper.source_dir == source
            assert wrapper.build_dir == build

    def test_cmake_wrapper_with_cmake_path(self) -> None:
        """Test CMakeWrapper with custom cmake path."""
        wrapper = CMakeWrapper(cmake_path=Path("/usr/bin/cmake"))
        assert wrapper.cmake_path == Path("/usr/bin/cmake")

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_configure_success(self, mock_execute: Any) -> None:
        """Test configure with success."""
        mock_execute.return_value = None

        wrapper = CMakeWrapper()
        result = wrapper.configure(build_type="Release")
        assert result == 0
        mock_execute.assert_called_once()

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_configure_with_generator(self, mock_execute: Any) -> None:
        """Test configure with generator."""
        mock_execute.return_value = None

        wrapper = CMakeWrapper()
        result = wrapper.configure(build_type="Debug", generator="Ninja")
        assert result == 0

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_configure_with_toolchain(self, mock_execute: Any) -> None:
        """Test configure with toolchain."""
        mock_execute.return_value = None

        with tempfile.TemporaryDirectory() as tmp_dir:
            toolchain = Path(tmp_dir) / "toolchain.cmake"
            toolchain.write_text("test")

            wrapper = CMakeWrapper()
            result = wrapper.configure(build_type="Release", toolchain=toolchain)
            assert result == 0

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_configure_with_preset(self, mock_execute: Any) -> None:
        """Test configure with preset."""
        mock_execute.return_value = None

        wrapper = CMakeWrapper()
        result = wrapper.configure(preset="default")
        assert result == 0

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_configure_invalid_toolchain(self, mock_execute: Any) -> None:
        """Test configure with invalid toolchain."""
        mock_execute.return_value = None

        with tempfile.TemporaryDirectory() as tmp_dir:
            toolchain = Path(tmp_dir) / "nonexistent.cmake"

            wrapper = CMakeWrapper()
            with pytest.raises(CMakeError):
                wrapper.configure(build_type="Release", toolchain=toolchain)

    @patch('warploom_scripts.build_system.cmake.execute_command')
    @patch('os.cpu_count')
    def test_build_success(self, mock_cpu_count: Any, mock_execute: Any) -> None:
        """Test build with success."""
        mock_cpu_count.return_value = 4
        mock_execute.return_value = None

        wrapper = CMakeWrapper()
        result = wrapper.build(target="all", config="Release")
        assert result == 0
        mock_execute.assert_called_once()

    @patch('warploom_scripts.build_system.cmake.execute_command')
    @patch('os.cpu_count')
    def test_build_with_parallel(self, mock_cpu_count: Any, mock_execute: Any) -> None:
        """Test build with parallel jobs."""
        mock_cpu_count.return_value = 8
        mock_execute.return_value = None

        wrapper = CMakeWrapper()
        result = wrapper.build(target="all", config="Release", parallel=4)
        assert result == 0

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_build_with_clean(self, mock_execute: Any) -> None:
        """Test build with clean."""
        mock_execute.return_value = None

        wrapper = CMakeWrapper()
        result = wrapper.build(target="all", config="Release", clean=True)
        assert result == 0

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_clean_with_target(self, mock_execute: Any) -> None:
        """Test clean with target."""
        mock_execute.return_value = None

        wrapper = CMakeWrapper()
        result = wrapper.clean(target="all")
        assert result == 0
        mock_execute.assert_called_once()

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_clean_without_target(self, mock_execute: Any) -> None:
        """Test clean without target."""
        mock_execute.return_value = None

        with tempfile.TemporaryDirectory() as tmp_dir:
            build_dir = Path(tmp_dir) / "build"
            build_dir.mkdir()

            wrapper = CMakeWrapper(build_dir=build_dir)
            result = wrapper.clean(target=None)
            assert result == 0
            assert not build_dir.exists()

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_install_success(self, mock_execute: Any) -> None:
        """Test install with success."""
        mock_execute.return_value = None

        wrapper = CMakeWrapper()
        result = wrapper.install(config="Release")
        assert result == 0
        mock_execute.assert_called_once()

    @patch('warploom_scripts.build_system.cmake.execute_command')
    def test_install_with_prefix(self, mock_execute: Any) -> None:
        """Test install with prefix."""
        mock_execute.return_value = None

        with tempfile.TemporaryDirectory() as tmp_dir:
            prefix = Path(tmp_dir) / "install"
            wrapper = CMakeWrapper()
            result = wrapper.install(prefix=prefix, config="Release")
            assert result == 0

    def test_list_presets_no_file(self) -> None:
        """Test list_presets with no preset file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            wrapper = CMakeWrapper(source_dir=Path(tmp_dir))
            presets = wrapper.list_presets()
            assert presets == []

    def test_list_presets_with_file(self) -> None:
        """Test list_presets with preset file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            presets_file = Path(tmp_dir) / "CMakePresets.json"
            presets_data = {
                "version": 3,
                "configurePresets": [
                    {"name": "default", "description": "Default preset"},
                    {"name": "debug", "description": "Debug preset"},
                ]
            }
            presets_file.write_text(json.dumps(presets_data))

            wrapper = CMakeWrapper(source_dir=Path(tmp_dir))
            presets = wrapper.list_presets()
            assert len(presets) == 2
            assert presets[0]["name"] == "default"
            assert presets[1]["name"] == "debug"

    def test_get_preset_found(self) -> None:
        """Test get_preset when preset is found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            presets_file = Path(tmp_dir) / "CMakePresets.json"
            presets_data = {
                "version": 3,
                "configurePresets": [
                    {"name": "default", "description": "Default preset"},
                ]
            }
            presets_file.write_text(json.dumps(presets_data))

            wrapper = CMakeWrapper(source_dir=Path(tmp_dir))
            preset = wrapper.get_preset("default")
            assert preset is not None
            assert preset["name"] == "default"

    def test_get_preset_not_found(self) -> None:
        """Test get_preset when preset is not found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            wrapper = CMakeWrapper(source_dir=Path(tmp_dir))
            preset = wrapper.get_preset("nonexistent")
            assert preset is None

    def test_select_toolchain_linux_x86_64(self) -> None:
        """Test select_toolchain for Linux x86_64."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            toolchains_dir = Path(tmp_dir) / "toolchains"
            toolchains_dir.mkdir()
            toolchain_file = toolchains_dir / "x86-linux-gnu.cmake"
            toolchain_file.write_text("test")

            wrapper = CMakeWrapper(source_dir=Path(tmp_dir))
            toolchain = wrapper.select_toolchain("linux", "x86_64", toolchains_dir)
            assert toolchain == toolchain_file

    def test_select_toolchain_windows_arm64(self) -> None:
        """Test select_toolchain for Windows ARM64."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            toolchains_dir = Path(tmp_dir) / "toolchains"
            toolchains_dir.mkdir()
            toolchain_file = toolchains_dir / "arm64-windows-msvc.cmake"
            toolchain_file.write_text("test")

            wrapper = CMakeWrapper(source_dir=Path(tmp_dir))
            toolchain = wrapper.select_toolchain("windows", "ARM64", toolchains_dir)
            assert toolchain == toolchain_file

    def test_select_toolchain_not_found(self) -> None:
        """Test select_toolchain when toolchain not found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            wrapper = CMakeWrapper(source_dir=Path(tmp_dir))
            with pytest.raises(CMakeError):
                wrapper.select_toolchain("invalid", "invalid")

    def test_validate_toolchain_valid(self) -> None:
        """Test validate_toolchain with valid toolchain."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            toolchain = Path(tmp_dir) / "toolchain.cmake"
            toolchain.write_text("set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)")

            wrapper = CMakeWrapper()
            result = wrapper.validate_toolchain(toolchain)
            assert result is True

    def test_validate_toolchain_nonexistent(self) -> None:
        """Test validate_toolchain with nonexistent toolchain."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            toolchain = Path(tmp_dir) / "nonexistent.cmake"

            wrapper = CMakeWrapper()
            result = wrapper.validate_toolchain(toolchain)
            assert result is False

    def test_validate_toolchain_empty(self) -> None:
        """Test validate_toolchain with empty toolchain."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            toolchain = Path(tmp_dir) / "empty.cmake"
            toolchain.write_text("")

            wrapper = CMakeWrapper()
            result = wrapper.validate_toolchain(toolchain)
            assert result is False

    @patch('subprocess.run')
    def test_get_version_success(self, mock_subprocess: Any) -> None:
        """Test get_version with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "cmake version 3.28.0\n"
        mock_subprocess.return_value = mock_result

        wrapper = CMakeWrapper()
        version = wrapper.get_version()
        assert version == "3.28.0"

    @patch('subprocess.run')
    def test_get_version_failure(self, mock_subprocess: Any) -> None:
        """Test get_version with failure."""
        mock_subprocess.side_effect = Exception("Command failed")

        wrapper = CMakeWrapper()
        version = wrapper.get_version()
        assert version is None


class TestConanError:
    """Unit tests for ConanError exception."""

    def test_conan_error_creation(self) -> None:
        """Test ConanError creation."""
        error = ConanError("Test error", "conan command", 1)
        assert str(error) == "Test error"
        assert error.command == "conan command"
        assert error.exit_code == 1


class TestConanWrapper:
    """Unit tests for ConanWrapper class."""

    def test_conan_wrapper_initialization(self) -> None:
        """Test ConanWrapper initialization."""
        wrapper = ConanWrapper()
        assert wrapper.conan_path == Path("conan")
        assert wrapper.project_dir == Path.cwd()
        assert wrapper.conan_dir == Path.cwd() / "conan"

    def test_conan_wrapper_with_paths(self) -> None:
        """Test ConanWrapper with custom paths."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()
            wrapper = ConanWrapper(project_dir=project)
            assert wrapper.project_dir == project
            assert wrapper.conan_dir == project / "conan"

    @patch('warploom_scripts.build_system.conan.execute_command')
    def test_install_success(self, mock_execute: Any) -> None:
        """Test install with success."""
        mock_execute.return_value = None

        wrapper = ConanWrapper()
        result = wrapper.install(profile="default", build_type="Release")
        assert result == 0
        mock_execute.assert_called_once()

    @patch('warploom_scripts.build_system.conan.execute_command')
    def test_install_with_conanfile(self, mock_execute: Any) -> None:
        """Test install with conanfile path."""
        mock_execute.return_value = None

        with tempfile.TemporaryDirectory() as tmp_dir:
            conanfile = Path(tmp_dir) / "conanfile.txt"
            conanfile.write_text("test")

            wrapper = ConanWrapper()
            result = wrapper.install(profile="default", build_type="Release", conanfile_path=conanfile)
            assert result == 0

    @patch('warploom_scripts.build_system.conan.execute_command')
    def test_install_invalid_conanfile(self, mock_execute: Any) -> None:
        """Test install with invalid conanfile."""
        mock_execute.return_value = None

        with tempfile.TemporaryDirectory() as tmp_dir:
            conanfile = Path(tmp_dir) / "nonexistent.txt"

            wrapper = ConanWrapper()
            with pytest.raises(ConanError):
                wrapper.install(profile="default", build_type="Release", conanfile_path=conanfile)

    def test_create_profile_success(self) -> None:
        """Test create_profile with success."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()

            wrapper = ConanWrapper(project_dir=project)
            profile_path = wrapper.create_profile(
                profile_name="test-profile",
                compiler="gcc",
                compiler_version="11",
                arch="x86_64",
                build_type="Release",
            )
            assert profile_path.exists()
            assert profile_path.name == "test-profile"

    def test_create_profile_with_extra_settings(self) -> None:
        """Test create_profile with extra settings."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()

            wrapper = ConanWrapper(project_dir=project)
            profile_path = wrapper.create_profile(
                profile_name="test-profile",
                compiler="clang",
                compiler_version="14",
                arch="ARM64",
                build_type="Debug",
                extra_settings={"fPIC": "True"},
            )
            assert profile_path.exists()

    def test_list_profiles_empty(self) -> None:
        """Test list_profiles with no profiles."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()

            wrapper = ConanWrapper(project_dir=project)
            profiles = wrapper.list_profiles()
            assert profiles == []

    def test_list_profiles_with_files(self) -> None:
        """Test list_profiles with profile files."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()
            profiles_dir = project / "conan" / "profiles"
            profiles_dir.mkdir(parents=True)
            (profiles_dir / "profile1").write_text("test")
            (profiles_dir / "profile2").write_text("test")

            wrapper = ConanWrapper(project_dir=project)
            profiles = wrapper.list_profiles()
            assert len(profiles) >= 2

    def test_get_profile_found(self) -> None:
        """Test get_profile when profile is found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()
            profiles_dir = project / "conan" / "profiles"
            profiles_dir.mkdir(parents=True)
            profile_file = profiles_dir / "test-profile"
            profile_file.write_text("[settings]\nos=Linux\narch=x86_64")

            wrapper = ConanWrapper(project_dir=project)
            profile = wrapper.get_profile("test-profile")
            assert profile is not None
            assert profile["settings"]["os"] == "Linux"

    def test_get_profile_not_found(self) -> None:
        """Test get_profile when profile is not found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()

            wrapper = ConanWrapper(project_dir=project)
            profile = wrapper.get_profile("nonexistent")
            assert profile is None

    @patch('warploom_scripts.build_system.conan.execute_command')
    def test_integrate_cmake_success(self, mock_execute: Any) -> None:
        """Test integrate_cmake with success."""
        mock_execute.return_value = None

        wrapper = ConanWrapper()
        result = wrapper.integrate_cmake()
        assert result == 0
        mock_execute.assert_called_once()

    @patch('subprocess.run')
    def test_search_success(self, mock_subprocess: Any) -> None:
        """Test search with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "fmt/10.0.0\nspdlog/1.12.0\n"
        mock_subprocess.return_value = mock_result

        wrapper = ConanWrapper()
        packages = wrapper.search("fmt")
        assert len(packages) >= 1
        assert packages[0]["name"] == "fmt"

    def test_remove_profile_success(self) -> None:
        """Test remove_profile with success."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()
            profiles_dir = project / "conan" / "profiles"
            profiles_dir.mkdir(parents=True)
            profile_file = profiles_dir / "test-profile"
            profile_file.write_text("test")

            wrapper = ConanWrapper(project_dir=project)
            result = wrapper.remove_profile("test-profile")
            assert result == 0
            assert not profile_file.exists()

    def test_remove_profile_not_found(self) -> None:
        """Test remove_profile when profile not found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()

            wrapper = ConanWrapper(project_dir=project)
            result = wrapper.remove_profile("nonexistent")
            assert result == 1

    @patch('subprocess.run')
    def test_get_version_success(self, mock_subprocess: Any) -> None:
        """Test get_version with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "Conan version 2.0.0\n"
        mock_subprocess.return_value = mock_result

        wrapper = ConanWrapper()
        version = wrapper.get_version()
        assert version == "2.0.0"

    def test_validate_profile_valid(self) -> None:
        """Test validate_profile with valid profile."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()
            profiles_dir = project / "conan" / "profiles"
            profiles_dir.mkdir(parents=True)
            profile_file = profiles_dir / "test-profile"
            profile_file.write_text("[settings]\nos=Linux\narch=x86_64\ncompiler=gcc\ncompiler.version=11\nbuild_type=Release")

            wrapper = ConanWrapper(project_dir=project)
            result = wrapper.validate_profile("test-profile")
            assert result is True

    def test_validate_profile_invalid(self) -> None:
        """Test validate_profile with invalid profile."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            project = Path(tmp_dir) / "project"
            project.mkdir()

            wrapper = ConanWrapper(project_dir=project)
            result = wrapper.validate_profile("nonexistent")
            assert result is False


class TestVcpkgError:
    """Unit tests for VcpkgError exception."""

    def test_vcpkg_error_creation(self) -> None:
        """Test VcpkgError creation."""
        error = VcpkgError("Test error", "vcpkg command", 1)
        assert str(error) == "Test error"
        assert error.command == "vcpkg command"
        assert error.exit_code == 1


class TestVcpkgWrapper:
    """Unit tests for VcpkgWrapper class."""

    def test_vcpkg_wrapper_initialization(self) -> None:
        """Test VcpkgWrapper initialization."""
        wrapper = VcpkgWrapper()
        assert wrapper.vcpkg_exe == Path("vcpkg")
        assert wrapper.project_dir == Path.cwd()

    def test_vcpkg_wrapper_with_directory(self) -> None:
        """Test VcpkgWrapper with vcpkg directory."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            vcpkg_dir = Path(tmp_dir) / "vcpkg"
            vcpkg_dir.mkdir()

            wrapper = VcpkgWrapper(vcpkg_path=vcpkg_dir)
            assert wrapper.vcpkg_root == vcpkg_dir

    @patch('warploom_scripts.build_system.vcpkg.execute_command')
    def test_install_success(self, mock_execute: Any) -> None:
        """Test install with success."""
        mock_execute.return_value = None

        wrapper = VcpkgWrapper()
        result = wrapper.install(packages=["fmt"], triplet="x64-windows")
        assert result == 0
        mock_execute.assert_called_once()

    @patch('warploom_scripts.build_system.vcpkg.execute_command')
    def test_remove_success(self, mock_execute: Any) -> None:
        """Test remove with success."""
        mock_execute.return_value = None

        wrapper = VcpkgWrapper()
        result = wrapper.remove(packages=["fmt"], triplet="x64-windows")
        assert result == 0
        mock_execute.assert_called_once()

    @patch('warploom_scripts.build_system.vcpkg.execute_command')
    def test_integrate_success(self, mock_execute: Any) -> None:
        """Test integrate with success."""
        mock_execute.return_value = None

        wrapper = VcpkgWrapper()
        result = wrapper.integrate()
        assert result == 0
        mock_execute.assert_called_once()

    @patch('subprocess.run')
    def test_list_packages_success(self, mock_subprocess: Any) -> None:
        """Test list_packages with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "fmt:x64-windows\nspdlog:x64-windows\n"
        mock_subprocess.return_value = mock_result

        wrapper = VcpkgWrapper()
        packages = wrapper.list_packages()
        assert len(packages) >= 1
        assert packages[0]["name"] == "fmt"

    @patch('subprocess.run')
    def test_search_success(self, mock_subprocess: Any) -> None:
        """Test search with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "fmt\nspdlog\n"
        mock_subprocess.return_value = mock_result

        wrapper = VcpkgWrapper()
        packages = wrapper.search("fmt")
        assert len(packages) >= 1

    def test_list_triplets_empty(self) -> None:
        """Test list_triplets with no triplets."""
        wrapper = VcpkgWrapper()
        triplets = wrapper.list_triplets()
        assert triplets == []

    def test_list_triplets_with_files(self) -> None:
        """Test list_triplets with triplet files."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            vcpkg_dir = Path(tmp_dir) / "vcpkg"
            vcpkg_dir.mkdir()
            triplets_dir = vcpkg_dir / "triplets"
            triplets_dir.mkdir()
            (triplets_dir / "x64-windows.cmake").write_text("test")
            (triplets_dir / "x64-linux.cmake").write_text("test")

            wrapper = VcpkgWrapper(vcpkg_path=vcpkg_dir)
            triplets = wrapper.list_triplets()
            assert len(triplets) >= 2

    def test_get_triplet_found(self) -> None:
        """Test get_triplet when triplet is found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            vcpkg_dir = Path(tmp_dir) / "vcpkg"
            vcpkg_dir.mkdir()
            triplets_dir = vcpkg_dir / "triplets"
            triplets_dir.mkdir()
            triplet_file = triplets_dir / "x64-windows.cmake"
            triplet_file.write_text("set(VCPKG_TARGET_ARCHITECTURE x64)")

            wrapper = VcpkgWrapper(vcpkg_path=vcpkg_dir)
            triplet = wrapper.get_triplet("x64-windows")
            assert triplet is not None

    def test_get_triplet_not_found(self) -> None:
        """Test get_triplet when triplet is not found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            vcpkg_dir = Path(tmp_dir) / "vcpkg"
            vcpkg_dir.mkdir()

            wrapper = VcpkgWrapper(vcpkg_path=vcpkg_dir)
            triplet = wrapper.get_triplet("nonexistent")
            assert triplet is None

    def test_select_triplet_windows_x64(self) -> None:
        """Test select_triplet for Windows x64."""
        wrapper = VcpkgWrapper()
        triplet = wrapper.select_triplet("windows", "x64")
        assert triplet == "x64-windows"

    def test_select_triplet_linux_arm64(self) -> None:
        """Test select_triplet for Linux ARM64."""
        wrapper = VcpkgWrapper()
        triplet = wrapper.select_triplet("linux", "arm64")
        assert triplet == "arm64-linux"

    def test_select_triplet_invalid(self) -> None:
        """Test select_triplet with invalid platform."""
        wrapper = VcpkgWrapper()
        with pytest.raises(VcpkgError):
            wrapper.select_triplet("invalid", "invalid")

    @patch('warploom_scripts.build_system.vcpkg.execute_command')
    def test_export_success(self, mock_execute: Any) -> None:
        """Test export with success."""
        mock_execute.return_value = None

        with tempfile.TemporaryDirectory() as tmp_dir:
            output_dir = Path(tmp_dir) / "output"
            wrapper = VcpkgWrapper()
            result = wrapper.export(packages=["fmt"], output_dir=output_dir)
            assert result == 0

    @patch('subprocess.run')
    def test_get_version_success(self, mock_subprocess: Any) -> None:
        """Test get_version with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "vcpkg package management program version 2023-12-12-hash\n"
        mock_subprocess.return_value = mock_result

        wrapper = VcpkgWrapper()
        version = wrapper.get_version()
        assert version == "2023-12-12-hash"

    def test_validate_triplet_valid(self) -> None:
        """Test validate_triplet with valid triplet."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            vcpkg_dir = Path(tmp_dir) / "vcpkg"
            vcpkg_dir.mkdir()
            triplets_dir = vcpkg_dir / "triplets"
            triplets_dir.mkdir()
            triplet_file = triplets_dir / "x64-windows.cmake"
            triplet_file.write_text("set(VCPKG_TARGET_ARCHITECTURE x64)\nset(VCPKG_CRT_LINKAGE dynamic)")

            wrapper = VcpkgWrapper(vcpkg_path=vcpkg_dir)
            result = wrapper.validate_triplet("x64-windows")
            assert result is True

    def test_validate_triplet_invalid(self) -> None:
        """Test validate_triplet with invalid triplet."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            vcpkg_dir = Path(tmp_dir) / "vcpkg"
            vcpkg_dir.mkdir()

            wrapper = VcpkgWrapper(vcpkg_path=vcpkg_dir)
            result = wrapper.validate_triplet("nonexistent")
            assert result is False


class TestPackageManager:
    """Unit tests for PackageManager base class."""

    def test_package_manager_initialization(self) -> None:
        """Test PackageManager initialization."""
        pm = PackageManager("test", priority=1)
        assert pm.name == "test"
        assert pm.priority == 1
        assert pm.available is False

    def test_package_manager_check_available_not_implemented(self) -> None:
        """Test PackageManager.check_available raises NotImplementedError."""
        pm = PackageManager("test", priority=1)
        with pytest.raises(NotImplementedError):
            pm.check_available()

    def test_package_manager_install_package_not_implemented(self) -> None:
        """Test PackageManager.install_package raises NotImplementedError."""
        pm = PackageManager("test", priority=1)
        with pytest.raises(NotImplementedError):
            pm.install_package("test-package")

    def test_package_manager_verify_package_not_implemented(self) -> None:
        """Test PackageManager.verify_package raises NotImplementedError."""
        pm = PackageManager("test", priority=1)
        with pytest.raises(NotImplementedError):
            pm.verify_package("test-package")


class TestConanManager:
    """Unit tests for ConanManager class."""

    @patch('subprocess.run')
    def test_conan_manager_available(self, mock_subprocess: Any) -> None:
        """Test ConanManager when Conan is available."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        manager = ConanManager()
        assert manager.available is True
        assert manager.check_available() is True

    @patch('subprocess.run')
    def test_conan_manager_not_available(self, mock_subprocess: Any) -> None:
        """Test ConanManager when Conan is not available."""
        mock_subprocess.side_effect = FileNotFoundError()

        manager = ConanManager()
        assert manager.available is False
        assert manager.check_available() is False

    @patch('subprocess.run')
    def test_install_package_success(self, mock_subprocess: Any) -> None:
        """Test ConanManager.install_package with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        manager = ConanManager()
        manager.available = True
        result = manager.install_package("fmt")
        assert result is True

    @patch('subprocess.run')
    def test_install_package_with_version(self, mock_subprocess: Any) -> None:
        """Test ConanManager.install_package with version."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        manager = ConanManager()
        manager.available = True
        result = manager.install_package("fmt", "10.0.0")
        assert result is True

    @patch('subprocess.run')
    def test_install_package_failure(self, mock_subprocess: Any) -> None:
        """Test ConanManager.install_package with failure."""
        mock_result = Mock()
        mock_result.returncode = 1
        mock_result.stderr = "Error"
        mock_subprocess.return_value = mock_result

        manager = ConanManager()
        manager.available = True
        result = manager.install_package("fmt")
        assert result is False

    @patch('subprocess.run')
    def test_verify_package_success(self, mock_subprocess: Any) -> None:
        """Test ConanManager.verify_package with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "fmt/10.0.0"
        mock_subprocess.return_value = mock_result

        manager = ConanManager()
        manager.available = True
        result = manager.verify_package("fmt")
        assert result is True

    @patch('subprocess.run')
    def test_verify_package_failure(self, mock_subprocess: Any) -> None:
        """Test ConanManager.verify_package with failure."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "other-package"
        mock_subprocess.return_value = mock_result

        manager = ConanManager()
        manager.available = True
        result = manager.verify_package("fmt")
        assert result is False


class TestVcpkgManager:
    """Unit tests for VcpkgManager class."""

    @patch('subprocess.run')
    def test_vcpkg_manager_available(self, mock_subprocess: Any) -> None:
        """Test VcpkgManager when vcpkg is available."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        manager = VcpkgManager()
        assert manager.available is True
        assert manager.check_available() is True

    @patch('subprocess.run')
    def test_vcpkg_manager_not_available(self, mock_subprocess: Any) -> None:
        """Test VcpkgManager when vcpkg is not available."""
        mock_subprocess.side_effect = FileNotFoundError()

        manager = VcpkgManager()
        assert manager.available is False
        assert manager.check_available() is False

    @patch('subprocess.run')
    def test_install_package_success(self, mock_subprocess: Any) -> None:
        """Test VcpkgManager.install_package with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        manager = VcpkgManager()
        manager.available = True
        result = manager.install_package("fmt")
        assert result is True

    @patch('subprocess.run')
    def test_install_package_with_version(self, mock_subprocess: Any) -> None:
        """Test VcpkgManager.install_package with version."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        manager = VcpkgManager()
        manager.available = True
        result = manager.install_package("fmt", "10.0.0")
        assert result is True

    @patch('subprocess.run')
    def test_install_package_failure(self, mock_subprocess: Any) -> None:
        """Test VcpkgManager.install_package with failure."""
        mock_result = Mock()
        mock_result.returncode = 1
        mock_result.stderr = "Error"
        mock_subprocess.return_value = mock_result

        manager = VcpkgManager()
        manager.available = True
        result = manager.install_package("fmt")
        assert result is False

    @patch('subprocess.run')
    def test_verify_package_success(self, mock_subprocess: Any) -> None:
        """Test VcpkgManager.verify_package with success."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "fmt:x64-windows"
        mock_subprocess.return_value = mock_result

        manager = VcpkgManager()
        manager.available = True
        result = manager.verify_package("fmt")
        assert result is True

    @patch('subprocess.run')
    def test_verify_package_failure(self, mock_subprocess: Any) -> None:
        """Test VcpkgManager.verify_package with failure."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "other-package"
        mock_subprocess.return_value = mock_result

        manager = VcpkgManager()
        manager.available = True
        result = manager.verify_package("fmt")
        assert result is False


class TestCPMManager:
    """Unit tests for CPMManager class."""

    def test_cpm_manager_available(self) -> None:
        """Test CPMManager when CMakeLists.txt exists."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            manager = CPMManager(cmake_file=cmake_file)
            assert manager.available is True
            assert manager.check_available() is True

    def test_cpm_manager_not_available(self) -> None:
        """Test CPMManager when CMakeLists.txt does not exist."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"

            manager = CPMManager(cmake_file=cmake_file)
            assert manager.available is False
            assert manager.check_available() is False

    def test_install_package_success(self) -> None:
        """Test CPMManager.install_package."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            manager = CPMManager(cmake_file=cmake_file)
            manager.available = True
            result = manager.install_package("fmt")
            assert result is True

    def test_verify_package_success(self) -> None:
        """Test CPMManager.verify_package."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            manager = CPMManager(cmake_file=cmake_file)
            manager.available = True
            result = manager.verify_package("fmt")
            assert result is True


class TestPackageManagerFactory:
    """Unit tests for PackageManagerFactory class."""

    def test_factory_initialization(self) -> None:
        """Test PackageManagerFactory initialization."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            factory = PackageManagerFactory(cmake_file=cmake_file)
            assert factory.conan is not None
            assert factory.vcpkg is not None
            assert factory.cpm is not None
            assert len(factory.managers) == 3

    @patch('subprocess.run')
    def test_get_available_managers(self, mock_subprocess: Any) -> None:
        """Test get_available_managers."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            factory = PackageManagerFactory(cmake_file=cmake_file)
            available = factory.get_available_managers()
            assert len(available) >= 1
            # Should be sorted by priority
            if len(available) > 1:
                assert available[0].priority <= available[1].priority

    @patch('subprocess.run')
    def test_get_best_manager(self, mock_subprocess: Any) -> None:
        """Test get_best_manager."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            factory = PackageManagerFactory(cmake_file=cmake_file)
            best = factory.get_best_manager()
            assert best is not None
            assert best.priority == 1  # Conan has highest priority

    @patch('subprocess.run')
    def test_install_package_best_manager(self, mock_subprocess: Any) -> None:
        """Test install_package with best manager."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            factory = PackageManagerFactory(cmake_file=cmake_file)
            result = factory.install_package("fmt")
            assert result is True

    @patch('subprocess.run')
    def test_install_package_preferred_manager(self, mock_subprocess: Any) -> None:
        """Test install_package with preferred manager."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_subprocess.return_value = mock_result

        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            factory = PackageManagerFactory(cmake_file=cmake_file)
            result = factory.install_package("fmt", preferred_manager="conan")
            assert result is True

    @patch('subprocess.run')
    def test_verify_package_best_manager(self, mock_subprocess: Any) -> None:
        """Test verify_package with best manager."""
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "fmt/10.0.0"
        mock_subprocess.return_value = mock_result

        with tempfile.TemporaryDirectory() as tmp_dir:
            cmake_file = Path(tmp_dir) / "CMakeLists.txt"
            cmake_file.write_text("test")

            factory = PackageManagerFactory(cmake_file=cmake_file)
            result = factory.verify_package("fmt")
            assert result is True


class TestVerifyPackageSecurity:
    """Unit tests for verify_package_security function."""

    def test_verify_package_security_success(self) -> None:
        """Test verify_package_security with success."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            package_file = Path(tmp_dir) / "package.tar.gz"
            package_file.write_bytes(b"test content")

            result = verify_package_security("test-package", package_file)
            assert result is True

    def test_verify_package_security_file_not_found(self) -> None:
        """Test verify_package_security with file not found."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            package_file = Path(tmp_dir) / "nonexistent.tar.gz"

            with pytest.raises(PackageSecurityError):
                verify_package_security("test-package", package_file)

    def test_package_security_error_creation(self) -> None:
        """Test PackageSecurityError creation."""
        error = PackageSecurityError(
            "Security verification failed",
            package_name="test-package",
            issue="Hash mismatch"
        )
        assert str(error) == "Security verification failed"
        assert error.package_name == "test-package"
        assert error.issue == "Hash mismatch"


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
