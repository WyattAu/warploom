"""
OmniCPP Logging System

This package provides a comprehensive logging infrastructure for the OmniCPP project.
It supports multiple handlers, custom formatters, and configuration-driven behavior.

Usage:
    from warploom_scripts.logging import setup_logging, get_logger
    
    # Initialize logging (call once at application start)
    setup_logging()
    
    # Get a logger for your module
    logger = get_logger(__name__)
    logger.info("Hello, World!")

For backward compatibility with old logging functions:
    from warploom_scripts.logging import log_info, log_warning, log_error, log_success
    
    log_info("This is an info message")
    log_warning("This is a warning")
    log_error("This is an error")
    log_success("This is a success message")
"""

from .config import (
    DEFAULT_CONFIG,
    LoggingConfigError,
    get_config_value,
    get_log_level,
    is_valid_config,
    load_config,
    validate_config,
)
from .formatters import ColoredFormatter, CustomFormatter, JsonFormatter
from .handlers import ConsoleHandler, FileHandler, create_console_handler, create_file_handler
from .logger import (
    get_logger,
    is_logging_initialized,
    log_critical,
    log_debug,
    log_error,
    log_exception,
    log_info,
    log_success,
    log_warning,
    set_log_level,
    setup_logging,
    shutdown_logging,
)

__all__ = [
    # Configuration
    "DEFAULT_CONFIG",
    "LoggingConfigError",
    "load_config",
    "validate_config",
    "get_log_level",
    "get_config_value",
    "is_valid_config",
    # Formatters
    "CustomFormatter",
    "ColoredFormatter",
    "JsonFormatter",
    # Handlers
    "ConsoleHandler",
    "FileHandler",
    "create_console_handler",
    "create_file_handler",
    # Logger setup
    "setup_logging",
    "get_logger",
    "set_log_level",
    "is_logging_initialized",
    "shutdown_logging",
    # Convenience functions
    "log_info",
    "log_warning",
    "log_error",
    "log_debug",
    "log_critical",
    "log_success",
    "log_exception",
]
