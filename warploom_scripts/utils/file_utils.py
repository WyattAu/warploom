# warploom_scripts/utils/file_utils.py
"""
File utility functions for Warploom project.

Provides common file operations with error handling and logging.
"""

import hashlib
import logging
import shutil
from pathlib import Path
from typing import List, Optional, Set

logger = logging.getLogger(__name__)


class FileUtils:
    """Utility class for file operations"""

    @staticmethod
    def calculate_file_hash(file_path: Path, algorithm: str = 'sha256') -> Optional[str]:
        """Calculate hash of a file"""
        if not file_path.exists():
            return None

        hash_func = getattr(hashlib, algorithm)()
        try:
            with open(file_path, 'rb') as f:
                for chunk in iter(lambda: f.read(4096), b""):
                    hash_func.update(chunk)
            return str(hash_func.hexdigest())
        except Exception as e:
            logger.error(f"Failed to calculate {algorithm} hash for {file_path}: {e}")
            return None

    @staticmethod
    def find_files_by_extension(directory: Path, extensions: Set[str]) -> List[Path]:
        """Find all files with specified extensions in directory"""
        if not directory.exists():
            return []

        files: List[Path] = []
        for ext in extensions:
            files.extend(directory.rglob(f"*{ext}"))
        return files

    @staticmethod
    def find_files_by_pattern(directory: Path, pattern: str) -> List[Path]:
        """Find files matching a glob pattern"""
        if not directory.exists():
            return []

        return list(directory.glob(pattern))

    @staticmethod
    def safe_copy(src: Path, dst: Path, overwrite: bool = False) -> bool:
        """Safely copy a file with error handling"""
        try:
            if dst.exists() and not overwrite:
                logger.warning(f"Destination {dst} exists and overwrite=False")
                return False

            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            logger.debug(f"Copied {src} to {dst}")
            return True
        except Exception as e:
            logger.error(f"Failed to copy {src} to {dst}: {e}")
            return False

    @staticmethod
    def safe_move(src: Path, dst: Path, overwrite: bool = False) -> bool:
        """Safely move a file with error handling"""
        try:
            if dst.exists() and not overwrite:
                logger.warning(f"Destination {dst} exists and overwrite=False")
                return False

            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(src), str(dst))
            logger.debug(f"Moved {src} to {dst}")
            return True
        except Exception as e:
            logger.error(f"Failed to move {src} to {dst}: {e}")
            return False

    @staticmethod
    def ensure_directory_exists(directory: Path) -> bool:
        """Ensure a directory exists, creating it if necessary"""
        try:
            directory.mkdir(parents=True, exist_ok=True)
            return True
        except Exception as e:
            logger.error(f"Failed to create directory {directory}: {e}")
            return False

    @staticmethod
    def get_file_size(file_path: Path) -> Optional[int]:
        """Get file size in bytes"""
        try:
            return file_path.stat().st_size
        except Exception:
            return None

    @staticmethod
    def is_text_file(file_path: Path, sample_size: int = 1024) -> bool:
        """Check if a file is likely a text file"""
        if not file_path.exists():
            return False

        try:
            with open(file_path, 'rb') as f:
                sample = f.read(sample_size)

            # Check for null bytes (common in binary files)
            if b'\x00' in sample:
                return False

            # Try to decode as UTF-8
            try:
                sample.decode('utf-8')
                return True
            except UnicodeDecodeError:
                return False

        except Exception:
            return False

    @staticmethod
    def read_file_lines(file_path: Path, encoding: str = 'utf-8') -> Optional[List[str]]:
        """Read all lines from a file"""
        if not file_path.exists():
            return None

        try:
            with open(file_path, 'r', encoding=encoding) as f:
                return f.readlines()
        except Exception as e:
            logger.error(f"Failed to read {file_path}: {e}")
            return None

    @staticmethod
    def write_file_lines(file_path: Path, lines: List[str], encoding: str = 'utf-8') -> bool:
        """Write lines to a file"""
        try:
            file_path.parent.mkdir(parents=True, exist_ok=True)
            with open(file_path, 'w', encoding=encoding) as f:
                f.writelines(lines)
            return True
        except Exception as e:
            logger.error(f"Failed to write to {file_path}: {e}")
            return False

    @staticmethod
    def replace_in_file(file_path: Path, old_text: str, new_text: str) -> bool:
        """Replace text in a file"""
        try:
            content = file_path.read_text(encoding='utf-8')
            new_content = content.replace(old_text, new_text)

            if new_content != content:
                file_path.write_text(new_content, encoding='utf-8')
                logger.debug(f"Replaced text in {file_path}")
                return True
            else:
                logger.debug(f"No changes needed in {file_path}")
                return True
        except Exception as e:
            logger.error(f"Failed to replace text in {file_path}: {e}")
            return False

    @staticmethod
    def backup_file(file_path: Path, backup_suffix: str = '.backup') -> Optional[Path]:
        """Create a backup of a file"""
        if not file_path.exists():
            return None

        backup_path = file_path.with_suffix(file_path.suffix + backup_suffix)
        try:
            shutil.copy2(file_path, backup_path)
            logger.debug(f"Created backup: {backup_path}")
            return backup_path
        except Exception as e:
            logger.error(f"Failed to create backup for {file_path}: {e}")
            return None

    @staticmethod
    def copy_file(src: Path, dst: Path, overwrite: bool = False) -> bool:
        """Copy a file from source to destination with error handling

        Args:
            src: Source file path
            dst: Destination file path
            overwrite: Whether to overwrite existing destination file

        Returns:
            True if copy succeeded, False otherwise
        """
        try:
            if not src.exists():
                logger.error(f"Source file does not exist: {src}")
                return False

            if dst.exists() and not overwrite:
                logger.warning(f"Destination file exists and overwrite=False: {dst}")
                return False

            # Create parent directories if they don't exist
            dst.parent.mkdir(parents=True, exist_ok=True)

            # Copy the file preserving metadata
            shutil.copy2(src, dst)
            logger.debug(f"Copied file from {src} to {dst}")
            return True
        except Exception as e:
            logger.error(f"Failed to copy file from {src} to {dst}: {e}")
            return False

    @staticmethod
    def copy_directory(src: Path, dst: Path, overwrite: bool = False) -> bool:
        """Copy a directory from source to destination with error handling

        Args:
            src: Source directory path
            dst: Destination directory path
            overwrite: Whether to overwrite existing destination directory

        Returns:
            True if copy succeeded, False otherwise
        """
        try:
            if not src.exists():
                logger.error(f"Source directory does not exist: {src}")
                return False

            if not src.is_dir():
                logger.error(f"Source path is not a directory: {src}")
                return False

            if dst.exists():
                if not overwrite:
                    logger.warning(f"Destination directory exists and overwrite=False: {dst}")
                    return False
                # Remove existing directory if overwrite is True
                shutil.rmtree(dst)

            # Copy the directory tree
            shutil.copytree(src, dst, dirs_exist_ok=True)
            logger.debug(f"Copied directory from {src} to {dst}")
            return True
        except Exception as e:
            logger.error(f"Failed to copy directory from {src} to {dst}: {e}")
            return False
