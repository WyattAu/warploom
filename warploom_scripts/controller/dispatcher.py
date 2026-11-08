"""
Command dispatcher for WarploomController.

This module provides the command dispatcher that routes commands to
appropriate handlers, validates arguments, and manages error handling.
"""

from __future__ import annotations

import argparse
import logging
from typing import Optional

from warploom_scripts.controller.base import setup_controller_logging
from warploom_scripts.controller.cli import parse_args
from warploom_scripts.exceptions import ControllerError
from warploom_scripts.controller.build_controller import BuildController
from warploom_scripts.controller.configure_controller import ConfigureController
from warploom_scripts.controller.install_controller import InstallController
from warploom_scripts.controller.package_controller import PackageController
from warploom_scripts.controller.test_controller import TestController


class CommandDispatcher:
    """Command dispatcher for routing CLI commands to handlers.

    This class is responsible for:
    - Parsing command-line arguments
    - Routing commands to appropriate handlers
    - Validating command arguments
    - Handling errors and providing user-friendly messages
    - Managing logging configuration

    Attributes:
        args: Parsed command-line arguments.
        logger: Logger instance for the dispatcher.
    """

    def __init__(self, args: Optional[argparse.Namespace] = None) -> None:
        """Initialize the command dispatcher.

        Args:
            args: Optional pre-parsed arguments. If None, will parse from sys.argv.
        """
        self.args = args if args is not None else parse_args()
        self.logger = logging.getLogger(self.__class__.__name__)

    def dispatch(self) -> int:
        """Dispatch the command to the appropriate handler.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        # Set up logging
        self._setup_logging()

        # Log command start
        self.logger.info(f"Dispatching command: {self.args.command}")

        try:
            # Route to appropriate handler
            exit_code = self._route_command()

            # Log command completion
            if exit_code == 0:
                self.logger.info(f"Command completed successfully: {self.args.command}")
            else:
                self.logger.warning(f"Command completed with exit code {exit_code}: {self.args.command}")

            return exit_code

        except ControllerError as e:
            # Handle controller-specific errors
            self._handle_controller_error(e)
            return e.exit_code

        except Exception as e:
            # Handle unexpected errors
            self._handle_unexpected_error(e)
            return 1

    def _setup_logging(self) -> None:
        """Set up logging based on command-line arguments."""
        # Set up logging with appropriate level
        setup_controller_logging()

        # Update log level if verbose
        if self.args.verbose:
            from warploom_scripts.logging.logger import set_log_level
            set_log_level("DEBUG")

    def _route_command(self) -> int:
        """Route the command to the appropriate handler.

        Returns:
            Exit code from the command handler.

        Raises:
            ControllerError: If command validation fails.
        """
        command = self.args.command

        # Route to appropriate handler
        if command == "configure":
            return self._handle_configure()
        elif command == "build":
            return self._handle_build()
        elif command == "clean":
            return self._handle_clean()
        elif command == "install":
            return self._handle_install()
        elif command == "test":
            return self._handle_test()
        elif command == "package":
            return self._handle_package()
        elif command == "format":
            return self._handle_format()
        elif command == "lint":
            return self._handle_lint()
        else:
            raise ControllerError(
                message=f"Unknown command: {command}",
                command=command,
                context={"valid_commands": ["configure", "build", "clean", "install", "test", "package", "format", "lint"]},
                exit_code=2,
            )

    def _handle_configure(self) -> int:
        """Handle the configure command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Handling configure command")

        # Import and use configure controller for configure command
        try:
            from warploom_scripts.controller.configure_controller import ConfigureController

            controller = ConfigureController(self.args)
            return controller.execute()
        except ImportError as e:
            self.logger.error(f"ConfigureController not implemented yet: {e}")
            self.logger.info("Configure command will be implemented in future")
            return 0

    def _handle_build(self) -> int:
        """Handle the build command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Handling build command")

        # Import and use build controller
        try:
            from warploom_scripts.controller.build_controller import BuildController

            controller = BuildController(self.args)
            return controller.execute()
        except ImportError as e:
            self.logger.error(f"BuildController not implemented yet: {e}")
            self.logger.info("Build command will be implemented in future")
            return 0

    def _handle_clean(self) -> int:
        """Handle the clean command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Handling clean command")

        # Import and use clean controller
        try:
            from warploom_scripts.controller.clean_controller import CleanController

            controller = CleanController(self.args)
            return controller.execute()
        except ImportError as e:
            self.logger.error(f"CleanController not implemented yet: {e}")
            self.logger.info("Clean command will be implemented in future")
            return 0

    def _handle_install(self) -> int:
        """Handle the install command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Handling install command")

        # Import and use install controller for install command
        try:
            from warploom_scripts.controller.install_controller import InstallController

            controller = InstallController(self.args)
            return controller.execute()
        except ImportError as e:
            self.logger.error(f"InstallController not implemented yet: {e}")
            self.logger.info("Install command will be implemented in future")
            return 0

    def _handle_test(self) -> int:
        """Handle the test command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Handling test command")

        # Import and use test controller
        try:
            from warploom_scripts.controller.test_controller import TestController

            controller = TestController(self.args)
            return controller.execute()
        except ImportError as e:
            self.logger.error(f"TestController not implemented yet: {e}")
            self.logger.info("Test command will be implemented in future")
            return 0

    def _handle_package(self) -> int:
        """Handle the package command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Handling package command")

        # Import and use package controller for package command
        try:
            from warploom_scripts.controller.package_controller import PackageController

            controller = PackageController(self.args)
            return controller.execute()
        except ImportError as e:
            self.logger.error(f"PackageController not implemented yet: {e}")
            self.logger.info("Package command will be implemented in future")
            return 0

    def _handle_format(self) -> int:
        """Handle the format command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Handling format command")

        # Format command will be implemented in future
        self.logger.info("Format command will be implemented in future")
        return 0

    def _handle_lint(self) -> int:
        """Handle the lint command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Handling lint command")

        # Lint command will be implemented in future
        self.logger.info("Lint command will be implemented in future")
        return 0

    def _handle_controller_error(self, error: ControllerError) -> None:
        """Handle a controller-specific error.

        Args:
            error: The controller error to handle.
        """
        self.logger.error(
            f"Command failed: {error.command} - {error.message}",
            extra={"context": error.context},
        )

        # Provide user-friendly error message
        print(f"\nError: {error.message}", file=__import__("sys").stderr)

        # Provide suggestions if available
        if error.context and "suggestions" in error.context:
            suggestions = error.context["suggestions"]
            if suggestions:
                print("\nSuggestions:", file=__import__("sys").stderr)
                for suggestion in suggestions:
                    print(f"  - {suggestion}", file=__import__("sys").stderr)

    def _handle_unexpected_error(self, error: Exception) -> None:
        """Handle an unexpected error.

        Args:
            error: The unexpected error to handle.
        """
        self.logger.exception(f"Unexpected error: {str(error)}")

        # Provide user-friendly error message
        print(f"\nUnexpected error: {str(error)}", file=__import__("sys").stderr)
        print("Run with --verbose for more details", file=__import__("sys").stderr)


def main(args: Optional[list[str]] = None) -> int:
    """Main entry point for the controller.

    Args:
        args: Optional list of command-line arguments. If None, uses sys.argv[1:].

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    # Parse arguments
    parsed_args = parse_args(args)

    # Create dispatcher and dispatch command
    dispatcher = CommandDispatcher(parsed_args)
    return dispatcher.dispatch()


__all__ = [
    "CommandDispatcher",
    "main",
]
