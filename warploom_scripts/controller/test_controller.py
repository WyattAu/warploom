"""
Test controller for WarploomController.

This module provides a test controller that handles test-related commands.
"""

from __future__ import annotations

import argparse
import logging
import subprocess
from pathlib import Path

from warploom_scripts.controller.base import BaseController
from warploom_scripts.exceptions import ControllerError
from warploom_scripts.build_system.cmake import CMakeWrapper


class TestController(BaseController):
    """Controller for test command.

    This controller handles test-related operations such as running
    test suites, validating test configurations, and managing test
    execution.
    """

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize test controller.

        Args:
            args: Parsed command-line arguments.
        """
        super().__init__(args)
        self.compiler = getattr(args, "compiler", None)
        self.logger = logging.getLogger(self.__class__.__name__)
        self.cmake_wrapper = CMakeWrapper(
            source_dir=Path.cwd(),
            build_dir=Path("build")
        )

    def validate_arguments(self) -> None:
        """Validate test command arguments.

        Raises:
            ControllerError: If arguments are invalid.
        """
        # Validate compiler if specified
        self.validate_compiler(self.compiler)

    def execute(self) -> int:
        """Execute test command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.log_command_start("test")

        try:
            # Parse test target and config
            target = getattr(self.args, "target", "all")
            config = getattr(self.args, "config", "release")

            # Convert config to CMake build type
            build_type = config.capitalize()

            self.logger.info(f"Running C++ tests: target={target}, config={config}")

            # Build the project first
            self.logger.info("Building project...")
            build_result = self.cmake_wrapper.build(
                target=target,
                config=build_type
            )

            if build_result != 0:
                self.logger.error("Build failed, cannot run tests")
                return build_result

            # Run CTest
            self.logger.info("Running CTest...")
            test_result = self._run_ctest(build_type)

            if test_result != 0:
                self.logger.error("Tests failed")
                return test_result

            self.log_command_success("test")
            return 0

        except ControllerError as e:
            self.log_command_error("test", e)
            return e.exit_code

        except Exception as e:
            self.logger.exception(f"Unexpected error during test: {e}")
            return 1

    def _run_ctest(self, config: str) -> int:
        """Run CTest to execute C++ tests.

        Args:
            config: Build configuration (Debug, Release, etc.).

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        # Build CTest command
        cmd = [str(self.cmake_wrapper.cmake_path), "-E", "chdir", str(self.cmake_wrapper.build_dir)]
        cmd.extend(["ctest", "--output-on-failure", f"--build-config", config])

        # Add parallel jobs
        try:
            import os
            parallel = os.cpu_count() or 1
            cmd.extend(["--parallel", str(parallel)])
        except Exception:
            parallel = 1

        self.logger.debug(f"CTest command: {' '.join(cmd)}")

        try:
            result = subprocess.run(
                cmd,
                cwd=str(self.cmake_wrapper.source_dir),
                capture_output=True,
                text=True,
                timeout=600
            )

            # Log test output
            if result.stdout:
                self.logger.info(f"Test output:\n{result.stdout}")

            if result.returncode != 0:
                self.logger.error(f"CTest failed with exit code {result.returncode}")
                if result.stderr:
                    self.logger.error(f"CTest error output:\n{result.stderr}")
            else:
                self.logger.info("Tests completed successfully")

            return result.returncode

        except subprocess.TimeoutExpired:
            self.logger.error("CTest execution timed out after 600 seconds")
            return 1

        except FileNotFoundError:
            self.logger.error("CTest executable not found")
            return 1

        except Exception as e:
            self.logger.exception(f"Unexpected error running CTest: {e}")
            return 1


__all__ = [
    "TestController",
]
