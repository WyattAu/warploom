#!/usr/bin/env python3
"""
Cross-Platform Validation Script

This script validates cross-platform consistency for the WarploomController build pipeline.
It checks that all supported toolchains produce equivalent results across platforms.

Features:
- Validates toolchain detection and configuration
- Checks build artifact consistency
- Verifies CMake configuration equivalence
- Validates dependency resolution
- Generates detailed validation reports

Usage:
    python cross_platform_validation.py [--platform PLATFORM] [--toolchains TOOLCHAINS] [--output OUTPUT]

Arguments:
    --platform: Target platform (windows, linux, auto)
    --toolchains: Comma-separated list of toolchains to test
    --output: Output directory for reports
"""

import os
import sys
import json
import subprocess
import platform
import argparse
from pathlib import Path
from typing import Dict, List, Any, Optional
import hashlib
import time

class CrossPlatformValidator:
    def __init__(self, workspace_dir: str):
        self.workspace_dir = Path(workspace_dir)
        self.controller_script = self.workspace_dir / "WarploomController.py"
        self.results: Dict[str, Any] = {}
        self.errors: List[str] = []

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

    def run_build(self, toolchain: str, build_type: str = "debug") -> Dict[str, Any]:
        """Run a build with specified toolchain and capture results."""
        try:
            cmd = [
                sys.executable, str(self.controller_script),
                "build", "standalone", "Clean Build Pipeline",
                "default", build_type,
                f"--compiler={toolchain}"
            ]

            start_time = time.time()
            result = subprocess.run(
                cmd,
                cwd=self.workspace_dir,
                capture_output=True,
                text=True,
                timeout=300  # 5 minute timeout
            )
            end_time = time.time()

            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": result.returncode == 0,
                "returncode": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr,
                "duration": end_time - start_time,
                "timestamp": time.time()
            }
        except subprocess.TimeoutExpired:
            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": False,
                "error": "Build timeout",
                "duration": 300
            }
        except Exception as e:
            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": False,
                "error": str(e)
            }

    def validate_build_artifacts(self, toolchain_results: Dict[str, Any]) -> Dict[str, Any]:
        """Validate that build artifacts are consistent across toolchains."""
        artifacts: Dict[str, List[Path]] = {}
        build_dir = self.workspace_dir / "build"

        if build_dir.exists():
            # Find executable/library files
            for ext in ["exe", "dll", "lib", "so", "a"]:
                files = list(build_dir.rglob(f"*.{ext}"))
                if files:
                    artifacts[ext] = files

        # Calculate hashes for comparison
        artifact_hashes: Dict[str, str] = {}
        for files in artifacts.values():
            for file_path in files:
                if file_path.exists():
                    with open(file_path, "rb") as f:
                        artifact_hashes[str(file_path)] = hashlib.md5(f.read()).hexdigest()

        return {
            "artifacts_found": artifacts,
            "artifact_hashes": artifact_hashes,
            "build_success": toolchain_results.get("success", False)
        }

    def validate_cmake_config(self, toolchain: str) -> Dict[str, Any]:
        """Validate CMake configuration for a toolchain."""
        try:
            # Run configure step
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
            config_valid = cmake_cache.exists()

            config_content = ""
            if config_valid:
                with open(cmake_cache, "r") as f:
                    config_content = f.read()

            return {
                "toolchain": toolchain,
                "configure_success": result.returncode == 0,
                "cmake_cache_exists": config_valid,
                "config_content_hash": hashlib.md5(config_content.encode()).hexdigest(),
                "stderr": result.stderr
            }
        except Exception as e:
            return {
                "toolchain": toolchain,
                "error": str(e)
            }

    def validate_dependencies(self, toolchain: str) -> Dict[str, Any]:
        """Validate dependency resolution for a toolchain."""
        deps_dir = self.workspace_dir / "_deps"
        deps_found = {}

        if deps_dir.exists():
            for subdir in deps_dir.iterdir():
                if subdir.is_dir() and subdir.name.endswith("-build"):
                    deps_found[subdir.name] = True

        return {
            "toolchain": toolchain,
            "deps_dir_exists": deps_dir.exists(),
            "dependencies_found": deps_found
        }

    def run_validation(self, platform_name: str, toolchains: List[str], output_dir: Path) -> Dict[str, Any]:
        """Run complete cross-platform validation."""
        validation_results: Dict[str, Any] = {
            "platform": platform_name,
            "toolchains_tested": toolchains,
            "timestamp": time.time(),
            "build_results": {},
            "artifact_validation": {},
            "cmake_validation": {},
            "dependency_validation": {},
            "consistency_checks": {}
        }

        # Run builds for each toolchain
        for toolchain in toolchains:
            print(f"Testing toolchain: {toolchain}")
            build_result = self.run_build(toolchain, "debug")
            validation_results["build_results"][toolchain] = build_result

            # Validate artifacts
            artifact_result = self.validate_build_artifacts(build_result)
            validation_results["artifact_validation"][toolchain] = artifact_result

            # Validate CMake config
            cmake_result = self.validate_cmake_config(toolchain)
            validation_results["cmake_validation"][toolchain] = cmake_result

            # Validate dependencies
            dep_result = self.validate_dependencies(toolchain)
            validation_results["dependency_validation"][toolchain] = dep_result

        # Perform consistency checks
        validation_results["consistency_checks"] = self.check_consistency(validation_results)

        # Save results
        output_file = output_dir / f"cross_platform_validation_{int(time.time())}.json"
        with open(output_file, "w") as f:
            json.dump(validation_results, f, indent=2)

        return validation_results

    def check_consistency(self, results: Dict[str, Any]) -> Dict[str, Any]:
        """Check consistency across toolchains."""
        consistency: Dict[str, Any] = {
            "all_builds_successful": True,
            "cmake_configs_consistent": True,
            "artifacts_consistent": True,
            "dependencies_consistent": True,
            "issues": []
        }

        build_results = results["build_results"]
        cmake_results = results["cmake_validation"]
        artifact_results = results["artifact_validation"]
        dep_results = results["dependency_validation"]

        # Check build success
        for toolchain, result in build_results.items():
            if not result.get("success", False):
                consistency["all_builds_successful"] = False
                consistency["issues"].append(f"Build failed for {toolchain}: {result.get('error', 'Unknown error')}")

        # Check CMake consistency
        hashes = [r.get("config_content_hash") for r in cmake_results.values() if r.get("configure_success")]
        if len(set(hashes)) > 1:
            consistency["cmake_configs_consistent"] = False
            consistency["issues"].append("CMake configurations differ across toolchains")

        # Check artifact consistency (simplified - check if same files exist)
        artifact_sets = [set(r.get("artifacts_found", {}).keys()) for r in artifact_results.values()]
        if len(set(frozenset(s) for s in artifact_sets)) > 1:
            consistency["artifacts_consistent"] = False
            consistency["issues"].append("Build artifacts differ across toolchains")

        # Check dependency consistency
        dep_sets = [set(r.get("dependencies_found", {}).keys()) for r in dep_results.values()]
        if len(set(frozenset(s) for s in dep_sets)) > 1:
            consistency["dependencies_consistent"] = False
            consistency["issues"].append("Dependencies differ across toolchains")

        return consistency

def main() -> None:
    parser = argparse.ArgumentParser(description="Cross-Platform Validation Script")
    parser.add_argument("--platform", default="auto", help="Target platform (windows, linux, auto)")
    parser.add_argument("--toolchains", help="Comma-separated list of toolchains")
    parser.add_argument("--output", default="validation_reports", help="Output directory")

    args = parser.parse_args()

    workspace_dir = Path(__file__).parent.parent.parent
    output_dir = workspace_dir / args.output
    output_dir.mkdir(exist_ok=True)

    validator = CrossPlatformValidator(str(workspace_dir))

    # Detect platform
    if args.platform == "auto":
        platform_name = validator.detect_platform()
    else:
        platform_name = args.platform

    # Get toolchains
    if args.toolchains:
        toolchains = [t.strip() for t in args.toolchains.split(",")]
    else:
        toolchains = validator.get_supported_toolchains(platform_name)

    print(f"Running cross-platform validation for {platform_name}")
    print(f"Testing toolchains: {', '.join(toolchains)}")

    results = validator.run_validation(platform_name, toolchains, output_dir)

    # Print summary
    consistency = results["consistency_checks"]
    print("\nValidation Summary:")
    print(f"All builds successful: {consistency['all_builds_successful']}")
    print(f"CMake configs consistent: {consistency['cmake_configs_consistent']}")
    print(f"Artifacts consistent: {consistency['artifacts_consistent']}")
    print(f"Dependencies consistent: {consistency['dependencies_consistent']}")

    if consistency["issues"]:
        print("\nIssues found:")
        for issue in consistency["issues"]:
            print(f"- {issue}")
    else:
        print("\nNo consistency issues found!")

    print(f"\nDetailed results saved to: {output_dir}")

if __name__ == "__main__":
    main()
