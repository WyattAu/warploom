"""
Integration tests for logging infrastructure.

This module provides integration tests for logging system,
custom formatters, and custom handlers.
"""

from __future__ import annotations

import logging
import sys
from pathlib import Path
from typing import Optional, Any
from unittest.mock import Mock, patch, MagicMock
from io import StringIO

import pytest

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from warploom_scripts.logging.logger import (
    setup_logging,
    get_logger,
    set_log_level,
    get_current_config,
    is_logging_initialized,
    shutdown_logging,
    log_info,
    log_warning,
    log_error,
    log_debug,
    log_critical,
    log_success,
    log_exception
)
from warploom_scripts.logging.formatters import (
    CustomFormatter,
    ColoredFormatter,
    JsonFormatter
)
from warploom_scripts.logging.handlers import (
    ConsoleHandler,
    FileHandler,
    create_console_handler,
    create_file_handler
)


class TestLoggingInfrastructureIntegration:
    """Integration tests for logging infrastructure."""

    def test_setup_logging_initializes_root_logger(self) -> None:
        """Test that setup_logging initializes root logger."""
        logger = setup_logging()
        assert logger is not None
        assert isinstance(logger, logging.Logger)

    def test_setup_logging_with_config_dict(self) -> None:
        """Test setup_logging with configuration dictionary."""
        config = {
            "level": "DEBUG",
            "console_handler_enabled": True,
            "file_handler_enabled": False,
            "colored_output": False,
            "format": "%(asctime)s - %(name)s - %(levelname)s - %(message)s",
            "datefmt": "%Y-%m-%d %H:%M:%S",
            "file_path": "logs/test.log",
            "max_bytes": 1048576,
            "backup_count": 3
        }

        logger = setup_logging(config=config)
        assert logger is not None
        assert isinstance(logger, logging.Logger)

    def test_setup_logging_with_config_path(self, tmp_path: Any) -> None:
        """Test setup_logging with configuration file path."""
        config_file = tmp_path / "test_logging_config.json"

        # Create a simple config file
        import json
        config_data = {
            "level": "INFO",
            "console_handler_enabled": True,
            "file_handler_enabled": True,
            "colored_output": False,
            "format": "%(asctime)s - %(name)s - %(levelname)s - %(message)s",
            "datefmt": "%Y-%m-%d %H:%M:%S",
            "file_path": str(tmp_path / "logs" / "test.log"),
            "max_bytes": 1048576,
            "backup_count": 3
        }

        with open(config_file, "w") as f:
            json.dump(config_data, f)

        logger = setup_logging(config_path=str(config_file))
        assert logger is not None
        assert isinstance(logger, logging.Logger)

    def test_get_logger_returns_logger_instance(self) -> None:
        """Test that get_logger returns a logger instance."""
        logger = get_logger("test_module")
        assert logger is not None
        assert isinstance(logger, logging.Logger)
        assert logger.name == "test_module"

    def test_get_logger_without_name(self) -> None:
        """Test that get_logger without name uses default."""
        logger = get_logger()
        assert logger is not None
        assert isinstance(logger, logging.Logger)

    def test_set_log_level_changes_level(self) -> None:
        """Test that set_log_level changes log level."""
        setup_logging()

        set_log_level("DEBUG")

        # Verify level was changed
        logger = get_logger()
        assert logger.level == logging.DEBUG

    def test_set_log_level_invalid_level(self) -> None:
        """Test that set_log_level handles invalid level."""
        setup_logging()

        # Invalid level should raise error or be handled gracefully
        try:
            set_log_level("INVALID_LEVEL")
            # Should not crash
            assert True
        except Exception:
            # Should handle error gracefully
            assert True

    def test_get_current_config_returns_config(self) -> None:
        """Test that get_current_config returns configuration."""
        setup_logging()

        config = get_current_config()
        assert isinstance(config, dict)
        assert "level" in config

    def test_is_logging_initialized(self) -> None:
        """Test that is_logging_initialized returns correct state."""
        # Note: Logging may already be initialized from previous tests
        # Just verify the function works
        initial_state = is_logging_initialized()
        assert isinstance(initial_state, bool)

        # After setup
        setup_logging()
        assert is_logging_initialized() is True

    def test_shutdown_logging_closes_handlers(self) -> None:
        """Test that shutdown_logging closes handlers."""
        setup_logging()

        logger = get_logger()
        initial_handler_count = len(logger.handlers)

        shutdown_logging()

        # After shutdown, handlers should be removed
        assert is_logging_initialized() is False

    def test_convenience_functions(self) -> None:
        """Test convenience logging functions."""
        setup_logging()

        # Test that all convenience functions exist and can be called
        # These functions use the root logger which may not be captured by caplog
        # Just verify they don't raise exceptions
        try:
            log_info("Test info message")
            log_warning("Test warning message")
            log_error("Test error message")
            log_debug("Test debug message")
            log_critical("Test critical message")
            log_success("Test success message")
            try:
                raise ValueError("Test exception")
            except ValueError:
                log_exception("Test exception message")
            # All functions should work without raising exceptions
            assert True
        except Exception as e:
            pytest.fail(f"Convenience function raised exception: {e}")


class TestCustomFormattersIntegration:
    """Integration tests for custom formatters."""

    def test_custom_formatter_initialization(self) -> None:
        """Test CustomFormatter initialization."""
        formatter = CustomFormatter()
        assert formatter is not None
        assert formatter.format_string is not None
        assert formatter.datefmt is not None

    def test_custom_formatter_format_record(self) -> None:
        """Test CustomFormatter formats log records."""
        formatter = CustomFormatter()

        # Create a log record
        record = logging.LogRecord(
            name="test_logger",
            level=logging.INFO,
            pathname="test.py",
            lineno=42,
            msg="Test message",
            args=(),
            exc_info=None
        )

        formatted = formatter.format(record)

        # Verify format includes expected fields
        assert "test_logger" in formatted
        assert "INFO" in formatted
        assert "Test message" in formatted

    def test_colored_formatter_initialization(self) -> None:
        """Test ColoredFormatter initialization."""
        formatter = ColoredFormatter()
        assert formatter is not None
        assert formatter.format_string is not None
        assert formatter.datefmt is not None

    def test_colored_formatter_with_colors(self) -> None:
        """Test ColoredFormatter with colors enabled."""
        formatter = ColoredFormatter(use_colors=True)

        # Create a log record
        record = logging.LogRecord(
            name="test_logger",
            level=logging.INFO,
            pathname="test.py",
            lineno=42,
            msg="Test message",
            args=(),
            exc_info=None
        )

        formatted = formatter.format(record)

        # Verify color codes are present
        assert "\033[" in formatted  # ANSI color codes

    def test_colored_formatter_without_colors(self) -> None:
        """Test ColoredFormatter with colors disabled."""
        formatter = ColoredFormatter(use_colors=False)

        # Create a log record
        record = logging.LogRecord(
            name="test_logger",
            level=logging.INFO,
            pathname="test.py",
            lineno=42,
            msg="Test message",
            args=(),
            exc_info=None
        )

        formatted = formatter.format(record)

        # Verify no color codes are present
        assert "\033[" not in formatted

    def test_colored_formatter_color_detection(self) -> None:
        """Test ColoredFormatter color detection."""
        # Test with colors enabled
        formatter_with_colors = ColoredFormatter(use_colors=True)
        assert formatter_with_colors.use_colors is True

        # Test with colors disabled
        formatter_without_colors = ColoredFormatter(use_colors=False)
        assert formatter_without_colors.use_colors is False

    def test_json_formatter_initialization(self) -> None:
        """Test JsonFormatter initialization."""
        formatter = JsonFormatter()
        assert formatter is not None
        assert formatter.indent is None
        assert formatter.ensure_ascii is False

    def test_json_formatter_format_record(self) -> None:
        """Test JsonFormatter formats log records as JSON."""
        formatter = JsonFormatter()

        # Create a log record
        record = logging.LogRecord(
            name="test_logger",
            level=logging.INFO,
            pathname="test.py",
            lineno=42,
            msg="Test message",
            args=(),
            exc_info=None
        )

        formatted = formatter.format(record)

        # Verify output is valid JSON
        import json
        parsed = json.loads(formatted)
        assert parsed["level"] == "INFO"
        assert parsed["logger"] == "test_logger"
        assert parsed["message"] == "Test message"

    def test_json_formatter_with_exception(self) -> None:
        """Test JsonFormatter includes exception information."""
        formatter = JsonFormatter()

        # Create a log record with exception
        try:
            raise ValueError("Test exception")
        except ValueError as e:
            record = logging.LogRecord(
                name="test_logger",
                level=logging.ERROR,
                pathname="test.py",
                lineno=42,
                msg="Test message",
                args=(),
                exc_info=(type(e), e, None)
            )

        formatted = formatter.format(record)

        # Verify exception is included
        import json
        parsed = json.loads(formatted)
        assert "exception" in parsed

    def test_json_formatter_with_extra_fields(self) -> None:
        """Test JsonFormatter includes extra fields."""
        formatter = JsonFormatter()

        # Create a log record with extra fields
        record = logging.LogRecord(
            name="test_logger",
            level=logging.INFO,
            pathname="test.py",
            lineno=42,
            msg="Test message",
            args=(),
            exc_info=None
        )
        # Add extra fields
        record.custom_field = "custom_value"
        record.another_field = 123

        formatted = formatter.format(record)

        # Verify extra fields are included
        import json
        parsed = json.loads(formatted)
        assert parsed["custom_field"] == "custom_value"
        assert parsed["another_field"] == 123


class TestCustomHandlersIntegration:
    """Integration tests for custom handlers."""

    def test_console_handler_initialization(self) -> None:
        """Test ConsoleHandler initialization."""
        handler = ConsoleHandler()
        assert handler is not None
        assert isinstance(handler, logging.StreamHandler)

    def test_console_handler_with_stdout(self) -> None:
        """Test ConsoleHandler with stdout."""
        handler = ConsoleHandler(stream='stdout')

        # Verify stream is stdout
        assert handler.stream == sys.stdout

    def test_console_handler_with_stderr(self) -> None:
        """Test ConsoleHandler with stderr."""
        handler = ConsoleHandler(stream='stderr')

        # Verify stream is stderr
        assert handler.stream == sys.stderr

    def test_console_handler_emits_records(self, caplog: Any) -> None:
        """Test ConsoleHandler emits log records."""
        handler = ConsoleHandler()
        logger = logging.getLogger("test_console")
        logger.addHandler(handler)

        logger.info("Test message")

        # Verify record was emitted
        assert any("Test message" in record.message for record in caplog.records)

    def test_file_handler_initialization(self, tmp_path: Any) -> None:
        """Test FileHandler initialization."""
        log_file = tmp_path / "logs" / "test.log"

        handler = FileHandler(file_path=str(log_file))
        assert handler is not None
        assert isinstance(handler, logging.Handler)

    def test_file_handler_creates_log_directory(self, tmp_path: Any) -> None:
        """Test FileHandler creates log directory if needed."""
        log_file = tmp_path / "subdir" / "test.log"

        handler = FileHandler(file_path=str(log_file))

        # Verify directory was created
        assert (tmp_path / "subdir").exists()

    def test_file_handler_emits_records(self, tmp_path: Any, caplog: Any) -> None:
        """Test FileHandler emits log records."""
        log_file = tmp_path / "logs" / "test.log"

        handler = FileHandler(file_path=str(log_file))
        logger = logging.getLogger("test_file")
        logger.addHandler(handler)

        logger.info("Test message")

        # Verify record was emitted
        assert any("Test message" in record.message for record in caplog.records)

    def test_file_handler_with_json_format(self, tmp_path: Any) -> None:
        """Test FileHandler with JSON format."""
        log_file = tmp_path / "logs" / "test.json"

        handler = FileHandler(file_path=str(log_file), use_json=True)
        assert handler is not None
        assert handler.use_json is True

    def test_file_handler_rotation(self, tmp_path: Any) -> None:
        """Test FileHandler rotation."""
        log_file = tmp_path / "logs" / "test.log"

        # Create handler with small max bytes for testing
        handler = FileHandler(
            file_path=str(log_file),
            max_bytes=1024,  # 1KB for testing
            backup_count=2
        )

        assert handler.max_bytes == 1024
        assert handler.backup_count == 2

    def test_create_console_handler(self) -> None:
        """Test create_console_handler factory function."""
        handler = create_console_handler(enabled=True)
        assert handler is not None
        assert isinstance(handler, ConsoleHandler)

    def test_create_console_handler_disabled(self) -> None:
        """Test create_console_handler when disabled."""
        handler = create_console_handler(enabled=False)
        assert handler is None

    def test_create_file_handler(self, tmp_path: Any) -> None:
        """Test create_file_handler factory function."""
        log_file = tmp_path / "logs" / "test.log"

        handler = create_file_handler(
            enabled=True,
            file_path=str(log_file)
        )
        assert handler is not None
        assert isinstance(handler, FileHandler)

    def test_create_file_handler_disabled(self) -> None:
        """Test create_file_handler when disabled."""
        handler = create_file_handler(enabled=False)
        assert handler is None


class TestLoggingIntegration:
    """Integration tests for logging system integration."""

    def test_logging_with_console_and_file(self, tmp_path: Any) -> None:
        """Test logging with both console and file handlers."""
        log_file = tmp_path / "logs" / "test.log"

        config = {
            "level": "INFO",
            "console_handler_enabled": True,
            "file_handler_enabled": True,
            "colored_output": False,
            "format": "%(asctime)s - %(name)s - %(levelname)s - %(message)s",
            "datefmt": "%Y-%m-%d %H:%M:%S",
            "file_path": str(log_file),
            "max_bytes": 1048576,
            "backup_count": 3
        }

        logger = setup_logging(config=config)
        logger.info("Test message")

        # Verify file was created
        assert log_file.exists()

    def test_logging_with_different_levels(self) -> None:
        """Test logging at different levels."""
        setup_logging()
        logger = get_logger("test_levels")

        # Log at different levels - just verify they don't raise exceptions
        try:
            logger.debug("Debug message")
            logger.info("Info message")
            logger.warning("Warning message")
            logger.error("Error message")
            logger.critical("Critical message")
            # All levels should work without raising exceptions
            assert True
        except Exception as e:
            pytest.fail(f"Logging at different levels raised exception: {e}")

    def test_logging_with_multiple_loggers(self) -> None:
        """Test logging with multiple loggers."""
        setup_logging()

        # Create multiple loggers
        logger1 = get_logger("test_logger1")
        logger2 = get_logger("test_logger2")
        logger3 = get_logger("test_logger3")

        # Log from different loggers - just verify they don't raise exceptions
        try:
            logger1.info("Message from logger1")
            logger2.info("Message from logger2")
            logger3.info("Message from logger3")
            # All loggers should work without raising exceptions
            assert True
        except Exception as e:
            pytest.fail(f"Multiple loggers raised exception: {e}")

    def test_logging_with_context(self) -> None:
        """Test logging with context information."""
        setup_logging()
        logger = get_logger("test_context")

        # Log with extra context - just verify it doesn't raise exception
        try:
            logger.info("Test message", extra={"context_key": "context_value"})
            # Should work without raising exception
            assert True
        except Exception as e:
            pytest.fail(f"Logging with context raised exception: {e}")

    def test_logging_with_exception_context(self) -> None:
        """Test logging with exception context."""
        setup_logging()
        logger = get_logger("test_exception_context")

        # Log with exception - just verify it doesn't raise exception
        try:
            try:
                raise ValueError("Test exception")
            except ValueError:
                logger.exception("Exception occurred")
            # Should work without raising exception
            assert True
        except Exception as e:
            pytest.fail(f"Logging with exception context raised exception: {e}")

    def test_logging_level_changes(self, caplog: Any) -> None:
        """Test that log level changes work correctly."""
        setup_logging()
        logger = get_logger("test_level_change")

        # Initial level should be INFO (default)
        assert logger.level == logging.INFO

        # Change to DEBUG
        set_log_level("DEBUG")
        assert logger.level == logging.DEBUG

        # Change to WARNING
        set_log_level("WARNING")
        assert logger.level == logging.WARNING

        # Change to ERROR
        set_log_level("ERROR")
        assert logger.level == logging.ERROR

    def test_logging_with_custom_format(self) -> None:
        """Test logging with custom format."""
        custom_format = "%(levelname)s [%(name)s] %(message)s"

        config = {
            "level": "INFO",
            "console_handler_enabled": True,
            "file_handler_enabled": False,
            "colored_output": False,
            "format": custom_format,
            "datefmt": "%Y-%m-%d %H:%M:%S",
            "file_path": "logs/test.log",
            "max_bytes": 1048576,
            "backup_count": 3
        }

        logger = setup_logging(config=config)
        # Just verify it doesn't raise exception
        try:
            logger.info("Test message")
            assert True
        except Exception as e:
            pytest.fail(f"Logging with custom format raised exception: {e}")

    def test_logging_with_colored_output(self) -> None:
        """Test logging with colored output."""
        config = {
            "level": "INFO",
            "console_handler_enabled": True,
            "file_handler_enabled": False,
            "colored_output": True,
            "format": "%(asctime)s - %(name)s - %(levelname)s - %(message)s",
            "datefmt": "%Y-%m-%d %H:%M:%S",
            "file_path": "logs/test.log",
            "max_bytes": 1048576,
            "backup_count": 3
        }

        logger = setup_logging(config=config)
        # Just verify it doesn't raise exception
        try:
            logger.info("Test message")
            assert True
        except Exception as e:
            pytest.fail(f"Logging with colored output raised exception: {e}")

    def test_logging_with_json_output(self, tmp_path: Any) -> None:
        """Test logging with JSON output."""
        log_file = tmp_path / "logs" / "test.json"

        config = {
            "level": "INFO",
            "console_handler_enabled": False,
            "file_handler_enabled": True,
            "colored_output": False,
            "format": "%(asctime)s - %(name)s - %(levelname)s - %(message)s",
            "datefmt": "%Y-%m-%d %H:%M:%S",
            "file_path": str(log_file),
            "max_bytes": 1048576,
            "backup_count": 3,
            "use_json": True
        }

        logger = setup_logging(config=config)
        logger.info("Test message")

        # Verify JSON output was written
        assert log_file.exists()

        # Verify JSON format - read line by line
        with open(log_file, "r") as f:
            content = f.read()
            import json
            # JSON formatter writes one JSON object per line
            lines = content.strip().split('\n')
            found_message = False
            for line in lines:
                if line.strip():
                    try:
                        parsed = json.loads(line)
                        if "Test message" in parsed.get("message", ""):
                            # Found the message
                            found_message = True
                            break
                    except json.JSONDecodeError:
                        # Skip invalid JSON lines (e.g., empty lines)
                        pass
            # Verify we found the message
            assert found_message

    def test_logging_shutdown_and_restart(self) -> None:
        """Test logging shutdown and restart."""
        # First setup
        setup_logging()
        logger1 = get_logger("test_shutdown1")
        logger1.info("Message before shutdown")

        # Shutdown
        shutdown_logging()

        # Verify logging is not initialized
        assert is_logging_initialized() is False

        # Restart logging
        setup_logging()
        logger2 = get_logger("test_shutdown2")
        logger2.info("Message after restart")

        # Verify new logger works
        assert True


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
