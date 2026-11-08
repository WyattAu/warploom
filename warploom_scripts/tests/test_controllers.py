"""
Unit tests for controller modules.

Tests for build_controller, clean_controller, configure_controller,
test_controller, and other controller modules.
"""

from __future__ import annotations

import argparse
import tempfile
from pathlib import Path
from typing import Any
from unittest.mock import Mock, patch, MagicMock
import pytest

# Add parent directories to path for imports
import sys
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.controller.build_controller import BuildController, build
from warploom_scripts.controller.clean_controller import CleanController, clean
from warploom_scripts.controller.configure_controller import ConfigureController, configure
from warploom_scripts.controller.test_controller import TestController
from warploom_scripts.exceptions import InvalidTargetError, ConfigurationError


class TestBuildController:
    """Unit tests for BuildController class."""

    def test_build_controller_initialization(self) -> None:
        """Test BuildController initialization."""
        args = argparse.Namespace(
            target="all",
            pipeline="default",
            preset="default",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
        )
        controller = BuildController(args)
        assert controller.target == "all"
        assert controller.pipeline == "default"
        assert controller.preset == "default"
        assert controller.config == "release"
        assert controller.compiler is None
        assert controller.clean is False
        assert controller.parallel is None

    def test_build_controller_with_compiler(self) -> None:
        """Test BuildController with compiler specified."""
        args = argparse.Namespace(
            target="engine",
            pipeline="default",
            preset="default",
            config="debug",
            compiler="gcc",
            clean=True,
            parallel=4,
        )
        controller = BuildController(args)
        assert controller.target == "engine"
        assert controller.config == "debug"
        assert controller.compiler == "gcc"
        assert controller.clean is True
        assert controller.parallel == 4

    def test_get_cmake_config_debug(self) -> None:
        """Test get_cmake_config for debug."""
        args = argparse.Namespace(
            target="all",
            config="debug",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        config = controller.get_cmake_config()
        assert config == "Debug"

    def test_get_cmake_config_release(self) -> None:
        """Test get_cmake_config for release."""
        args = argparse.Namespace(
            target="all",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        config = controller.get_cmake_config()
        assert config == "Release"

    def test_get_cmake_config_invalid(self) -> None:
        """Test get_cmake_config with invalid config."""
        args = argparse.Namespace(
            target="all",
            config="invalid",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        config = controller.get_cmake_config()
        # Should default to Release for invalid config
        assert config == "Release"

    def test_get_build_target_all(self) -> None:
        """Test get_build_target for all."""
        args = argparse.Namespace(
            target="all",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        target = controller.get_build_target()
        assert target == "all"

    def test_get_build_target_engine(self) -> None:
        """Test get_build_target for engine."""
        args = argparse.Namespace(
            target="engine",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        target = controller.get_build_target()
        assert target == "warploom_engine"

    def test_get_build_target_game(self) -> None:
        """Test get_build_target for game."""
        args = argparse.Namespace(
            target="game",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        target = controller.get_build_target()
        assert target == "warploom_pong"

    def test_get_build_target_standalone(self) -> None:
        """Test get_build_target for standalone."""
        args = argparse.Namespace(
            target="standalone",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        target = controller.get_build_target()
        assert target == "warploom_standalone"

    def test_get_build_target_invalid(self) -> None:
        """Test get_build_target with invalid target."""
        args = argparse.Namespace(
            target="invalid",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        target = controller.get_build_target()
        # Should default to "all" for invalid target
        assert target == "all"

    @patch('warploom_scripts.controller.build_controller.CMakeWrapper')
    def test_clean_before_build_success(self, mock_cmake_wrapper: Any) -> None:
        """Test clean_before_build with success."""
        mock_wrapper = Mock()
        mock_wrapper.clean.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            target="all",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        result = controller.clean_before_build()
        assert result == 0
        mock_wrapper.clean.assert_called_once()

    @patch('warploom_scripts.controller.build_controller.CMakeWrapper')
    def test_clean_before_build_failure(self, mock_cmake_wrapper: Any) -> None:
        """Test clean_before_build with failure."""
        mock_wrapper = Mock()
        mock_wrapper.clean.return_value = 1
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            target="all",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        result = controller.clean_before_build()
        assert result == 1

    @patch('warploom_scripts.controller.build_controller.CMakeWrapper')
    @patch('os.cpu_count')
    def test_build_target_success(self, mock_cpu_count: Any, mock_cmake_wrapper: Any) -> None:
        """Test build_target with success."""
        mock_cpu_count.return_value = 4
        mock_wrapper = Mock()
        mock_wrapper.build.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            target="engine",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        result = controller.build_target()
        assert result == 0
        mock_wrapper.build.assert_called_once()

    @patch('warploom_scripts.controller.build_controller.CMakeWrapper')
    @patch('os.cpu_count')
    def test_build_target_with_parallel(self, mock_cpu_count: Any, mock_cmake_wrapper: Any) -> None:
        """Test build_target with parallel jobs."""
        mock_cpu_count.return_value = 8
        mock_wrapper = Mock()
        mock_wrapper.build.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            target="all",
            config="debug",
            compiler=None,
            clean=False,
            parallel=4,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        result = controller.build_target()
        assert result == 0

    @patch('warploom_scripts.controller.build_controller.CMakeWrapper')
    def test_build_target_failure(self, mock_cmake_wrapper: Any) -> None:
        """Test build_target with failure."""
        mock_wrapper = Mock()
        mock_wrapper.build.return_value = 1
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            target="all",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        result = controller.build_target()
        assert result == 1

    def test_validate_build_output_no_build_dir(self) -> None:
        """Test validate_build_output with no build directory."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            args = argparse.Namespace(
                target="all",
                config="release",
                compiler=None,
                clean=False,
                parallel=None,
                preset="default",
                pipeline="default",
            )
            controller = BuildController(args)
            # Mock get_project_root to return temp dir
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            result = controller.validate_build_output()
            assert result is False

    def test_validate_build_output_with_files(self) -> None:
        """Test validate_build_output with output files."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            build_dir = Path(tmp_dir) / "build"
            build_dir.mkdir()
            (build_dir / "test.exe").write_text("test")

            args = argparse.Namespace(
                target="all",
                config="release",
                compiler=None,
                clean=False,
                parallel=None,
                preset="default",
                pipeline="default",
            )
            controller = BuildController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            result = controller.validate_build_output()
            assert result is True

    @patch('warploom_scripts.controller.build_controller.CMakeWrapper')
    def test_execute_success(self, mock_cmake_wrapper: Any) -> None:
        """Test execute with success."""
        mock_wrapper = Mock()
        mock_wrapper.get_preset.return_value = {"name": "default"}
        mock_wrapper.build.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            target="all",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        result = controller.execute()
        assert result == 0

    @patch('warploom_scripts.controller.build_controller.CMakeWrapper')
    def test_execute_with_clean(self, mock_cmake_wrapper: Any) -> None:
        """Test execute with clean before build."""
        mock_wrapper = Mock()
        mock_wrapper.get_preset.return_value = {"name": "default"}
        mock_wrapper.clean.return_value = 0
        mock_wrapper.build.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            target="all",
            config="release",
            compiler=None,
            clean=True,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        result = controller.execute()
        assert result == 0
        mock_wrapper.clean.assert_called_once()

    @patch('warploom_scripts.controller.build_controller.CMakeWrapper')
    def test_execute_invalid_target(self, mock_cmake_wrapper: Any) -> None:
        """Test execute with invalid target."""
        mock_wrapper = Mock()
        mock_wrapper.get_preset.return_value = {"name": "default"}
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            target="invalid",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        controller = BuildController(args)
        result = controller.execute()
        assert result != 0


class TestCleanController:
    """Unit tests for CleanController class."""

    def test_clean_controller_initialization(self) -> None:
        """Test CleanController initialization."""
        args = argparse.Namespace(target="all")
        controller = CleanController(args)
        assert controller.target == "all"

    def test_clean_controller_engine_target(self) -> None:
        """Test CleanController with engine target."""
        args = argparse.Namespace(target="engine")
        controller = CleanController(args)
        assert controller.target == "engine"

    def test_get_clean_directories_all(self) -> None:
        """Test get_clean_directories for all target."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            build_dir = Path(tmp_dir) / "build"
            build_dir.mkdir()
            packages_dir = Path(tmp_dir) / "packages"
            packages_dir.mkdir()

            args = argparse.Namespace(target="all")
            controller = CleanController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            directories = controller.get_clean_directories()
            assert len(directories) >= 1
            assert build_dir in directories

    def test_get_clean_directories_engine(self) -> None:
        """Test get_clean_directories for engine target."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            build_dir = Path(tmp_dir) / "build"
            build_dir.mkdir()
            engine_build = build_dir / "engine"
            engine_build.mkdir()

            args = argparse.Namespace(target="engine")
            controller = CleanController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            directories = controller.get_clean_directories()
            assert len(directories) >= 1

    def test_get_clean_files(self) -> None:
        """Test get_clean_files method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create some test files
            (Path(tmp_dir) / "test.o").write_text("test")
            (Path(tmp_dir) / "test.exe").write_text("test")
            (Path(tmp_dir) / "test.py").write_text("test")

            args = argparse.Namespace(target="all")
            controller = CleanController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            files = controller.get_clean_files()
            assert len(files) >= 2  # Should find .o and .exe files

    def test_clean_directory_success(self) -> None:
        """Test clean_directory with success."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir) / "test_dir"
            test_dir.mkdir()

            args = argparse.Namespace(target="all")
            controller = CleanController(args)
            result = controller.clean_directory(test_dir)
            assert result == 0
            assert not test_dir.exists()

    def test_clean_directory_nonexistent(self) -> None:
        """Test clean_directory with nonexistent directory."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir) / "nonexistent"

            args = argparse.Namespace(target="all")
            controller = CleanController(args)
            result = controller.clean_directory(test_dir)
            assert result == 0

    def test_clean_file_success(self) -> None:
        """Test clean_file with success."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            test_file.write_text("test")

            args = argparse.Namespace(target="all")
            controller = CleanController(args)
            result = controller.clean_file(test_file)
            assert result == 0
            assert not test_file.exists()

    def test_clean_file_nonexistent(self) -> None:
        """Test clean_file with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "nonexistent.txt"

            args = argparse.Namespace(target="all")
            controller = CleanController(args)
            result = controller.clean_file(test_file)
            assert result == 0

    @patch('warploom_scripts.controller.clean_controller.CMakeWrapper')
    def test_clean_cmake_cache_success(self, mock_cmake_wrapper: Any) -> None:
        """Test clean_cmake_cache with success."""
        mock_wrapper = Mock()
        mock_wrapper.clean.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(target="all")
        controller = CleanController(args)
        result = controller.clean_cmake_cache()
        assert result == 0
        mock_wrapper.clean.assert_called_once()

    @patch('warploom_scripts.controller.clean_controller.CMakeWrapper')
    def test_execute_success(self, mock_cmake_wrapper: Any) -> None:
        """Test execute with success."""
        mock_wrapper = Mock()
        mock_wrapper.clean.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        with tempfile.TemporaryDirectory() as tmp_dir:
            args = argparse.Namespace(target="all")
            controller = CleanController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            result = controller.execute()
            assert result == 0


class TestConfigureController:
    """Unit tests for ConfigureController class."""

    def test_configure_controller_initialization(self) -> None:
        """Test ConfigureController initialization."""
        args = argparse.Namespace(
            build_type="Release",
            generator="Ninja",
            toolchain=None,
            preset=None,
            configure_conan=False,
            configure_vcpkg=False,
        )
        controller = ConfigureController(args)
        assert controller.build_type == "Release"
        assert controller.generator == "Ninja"
        assert controller.toolchain is None
        assert controller.preset is None
        assert controller.configure_conan is False
        assert controller.configure_vcpkg is False

    def test_configure_controller_with_preset(self) -> None:
        """Test ConfigureController with preset."""
        args = argparse.Namespace(
            build_type="Debug",
            generator=None,
            toolchain=None,
            preset="default",
            configure_conan=True,
            configure_vcpkg=True,
        )
        controller = ConfigureController(args)
        assert controller.build_type == "Debug"
        assert controller.preset == "default"
        assert controller.configure_conan is True
        assert controller.configure_vcpkg is True

    def test_validate_arguments_no_config_method(self) -> None:
        """Test validate_arguments with no configuration method."""
        args = argparse.Namespace(
            build_type="Release",
            generator=None,
            toolchain=None,
            preset=None,
            configure_conan=False,
            configure_vcpkg=False,
        )
        controller = ConfigureController(args)
        with pytest.raises(ConfigurationError):
            controller.validate_arguments()

    @patch('warploom_scripts.controller.configure_controller.CMakeWrapper')
    def test_validate_arguments_with_preset(self, mock_cmake_wrapper: Any) -> None:
        """Test validate_arguments with valid preset."""
        mock_wrapper = Mock()
        mock_wrapper.get_preset.return_value = {"name": "default"}
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            build_type="Release",
            generator=None,
            toolchain=None,
            preset="default",
            configure_conan=False,
            configure_vcpkg=False,
        )
        controller = ConfigureController(args)
        # Should not raise exception
        controller.validate_arguments()

    @patch('warploom_scripts.controller.configure_controller.CMakeWrapper')
    def test_validate_arguments_invalid_preset(self, mock_cmake_wrapper: Any) -> None:
        """Test validate_arguments with invalid preset."""
        mock_wrapper = Mock()
        mock_wrapper.get_preset.return_value = None
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            build_type="Release",
            generator=None,
            toolchain=None,
            preset="invalid",
            configure_conan=False,
            configure_vcpkg=False,
        )
        controller = ConfigureController(args)
        with pytest.raises(ConfigurationError):
            controller.validate_arguments()

    @patch('warploom_scripts.controller.configure_controller.CMakeWrapper')
    def test_configure_cmake_success(self, mock_cmake_wrapper: Any) -> None:
        """Test configure_cmake with success."""
        mock_wrapper = Mock()
        mock_wrapper.configure.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            build_type="Release",
            generator="Ninja",
            toolchain=None,
            preset=None,
            configure_conan=False,
            configure_vcpkg=False,
        )
        controller = ConfigureController(args)
        result = controller.configure_cmake()
        assert result == 0
        mock_wrapper.configure.assert_called_once()

    @patch('warploom_scripts.controller.configure_controller.ConanWrapper')
    def test_configure_conan_dependencies_success(self, mock_conan_wrapper: Any) -> None:
        """Test configure_conan_dependencies with success."""
        mock_wrapper = Mock()
        mock_wrapper.validate_profile.return_value = True
        mock_wrapper.install.return_value = 0
        mock_conan_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            build_type="Release",
            generator=None,
            toolchain=None,
            preset=None,
            configure_conan=True,
            configure_vcpkg=False,
        )
        controller = ConfigureController(args)
        result = controller.configure_conan_dependencies()
        assert result == 0

    @patch('warploom_scripts.controller.configure_controller.ConanWrapper')
    def test_configure_conan_dependencies_no_profile(self, mock_conan_wrapper: Any) -> None:
        """Test configure_conan_dependencies with no profile."""
        mock_wrapper = Mock()
        mock_wrapper.validate_profile.return_value = False
        mock_conan_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(
            build_type="Release",
            generator=None,
            toolchain=None,
            preset=None,
            configure_conan=True,
            configure_vcpkg=False,
        )
        controller = ConfigureController(args)
        result = controller.configure_conan_dependencies()
        # Should return 0 (skip) when profile not found
        assert result == 0

    @patch('warploom_scripts.controller.configure_controller.VcpkgWrapper')
    @patch('platform.system')
    def test_configure_vcpkg_dependencies_success(self, mock_platform: Any, mock_vcpkg_wrapper: Any) -> None:
        """Test configure_vcpkg_dependencies with success."""
        mock_platform.return_value = "Windows"
        mock_wrapper = Mock()
        mock_wrapper.install.return_value = 0
        mock_vcpkg_wrapper.return_value = mock_wrapper

        with tempfile.TemporaryDirectory() as tmp_dir:
            # Create vcpkg.json
            vcpkg_json = Path(tmp_dir) / "vcpkg.json"
            vcpkg_json.write_text('{"dependencies": ["fmt"]}')

            args = argparse.Namespace(
                build_type="Release",
                generator=None,
                toolchain=None,
                preset=None,
                configure_conan=False,
                configure_vcpkg=True,
            )
            controller = ConfigureController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            result = controller.configure_vcpkg_dependencies()
            assert result == 0

    @patch('warploom_scripts.controller.configure_controller.VcpkgWrapper')
    @patch('platform.system')
    def test_configure_vcpkg_dependencies_no_json(self, mock_platform: Any, mock_vcpkg_wrapper: Any) -> None:
        """Test configure_vcpkg_dependencies with no vcpkg.json."""
        mock_platform.return_value = "Windows"
        mock_wrapper = Mock()
        mock_vcpkg_wrapper.return_value = mock_wrapper

        with tempfile.TemporaryDirectory() as tmp_dir:
            args = argparse.Namespace(
                build_type="Release",
                generator=None,
                toolchain=None,
                preset=None,
                configure_conan=False,
                configure_vcpkg=True,
            )
            controller = ConfigureController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            result = controller.configure_vcpkg_dependencies()
            # Should return 0 (skip) when vcpkg.json not found
            assert result == 0

    def test_validate_configuration_no_build_dir(self) -> None:
        """Test validate_configuration with no build directory."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            args = argparse.Namespace(
                build_type="Release",
                generator="Ninja",
                toolchain=None,
                preset=None,
                configure_conan=False,
                configure_vcpkg=False,
            )
            controller = ConfigureController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            result = controller.validate_configuration()
            assert result is False

    def test_validate_configuration_success(self) -> None:
        """Test validate_configuration with valid configuration."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            build_dir = Path(tmp_dir) / "build"
            build_dir.mkdir()
            (build_dir / "CMakeCache.txt").write_text("test")
            (build_dir / "CMakeFiles").mkdir()

            args = argparse.Namespace(
                build_type="Release",
                generator="Ninja",
                toolchain=None,
                preset=None,
                configure_conan=False,
                configure_vcpkg=False,
            )
            controller = ConfigureController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            result = controller.validate_configuration()
            assert result is True

    @patch('warploom_scripts.controller.configure_controller.CMakeWrapper')
    def test_execute_success(self, mock_cmake_wrapper: Any) -> None:
        """Test execute with success."""
        mock_wrapper = Mock()
        mock_wrapper.configure.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        with tempfile.TemporaryDirectory() as tmp_dir:
            build_dir = Path(tmp_dir) / "build"
            build_dir.mkdir()
            (build_dir / "CMakeCache.txt").write_text("test")
            (build_dir / "CMakeFiles").mkdir()

            args = argparse.Namespace(
                build_type="Release",
                generator="Ninja",
                toolchain=None,
                preset=None,
                configure_conan=False,
                configure_vcpkg=False,
            )
            controller = ConfigureController(args)
            controller.get_project_root = Mock(return_value=Path(tmp_dir))
            result = controller.execute()
            assert result == 0


class TestTestController:
    """Unit tests for TestController class."""

    def test_test_controller_initialization(self) -> None:
        """Test TestController initialization."""
        args = argparse.Namespace(target="all", config="release")
        controller = TestController(args)
        assert controller.args == args
        assert controller.cmake_wrapper is not None

    @patch('warploom_scripts.controller.test_controller.CMakeWrapper')
    def test_execute_success(self, mock_cmake_wrapper: Any) -> None:
        """Test execute with success."""
        mock_wrapper = Mock()
        mock_wrapper.build.return_value = 0
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(target="all", config="release")
        controller = TestController(args)

        with patch.object(controller, '_run_ctest', return_value=0):
            result = controller.execute()
            assert result == 0

    @patch('warploom_scripts.controller.test_controller.CMakeWrapper')
    def test_execute_build_failure(self, mock_cmake_wrapper: Any) -> None:
        """Test execute with build failure."""
        mock_wrapper = Mock()
        mock_wrapper.build.return_value = 1
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(target="all", config="release")
        controller = TestController(args)
        result = controller.execute()
        assert result == 1

    @patch('warploom_scripts.controller.test_controller.CMakeWrapper')
    @patch('subprocess.run')
    @patch('os.cpu_count')
    def test_run_ctest_success(self, mock_cpu_count: Any, mock_subprocess: Any, mock_cmake_wrapper: Any) -> None:
        """Test _run_ctest with success."""
        mock_cpu_count.return_value = 4
        mock_result = Mock()
        mock_result.returncode = 0
        mock_result.stdout = "Test output"
        mock_result.stderr = ""
        mock_subprocess.return_value = mock_result
        mock_wrapper = Mock()
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(target="all", config="release")
        controller = TestController(args)
        result = controller._run_ctest("Release")
        assert result == 0

    @patch('warploom_scripts.controller.test_controller.CMakeWrapper')
    @patch('subprocess.run')
    def test_run_ctest_failure(self, mock_subprocess: Any, mock_cmake_wrapper: Any) -> None:
        """Test _run_ctest with failure."""
        mock_result = Mock()
        mock_result.returncode = 1
        mock_result.stdout = "Test output"
        mock_result.stderr = "Test error"
        mock_subprocess.return_value = mock_result
        mock_wrapper = Mock()
        mock_cmake_wrapper.return_value = mock_wrapper

        args = argparse.Namespace(target="all", config="release")
        controller = TestController(args)
        result = controller._run_ctest("Release")
        assert result == 1


class TestBuildFunction:
    """Unit tests for build function."""

    @patch('warploom_scripts.controller.build_controller.BuildController')
    def test_build_function(self, mock_controller: Any) -> None:
        """Test build function."""
        mock_instance = Mock()
        mock_instance.execute.return_value = 0
        mock_controller.return_value = mock_instance

        args = argparse.Namespace(
            target="all",
            config="release",
            compiler=None,
            clean=False,
            parallel=None,
            preset="default",
            pipeline="default",
        )
        result = build(args)
        assert result == 0
        mock_instance.execute.assert_called_once()


class TestCleanFunction:
    """Unit tests for clean function."""

    @patch('warploom_scripts.controller.clean_controller.CleanController')
    def test_clean_function(self, mock_controller: Any) -> None:
        """Test clean function."""
        mock_instance = Mock()
        mock_instance.execute.return_value = 0
        mock_controller.return_value = mock_instance

        args = argparse.Namespace(target="all")
        result = clean(args)
        assert result == 0
        mock_instance.execute.assert_called_once()


class TestConfigureFunction:
    """Unit tests for configure function."""

    @patch('warploom_scripts.controller.configure_controller.ConfigureController')
    def test_configure_function(self, mock_controller: Any) -> None:
        """Test configure function."""
        mock_instance = Mock()
        mock_instance.execute.return_value = 0
        mock_controller.return_value = mock_instance

        args = argparse.Namespace(
            build_type="Release",
            generator="Ninja",
            toolchain=None,
            preset=None,
            configure_conan=False,
            configure_vcpkg=False,
        )
        result = configure(args)
        assert result == 0
        mock_instance.execute.assert_called_once()


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
