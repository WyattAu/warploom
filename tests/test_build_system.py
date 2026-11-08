"""Focused tests for the active Warploom Python build helpers."""

from pathlib import Path
from unittest.mock import patch

from warploom_scripts.build import BuildContext, BuildManager, BuildError
from warploom_scripts.cmake import CMakeManager
from warploom_scripts.conan import ConanManager
from warploom_scripts.utils import (
    detect_platform,
    format_duration,
    get_compiler_path,
    run_command,
    sanitize_filename,
)


def test_build_context_round_trip() -> None:
    context = BuildContext(target="standalone", build_type="debug", compiler="gcc", preset="default")
    data = context.to_dict()
    assert data["target"] == "standalone"
    assert data["preset"] == "default"
    restored = BuildContext.from_dict(data)
    assert restored.target == context.target
    assert restored.build_type == context.build_type


def test_build_manager_defaults_to_current_workspace() -> None:
    manager = BuildManager()
    assert manager.workspace_dir.is_dir()


def test_build_manager_compatibility_operations() -> None:
    manager = BuildManager()
    context = BuildContext(target="standalone", build_type="debug", compiler="gcc", preset="default")
    assert manager.clean(context)
    assert manager.configure(context)
    assert manager.build(context)


def test_cmake_manager_generate_is_side_effect_free() -> None:
    manager = CMakeManager()
    assert manager.generate(".", "build", "default")


def test_cmake_manager_commands_are_delegated() -> None:
    manager = CMakeManager()
    with patch("warploom_scripts.cmake.execute_command") as execute:
        execute.return_value = 0
        build_dir = Path("/tmp")
        manager.build(build_dir, config="debug")
        manager.install(build_dir, config="debug")
        assert execute.call_count == 2


def test_conan_profile_creation(tmp_path: Path) -> None:
    manager = ConanManager(tmp_path)
    assert manager.create_profile("test", {"compiler": "gcc"})
    assert (tmp_path / "conan" / "profiles" / "test").exists()


def test_command_and_format_helpers() -> None:
    with patch("warploom_scripts.utils.subprocess.run") as run:
        run.return_value.returncode = 0
        run.return_value.stdout = "ok"
        run.return_value.stderr = ""
        assert run_command(["echo", "ok"])[0] == 0
    assert detect_platform()
    assert format_duration(3661) == "1h 1m 1s"
    assert sanitize_filename("test file") == "test_file"
    assert get_compiler_path("definitely-not-a-compiler") is None


def test_build_error_message() -> None:
    assert str(BuildError("Test error")) == "Test error"
