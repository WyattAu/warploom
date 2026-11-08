"""
Logger setup and unified interface for the Warploom logging system.

This module provides a unified logging interface that can be imported by all
Python scripts in the project. It handles logger initialization, configuration,
and provides convenience functions for common logging operations.
"""

import logging
from typing import Any, Dict, Optional

from .config import get_log_level, load_config
from .handlers import create_console_handler, create_file_handler


# Global logger instance
_root_logger: Optional[logging.Logger] = None
_config: Optional[Dict[str, Any]] = None


def setup_logging(
    config_path: Optional[str] = None,
    config: Optional[Dict[str, Any]] = None
) -> logging.Logger:
    """
    Set up the logging system with the specified configuration.

    This function initializes the root logger with handlers and formatters
    based on the provided configuration. It should be called once at the
    start of the application.

    Args:
        config_path: Path to the logging configuration file. If None and
                     config is also None, uses the default path
        config: Configuration dictionary. If provided, takes precedence over
                config_path

    Returns:
        The configured root logger instance

    Raises:
        LoggingConfigError: If the configuration is invalid
    """
    global _root_logger, _config

    # Load configuration
    if config is not None:
        _config = config
    else:
        _config = load_config(config_path)

    # Get the root logger
    _root_logger = logging.getLogger()

    # Clear any existing handlers
    _root_logger.handlers.clear()

    # Set the log level
    log_level = get_log_level(_config["level"])
    _root_logger.setLevel(log_level)

    # Create and add console handler if enabled
    if _config["console_handler_enabled"]:
        console_handler = create_console_handler(
            enabled=True,
            use_colors=_config["colored_output"],
            format_string=_config["format"],
            datefmt=_config["datefmt"]
        )
        if console_handler:
            _root_logger.addHandler(console_handler)

    # Create and add file handler if enabled
    if _config["file_handler_enabled"]:
        file_handler = create_file_handler(
            enabled=True,
            file_path=_config["file_path"],
            max_bytes=_config["max_bytes"],
            backup_count=_config["backup_count"],
            format_string=_config["format"],
            datefmt=_config["datefmt"],
            use_json=_config.get("use_json", False)
        )
        if file_handler:
            _root_logger.addHandler(file_handler)

    # Prevent propagation to avoid duplicate logs
    _root_logger.propagate = False

    return _root_logger


def get_logger(name: Optional[str] = None) -> logging.Logger:
    """
    Get a logger instance with the specified name.

    This function returns a logger instance. If no name is provided,
    it uses the calling module's __name__. If logging has not been
    set up, it initializes it with default configuration.

    Args:
        name: The name of the logger. If None, uses the calling module's name

    Returns:
        A logger instance
    """
    global _root_logger

    # Initialize logging if not already done
    if _root_logger is None:
        setup_logging()

    # Get the logger with the specified name
    if name is None:
        # Get the caller's module name
        import inspect
        frame = inspect.currentframe()
        if frame is not None and frame.f_back is not None:
            name = frame.f_back.f_globals.get('__name__', 'warploom_scripts')
        else:
            name = 'warploom_scripts'

    logger = logging.getLogger(name)

    # Ensure the logger has the correct level
    if _config is not None:
        log_level = get_log_level(_config["level"])
        logger.setLevel(log_level)

    return logger


def set_log_level(level: str) -> None:
    """
    Dynamically change the log level at runtime.

    This function allows changing the log level without restarting the
    application. It updates both the root logger and all child loggers.

    Args:
        level: The new log level (DEBUG, INFO, WARNING, ERROR, CRITICAL)

    Raises:
        LoggingConfigError: If the log level is invalid
    """
    global _config

    # Validate the log level
    log_level = get_log_level(level)

    # Update the configuration
    if _config is not None:
        _config["level"] = level.upper()

    # Update the root logger
    if _root_logger is not None:
        _root_logger.setLevel(log_level)

        # Update all child loggers
        # Access loggerDict through manager (implementation detail but necessary)
        manager = logging.root.manager
        if hasattr(manager, 'loggerDict'):
            for logger_name in manager.loggerDict.keys():
                logger = logging.getLogger(logger_name)
                logger.setLevel(log_level)


def get_current_config() -> Dict[str, Any]:
    """
    Get the current logging configuration.

    Returns:
        The current logging configuration dictionary
    """
    global _config

    if _config is None:
        return {}

    return _config.copy()


def is_logging_initialized() -> bool:
    """
    Check if the logging system has been initialized.

    Returns:
        True if logging has been initialized, False otherwise
    """
    global _root_logger
    return _root_logger is not None


def shutdown_logging() -> None:
    """
    Shutdown the logging system.

    This function closes all handlers and performs cleanup.
    It should be called before application exit.
    """
    global _root_logger, _config

    if _root_logger is not None:
        # Close all handlers
        for handler in _root_logger.handlers[:]:
            handler.close()
            _root_logger.removeHandler(handler)

        _root_logger = None

    _config = None


# Convenience functions for backward compatibility
def log_info(message: str, *args: Any, **kwargs: Any) -> None:
    """
    Log an info-level message.

    This function provides backward compatibility with the old logging API.

    Args:
        message: The message to log
        *args: Additional arguments for string formatting
        **kwargs: Additional keyword arguments for the logger
    """
    logger = get_logger()
    logger.info(message, *args, **kwargs)


def log_warning(message: str, *args: Any, **kwargs: Any) -> None:
    """
    Log a warning-level message.

    This function provides backward compatibility with the old logging API.

    Args:
        message: The message to log
        *args: Additional arguments for string formatting
        **kwargs: Additional keyword arguments for the logger
    """
    logger = get_logger()
    logger.warning(message, *args, **kwargs)


def log_error(message: str, *args: Any, **kwargs: Any) -> None:
    """
    Log an error-level message.

    This function provides backward compatibility with the old logging API.

    Args:
        message: The message to log
        *args: Additional arguments for string formatting
        **kwargs: Additional keyword arguments for the logger
    """
    logger = get_logger()
    logger.error(message, *args, **kwargs)


def log_debug(message: str, *args: Any, **kwargs: Any) -> None:
    """
    Log a debug-level message.

    This function provides backward compatibility with the old logging API.

    Args:
        message: The message to log
        *args: Additional arguments for string formatting
        **kwargs: Additional keyword arguments for the logger
    """
    logger = get_logger()
    logger.debug(message, *args, **kwargs)


def log_critical(message: str, *args: Any, **kwargs: Any) -> None:
    """
    Log a critical-level message.

    This function provides backward compatibility with the old logging API.

    Args:
        message: The message to log
        *args: Additional arguments for string formatting
        **kwargs: Additional keyword arguments for the logger
    """
    logger = get_logger()
    logger.critical(message, *args, **kwargs)


def log_success(message: str, *args: Any, **kwargs: Any) -> None:
    """
    Log a success message (info level with special formatting).

    This function provides backward compatibility with the old logging API.
    Success messages are logged at INFO level.

    Args:
        message: The message to log
        *args: Additional arguments for string formatting
        **kwargs: Additional keyword arguments for the logger
    """
    logger = get_logger()
    logger.info(f"[SUCCESS] {message}", *args, **kwargs)


def log_exception(message: str = "An exception occurred", *args: Any, **kwargs: Any) -> None:
    """
    Log an exception with traceback information.

    This function logs an exception message along with the current
    exception traceback.

    Args:
        message: The message to log
        *args: Additional arguments for string formatting
        **kwargs: Additional keyword arguments for the logger
    """
    logger = get_logger()
    logger.exception(message, *args, **kwargs)
