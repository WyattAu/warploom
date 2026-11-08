# warploom_scripts/utils/path_utils.py
"""
Path utility functions for Warploom project.

Provides path manipulation, normalization, and cross-platform path operations.
"""

import logging
from pathlib import Path, PurePath
from typing import List, Optional, Set

logger = logging.getLogger(__name__)


class PathUtils:
    """Utility class for path operations"""

    @staticmethod
    def normalize_path(path: str) -> str:
        """Normalize a path for the current platform"""
        return str(Path(path).resolve())

    @staticmethod
    def to_posix_path(path: str) -> str:
        """Convert path to POSIX format (forward slashes)"""
        return str(PurePath(path))

    @staticmethod
    def to_native_path(path: str) -> str:
        """Convert path to native platform format"""
        return str(Path(path))

    @staticmethod
    def make_relative(base: Path, target: Path) -> Optional[str]:
        """Make target path relative to base path"""
        try:
            return str(target.relative_to(base))
        except ValueError:
            # Paths are not relative
            return None

    @staticmethod
    def find_common_root(paths: List[Path]) -> Optional[Path]:
        """Find the common root directory for a list of paths"""
        if not paths:
            return None

        # Convert to resolved paths
        resolved_paths = [p.resolve() for p in paths]

        # Start with the first path
        common_root = resolved_paths[0].parent

        for path in resolved_paths[1:]:
            # Find common parent
            while common_root != Path(common_root.anchor) and not path.is_relative_to(common_root):
                common_root = common_root.parent

            if common_root == Path(common_root.anchor):
                return None  # No common root

        return common_root

    @staticmethod
    def get_project_root(start_path: Optional[Path] = None) -> Path:
        """Find the project root directory"""
        if start_path is None:
            start_path = Path.cwd()

        # Look for common project markers
        markers = [
            'CMakeLists.txt',
            '.git',
            'conanfile.py',
            'WarploomController.py'
        ]

        current = start_path.resolve()
        while current != current.parent:
            for marker in markers:
                if (current / marker).exists():
                    return current
            current = current.parent

        # Fallback to current directory
        return Path.cwd()

    @staticmethod
    def sanitize_filename(filename: str) -> str:
        """Sanitize filename for safe filesystem operations"""
        import re

        # Remove or replace invalid characters
        invalid_chars = '<>:"/\\|?*'
        for char in invalid_chars:
            filename = filename.replace(char, '_')

        # Normalize whitespace and remove leading/trailing whitespace and dots
        filename = re.sub(r'\s+', '_', filename).strip(' .')

        # Ensure not empty
        if not filename:
            filename = 'unnamed'

        # Limit length
        if len(filename) > 255:
            name_part, ext = filename.rsplit('.', 1) if '.' in filename else (filename, '')
            max_name_len = 255 - len(ext) - 1 if ext else 255
            filename = name_part[:max_name_len]
            if ext:
                filename += '.' + ext

        return filename

    @staticmethod
    def create_backup_path(original_path: Path, suffix: str = '.backup') -> Path:
        """Create a backup path for a file"""
        return original_path.with_suffix(original_path.suffix + suffix)

    @staticmethod
    def get_unique_path(base_path: Path) -> Path:
        """Get a unique path by appending numbers if needed"""
        if not base_path.exists():
            return base_path

        counter = 1
        stem = base_path.stem
        suffix = base_path.suffix
        parent = base_path.parent

        while True:
            new_path = parent / f"{stem}_{counter}{suffix}"
            if not new_path.exists():
                return new_path
            counter += 1

    @staticmethod
    def expand_user_path(path: str) -> str:
        """Expand user home directory in path"""
        return str(Path(path).expanduser())

    @staticmethod
    def resolve_case_insensitive(base_path: Path, target_name: str) -> Optional[Path]:
        """Resolve a case-insensitive path component"""
        if not base_path.exists():
            return None

        target_lower = target_name.lower()

        try:
            for item in base_path.iterdir():
                if item.name.lower() == target_lower:
                    return item
        except PermissionError:
            pass

        return None

    @staticmethod
    def get_relative_depth(from_path: Path, to_path: Path) -> int:
        """Get the relative depth between two paths"""
        try:
            relative = to_path.relative_to(from_path)
            return len(relative.parts)
        except ValueError:
            # Paths are not relative, calculate common root
            from_resolved = from_path.resolve()
            to_resolved = to_path.resolve()

            common_root = PathUtils.find_common_root([from_resolved, to_resolved])
            if common_root:
                from_depth = len(from_resolved.relative_to(common_root).parts)
                to_depth = len(to_resolved.relative_to(common_root).parts)
                return abs(to_depth - from_depth)
            else:
                return 0

    @staticmethod
    def is_subdirectory(parent: Path, child: Path) -> bool:
        """Check if child is a subdirectory of parent"""
        try:
            child.relative_to(parent)
            return True
        except ValueError:
            return False

    @staticmethod
    def get_path_hash(path: Path) -> str:
        """Get a hash of a path for caching purposes"""
        import hashlib
        path_str = str(path.resolve())
        return hashlib.md5(path_str.encode()).hexdigest()[:8]

    @staticmethod
    def find_files_by_type(directory: Path, file_types: Set[str]) -> List[Path]:
        """Find files by their type/extension"""
        if not directory.exists():
            return []

        files: List[Path] = []
        for ext in file_types:
            files.extend(directory.rglob(f"*.{ext}"))
        return files

    @staticmethod
    def get_directory_size(directory: Path) -> int:
        """Get total size of a directory in bytes"""
        if not directory.exists():
            return 0

        total_size = 0
        try:
            for item in directory.rglob('*'):
                if item.is_file():
                    total_size += item.stat().st_size
        except PermissionError:
            pass

        return total_size

    @staticmethod
    def create_temporary_directory(prefix: str = 'warploom_') -> Path:
        """Create a temporary directory"""
        import tempfile
        return Path(tempfile.mkdtemp(prefix=prefix))

    @staticmethod
    def cleanup_empty_directories(directory: Path) -> int:
        """Remove empty directories recursively, return count of removed directories"""
        if not directory.exists():
            return 0

        removed_count = 0

        try:
            for item in directory.rglob('*'):
                if item.is_dir() and not any(item.iterdir()):
                    item.rmdir()
                    removed_count += 1
                    logger.debug(f"Removed empty directory: {item}")
        except PermissionError:
            pass

        return removed_count
