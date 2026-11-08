"""
Custom log handlers for the OmniCPP logging system.

This module provides custom handlers for different logging output destinations:
- ConsoleHandler: Console output with optional color support
- FileHandler: File output with rotation and retention policies
"""

import logging
import os
import sys
from logging.handlers import RotatingFileHandler
from typing import IO, Optional

from .formatters import ColoredFormatter, CustomFormatter, JsonFormatter


def _get_console_encoding() -> str:
    """
    Get the appropriate encoding for console output.

    On Windows, this returns 'utf-8' if available, otherwise 'cp1252'.
    On other platforms, it returns 'utf-8'.

    Returns:
        The encoding string to use for console output
    """
    if sys.platform == 'win32':
        # Try to use UTF-8 on Windows 10+
        try:
            import locale
            encoding = locale.getpreferredencoding(False)
            # If the preferred encoding is cp1252, try to use UTF-8
            if encoding.lower() in ('cp1252', 'windows-1252'):
                return 'utf-8'
            return encoding
        except Exception:
            return 'utf-8'
    return 'utf-8'


class ConsoleHandler(logging.StreamHandler[IO[str]]):
    """
    Console output handler with optional color support.

    This handler provides console output for real-time logging during
    development and debugging. It supports color-coded output based on
    log levels and can be configured to use stdout or stderr.

    Attributes:
        use_colors: Whether to use colored output
        stream: The output stream (stdout or stderr)
    """

    def __init__(
        self,
        stream: Optional[str] = None,
        use_colors: Optional[bool] = None,
        format_string: str = "%(asctime)s - %(name)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s",
        datefmt: str = "%Y-%m-%d %H:%M:%S"
    ) -> None:
        """
        Initialize the ConsoleHandler.

        Args:
            stream: The output stream ('stdout' or 'stderr'). Defaults to stdout
            use_colors: Whether to use colored output. If None, auto-detects
            format_string: The format string for log messages
            datefmt: The format string for timestamps
        """
        # Determine the stream to use
        if stream == 'stderr':
            super().__init__(sys.stderr)
        else:
            super().__init__(sys.stdout)

        # Create and set the formatter
        formatter = ColoredFormatter(
            format_string=format_string,
            datefmt=datefmt,
            use_colors=use_colors
        )
        self.setFormatter(formatter)

        self.use_colors = formatter.use_colors

    def emit(self, record: logging.LogRecord) -> None:
        """
        Emit a log record to the console.

        This method overrides the parent emit method to handle potential
        errors during output gracefully, including Unicode encoding issues.

        Args:
            record: The log record to emit
        """
        try:
            msg = self.format(record)
            stream = self.stream

            # Handle Unicode encoding on Windows
            if sys.platform == 'win32' and hasattr(stream, 'encoding'):
                try:
                    # Try to write with UTF-8 encoding
                    stream.write(msg + self.terminator)
                except UnicodeEncodeError:
                    # Fall back to ASCII-safe encoding
                    msg = msg.encode('ascii', 'replace').decode('ascii')
                    stream.write(msg + self.terminator)
            else:
                stream.write(msg + self.terminator)

            self.flush()
        except Exception:
            self.handleError(record)


class FileHandler(RotatingFileHandler):
    """
    File output handler with rotation and retention policies.

    This handler provides persistent log storage with automatic rotation
    based on file size and configurable retention policies. It creates
    log directories automatically if they don't exist.

    Attributes:
        max_bytes: Maximum file size before rotation (default: 10MB)
        backup_count: Number of backup files to retain (default: 5)
        log_directory: Directory where log files are stored
    """

    DEFAULT_MAX_BYTES = 10 * 1024 * 1024  # 10MB
    DEFAULT_BACKUP_COUNT = 5
    DEFAULT_LOG_PATH = "logs/warploom_python.log"

    def __init__(
        self,
        file_path: Optional[str] = None,
        max_bytes: Optional[int] = None,
        backup_count: Optional[int] = None,
        format_string: str = "%(asctime)s - %(name)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s",
        datefmt: str = "%Y-%m-%d %H:%M:%S",
        use_json: bool = False
    ) -> None:
        """
        Initialize the FileHandler.

        Args:
            file_path: Path to the log file. Defaults to logs/warploom_python.log
            max_bytes: Maximum file size before rotation. Defaults to 10MB
            backup_count: Number of backup files to retain. Defaults to 5
            format_string: The format string for log messages
            datefmt: The format string for timestamps
            use_json: Whether to use JSON formatting for log messages
        """
        # Set defaults
        if file_path is None:
            file_path = self.DEFAULT_LOG_PATH
        if max_bytes is None:
            max_bytes = self.DEFAULT_MAX_BYTES
        if backup_count is None:
            backup_count = self.DEFAULT_BACKUP_COUNT

        # Ensure the log directory exists
        log_dir = os.path.dirname(file_path)
        if log_dir and not os.path.exists(log_dir):
            try:
                os.makedirs(log_dir, exist_ok=True)
            except OSError as e:
                # Fall back to current directory if we can't create the log directory
                file_path = os.path.basename(file_path)
                print(f"Warning: Could not create log directory {log_dir}: {e}", file=sys.stderr)
                print(f"Warning: Using current directory for log file: {file_path}", file=sys.stderr)

        # Initialize the parent RotatingFileHandler
        super().__init__(
            filename=file_path,
            maxBytes=max_bytes,
            backupCount=backup_count,
            encoding='utf-8'
        )

        # Create and set the formatter
        if use_json:
            formatter: logging.Formatter = JsonFormatter(indent=None, ensure_ascii=False)
        else:
            formatter = CustomFormatter(
                format_string=format_string,
                datefmt=datefmt
            )
        self.setFormatter(formatter)

        # Store configuration
        self.max_bytes = max_bytes
        self.backup_count = backup_count
        self.log_directory = log_dir
        self.use_json = use_json

    def emit(self, record: logging.LogRecord) -> None:
        """
        Emit a log record to the file.

        This method overrides the parent emit method to handle potential
        errors during file I/O gracefully.

        Args:
            record: The log record to emit
        """
        try:
            super().emit(record)
        except Exception:
            self.handleError(record)

    def close(self) -> None:
        """
        Close the file handler.

        This method ensures the log file is properly closed on shutdown.
        """
        try:
            super().close()
        except Exception:
            # Ignore errors during close
            pass


def create_console_handler(
    enabled: bool = True,
    use_colors: Optional[bool] = None,
    format_string: str = "%(asctime)s - %(name)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s",
    datefmt: str = "%Y-%m-%d %H:%M:%S"
) -> Optional[ConsoleHandler]:
    """
    Factory function to create a console handler.

    Args:
        enabled: Whether to enable the console handler
        use_colors: Whether to use colored output. If None, auto-detects
        format_string: The format string for log messages
        datefmt: The format string for timestamps

    Returns:
        A ConsoleHandler instance if enabled, None otherwise
    """
    if not enabled:
        return None

    return ConsoleHandler(
        stream='stdout',
        use_colors=use_colors,
        format_string=format_string,
        datefmt=datefmt
    )


def create_file_handler(
    enabled: bool = True,
    file_path: Optional[str] = None,
    max_bytes: Optional[int] = None,
    backup_count: Optional[int] = None,
    format_string: str = "%(asctime)s - %(name)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s",
    datefmt: str = "%Y-%m-%d %H:%M:%S",
    use_json: bool = False
) -> Optional[FileHandler]:
    """
    Factory function to create a file handler.

    Args:
        enabled: Whether to enable the file handler
        file_path: Path to the log file
        max_bytes: Maximum file size before rotation
        backup_count: Number of backup files to retain
        format_string: The format string for log messages
        datefmt: The format string for timestamps
        use_json: Whether to use JSON formatting for log messages

    Returns:
        A FileHandler instance if enabled, None otherwise
    """
    if not enabled:
        return None

    return FileHandler(
        file_path=file_path,
        max_bytes=max_bytes,
        backup_count=backup_count,
        format_string=format_string,
        datefmt=datefmt,
        use_json=use_json
    )
