#!/usr/bin/env python3
"""
Toolchain Validation Script

This script validates toolchain-specific configurations and dependencies
for the WarploomController build pipeline.

Features:
- Validates toolchain detection
- Checks toolchain-specific paths
- Verifies toolchain-specific flags
- Validates toolchain-specific dependencies
- Generates detailed validation reports

Usage:
    python toolchain_validation.py [--toolchains TOOLCHAINS] [--output OUTPUT]

Arguments:
    --toolchains: Comma-separated list of toolchains to validate
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
import re

class ToolchainValidator:
    def __init__(self, workspace_dir: str):
        self.workspace_dir = Path(workspace_dir)
        self.controller_script = self.workspace_dir / "WarploomController.py"
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

    def get_supported_toolchains(self, platform_name: str) -> List[str]:
        """Get supported toolchains for the platform."""
        toolchains = {
            "windows": ["msvc", "clang-msvc", "gcc-mingw", "clang-mingw"],
            "linux": ["gcc", "clang"]
        }
        return toolchains.get(platform_name, [])

    def validate_toolchain_detection(self, toolchain: str) -> Dict[str, Any]:
        """Validate that the toolchain is properly detected."""
        try:
            # Run a dry-run configure to see detection
            cmd = [
                sys.executable, str(self.controller_script),
                "configure", "default", "debug",
                f"--compiler={toolchain}",
                "--dry-run"
            ]

            result = subprocess.run(
                cmd,
                cwd=self.workspace_dir,
                capture_output=True,
                text=True,
                timeout=30
            )

            # Parse output for detection messages
            detection_success = False
            detection_messages = []

            for line in result.stdout.splitlines():
                if "detected" in line.lower() and toolchain in line.lower():
                    detection_success = True
                    detection_messages.append(line.strip())
                elif "toolchain" in line.lower() and "found" in line.lower():
                    detection_messages.append(line.strip())

            return {
                "toolchain": toolchain,
                "detection_success": detection_success,
                "configure_exit_code": result.returncode,
                "detection_messages": detection_messages,
                "stderr": result.stderr
            }
        except Exception as e:
            return {
                "toolchain": toolchain,
                "detection_success": False,
                "error": str(e)
            }

    def validate_toolchain_paths(self, toolchain: str) -> Dict[str, Any]:
        """Validate toolchain-specific paths."""
        paths = {}
        issues = []

        # Check common toolchain executables
        executables = {
            "msvc": ["cl.exe", "link.exe"],
            "clang-msvc": ["clang.exe", "lld-link.exe"],
            "gcc-mingw": ["gcc.exe", "g++.exe"],
            "clang-mingw": ["clang.exe"],
            "gcc": ["gcc", "g++"],
            "clang": ["clang", "clang++"]
        }

        toolchain_exes = executables.get(toolchain, [])

        for exe in toolchain_exes:
            found = shutil.which(exe) is not None
            paths[exe] = found
            if not found:
                issues.append(f"Executable {exe} not found in PATH")

        # Check for toolchain-specific directories
        if toolchain.startswith("msvc"):
            # Check for VS installation
            vs_paths = [
                "C:/Program Files/Microsoft Visual Studio",
                "C:/Program Files (x86)/Microsoft Visual Studio"
            ]
            vs_found = any(Path(p).exists() for p in vs_paths)
            paths["vs_installation"] = vs_found
            if not vs_found:
                issues.append("Visual Studio installation not found")

        elif "mingw" in toolchain:
            # Check for MinGW
            mingw_paths = [
                "C:/mingw64",
                "C:/mingw32"
            ]
            mingw_found = any(Path(p).exists() for p in mingw_paths)
            paths["mingw_installation"] = mingw_found
            if not mingw_found:
                issues.append("MinGW installation not found")

        return {
            "toolchain": toolchain,
            "paths": paths,
            "issues": issues
        }

    def validate_toolchain_flags(self, toolchain: str) -> Dict[str, Any]:
        """Validate toolchain-specific flags and configuration."""
        try:
            # Run configure and check CMake cache for flags
            cmd = [
                sys.executable, str(self.controller_script),
                "configure", "default", "debug",
                f"--compiler={toolchain}"
            ]

            result = subprocess.run(
                cmd,
                cwd=self.workspace_dir,
                capture_output=True,
                text=True,
                timeout=60
            )

            cmake_cache = self.workspace_dir / "build" / "CMakeCache.txt"
            flags_found = {}

            if cmake_cache.exists():
                with open(cmake_cache, "r") as f:
                    content = f.read()

                    # Look for common flag patterns
                    flag_patterns = {
                        "cxx_flags": r"CMAKE_CXX_FLAGS.*=(.*)",
                        "c_flags": r"CMAKE_C_FLAGS.*=(.*)",
                        "link_flags": r"CMAKE_EXE_LINKER_FLAGS.*=(.*)"
                    }

                    for flag_name, pattern in flag_patterns.items():
                        match = re.search(pattern, content, re.MULTILINE)
                        if match:
                            flags_found[flag_name] = match.group(1).strip()

            return {
                "toolchain": toolchain,
                "configure_success": result.returncode == 0,
                "flags_found": flags_found,
                "cmake_cache_exists": cmake_cache.exists(),
                "stderr": result.stderr
            }
        except Exception as e:
            return {
                "toolchain": toolchain,
                "error": str(e)
            }

    def validate_dependencies(self, toolchain: str) -> Dict[str, Any]:
        """Validate toolchain-specific dependencies."""
        deps_status = {}
        issues = []

        # Check for CMake
        cmake_found = shutil.which("cmake") is not None
        deps_status["cmake"] = cmake_found
        if not cmake_found:
            issues.append("CMake not found in PATH")

        # Check for Ninja (optional but recommended)
        ninja_found = shutil.which("ninja") is not None
        deps_status["ninja"] = ninja_found

        # Toolchain-specific dependencies
        if toolchain in ["msvc", "clang-msvc"]:
            # Check for Windows SDK
            sdk_paths = [
                "C:/Program Files (x86)/Windows Kits",
                "C:/Program Files/Windows Kits"
            ]
            sdk_found = any(Path(p).exists() for p in sdk_paths)
            deps_status["windows_sdk"] = sdk_found
            if not sdk_found:
                issues.append("Windows SDK not found")

        elif "mingw" in toolchain:
            # Check for MSYS2 or similar
            msys_paths = [
                "C:/msys64",
                "C:/msys2"
            ]
            msys_found = any(Path(p).exists() for p in msys_paths)
            deps_status["msys"] = msys_found
            if not msys_found:
                issues.append("MSYS2 not found")

        # Check for Vulkan SDK if needed
        vulkan_paths = [
            "C:/VulkanSDK",
            "/usr/local/VulkanSDK"
        ]
        vulkan_found = any(Path(p).exists() for p in vulkan_paths)
        deps_status["vulkan_sdk"] = vulkan_found

        # Check for Qt if needed
        qt_paths = [
            "C:/Qt",
            "/opt/Qt"
        ]
        qt_found = any(Path(p).exists() for p in qt_paths)
        deps_status["qt"] = qt_found

        return {
            "toolchain": toolchain,
            "dependencies": deps_status,
            "issues": issues
        }

    def run_validation(self, toolchains: List[str], output_dir: Path) -> Dict[str, Any]:
        """Run complete toolchain validation."""
        validation_results: Dict[str, Any] = {
            "platform": self.detect_platform(),
            "toolchains_tested": toolchains,
            "timestamp": time.time(),
            "detection_validation": {},
            "path_validation": {},
            "flag_validation": {},
            "dependency_validation": {},
            "summary": {}
        }

        for toolchain in toolchains:
            print(f"Validating toolchain: {toolchain}")

            # Detection validation
            detection_result = self.validate_toolchain_detection(toolchain)
            validation_results["detection_validation"][toolchain] = detection_result

            # Path validation
            path_result = self.validate_toolchain_paths(toolchain)
            validation_results["path_validation"][toolchain] = path_result

            # Flag validation
            flag_result = self.validate_toolchain_flags(toolchain)
            validation_results["flag_validation"][toolchain] = flag_result

            # Dependency validation
            dep_result = self.validate_dependencies(toolchain)
            validation_results["dependency_validation"][toolchain] = dep_result

        # Generate summary
        validation_results["summary"] = self.generate_summary(validation_results)

        # Save results
        output_file = output_dir / f"toolchain_validation_{int(time.time())}.json"
        with open(output_file, "w") as f:
            json.dump(validation_results, f, indent=2)

        return validation_results

    def generate_summary(self, results: Dict[str, Any]) -> Dict[str, Any]:
        """Generate validation summary."""
        summary: Dict[str, Any] = {
            "total_toolchains": len(results["toolchains_tested"]),
            "detection_success_rate": 0.0,
            "path_validation_success_rate": 0.0,
            "flag_validation_success_rate": 0.0,
            "dependency_success_rate": 0.0,
            "overall_success_rate": 0.0,
            "critical_issues": []
        }

        detection_successes = 0
        path_successes = 0
        flag_successes = 0
        dep_successes = 0

        for toolchain in results["toolchains_tested"]:
            # Detection
            if results["detection_validation"][toolchain].get("detection_success", False):
                detection_successes += 1

            # Paths
            path_issues = results["path_validation"][toolchain].get("issues", [])
            if not path_issues:
                path_successes += 1
            else:
                summary["critical_issues"].extend(path_issues)

            # Flags
            if results["flag_validation"][toolchain].get("configure_success", False):
                flag_successes += 1

            # Dependencies
            dep_issues = results["dependency_validation"][toolchain].get("issues", [])
            if not dep_issues:
                dep_successes += 1
            else:
                summary["critical_issues"].extend(dep_issues)

        total = len(results["toolchains_tested"])
        summary["detection_success_rate"] = detection_successes / total if total > 0 else 0
        summary["path_validation_success_rate"] = path_successes / total if total > 0 else 0
        summary["flag_validation_success_rate"] = flag_successes / total if total > 0 else 0
        summary["dependency_success_rate"] = dep_successes / total if total > 0 else 0

        # Overall success (all validations pass)
        overall_successes = sum([
            detection_successes,
            path_successes,
            flag_successes,
            dep_successes
        ]) / 4
        summary["overall_success_rate"] = overall_successes / total if total > 0 else 0

        return summary

def main() -> None:
    parser = argparse.ArgumentParser(description="Toolchain Validation Script")
    parser.add_argument("--toolchains", help="Comma-separated list of toolchains to validate")
    parser.add_argument("--output", default="validation_reports", help="Output directory")

    args = parser.parse_args()

    workspace_dir = Path(__file__).parent.parent.parent
    output_dir = workspace_dir / args.output
    output_dir.mkdir(exist_ok=True)

    validator = ToolchainValidator(str(workspace_dir))

    # Get toolchains
    if args.toolchains:
        toolchains = [t.strip() for t in args.toolchains.split(",")]
    else:
        platform_name = validator.detect_platform()
        toolchains = validator.get_supported_toolchains(platform_name)

    print(f"Running toolchain validation")
    print(f"Testing toolchains: {', '.join(toolchains)}")

    results = validator.run_validation(toolchains, output_dir)

    # Print summary
    summary = results["summary"]
    print("\nValidation Summary:")
    print(f"Detection success rate: {summary['detection_success_rate']:.1%}")
    print(f"Path validation success rate: {summary['path_validation_success_rate']:.1%}")
    print(f"Flag validation success rate: {summary['flag_validation_success_rate']:.1%}")
    print(f"Dependency success rate: {summary['dependency_success_rate']:.1%}")
    print(f"Overall success rate: {summary['overall_success_rate']:.1%}")

    if summary["critical_issues"]:
        print("\nCritical issues found:")
        for issue in summary["critical_issues"]:
            print(f"- {issue}")
    else:
        print("\nNo critical issues found!")

    print(f"\nDetailed results saved to: {output_dir}")

if __name__ == "__main__":
    main()
