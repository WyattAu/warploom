#!/usr/bin/env python3
"""
Platform-Specific Checks Script

This script performs platform-specific validation checks for the WarploomController build pipeline.
It validates Windows and Linux specific requirements, paths, and environments.

Features:
- Windows-specific checks (VS DevCmd, MSYS2, paths)
- Linux-specific checks (bash, package managers, paths)
- Cross-platform path handling validation
- Platform-specific dependency checks
- Platform-specific environment validation

Usage:
    python platform_checks.py [--platform PLATFORM] [--output OUTPUT]

Arguments:
    --platform: Target platform (windows, linux, auto)
    --output: Output directory for reports
"""

import os
import sys
import json
import subprocess
import platform
import argparse
import time
from pathlib import Path
from typing import Dict, List, Any, Optional
import shutil

class PlatformChecker:
    def __init__(self, workspace_dir: str):
        self.workspace_dir = Path(workspace_dir)
        self.platform = self.detect_platform()
        self.results: Dict[str, Any] = {}

    def detect_platform(self) -> str:
        """Detect the current platform."""
        system = platform.system().lower()
        if system == "windows":
            return "windows"
        elif system == "linux":
            return "linux"
        else:
            return "unknown"

    def check_windows_specific(self) -> Dict[str, Any]:
        """Perform Windows-specific checks."""
        checks: Dict[str, Any] = {
            "vs_devcmd_available": False,
            "msys2_available": False,
            "mingw_available": False,
            "windows_sdk": False,
            "path_environment": {},
            "registry_checks": {},
            "issues": []
        }

        # Check for VS DevCmd
        try:
            # Try to find vsdevcmd.bat or similar
            vs_paths = [
                "C:/Program Files/Microsoft Visual Studio",
                "C:/Program Files (x86)/Microsoft Visual Studio"
            ]

            vs_found = False
            for vs_path in vs_paths:
                if Path(vs_path).exists():
                    # Look for Common7/Tools/VsDevCmd.bat
                    vsdevcmd = Path(vs_path) / "Common7" / "Tools" / "VsDevCmd.bat"
                    if vsdevcmd.exists():
                        checks["vs_devcmd_available"] = True
                        vs_found = True
                        break

            if not vs_found:
                checks["issues"].append("Visual Studio DevCmd not found")
        except Exception as e:
            checks["issues"].append(f"Error checking VS DevCmd: {e}")

        # Check for MSYS2
        msys_paths = ["C:/msys64", "C:/msys2"]
        msys_found = any(Path(p).exists() for p in msys_paths)
        checks["msys2_available"] = msys_found
        if not msys_found:
            checks["issues"].append("MSYS2 not found")

        # Check for MinGW
        mingw_paths = ["C:/mingw64", "C:/mingw32"]
        mingw_found = any(Path(p).exists() for p in mingw_paths)
        checks["mingw_available"] = mingw_found
        if not mingw_found:
            checks["issues"].append("MinGW not found")

        # Check Windows SDK
        sdk_paths = [
            "C:/Program Files (x86)/Windows Kits/10",
            "C:/Program Files/Windows Kits/10"
        ]
        sdk_found = any(Path(p).exists() for p in sdk_paths)
        checks["windows_sdk"] = sdk_found
        if not sdk_found:
            checks["issues"].append("Windows SDK not found")

        # Check PATH environment
        path_env = os.environ.get("PATH", "")
        path_dirs = path_env.split(os.pathsep)

        important_paths = [
            "cmake",
            "ninja",
            "cl.exe",
            "gcc.exe",
            "clang.exe"
        ]

        for tool in important_paths:
            found = any(shutil.which(tool) is not None for d in path_dirs if d)
            checks["path_environment"][tool] = found
            if not found:
                checks["issues"].append(f"{tool} not found in PATH")

        return checks

    def check_linux_specific(self) -> Dict[str, Any]:
        """Perform Linux-specific checks."""
        checks: Dict[str, Any] = {
            "bash_available": False,
            "package_managers": {},
            "development_packages": {},
            "system_libraries": {},
            "path_environment": {},
            "issues": []
        }

        # Check bash
        bash_path = shutil.which("bash")
        checks["bash_available"] = bash_path is not None
        if not checks["bash_available"]:
            checks["issues"].append("bash not found")

        # Check package managers
        package_managers = ["apt", "yum", "dnf", "pacman", "zypper"]
        for pm in package_managers:
            found = shutil.which(pm) is not None
            checks["package_managers"][pm] = found

        # Check development packages (try to detect common ones)
        dev_packages = {
            "build-essential": ["gcc", "g++", "make"],
            "cmake": ["cmake"],
            "ninja": ["ninja-build"],
            "python3-dev": ["python3-config"],
            "libvulkan-dev": ["vulkan-headers"],
            "qtbase5-dev": ["qmake"]
        }

        for package, tools in dev_packages.items():
            package_found = all(shutil.which(tool) is not None for tool in tools)
            checks["development_packages"][package] = package_found
            if not package_found:
                checks["issues"].append(f"Development package {package} not found (tools: {', '.join(tools)})")

        # Check system libraries
        system_libs = [
            "libstdc++.so",
            "libgcc_s.so",
            "libc.so"
        ]

        for lib in system_libs:
            # Try to find in common library paths
            lib_paths = ["/lib", "/lib64", "/usr/lib", "/usr/lib64"]
            lib_found = any((Path(p) / lib).exists() for p in lib_paths)
            checks["system_libraries"][lib] = lib_found
            if not lib_found:
                checks["issues"].append(f"System library {lib} not found")

        # Check PATH environment
        path_env = os.environ.get("PATH", "")
        path_dirs = path_env.split(os.pathsep)

        important_paths = [
            "gcc",
            "g++",
            "cmake",
            "ninja",
            "python3"
        ]

        for tool in important_paths:
            found = shutil.which(tool) is not None
            checks["path_environment"][tool] = found
            if not found:
                checks["issues"].append(f"{tool} not found in PATH")

        return checks

    def validate_path_handling(self) -> Dict[str, Any]:
        """Validate cross-platform path handling."""
        validation: Dict[str, Any] = {
            "path_separators": {},
            "absolute_paths": {},
            "relative_paths": {},
            "path_operations": {},
            "issues": []
        }

        # Test path separators
        test_path = "a/b\\c/d\\e"
        if self.platform == "windows":
            expected_sep = "\\"
            normalized = test_path.replace("/", "\\")
        else:
            expected_sep = "/"
            normalized = test_path.replace("\\", "/")

        validation["path_separators"] = {
            "platform_expected": expected_sep,
            "test_path_normalized": normalized
        }

        # Test absolute path handling
        abs_paths = [
            "/usr/local/bin",
            "C:\\Program Files\\bin",
            str(self.workspace_dir / "test")
        ]

        for path_str in abs_paths:
            try:
                p = Path(path_str)
                is_abs = p.is_absolute()
                validation["absolute_paths"][path_str] = {
                    "is_absolute": is_abs,
                    "exists": p.exists()
                }
            except Exception as e:
                validation["absolute_paths"][path_str] = {
                    "error": str(e)
                }

        # Test relative path handling
        rel_paths = [
            "src/main.cpp",
            "../include/header.h",
            "build/debug"
        ]

        for path_str in rel_paths:
            try:
                p = Path(path_str)
                resolved = (self.workspace_dir / p).resolve()
                validation["relative_paths"][path_str] = {
                    "resolved": str(resolved),
                    "exists": resolved.exists()
                }
            except Exception as e:
                validation["relative_paths"][path_str] = {
                    "error": str(e)
                }

        # Test path operations
        test_ops = {
            "join": str(Path("a") / "b" / "c"),
            "parent": str(Path("a/b/c").parent),
            "name": Path("a/b/c.txt").name,
            "stem": Path("a/b/c.txt").stem,
            "suffix": Path("a/b/c.txt").suffix
        }

        validation["path_operations"] = test_ops

        # Check for path-related issues
        if self.platform == "windows":
            # Check for mixed separators in workspace
            mixed_sep_files = []
            for file_path in self.workspace_dir.rglob("*"):
                if "/" in str(file_path) and "\\" in str(file_path):
                    mixed_sep_files.append(str(file_path.relative_to(self.workspace_dir)))

            if mixed_sep_files:
                validation["issues"].append(f"Mixed path separators found in {len(mixed_sep_files)} files")

        return validation

    def validate_environment_variables(self) -> Dict[str, Any]:
        """Validate platform-specific environment variables."""
        validation: Dict[str, Any] = {
            "required_vars": {},
            "optional_vars": {},
            "path_related": {},
            "issues": []
        }

        # Required environment variables by platform
        required_vars = {
            "windows": ["PATH", "SystemRoot", "TEMP"],
            "linux": ["PATH", "HOME", "USER"]
        }

        optional_vars = {
            "windows": ["VSINSTALLDIR", "DevEnvDir", "MSYS2_PATH"],
            "linux": ["LD_LIBRARY_PATH", "PKG_CONFIG_PATH", "CC", "CXX"]
        }

        platform_required = required_vars.get(self.platform, [])
        platform_optional = optional_vars.get(self.platform, [])

        # Check required variables
        for var in platform_required:
            value = os.environ.get(var)
            exists = value is not None
            validation["required_vars"][var] = {
                "exists": exists,
                "value": value[:100] + "..." if value and len(value) > 100 else value
            }
            if not exists:
                validation["issues"].append(f"Required environment variable {var} not set")

        # Check optional variables
        for var in platform_optional:
            value = os.environ.get(var)
            exists = value is not None
            validation["optional_vars"][var] = {
                "exists": exists,
                "value": value[:100] + "..." if value and len(value) > 100 else value
            }

        # Check PATH-related variables
        path_vars = ["PATH"]
        if self.platform == "linux":
            path_vars.extend(["LD_LIBRARY_PATH", "PKG_CONFIG_PATH"])

        for var in path_vars:
            value = os.environ.get(var)
            if value:
                dirs = value.split(os.pathsep)
                validation["path_related"][var] = {
                    "count": len(dirs),
                    "sample_dirs": dirs[:5],  # First 5 directories
                    "all_exist": all(Path(d).exists() for d in dirs if d)
                }

        return validation

    def run_platform_checks(self, output_dir: Path) -> Dict[str, Any]:
        """Run complete platform-specific validation."""
        platform_results: Dict[str, Any] = {
            "platform": self.platform,
            "timestamp": time.time(),
            "platform_specific_checks": {},
            "path_handling_validation": {},
            "environment_validation": {},
            "summary": {}
        }

        print(f"Running platform checks for {self.platform}")

        # Platform-specific checks
        if self.platform == "windows":
            platform_results["platform_specific_checks"] = self.check_windows_specific()
        elif self.platform == "linux":
            platform_results["platform_specific_checks"] = self.check_linux_specific()
        else:
            platform_results["platform_specific_checks"] = {
                "error": f"Unsupported platform: {self.platform}"
            }

        # Path handling validation
        platform_results["path_handling_validation"] = self.validate_path_handling()

        # Environment validation
        platform_results["environment_validation"] = self.validate_environment_variables()

        # Generate summary
        platform_results["summary"] = self.generate_summary(platform_results)

        # Save results
        output_file = output_dir / f"platform_checks_{self.platform}_{int(time.time())}.json"
        with open(output_file, "w") as f:
            json.dump(platform_results, f, indent=2)

        return platform_results

    def generate_summary(self, results: Dict[str, Any]) -> Dict[str, Any]:
        """Generate platform check summary."""
        summary: Dict[str, Any] = {
            "platform": results["platform"],
            "total_checks": 0,
            "passed_checks": 0,
            "failed_checks": 0,
            "issues": []
        }

        # Collect issues from all sections
        all_issues = []

        for section in ["platform_specific_checks", "path_handling_validation", "environment_validation"]:
            section_data = results.get(section, {})
            issues = section_data.get("issues", [])
            all_issues.extend(issues)

        # Count checks (simplified)
        summary["issues"] = all_issues
        summary["failed_checks"] = len(all_issues)
        summary["passed_checks"] = max(0, summary["total_checks"] - summary["failed_checks"])

        return summary

def main() -> None:
    parser = argparse.ArgumentParser(description="Platform-Specific Checks Script")
    parser.add_argument("--platform", default="auto", help="Target platform (windows, linux, auto)")
    parser.add_argument("--output", default="validation_reports", help="Output directory")

    args = parser.parse_args()

    workspace_dir = Path(__file__).parent.parent.parent
    output_dir = workspace_dir / args.output
    output_dir.mkdir(exist_ok=True)

    checker = PlatformChecker(str(workspace_dir))

    if args.platform != "auto":
        checker.platform = args.platform

    print(f"Running platform checks for {checker.platform}")

    results = checker.run_platform_checks(output_dir)

    # Print summary
    summary = results["summary"]
    print("\nPlatform Check Summary:")
    print(f"Platform: {summary['platform']}")
    print(f"Total checks: {summary['total_checks']}")
    print(f"Passed checks: {summary['passed_checks']}")
    print(f"Failed checks: {summary['failed_checks']}")

    if summary["issues"]:
        print("\nIssues found:")
        for issue in summary["issues"]:
            print(f"- {issue}")
    else:
        print("\nNo issues found!")

    print(f"\nDetailed results saved to: {output_dir}")

if __name__ == "__main__":
    main()
