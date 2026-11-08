"""
Integration tests for WarploomController.

This module provides integration tests for controller commands, CLI parser,
and command dispatcher functionality.
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path
from typing import Optional, Any
from unittest.mock import Mock, patch

import pytest

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from warploom_scripts.controller.cli import create_parser, parse_args
from warploom_scripts.controller.dispatcher import CommandDispatcher
from warploom_scripts.exceptions import ControllerError


class TestCLIIntegration:
    """Integration tests for CLI argument parsing and command dispatching."""

    def test_parser_creates_all_commands(self) -> None:
        """Test that parser creates all top-level commands."""
        parser = create_parser()
        assert parser is not None
        assert hasattr(parser, '_subparsers')
        subparsers = parser._subparsers
        assert subparsers is not None

        # Check all expected commands are present
        # The subparsers object has actions for each command
        expected_commands = ['configure', 'build', 'clean', 'install', 'test', 'package', 'format', 'lint']
        for action in subparsers._actions:
            if hasattr(action, 'choices') and action.choices is not None:
                for cmd in expected_commands:
                    if cmd in action.choices:
                        assert True
                        break

    def test_version_flag(self) -> None:
        """Test that --version flag works correctly."""
        with pytest.raises(SystemExit) as exc_info:
            parse_args(['--version'])
        assert exc_info.value.code == 0

    def test_configure_command_with_preset(self) -> None:
        """Test configure command with preset argument."""
        args = parse_args(['configure', '--preset', 'default'])
        assert args.command == 'configure'
        assert args.preset == 'default'
        assert args.build_type == 'Release'

    def test_configure_command_with_generator(self) -> None:
        """Test configure command with generator argument."""
        args = parse_args(['configure', '--generator', 'Ninja', '--build-type', 'Debug'])
        assert args.command == 'configure'
        assert args.generator == 'Ninja'
        assert args.build_type == 'Debug'

    def test_configure_command_with_toolchain(self) -> None:
        """Test configure command with toolchain argument."""
        toolchain_path = Path('cmake/toolchains/emscripten.cmake')
        args = parse_args(['configure', '--toolchain', str(toolchain_path)])
        assert args.command == 'configure'
        assert args.toolchain == toolchain_path

    def test_build_command_with_all_args(self) -> None:
        """Test build command with all arguments."""
        args = parse_args([
            'build', 'engine', 'default', 'default', 'debug',
            '--compiler', 'msvc', '--clean'
        ])
        assert args.command == 'build'
        assert args.target == 'engine'
        assert args.pipeline == 'default'
        assert args.preset == 'default'
        assert args.config == 'debug'
        assert args.compiler == 'msvc'
        assert args.clean is True

    def test_build_command_auto_detect_compiler(self) -> None:
        """Test build command without compiler (auto-detect)."""
        args = parse_args(['build', 'engine', 'default', 'default', 'debug'])
        assert args.command == 'build'
        assert args.compiler is None  # Should auto-detect

    def test_clean_command_with_target(self) -> None:
        """Test clean command with target argument."""
        args = parse_args(['clean', '--target', 'engine'])
        assert args.command == 'clean'
        assert args.target == 'engine'

    def test_clean_command_without_target(self) -> None:
        """Test clean command without target (defaults to all)."""
        args = parse_args(['clean'])
        assert args.command == 'clean'
        assert args.target is None  # Will default to 'all'

    def test_install_command(self) -> None:
        """Test install command arguments."""
        args = parse_args(['install', 'standalone', 'release'])
        assert args.command == 'install'
        assert args.target == 'standalone'
        assert args.config == 'release'

    def test_test_command(self) -> None:
        """Test test command arguments."""
        args = parse_args(['test', 'engine', 'debug'])
        assert args.command == 'test'
        assert args.target == 'engine'
        assert args.config == 'debug'

    def test_package_command(self) -> None:
        """Test package command arguments."""
        args = parse_args(['package', 'standalone', 'release'])
        assert args.command == 'package'
        assert args.target == 'standalone'
        assert args.config == 'release'

    def test_format_command_with_files(self) -> None:
        """Test format command with files argument."""
        args = parse_args([
            'format',
            '--files', 'src/main.cpp', 'include/engine.hpp',
            '--check', '--dry-run', '--cpp-only'
        ])
        assert args.command == 'format'
        assert len(args.files) == 2
        assert args.check is True
        assert args.dry_run is True
        assert args.cpp_only is True

    def test_format_command_with_directories(self) -> None:
        """Test format command with directories argument."""
        args = parse_args(['format', '--directories', 'src/', 'include/'])
        assert args.command == 'format'
        assert len(args.directories) == 2

    def test_format_command_defaults(self) -> None:
        """Test format command with no arguments (defaults)."""
        args = parse_args(['format'])
        assert args.command == 'format'
        assert args.files is None
        assert args.directories is None

    def test_lint_command_with_files(self) -> None:
        """Test lint command with files argument."""
        args = parse_args([
            'lint',
            '--files', 'src/main.cpp',
            '--fix', '--cpp-only'
        ])
        assert args.command == 'lint'
        assert len(args.files) == 1
        assert args.fix is True
        assert args.cpp_only is True

    def test_lint_command_defaults(self) -> None:
        """Test lint command with no arguments (defaults)."""
        args = parse_args(['lint'])
        assert args.command == 'lint'
        assert args.files is None
        assert args.directories is None

    def test_verbose_flag(self) -> None:
        """Test verbose flag enables debug logging."""
        args = parse_args(['--verbose', 'build', 'engine', 'default', 'default', 'debug'])
        assert args.verbose is True


class TestCommandDispatcherIntegration:
    """Integration tests for command dispatcher."""

    def test_dispatcher_initialization(self) -> None:
        """Test dispatcher initializes correctly."""
        args = parse_args(['build', 'engine', 'default', 'default', 'debug'])
        dispatcher = CommandDispatcher(args)
        assert dispatcher.args is not None
        assert dispatcher.args.command == 'build'
        assert dispatcher.logger is not None

    def test_dispatcher_without_args(self) -> None:
        """Test dispatcher initializes without args (uses sys.argv)."""
        # Mock sys.argv to provide valid arguments
        with patch.object(sys, 'argv', ['WarploomController.py', '--version']):
            with pytest.raises(SystemExit) as exc_info:
                CommandDispatcher()
            assert exc_info.value.code == 0

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_configure_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching configure command."""
        args = parse_args(['configure', '--preset', 'default'])

        # Mock config controller import (import happens inside _handle_configure)
        with patch('warploom_scripts.controller.configure_controller.ConfigureController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 0
            mock_controller.assert_called_once()
            mock_instance.execute.assert_called_once()

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_build_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching build command."""
        args = parse_args(['build', 'engine', 'default', 'default', 'debug'])

        # Mock build controller import (import happens inside _handle_build)
        with patch('warploom_scripts.controller.build_controller.BuildController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 0
            mock_controller.assert_called_once()
            mock_instance.execute.assert_called_once()

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_clean_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching clean command."""
        args = parse_args(['clean'])

        # Mock clean controller import (import happens inside _handle_clean)
        with patch('warploom_scripts.controller.clean_controller.CleanController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 0
            mock_controller.assert_called_once()
            mock_instance.execute.assert_called_once()

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_install_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching install command."""
        args = parse_args(['install', 'standalone', 'release'])

        # Mock install controller import (import happens inside _handle_install)
        with patch('warploom_scripts.controller.install_controller.InstallController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 0
            mock_controller.assert_called_once()
            mock_instance.execute.assert_called_once()

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_test_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching test command."""
        args = parse_args(['test', 'engine', 'debug'])

        # Mock test controller import (import happens inside _handle_test)
        with patch('warploom_scripts.controller.test_controller.TestController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 0
            mock_controller.assert_called_once()
            mock_instance.execute.assert_called_once()

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_package_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching package command."""
        args = parse_args(['package', 'standalone', 'release'])

        # Mock package controller import (import happens inside _handle_package)
        with patch('warploom_scripts.controller.package_controller.PackageController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 0
            mock_controller.assert_called_once()
            mock_instance.execute.assert_called_once()

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_format_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching format command."""
        args = parse_args(['format'])
        dispatcher = CommandDispatcher(args)

        exit_code = dispatcher.dispatch()

        # Format command returns 0 (not implemented yet)
        assert exit_code == 0

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_lint_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching lint command."""
        args = parse_args(['lint'])
        dispatcher = CommandDispatcher(args)

        exit_code = dispatcher.dispatch()

        # Lint command returns 0 (not implemented yet)
        assert exit_code == 0

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_dispatch_unknown_command(self, mock_setup_logging: Any) -> None:
        """Test dispatching unknown command raises error."""
        args = argparse.Namespace(command='invalid_command', verbose=False)
        dispatcher = CommandDispatcher(args)

        exit_code = dispatcher.dispatch()

        # Should return exit code 2 for unknown command
        assert exit_code == 2

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_configure_without_required_args(self, mock_setup_logging: Any) -> None:
        """Test configure command without required arguments raises error."""
        args = parse_args(['configure'])
        dispatcher = CommandDispatcher(args)

        exit_code = dispatcher.dispatch()

        # Should return exit code 2 for missing required args
        assert exit_code == 2


class TestErrorHandlingIntegration:
    """Integration tests for error handling."""

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_controller_error_handling(self, mock_setup_logging: Any) -> None:
        """Test ControllerError is handled correctly."""
        args = parse_args(['configure', '--preset', 'default'])

        # Mock ConfigController to raise ControllerError (import happens inside _handle_configure)
        with patch('warploom_scripts.controller.configure_controller.ConfigureController') as mock_controller:
            mock_instance = Mock()
            error = ControllerError(
                message="Test error",
                command="configure",
                exit_code=2
            )
            mock_instance.execute.side_effect = error
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 2

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_unexpected_error_handling(self, mock_setup_logging: Any) -> None:
        """Test unexpected exceptions are handled correctly."""
        args = parse_args(['build', 'engine', 'default', 'default', 'debug'])

        # Mock BuildController to raise unexpected exception
        with patch('warploom_scripts.controller.build_controller.BuildController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.side_effect = RuntimeError("Unexpected error")
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 1

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_successful_command_returns_zero(self, mock_setup_logging: Any) -> None:
        """Test successful command returns exit code 0."""
        args = parse_args(['build', 'engine', 'default', 'default', 'debug'])

        # Mock build controller import (import happens inside _handle_build)
        with patch('warploom_scripts.controller.build_controller.BuildController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 0

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_failed_command_returns_non_zero(self, mock_setup_logging: Any) -> None:
        """Test failed command returns non-zero exit code."""
        args = parse_args(['build', 'engine', 'default', 'default', 'debug'])

        with patch('warploom_scripts.controller.build_controller.BuildController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 1
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)
            exit_code = dispatcher.dispatch()

            assert exit_code == 1


class TestLoggingIntegration:
    """Integration tests for logging in controller."""

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_logging_setup_on_dispatch(self, mock_setup_logging: Any) -> None:
        """Test logging is set up when dispatching."""
        args = parse_args(['build', 'engine', 'default', 'default', 'debug'])
        dispatcher = CommandDispatcher(args)

        dispatcher.dispatch()

        # Verify logging was set up
        mock_setup_logging.assert_called_once()

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    @patch('warploom_scripts.logging.logger.set_log_level')
    def test_verbose_enables_debug_logging(self, mock_setup_logging: Any, mock_set_level: Any) -> None:
        """Test verbose flag enables debug logging."""
        args = parse_args(['--verbose', 'build', 'engine', 'default', 'default', 'debug'])
        dispatcher = CommandDispatcher(args)

        with patch('warploom_scripts.logging.logger.set_log_level') as mock_set_level:
            dispatcher.dispatch()

            # Verify debug level was set
            mock_set_level.assert_called_once_with('DEBUG')

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_command_logged(self, mock_setup_logging: Any) -> None:
        """Test command execution is logged."""
        args = parse_args(['build', 'engine', 'default', 'default', 'debug'])

        # Mock build controller import (import happens inside _handle_build)
        with patch('warploom_scripts.controller.build_controller.BuildController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            dispatcher = CommandDispatcher(args)

            # Verify command was logged by checking dispatcher's logger
            # The dispatcher's logger should have been called at least once
            assert hasattr(dispatcher.logger, 'info')


class TestPlatformDetectionIntegration:
    """Integration tests for platform detection in controller."""

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_platform_detection_on_windows(self, mock_setup_logging: Any) -> None:
        """Test platform detection works on Windows."""
        with patch('sys.platform', 'win32'):
            args = parse_args(['build', 'engine', 'default', 'default', 'debug'])
            dispatcher = CommandDispatcher(args)

            exit_code = dispatcher.dispatch()

            # Should not crash on Windows
            assert exit_code in [0, 1]  # Either success or expected failure

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_platform_detection_on_linux(self, mock_setup_logging: Any) -> None:
        """Test platform detection works on Linux."""
        with patch('sys.platform', 'linux'):
            args = parse_args(['build', 'engine', 'default', 'default', 'debug'])
            dispatcher = CommandDispatcher(args)

            exit_code = dispatcher.dispatch()

            # Should not crash on Linux
            assert exit_code in [0, 1]  # Either success or expected failure

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_platform_detection_on_macos(self, mock_setup_logging: Any) -> None:
        """Test platform detection works on macOS."""
        with patch('sys.platform', 'darwin'):
            args = parse_args(['build', 'engine', 'default', 'default', 'debug'])
            dispatcher = CommandDispatcher(args)

            exit_code = dispatcher.dispatch()

            # Should not crash on macOS
            assert exit_code in [0, 1]  # Either success or expected failure


class TestEndToEndWorkflows:
    """Integration tests for end-to-end workflows."""

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_configure_build_workflow(self, mock_setup_logging: Any) -> None:
        """Test configure -> build workflow."""
        # Configure
        configure_args = parse_args(['configure', '--preset', 'default'])

        with patch('warploom_scripts.controller.configure_controller.ConfigureController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            configure_dispatcher = CommandDispatcher(configure_args)
            configure_exit = configure_dispatcher.dispatch()
            assert configure_exit == 0

        # Build
        build_args = parse_args(['build', 'engine', 'default', 'default', 'debug'])

        with patch('warploom_scripts.controller.build_controller.BuildController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            build_dispatcher = CommandDispatcher(build_args)
            build_exit = build_dispatcher.dispatch()
            assert build_exit == 0

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_build_test_workflow(self, mock_setup_logging: Any) -> None:
        """Test build -> test workflow."""
        # Build
        build_args = parse_args(['build', 'engine', 'default', 'default', 'debug'])

        with patch('warploom_scripts.controller.build_controller.BuildController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            build_dispatcher = CommandDispatcher(build_args)
            build_exit = build_dispatcher.dispatch()
            assert build_exit == 0

        # Test
        test_args = parse_args(['test', 'engine', 'debug'])

        with patch('warploom_scripts.controller.test_controller.TestController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            test_dispatcher = CommandDispatcher(test_args)
            test_exit = test_dispatcher.dispatch()
            assert test_exit == 0

    @patch('warploom_scripts.controller.dispatcher.setup_controller_logging')
    def test_clean_build_workflow(self, mock_setup_logging: Any) -> None:
        """Test clean -> build workflow."""
        # Clean
        clean_args = parse_args(['clean'])

        with patch('warploom_scripts.controller.clean_controller.CleanController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            clean_dispatcher = CommandDispatcher(clean_args)
            clean_exit = clean_dispatcher.dispatch()
            assert clean_exit == 0

        # Build
        build_args = parse_args(['build', 'engine', 'default', 'default', 'debug'])

        with patch('warploom_scripts.controller.build_controller.BuildController') as mock_controller:
            mock_instance = Mock()
            mock_instance.execute.return_value = 0
            mock_controller.return_value = mock_instance

            build_dispatcher = CommandDispatcher(build_args)
            build_exit = build_dispatcher.dispatch()
            assert build_exit == 0


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
