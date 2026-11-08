# warploom_scripts/config_manager.py
"""
Configuration Manager - Load, validate, and manage configuration files

This module provides configuration file loading, validation, caching,
and management for Warploom build system.
"""

from __future__ import annotations

import json
import os
from typing import Any, Dict, Optional

from warploom_scripts.logging.logger import get_logger
from warploom_scripts.utils.exceptions import ConfigurationError


class ConfigManager:
    """Configuration file loading, validation, and management."""

    def __init__(self, config_path: str) -> None:
        """Initialize configuration manager.

        Args:
            config_path: Path to configuration file
        """
        self.config_path: str = config_path
        self._config: Optional[Dict[str, Any]] = None
        self._cache: Dict[str, Any] = {}
        self._logger = get_logger(__name__)

    def load(self) -> Dict[str, Any]:
        """Load and validate configuration.

        Returns:
            Configuration dictionary

        Raises:
            ConfigurationError: If configuration file is invalid or missing
        """
        if not os.path.exists(self.config_path):
            raise ConfigurationError(
                f"Configuration file not found: {self.config_path}",
                {"path": self.config_path},
            )

        try:
            with open(self.config_path, "r", encoding="utf-8") as f:
                config: Dict[str, Any] = json.load(f)
        except json.JSONDecodeError as e:
            raise ConfigurationError(
                f"Invalid JSON in configuration file: {e}",
                {"path": self.config_path, "error": str(e)},
            ) from e
        except OSError as e:
            raise ConfigurationError(
                f"Failed to load configuration: {e}",
                {"path": self.config_path, "error": str(e)},
            ) from e

        if not self.validate(config):
            raise ConfigurationError(
                "Configuration validation failed",
                {"path": self.config_path},
            )

        self._config = config
        self._logger.info(f"Configuration loaded from {self.config_path}")
        return config

    def get(self, key: str, default: Any = None) -> Any:
        """Get configuration value.

        Args:
            key: Configuration key (supports dot notation for nested keys)
            default: Default value if key not found

        Returns:
            Configuration value
        """
        if self._config is None:
            self.load()

        # Support dot notation for nested keys
        keys = key.split(".")
        value = self._config

        for k in keys:
            if isinstance(value, dict) and k in value:
                value = value[k]
            else:
                return default

        return value

    def reload(self) -> None:
        """Reload configuration from file."""
        self._logger.info(f"Reloading configuration from {self.config_path}")
        self._config = None
        self._cache.clear()
        self.load()

    def validate(self, config: Dict[str, Any]) -> bool:
        """Validate configuration against schema.

        Args:
            config: Configuration to validate

        Returns:
            True if valid, False otherwise
        """
        required_fields = ["project_name", "project_version"]
        for field in required_fields:
            if field not in config:
                self._logger.error(f"Missing required field: {field}")
                return False

        if not isinstance(config.get("project_name"), str):
            self._logger.error("project_name must be a string")
            return False

        if not isinstance(config.get("project_version"), str):
            self._logger.error("project_version must be a string")
            return False

        if "cpp_standard" in config and str(config["cpp_standard"]) not in {"20", "23"}:
            self._logger.error(f"Invalid cpp_standard: {config['cpp_standard']}")
            return False

        self._logger.debug("Configuration validation passed")
        return True

    def get_all(self) -> Dict[str, Any]:
        """Get all configuration values.

        Returns:
            Complete configuration dictionary
        """
        if self._config is None:
            self.load()

        if self._config is None:
            return {}

        return self._config.copy()

    def set(self, key: str, value: Any) -> None:
        """Set configuration value in memory (not persisted).

        Args:
            key: Configuration key
            value: Value to set
        """
        if self._config is None:
            self.load()

        # Support dot notation for nested keys
        keys = key.split(".")
        current_config = self._config

        if current_config is None:
            self._config = {}
            current_config = self._config

        for k in keys[:-1]:
            if k not in current_config:
                current_config[k] = {}
            current_config = current_config[k]

        current_config[keys[-1]] = value
        self._logger.debug(f"Configuration value set: {key}")

    def save(self) -> None:
        """Save current configuration to file.

        Raises:
            ConfigurationError: If save fails
        """
        if self._config is None:
            raise ConfigurationError(
                "No configuration loaded to save",
                {"path": self.config_path}
            )

        try:
            # Ensure directory exists
            config_dir = os.path.dirname(self.config_path)
            if config_dir:
                os.makedirs(config_dir, exist_ok=True)

            with open(self.config_path, "w", encoding="utf-8") as f:
                json.dump(self._config, f, indent=2)

            self._logger.info(f"Configuration saved to {self.config_path}")
        except Exception as e:
            raise ConfigurationError(
                f"Failed to save configuration: {e}",
                {"path": self.config_path, "error": str(e)}
            )

    def get_path(self) -> str:
        """Get configuration file path.

        Returns:
            Configuration file path
        """
        return self.config_path

    def exists(self) -> bool:
        """Check if configuration file exists.

        Returns:
            True if configuration file exists, False otherwise
        """
        return os.path.exists(self.config_path)


__all__ = [
    'ConfigManager',
]
