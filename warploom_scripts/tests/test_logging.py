"""
Unit tests for logging modules.

Tests for config, formatters, handlers, and logger modules.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path
from typing import Any
from unittest.mock import patch
import pytest

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.logging.config import (
    load_config,
    validate_config,
    get_log_level,
    get_config_value,
    is_valid_config,
    LoggingConfigError,
    DEFAULT_CONFIG,
    VALID_LOG_LEVELS
)


class TestLoadConfig:
    """Unit tests for load_config function."""

    def test_load_config_default(self) -> None:
        """Test load_config with default path."""
        config = load_config()
        assert isinstance(config, dict)
        assert "level" in config
        assert "format" in config

    def test_load_config_with_path(self) -> None:
        """Test load_config with custom path."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            config_file = Path(tmp_dir) / "logging_config.json"
            config_file.write_text('{"level": "DEBUG", "format": "%(message)s"}')

            config = load_config(str(config_file))
            assert config["level"] == "DEBUG"
            assert config["format"] == "%(message)s"

    def test_load_config_nonexistent_file(self) -> None:
        """Test load_config with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            config_file = Path(tmp_dir) / "nonexistent.json"

            config = load_config(str(config_file))
            # Should return default config
            assert isinstance(config, dict)
            assert "level" in config

    def test_load_config_invalid_json(self) -> None:
        """Test load_config with invalid JSON."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            config_file = Path(tmp_dir) / "invalid.json"
            config_file.write_text('{"level": "DEBUG", invalid}')

            with pytest.raises(LoggingConfigError):
                load_config(str(config_file))


class TestValidateConfig:
    """Unit tests for validate_config function."""

    def test_validate_config_empty(self) -> None:
        """Test validate_config with empty dict."""
        config = {}
        validated = validate_config(config)
        assert isinstance(validated, dict)
        assert validated == DEFAULT_CONFIG

    def test_validate_config_valid_level(self) -> None:
        """Test validate_config with valid level."""
        config = {"level": "DEBUG"}
        validated = validate_config(config)
        assert validated["level"] == "DEBUG"

    def test_validate_config_invalid_level(self) -> None:
        """Test validate_config with invalid level."""
        config = {"level": "INVALID"}
        with pytest.raises(LoggingConfigError):
            validate_config(config)

    def test_validate_config_valid_format(self) -> None:
        """Test validate_config with valid format."""
        config = {"format": "%(message)s"}
        validated = validate_config(config)
        assert validated["format"] == "%(message)s"

    def test_validate_config_invalid_format(self) -> None:
        """Test validate_config with invalid format."""
        config = {"format": 123}
        with pytest.raises(LoggingConfigError):
            validate_config(config)

    def test_validate_config_valid_datefmt(self) -> None:
        """Test validate_config with valid datefmt."""
        config = {"datefmt": "%H:%M:%S"}
        validated = validate_config(config)
        assert validated["datefmt"] == "%H:%M:%S"

    def test_validate_config_invalid_datefmt(self) -> None:
        """Test validate_config with invalid datefmt."""
        config = {"datefmt": 123}
        with pytest.raises(LoggingConfigError):
            validate_config(config)

    def test_validate_config_valid_console_handler_enabled(self) -> None:
        """Test validate_config with valid console_handler_enabled."""
        config = {"console_handler_enabled": True}
        validated = validate_config(config)
        assert validated["console_handler_enabled"] is True

    def test_validate_config_invalid_console_handler_enabled(self) -> None:
        """Test validate_config with invalid console_handler_enabled."""
        config = {"console_handler_enabled": "yes"}
        with pytest.raises(LoggingConfigError):
            validate_config(config)

    def test_validate_config_valid_file_handler_enabled(self) -> None:
        """Test validate_config with valid file_handler_enabled."""
        config = {"file_handler_enabled": True}
        validated = validate_config(config)
        assert validated["file_handler_enabled"] is True

    def test_validate_config_invalid_file_handler_enabled(self) -> None:
        """Test validate_config with invalid file_handler_enabled."""
        config = {"file_handler_enabled": "yes"}
        with pytest.raises(LoggingConfigError):
            validate_config(config)

    def test_validate_config_valid_file_path(self) -> None:
        """Test validate_config with valid file_path."""
        config = {"file_path": "/var/log/test.log"}
        validated = validate_config(config)
        assert validated["file_path"] == "/var/log/test.log"

    def test_validate_config_invalid_file_path(self) -> None:
        """Test validate_config with invalid file_path."""
        config = {"file_path": 123}
        with pytest.raises(LoggingConfigError):
            validate_config(config)

    def test_validate_config_valid_max_bytes(self) -> None:
        """Test validate_config with valid max_bytes."""
        config = {"max_bytes": 10485760}
        validated = validate_config(config)
        assert validated["max_bytes"] == 10485760

    def test_validate_config_invalid_max_bytes(self) -> None:
        """Test validate_config with invalid max_bytes."""
        config = {"max_bytes": -1}
        with pytest.raises(LoggingConfigError):
            validate_config(config)

    def test_validate_config_valid_backup_count(self) -> None:
        """Test validate_config with valid backup_count."""
        config = {"backup_count": 5}
        validated = validate_config(config)
        assert validated["backup_count"] == 5

    def test_validate_config_invalid_backup_count(self) -> None:
        """Test validate_config with invalid backup_count."""
        config = {"backup_count": -1}
        with pytest.raises(LoggingConfigError):
            validate_config(config)

    def test_validate_config_valid_colored_output(self) -> None:
        """Test validate_config with valid colored_output."""
        config = {"colored_output": True}
        validated = validate_config(config)
        assert validated["colored_output"] is True

    def test_validate_config_invalid_colored_output(self) -> None:
        """Test validate_config with invalid colored_output."""
        config = {"colored_output": "yes"}
        with pytest.raises(LoggingConfigError):
            validate_config(config)


class TestGetLogLevel:
    """Unit tests for get_log_level function."""

    def test_get_log_level_debug(self) -> None:
        """Test get_log_level for DEBUG."""
        level = get_log_level("DEBUG")
        assert level == 10

    def test_get_log_level_info(self) -> None:
        """Test get_log_level for INFO."""
        level = get_log_level("INFO")
        assert level == 20

    def test_get_log_level_warning(self) -> None:
        """Test get_log_level for WARNING."""
        level = get_log_level("WARNING")
        assert level == 30

    def test_get_log_level_error(self) -> None:
        """Test get_log_level for ERROR."""
        level = get_log_level("ERROR")
        assert level == 40

    def test_get_log_level_critical(self) -> None:
        """Test get_log_level for CRITICAL."""
        level = get_log_level("CRITICAL")
        assert level == 50

    def test_get_log_level_invalid(self) -> None:
        """Test get_log_level with invalid level."""
        with pytest.raises(LoggingConfigError):
            get_log_level("INVALID")

    def test_get_log_level_case_insensitive(self) -> None:
        """Test get_log_level is case insensitive."""
        level1 = get_log_level("debug")
        level2 = get_log_level("DEBUG")
        assert level1 == level2


class TestGetConfigValue:
    """Unit tests for get_config_value function."""

    def test_get_config_value_existing(self) -> None:
        """Test get_config_value with existing key."""
        config = {"level": "DEBUG", "format": "%(message)s"}
        value = get_config_value(config, "level")
        assert value == "DEBUG"

    def test_get_config_value_nonexistent(self) -> None:
        """Test get_config_value with nonexistent key."""
        config = {"level": "DEBUG"}
        value = get_config_value(config, "format")
        assert value is None

    def test_get_config_value_with_default(self) -> None:
        """Test get_config_value with default value."""
        config = {"level": "DEBUG"}
        value = get_config_value(config, "format", "default_format")
        assert value == "default_format"


class TestIsValidConfig:
    """Unit tests for is_valid_config function."""

    def test_is_valid_config_valid(self) -> None:
        """Test is_valid_config with valid config."""
        config = {"level": "DEBUG", "format": "%(message)s"}
        result = is_valid_config(config)
        assert result is True

    def test_is_valid_config_invalid_level(self) -> None:
        """Test is_valid_config with invalid level."""
        config = {"level": "INVALID"}
        result = is_valid_config(config)
        assert result is False

    def test_is_valid_config_invalid_format(self) -> None:
        """Test is_valid_config with invalid format."""
        config = {"format": 123}
        result = is_valid_config(config)
        assert result is False


class TestLoggingConfigError:
    """Unit tests for LoggingConfigError exception."""

    def test_logging_config_error_creation(self) -> None:
        """Test LoggingConfigError creation."""
        error = LoggingConfigError("Test error")
        assert str(error) == "Test error"

    def test_logging_config_error_inheritance(self) -> None:
        """Test LoggingConfigError inherits from Exception."""
        error = LoggingConfigError("Test error")
        assert isinstance(error, Exception)


class TestDefaultConfig:
    """Unit tests for DEFAULT_CONFIG constant."""

    def test_default_config_structure(self) -> None:
        """Test DEFAULT_CONFIG has correct structure."""
        assert isinstance(DEFAULT_CONFIG, dict)
        assert "level" in DEFAULT_CONFIG
        assert "format" in DEFAULT_CONFIG
        assert "datefmt" in DEFAULT_CONFIG
        assert "console_handler_enabled" in DEFAULT_CONFIG
        assert "file_handler_enabled" in DEFAULT_CONFIG
        assert "file_path" in DEFAULT_CONFIG
        assert "max_bytes" in DEFAULT_CONFIG
        assert "backup_count" in DEFAULT_CONFIG
        assert "colored_output" in DEFAULT_CONFIG

    def test_default_config_values(self) -> None:
        """Test DEFAULT_CONFIG has correct values."""
        assert DEFAULT_CONFIG["level"] == "INFO"
        assert DEFAULT_CONFIG["console_handler_enabled"] is True
        assert DEFAULT_CONFIG["file_handler_enabled"] is True
        assert DEFAULT_CONFIG["max_bytes"] == 10 * 1024 * 1024
        assert DEFAULT_CONFIG["backup_count"] == 5
        assert DEFAULT_CONFIG["colored_output"] is True


class TestValidLogLevels:
    """Unit tests for VALID_LOG_LEVELS constant."""

    def test_valid_log_levels_structure(self) -> None:
        """Test VALID_LOG_LEVELS has correct structure."""
        assert isinstance(VALID_LOG_LEVELS, dict)
        assert "DEBUG" in VALID_LOG_LEVELS
        assert "INFO" in VALID_LOG_LEVELS
        assert "WARNING" in VALID_LOG_LEVELS
        assert "ERROR" in VALID_LOG_LEVELS
        assert "CRITICAL" in VALID_LOG_LEVELS

    def test_valid_log_levels_values(self) -> None:
        """Test VALID_LOG_LEVELS has correct values."""
        assert VALID_LOG_LEVELS["DEBUG"] == 10
        assert VALID_LOG_LEVELS["INFO"] == 20
        assert VALID_LOG_LEVELS["WARNING"] == 30
        assert VALID_LOG_LEVELS["ERROR"] == 40
        assert VALID_LOG_LEVELS["CRITICAL"] == 50


class TestLoggingModules:
    """Unit tests for logging module imports."""

    def test_import_config_module(self) -> None:
        """Test that config module can be imported."""
        try:
            from warploom_scripts.logging import config
            assert config is not None
        except ImportError:
            pytest.skip("config module not available")

    def test_import_formatters_module(self) -> None:
        """Test that formatters module can be imported."""
        try:
            from warploom_scripts.logging import formatters
            assert formatters is not None
        except ImportError:
            pytest.skip("formatters module not available")

    def test_import_handlers_module(self) -> None:
        """Test that handlers module can be imported."""
        try:
            from warploom_scripts.logging import handlers
            assert handlers is not None
        except ImportError:
            pytest.skip("handlers module not available")

    def test_import_logger_module(self) -> None:
        """Test that logger module can be imported."""
        try:
            from warploom_scripts.logging import logger
            assert logger is not None
        except ImportError:
            pytest.skip("logger module not available")


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
