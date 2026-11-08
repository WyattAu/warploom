"""
Custom log formatters for the OmniCPP logging system.

This module provides custom formatters for different logging output formats:
- CustomFormatter: Standard text format with timestamps, levels, and context
- ColoredFormatter: Color-coded console output
- JsonFormatter: Structured JSON logging for machine-readable output
"""

import json
import logging as stdlib_logging
import sys
from datetime import datetime
from typing import Any, Dict, Optional


class CustomFormatter(stdlib_logging.Formatter):
    """
    Custom log formatter with timestamp, level, module, function, and message.
    
    This formatter provides a consistent, human-readable log format with
    additional context information including function name and line number.
    
    Attributes:
        format_string: The format string for log messages
        datefmt: The format string for timestamps
    """
    
    def __init__(
        self,
        format_string: str = "%(asctime)s - %(name)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s",
        datefmt: str = "%Y-%m-%d %H:%M:%S"
    ) -> None:
        """
        Initialize the CustomFormatter.
        
        Args:
            format_string: The format string for log messages
            datefmt: The format string for timestamps
        """
        super().__init__(format_string, datefmt)
        self.format_string = format_string
        self.datefmt = datefmt
    
    def format(self, record: stdlib_logging.LogRecord) -> str:
        """
        Format the log record into a string.
        
        Args:
            record: The log record to format
            
        Returns:
            The formatted log message as a string
        """
        # Add custom fields if they don't exist
        if not hasattr(record, 'process_name'):
            record.process_name = f"PID:{record.process}"
        
        if not hasattr(record, 'thread_name'):
            record.thread_name = f"TID:{record.thread}"
        
        # Format the record using the parent class method
        result = super().format(record)
        
        return result


class ColoredFormatter(CustomFormatter):
    """
    Colored console output formatter.
    
    This formatter extends CustomFormatter to add ANSI color codes to log
    messages based on their severity level. Colors are automatically disabled
    if the terminal doesn't support them.
    
    Attributes:
        color_map: Mapping of log levels to ANSI color codes
        use_colors: Whether to use colored output
    """
    
    # ANSI color codes
    RESET = "\033[0m"
    BOLD = "\033[1m"
    
    # Color mappings for different log levels
    COLORS = {
        stdlib_logging.DEBUG: "\033[36m",      # Cyan
        stdlib_logging.INFO: "\033[32m",       # Green
        stdlib_logging.WARNING: "\033[33m",    # Yellow
        stdlib_logging.ERROR: "\033[31m",      # Red
        stdlib_logging.CRITICAL: "\033[35m",   # Magenta
    }
    
    def __init__(
        self,
        format_string: str = "%(asctime)s - %(name)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s",
        datefmt: str = "%Y-%m-%d %H:%M:%S",
        use_colors: Optional[bool] = None
    ) -> None:
        """
        Initialize the ColoredFormatter.
        
        Args:
            format_string: The format string for log messages
            datefmt: The format string for timestamps
            use_colors: Whether to use colored output. If None, auto-detects
        """
        super().__init__(format_string, datefmt)
        
        # Auto-detect color support if not specified
        if use_colors is None:
            self.use_colors = self._supports_color()
        else:
            self.use_colors = use_colors
    
    def _supports_color(self) -> bool:
        """
        Detect if the terminal supports ANSI color codes.
        
        Returns:
            True if the terminal supports colors, False otherwise
        """
        # Check if we're running in a terminal
        if not hasattr(sys.stdout, 'isatty') or not sys.stdout.isatty():
            return False
        
        # Check for Windows
        if sys.platform == 'win32':
            # Windows 10+ supports ANSI colors
            try:
                import ctypes
                kernel32 = ctypes.windll.kernel32
                # Enable virtual terminal processing
                kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
                return True
            except Exception:
                return False
        
        # Unix-like systems generally support colors
        return True
    
    def format(self, record: stdlib_logging.LogRecord) -> str:
        """
        Format the log record with color codes.
        
        Args:
            record: The log record to format
            
        Returns:
            The formatted log message with color codes
        """
        # Format the record using the parent class
        message = super().format(record)
        
        # Add color codes if enabled
        if self.use_colors:
            color = self.COLORS.get(record.levelno, self.RESET)
            # Apply color to the level name only
            level_name = record.levelname
            colored_level = f"{color}{self.BOLD}{level_name}{self.RESET}"
            message = message.replace(level_name, colored_level)
        
        return message


class JsonFormatter(stdlib_logging.Formatter):
    """
    JSON structured logging formatter.
    
    This formatter outputs log records as JSON objects, making them
    machine-readable and suitable for log aggregation systems.
    
    Attributes:
        indent: Number of spaces for JSON indentation (None for compact)
        ensure_ascii: Whether to escape non-ASCII characters
    """
    
    def __init__(
        self,
        indent: Optional[int] = None,
        ensure_ascii: bool = False
    ) -> None:
        """
        Initialize the JsonFormatter.
        
        Args:
            indent: Number of spaces for JSON indentation. None for compact output
            ensure_ascii: Whether to escape non-ASCII characters
        """
        super().__init__()
        self.indent = indent
        self.ensure_ascii = ensure_ascii
    
    def format(self, record: stdlib_logging.LogRecord) -> str:
        """
        Format the log record as a JSON object.
        
        Args:
            record: The log record to format
            
        Returns:
            The formatted log message as a JSON string
        """
        # Create a dictionary with log record data
        log_data: Dict[str, Any] = {
            "timestamp": datetime.fromtimestamp(record.created).isoformat(),
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
            "module": record.module,
            "function": record.funcName,
            "line": record.lineno,
            "path": record.pathname,
            "process_id": record.process,
            "thread_id": record.thread,
        }
        
        # Add exception information if present
        if record.exc_info:
            log_data["exception"] = self.formatException(record.exc_info)
        
        # Add stack trace if present
        if record.stack_info:
            log_data["stack_trace"] = self.formatStack(record.stack_info)
        
        # Add any extra fields from the record
        for key, value in record.__dict__.items():
            if key not in {
                'name', 'msg', 'args', 'levelname', 'levelno', 'pathname',
                'filename', 'module', 'exc_info', 'exc_text', 'stack_info',
                'lineno', 'funcName', 'created', 'msecs', 'relativeCreated',
                'thread', 'threadName', 'processName', 'process', 'message',
                'asctime'
            }:
                log_data[key] = value
        
        # Convert to JSON string
        return json.dumps(log_data, indent=self.indent, ensure_ascii=self.ensure_ascii)
