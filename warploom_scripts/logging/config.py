"""
Configuration management for the Warploom logging system.

This module provides functions to load and validate logging configuration
from the config/logging_python.json file. It includes sensible defaults
and validation to ensure the logging system works correctly.
"""

import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, Optional


# Default configuration values
DEFAULT_CONFIG = {
    "level": "INFO",
    "format": "%(asctime)s - %(name)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s",
    "datefmt": "%Y-%m-%d %H:%M:%S",
    "console_handler_enabled": True,
    "file_handler_enabled": True,
    "file_path": "logs/warploom_python.log",
    "max_bytes": 10 * 1024 * 1024,  # 10MB
    "backup_count": 5,
    "colored_output": True
}

# Valid log levels (using integer values directly to avoid circular import)
VALID_LOG_LEVELS = {
    "DEBUG": 10,
    "INFO": 20,
    "WARNING": 30,
    "ERROR": 40,
    "CRITICAL": 50
}


class LoggingConfigError(Exception):
    """Exception raised for logging configuration errors."""
    pass


def load_config(config_path: Optional[str] = None) -> Dict[str, Any]:
    """
    Load logging configuration from a JSON file.

    This function loads the logging configuration from the specified file,
    validates it, and merges it with default values for any missing fields.

    Args:
        config_path: Path to the configuration file. If None, uses the default
                     path (config/logging_python.json)

    Returns:
        A dictionary containing the validated logging configuration

    Raises:
        LoggingConfigError: If the configuration file cannot be loaded or is invalid
    """
    # Determine the config file path
    if config_path is None:
        # Try to find the config file relative to the project root
        project_root = Path(__file__).parent.parent.parent
        config_path = str(project_root / "config" / "logging_python.json")

    # Check if the config file exists
    if not os.path.exists(config_path):
        print(f"Warning: Logging configuration file not found at {config_path}", file=sys.stderr)
        print(f"Warning: Using default configuration", file=sys.stderr)
        return DEFAULT_CONFIG.copy()

    # Load the configuration file
    try:
        # Use utf-8-sig to handle UTF-8 BOM (Byte Order Mark)
        with open(config_path, 'r', encoding='utf-8-sig') as f:
            config = json.load(f)
    except json.JSONDecodeError as e:
        raise LoggingConfigError(f"Invalid JSON in configuration file {config_path}: {e}")
    except IOError as e:
        raise LoggingConfigError(f"Cannot read configuration file {config_path}: {e}")

    # Validate and merge with defaults
    validated_config = validate_config(config)

    return validated_config


def validate_config(config: Dict[str, Any]) -> Dict[str, Any]:
    """
    Validate logging configuration and merge with defaults.

    This function validates the configuration dictionary, ensuring all
    required fields are present and have valid values. Missing fields
    are filled with default values.

    Args:
        config: The configuration dictionary to validate

    Returns:
        A validated configuration dictionary with all required fields

    Raises:
        LoggingConfigError: If the configuration contains invalid values
    """
    # Start with defaults
    validated = DEFAULT_CONFIG.copy()

    # Validate and merge each field
    if "level" in config:
        level_str = str(config["level"]).upper()
        if level_str not in VALID_LOG_LEVELS:
            raise LoggingConfigError(
                f"Invalid log level '{config['level']}'. "
                f"Valid levels are: {', '.join(VALID_LOG_LEVELS.keys())}"
            )
        validated["level"] = level_str

    if "format" in config:
        if not isinstance(config["format"], str):
            raise LoggingConfigError("Log format must be a string")
        validated["format"] = config["format"]

    if "datefmt" in config:
        if not isinstance(config["datefmt"], str):
            raise LoggingConfigError("Date format must be a string")
        validated["datefmt"] = config["datefmt"]

    if "console_handler_enabled" in config:
        if not isinstance(config["console_handler_enabled"], bool):
            raise LoggingConfigError("console_handler_enabled must be a boolean")
        validated["console_handler_enabled"] = config["console_handler_enabled"]

    if "file_handler_enabled" in config:
        if not isinstance(config["file_handler_enabled"], bool):
            raise LoggingConfigError("file_handler_enabled must be a boolean")
        validated["file_handler_enabled"] = config["file_handler_enabled"]

    if "file_path" in config:
        if not isinstance(config["file_path"], str):
            raise LoggingConfigError("file_path must be a string")
        validated["file_path"] = config["file_path"]

    if "max_bytes" in config:
        if not isinstance(config["max_bytes"], int) or config["max_bytes"] <= 0:
            raise LoggingConfigError("max_bytes must be a positive integer")
        validated["max_bytes"] = config["max_bytes"]

    if "backup_count" in config:
        if not isinstance(config["backup_count"], int) or config["backup_count"] < 0:
            raise LoggingConfigError("backup_count must be a non-negative integer")
        validated["backup_count"] = config["backup_count"]

    if "colored_output" in config:
        if not isinstance(config["colored_output"], bool):
            raise LoggingConfigError("colored_output must be a boolean")
        validated["colored_output"] = config["colored_output"]

    return validated


def get_log_level(level_str: str) -> int:
    """
    Convert a log level string to its integer value.

    Args:
        level_str: The log level as a string (e.g., "INFO", "DEBUG")

    Returns:
        The corresponding log level integer value

    Raises:
        LoggingConfigError: If the log level string is invalid
    """
    level_str = level_str.upper()
    if level_str not in VALID_LOG_LEVELS:
        raise LoggingConfigError(
            f"Invalid log level '{level_str}'. "
            f"Valid levels are: {', '.join(VALID_LOG_LEVELS.keys())}"
        )
    return VALID_LOG_LEVELS[level_str]


def get_config_value(config: Dict[str, Any], key: str, default: Any = None) -> Any:
    """
    Get a configuration value with a fallback to default.

    Args:
        config: The configuration dictionary
        key: The configuration key to retrieve
        default: The default value if the key is not present

    Returns:
        The configuration value or the default
    """
    return config.get(key, default)


def is_valid_config(config: Dict[str, Any]) -> bool:
    """
    Check if a configuration dictionary is valid.

    Args:
        config: The configuration dictionary to validate

    Returns:
        True if the configuration is valid, False otherwise
    """
    try:
        validate_config(config)
        return True
    except LoggingConfigError:
        return False
