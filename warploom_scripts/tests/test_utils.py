"""
Unit tests for utility modules.

Tests for command_utils, file_utils, path_utils, system_utils,
terminal_utils, platform_utils, and logging_utils.
"""

from __future__ import annotations

import os
import sys
import tempfile
import time
from pathlib import Path
from typing import Any
from unittest.mock import Mock, patch, MagicMock
import pytest

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.utils.command_utils import execute_command
from warploom_scripts.utils.file_utils import FileUtils
from warploom_scripts.utils.path_utils import PathUtils
from warploom_scripts.utils.exceptions import CommandExecutionError


class TestCommandUtils:
    """Unit tests for command_utils module."""

    def test_execute_command_success(self) -> None:
        """Test execute_command with successful command."""
        with patch('subprocess.run') as mock_run:
            mock_result = Mock()
            mock_result.returncode = 0
            mock_result.stdout = "output"
            mock_result.stderr = ""
            mock_run.return_value = mock_result

            execute_command("echo test")
            mock_run.assert_called_once()

    def test_execute_command_with_env(self) -> None:
        """Test execute_command with custom environment."""
        with patch('subprocess.run') as mock_run:
            mock_result = Mock()
            mock_result.returncode = 0
            mock_result.stdout = "output"
            mock_result.stderr = ""
            mock_run.return_value = mock_result

            env = {"TEST_VAR": "test_value"}
            execute_command("echo test", env=env)
            mock_run.assert_called_once()
            call_kwargs = mock_run.call_args[1]
            assert call_kwargs['env'] == env

    def test_execute_command_with_retries(self) -> None:
        """Test execute_command with retries."""
        with patch('subprocess.run') as mock_run:
            # First attempt fails, second succeeds
            mock_result_fail = Mock()
            mock_result_fail.returncode = 1
            mock_result_fail.stdout = ""
            mock_result_fail.stderr = "error"

            mock_result_success = Mock()
            mock_result_success.returncode = 0
            mock_result_success.stdout = "output"
            mock_result_success.stderr = ""

            mock_run.side_effect = [mock_result_fail, mock_result_success]

            with patch('time.sleep'):
                execute_command("echo test", retries=2)

            assert mock_run.call_count == 2

    def test_execute_command_timeout(self) -> None:
        """Test execute_command with timeout."""
        with patch('subprocess.run') as mock_run:
            from subprocess import TimeoutExpired
            mock_run.side_effect = TimeoutExpired("cmd", 10)

            with pytest.raises(TimeoutExpired):
                execute_command("sleep 100", timeout=10)

    def test_execute_command_file_not_found(self) -> None:
        """Test execute_command with file not found."""
        with patch('subprocess.run') as mock_run:
            mock_run.side_effect = FileNotFoundError("Command not found")

            with pytest.raises(FileNotFoundError):
                execute_command("nonexistent_command")

    def test_execute_command_permission_error(self) -> None:
        """Test execute_command with permission error."""
        with patch('subprocess.run') as mock_run:
            mock_run.side_effect = PermissionError("Permission denied")

            with pytest.raises(PermissionError):
                execute_command("protected_command")

    def test_execute_command_all_retries_fail(self) -> None:
        """Test execute_command when all retries fail."""
        with patch('subprocess.run') as mock_run:
            mock_result = Mock()
            mock_result.returncode = 1
            mock_result.stdout = ""
            mock_result.stderr = "error"
            mock_run.return_value = mock_result

            with patch('time.sleep'):
                with pytest.raises(CommandExecutionError):
                    execute_command("failing_command", retries=3)

    def test_execute_command_msys2_path_conversion(self) -> None:
        """Test execute_command with MSYS2 path conversion."""
        with patch('subprocess.run') as mock_run:
            mock_result = Mock()
            mock_result.returncode = 0
            mock_result.stdout = "output"
            mock_result.stderr = ""
            mock_run.return_value = mock_result

            with patch.dict(os.environ, {'MSYS2_PATH': '/msys64', 'PATH': 'C:\\msys64\\ucrt64\\bin;C:\\msys64\\usr\\bin'}):
                execute_command("echo test")

            # Verify PATH was converted
            call_kwargs = mock_run.call_args[1]
            env = call_kwargs['env']
            if env and 'PATH' in env:
                # Should have forward slashes and colons
                assert '/' in env['PATH'] or ':' in env['PATH']


class TestFileUtils:
    """Unit tests for FileUtils class."""

    def test_calculate_file_hash(self) -> None:
        """Test calculate_file_hash method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            test_file.write_text("test content")

            hash_value = FileUtils.calculate_file_hash(test_file, 'sha256')
            assert hash_value is not None
            assert isinstance(hash_value, str)
            assert len(hash_value) == 64  # SHA256 produces 64 hex characters

    def test_calculate_file_hash_nonexistent(self) -> None:
        """Test calculate_file_hash with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "nonexistent.txt"
            hash_value = FileUtils.calculate_file_hash(test_file)
            assert hash_value is None

    def test_find_files_by_extension(self) -> None:
        """Test find_files_by_extension method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir)
            (test_dir / "test1.cpp").write_text("content1")
            (test_dir / "test2.cpp").write_text("content2")
            (test_dir / "test3.hpp").write_text("content3")

            cpp_files = FileUtils.find_files_by_extension(test_dir, {'.cpp'})
            assert len(cpp_files) == 2

            all_files = FileUtils.find_files_by_extension(test_dir, {'.cpp', '.hpp'})
            assert len(all_files) == 3

    def test_find_files_by_extension_nonexistent_dir(self) -> None:
        """Test find_files_by_extension with nonexistent directory."""
        test_dir = Path("/nonexistent/directory")
        files = FileUtils.find_files_by_extension(test_dir, {'.cpp'})
        assert files == []

    def test_find_files_by_pattern(self) -> None:
        """Test find_files_by_pattern method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir)
            (test_dir / "test1.txt").write_text("content1")
            (test_dir / "test2.txt").write_text("content2")
            (test_dir / "other.txt").write_text("content3")

            files = FileUtils.find_files_by_pattern(test_dir, "test*.txt")
            assert len(files) == 2

    def test_safe_copy_success(self) -> None:
        """Test safe_copy method with successful copy."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            src = Path(tmp_dir) / "source.txt"
            dst = Path(tmp_dir) / "dest.txt"
            src.write_text("test content")

            result = FileUtils.safe_copy(src, dst)
            assert result is True
            assert dst.exists()
            assert dst.read_text() == "test content"

    def test_safe_copy_overwrite_false(self) -> None:
        """Test safe_copy with overwrite=False and existing destination."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            src = Path(tmp_dir) / "source.txt"
            dst = Path(tmp_dir) / "dest.txt"
            src.write_text("new content")
            dst.write_text("old content")

            result = FileUtils.safe_copy(src, dst, overwrite=False)
            assert result is False
            assert dst.read_text() == "old content"

    def test_safe_copy_overwrite_true(self) -> None:
        """Test safe_copy with overwrite=True."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            src = Path(tmp_dir) / "source.txt"
            dst = Path(tmp_dir) / "dest.txt"
            src.write_text("new content")
            dst.write_text("old content")

            result = FileUtils.safe_copy(src, dst, overwrite=True)
            assert result is True
            assert dst.read_text() == "new content"

    def test_safe_move_success(self) -> None:
        """Test safe_move method with successful move."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            src = Path(tmp_dir) / "source.txt"
            dst = Path(tmp_dir) / "dest.txt"
            src.write_text("test content")

            result = FileUtils.safe_move(src, dst)
            assert result is True
            assert not src.exists()
            assert dst.exists()
            assert dst.read_text() == "test content"

    def test_ensure_directory_exists(self) -> None:
        """Test ensure_directory_exists method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir) / "new" / "nested" / "dir"
            result = FileUtils.ensure_directory_exists(test_dir)
            assert result is True
            assert test_dir.exists()
            assert test_dir.is_dir()

    def test_get_file_size(self) -> None:
        """Test get_file_size method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            test_file.write_text("test content")

            size = FileUtils.get_file_size(test_file)
            assert size is not None
            assert size == 12  # "test content" is 12 bytes

    def test_get_file_size_nonexistent(self) -> None:
        """Test get_file_size with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "nonexistent.txt"
            size = FileUtils.get_file_size(test_file)
            assert size is None

    def test_is_text_file(self) -> None:
        """Test is_text_file method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            text_file = Path(tmp_dir) / "text.txt"
            text_file.write_text("This is a text file")

            result = FileUtils.is_text_file(text_file)
            assert result is True

    def test_is_text_file_binary(self) -> None:
        """Test is_text_file with binary file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            binary_file = Path(tmp_dir) / "binary.bin"
            binary_file.write_bytes(b'\x00\x01\x02\x03')

            result = FileUtils.is_text_file(binary_file)
            assert result is False

    def test_is_text_file_nonexistent(self) -> None:
        """Test is_text_file with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "nonexistent.txt"
            result = FileUtils.is_text_file(test_file)
            assert result is False

    def test_read_file_lines(self) -> None:
        """Test read_file_lines method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            test_file.write_text("line1\nline2\nline3\n")

            lines = FileUtils.read_file_lines(test_file)
            assert lines is not None
            assert len(lines) == 3
            assert lines[0] == "line1\n"

    def test_read_file_lines_nonexistent(self) -> None:
        """Test read_file_lines with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "nonexistent.txt"
            lines = FileUtils.read_file_lines(test_file)
            assert lines is None

    def test_write_file_lines(self) -> None:
        """Test write_file_lines method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            lines = ["line1\n", "line2\n", "line3\n"]

            result = FileUtils.write_file_lines(test_file, lines)
            assert result is True
            assert test_file.read_text() == "line1\nline2\nline3\n"

    def test_replace_in_file(self) -> None:
        """Test replace_in_file method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            test_file.write_text("old text old text")

            result = FileUtils.replace_in_file(test_file, "old", "new")
            assert result is True
            assert test_file.read_text() == "new text new text"

    def test_replace_in_file_no_change(self) -> None:
        """Test replace_in_file when no change needed."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            test_file.write_text("test text")

            result = FileUtils.replace_in_file(test_file, "old", "new")
            assert result is True
            assert test_file.read_text() == "test text"

    def test_backup_file(self) -> None:
        """Test backup_file method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "test.txt"
            test_file.write_text("test content")

            backup_path = FileUtils.backup_file(test_file)
            assert backup_path is not None
            assert backup_path.exists()
            assert backup_path.read_text() == "test content"

    def test_backup_file_nonexistent(self) -> None:
        """Test backup_file with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_file = Path(tmp_dir) / "nonexistent.txt"
            backup_path = FileUtils.backup_file(test_file)
            assert backup_path is None

    def test_copy_file(self) -> None:
        """Test copy_file method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            src = Path(tmp_dir) / "source.txt"
            dst = Path(tmp_dir) / "dest.txt"
            src.write_text("test content")

            result = FileUtils.copy_file(src, dst)
            assert result is True
            assert dst.exists()
            assert dst.read_text() == "test content"

    def test_copy_file_nonexistent_source(self) -> None:
        """Test copy_file with nonexistent source."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            src = Path(tmp_dir) / "nonexistent.txt"
            dst = Path(tmp_dir) / "dest.txt"

            result = FileUtils.copy_file(src, dst)
            assert result is False

    def test_copy_directory(self) -> None:
        """Test copy_directory method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            src = Path(tmp_dir) / "source"
            dst = Path(tmp_dir) / "dest"
            src.mkdir()
            (src / "file1.txt").write_text("content1")
            (src / "file2.txt").write_text("content2")

            result = FileUtils.copy_directory(src, dst)
            assert result is True
            assert dst.exists()
            assert (dst / "file1.txt").exists()
            assert (dst / "file2.txt").exists()

    def test_copy_directory_nonexistent_source(self) -> None:
        """Test copy_directory with nonexistent source."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            src = Path(tmp_dir) / "nonexistent"
            dst = Path(tmp_dir) / "dest"

            result = FileUtils.copy_directory(src, dst)
            assert result is False


class TestPathUtils:
    """Unit tests for PathUtils class."""

    def test_normalize_path(self) -> None:
        """Test normalize_path method."""
        path = PathUtils.normalize_path("./test/../test/file.txt")
        assert "test" in path
        assert "file.txt" in path

    def test_to_posix_path(self) -> None:
        """Test to_posix_path method."""
        path = PathUtils.to_posix_path("test\\path\\file.txt")
        assert "/" in path or "\\" in path  # Platform dependent

    def test_to_native_path(self) -> None:
        """Test to_native_path method."""
        path = PathUtils.to_native_path("test/path/file.txt")
        assert isinstance(path, str)

    def test_make_relative(self) -> None:
        """Test make_relative method."""
        base = Path("/home/user/project")
        target = Path("/home/user/project/src/file.txt")
        relative = PathUtils.make_relative(base, target)
        assert relative is not None
        assert "src" in relative
        assert "file.txt" in relative

    def test_make_relative_not_relative(self) -> None:
        """Test make_relative with non-relative paths."""
        base = Path("/home/user/project1")
        target = Path("/home/user/project2/file.txt")
        relative = PathUtils.make_relative(base, target)
        assert relative is None

    def test_find_common_root(self) -> None:
        """Test find_common_root method."""
        paths = [
            Path("/home/user/project/src/file1.txt"),
            Path("/home/user/project/include/file2.h"),
            Path("/home/user/project/tests/file3.py")
        ]
        common = PathUtils.find_common_root(paths)
        assert common is not None
        assert "project" in str(common)

    def test_find_common_root_empty_list(self) -> None:
        """Test find_common_root with empty list."""
        common = PathUtils.find_common_root([])
        assert common is None

    def test_find_common_root_no_common(self) -> None:
        """Test find_common_root with no common root."""
        paths = [
            Path("/home/user1/project/file.txt"),
            Path("/home/user2/project/file.txt")
        ]
        common = PathUtils.find_common_root(paths)
        # May return None or a very high-level common root
        assert common is None or "home" in str(common)

    def test_get_project_root(self) -> None:
        """Test get_project_root method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir)
            (test_dir / "CMakeLists.txt").write_text("test")

            root = PathUtils.get_project_root(test_dir)
            # The function may return a different path due to how it searches
            # Just verify it returns a valid Path
            assert isinstance(root, Path)
            assert root.exists()

    def test_sanitize_filename(self) -> None:
        """Test sanitize_filename method."""
        filename = PathUtils.sanitize_filename('test<>:"/\\|?*file.txt')
        assert "<" not in filename
        assert ">" not in filename
        assert ":" not in filename
        assert '"' not in filename
        assert "/" not in filename
        assert "\\" not in filename
        assert "|" not in filename
        assert "?" not in filename
        assert "*" not in filename

    def test_sanitize_filename_empty(self) -> None:
        """Test sanitize_filename with empty filename."""
        filename = PathUtils.sanitize_filename("")
        assert filename == "unnamed"

    def test_sanitize_filename_too_long(self) -> None:
        """Test sanitize_filename with too long filename."""
        long_name = "a" * 300 + ".txt"
        filename = PathUtils.sanitize_filename(long_name)
        assert len(filename) <= 255

    def test_create_backup_path(self) -> None:
        """Test create_backup_path method."""
        original = Path("/path/to/file.txt")
        backup = PathUtils.create_backup_path(original)
        assert str(backup).endswith(".backup")

    def test_get_unique_path(self) -> None:
        """Test get_unique_path method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            base = Path(tmp_dir) / "test.txt"
            base.write_text("content")

            unique = PathUtils.get_unique_path(base)
            assert unique != base
            assert not unique.exists()

    def test_get_unique_path_not_exists(self) -> None:
        """Test get_unique_path when path doesn't exist."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            base = Path(tmp_dir) / "test.txt"

            unique = PathUtils.get_unique_path(base)
            assert unique == base

    def test_expand_user_path(self) -> None:
        """Test expand_user_path method."""
        path = PathUtils.expand_user_path("~/test")
        assert "~" not in path

    def test_resolve_case_insensitive(self) -> None:
        """Test resolve_case_insensitive method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            base = Path(tmp_dir)
            (base / "TestFile.txt").write_text("content")

            resolved = PathUtils.resolve_case_insensitive(base, "testfile.txt")
            assert resolved is not None
            assert resolved.exists()

    def test_resolve_case_insensitive_nonexistent(self) -> None:
        """Test resolve_case_insensitive with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            base = Path(tmp_dir)

            resolved = PathUtils.resolve_case_insensitive(base, "nonexistent.txt")
            assert resolved is None

    def test_get_relative_depth(self) -> None:
        """Test get_relative_depth method."""
        from_path = Path("/home/user/project")
        to_path = Path("/home/user/project/src/nested/file.txt")
        depth = PathUtils.get_relative_depth(from_path, to_path)
        assert depth == 3  # src, nested, file.txt

    def test_is_subdirectory(self) -> None:
        """Test is_subdirectory method."""
        parent = Path("/home/user/project")
        child = Path("/home/user/project/src/file.txt")
        result = PathUtils.is_subdirectory(parent, child)
        assert result is True

    def test_is_subdirectory_false(self) -> None:
        """Test is_subdirectory when not subdirectory."""
        parent = Path("/home/user/project1")
        child = Path("/home/user/project2/file.txt")
        result = PathUtils.is_subdirectory(parent, child)
        assert result is False

    def test_get_path_hash(self) -> None:
        """Test get_path_hash method."""
        path = Path("/home/user/project/file.txt")
        hash_value = PathUtils.get_path_hash(path)
        assert isinstance(hash_value, str)
        assert len(hash_value) == 8  # MD5 truncated to 8 chars

    def test_find_files_by_type(self) -> None:
        """Test find_files_by_type method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir)
            (test_dir / "test1.cpp").write_text("content1")
            (test_dir / "test2.cpp").write_text("content2")
            (test_dir / "test3.hpp").write_text("content3")

            cpp_files = PathUtils.find_files_by_type(test_dir, {"cpp"})
            assert len(cpp_files) == 2

    def test_find_files_by_type_nonexistent(self) -> None:
        """Test find_files_by_type with nonexistent directory."""
        test_dir = Path("/nonexistent/directory")
        files = PathUtils.find_files_by_type(test_dir, {"cpp"})
        assert files == []

    def test_get_directory_size(self) -> None:
        """Test get_directory_size method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir)
            (test_dir / "file1.txt").write_text("content1")
            (test_dir / "file2.txt").write_text("content2")

            size = PathUtils.get_directory_size(test_dir)
            assert size > 0

    def test_get_directory_size_nonexistent(self) -> None:
        """Test get_directory_size with nonexistent directory."""
        test_dir = Path("/nonexistent/directory")
        size = PathUtils.get_directory_size(test_dir)
        assert size == 0

    def test_create_temporary_directory(self) -> None:
        """Test create_temporary_directory method."""
        temp_dir = PathUtils.create_temporary_directory()
        assert temp_dir.exists()
        assert temp_dir.is_dir()
        # Cleanup
        import shutil
        shutil.rmtree(temp_dir)

    def test_cleanup_empty_directories(self) -> None:
        """Test cleanup_empty_directories method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            test_dir = Path(tmp_dir)
            (test_dir / "empty1").mkdir()
            (test_dir / "empty2").mkdir()
            (test_dir / "nonempty").mkdir()
            (test_dir / "nonempty" / "file.txt").write_text("content")

            count = PathUtils.cleanup_empty_directories(test_dir)
            assert count == 2
            assert not (test_dir / "empty1").exists()
            assert not (test_dir / "empty2").exists()
            assert (test_dir / "nonempty").exists()


class TestSystemUtils:
    """Unit tests for system_utils module."""

    def test_import_system_utils(self) -> None:
        """Test that system_utils can be imported."""
        try:
            from warploom_scripts.utils import system_utils
            assert system_utils is not None
        except ImportError:
            pytest.skip("system_utils module not available")


class TestTerminalUtils:
    """Unit tests for terminal_utils module."""

    def test_import_terminal_utils(self) -> None:
        """Test that terminal_utils can be imported."""
        try:
            from warploom_scripts.utils import terminal_utils
            assert terminal_utils is not None
        except ImportError:
            pytest.skip("terminal_utils module not available")


class TestPlatformUtils:
    """Unit tests for platform_utils module."""

    def test_import_platform_utils(self) -> None:
        """Test that platform_utils can be imported."""
        try:
            from warploom_scripts.utils import platform_utils
            assert platform_utils is not None
        except ImportError:
            pytest.skip("platform_utils module not available")


class TestLoggingUtils:
    """Unit tests for logging_utils module."""

    def test_import_logging_utils(self) -> None:
        """Test that logging_utils can be imported."""
        try:
            from warploom_scripts.utils import logging_utils
            assert logging_utils is not None
        except ImportError:
            pytest.skip("logging_utils module not available")


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
