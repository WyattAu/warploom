"""
WarploomController module.

This module provides the main controller components for the Warploom build system,
including the base controller, CLI parser, and command dispatcher.
"""

from warploom_scripts.controller.base import BaseController, create_base_parser, setup_controller_logging
from warploom_scripts.controller.cli import create_parser, parse_args
from warploom_scripts.controller.dispatcher import CommandDispatcher, main

__all__ = [
    # Base controller
    "BaseController",
    "setup_controller_logging",
    "create_base_parser",
    # CLI parser
    "create_parser",
    "parse_args",
    # Command dispatcher
    "CommandDispatcher",
    "main",
]
