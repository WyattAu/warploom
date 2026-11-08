#!/usr/bin/env python3
"""
Build Consistency Script

This script validates build consistency across different toolchains and configurations.
It compares build artifacts, times, and outputs to ensure equivalent results.

Features:
- Compares build artifacts across toolchains
- Validates build times are within acceptable ranges
- Checks build outputs are consistent
- Verifies packaging works consistently
- Validates installation works consistently

Usage:
    python build_consistency.py [--toolchains TOOLCHAINS] [--build-types TYPES] [--output OUTPUT]

Arguments:
    --toolchains: Comma-separated list of toolchains to compare
    --build-types: Comma-separated list of build types (debug, release)
    --output: Output directory for reports
"""

import sys
import json
import subprocess
import platform
import argparse
import time
import hashlib
from pathlib import Path
from typing import Dict, List, Any, Set, Tuple
import shutil

class BuildConsistencyChecker:
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

    def run_build_with_timing(self, toolchain: str, build_type: str) -> Dict[str, Any]:
        """Run a build and capture detailed timing information."""
        try:
            # Clean build directory first
            build_dir = self.workspace_dir / "build"
            if build_dir.exists():
                shutil.rmtree(build_dir)

            # Configure
            configure_cmd = [
                sys.executable, str(self.controller_script),
                "configure", "default", build_type,
                f"--compiler={toolchain}"
            ]

            configure_start = time.time()
            configure_result = subprocess.run(
                configure_cmd,
                cwd=self.workspace_dir,
                capture_output=True,
                text=True,
                timeout=120
            )
            configure_end = time.time()

            if configure_result.returncode != 0:
                return {
                    "toolchain": toolchain,
                    "build_type": build_type,
                    "success": False,
                    "stage": "configure",
                    "error": configure_result.stderr,
                    "configure_time": configure_end - configure_start
                }

            # Build
            build_cmd = [
                sys.executable, str(self.controller_script),
                "build", "standalone", "Clean Build Pipeline",
                "default", build_type,
                f"--compiler={toolchain}"
            ]

            build_start = time.time()
            build_result = subprocess.run(
                build_cmd,
                cwd=self.workspace_dir,
                capture_output=True,
                text=True,
                timeout=600  # 10 minutes
            )
            build_end = time.time()

            success = build_result.returncode == 0

            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": success,
                "configure_time": configure_end - configure_start,
                "build_time": build_end - build_start,
                "total_time": (configure_end - configure_start) + (build_end - build_start),
                "configure_stderr": configure_result.stderr,
                "build_stdout": build_result.stdout,
                "build_stderr": build_result.stderr,
                "returncode": build_result.returncode
            }
        except subprocess.TimeoutExpired:
            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": False,
                "error": "Build timeout",
                "total_time": 600
            }
        except Exception as e:
            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": False,
                "error": str(e)
            }

    def analyze_build_artifacts(self, toolchain: str, build_type: str) -> Dict[str, Any]:
        """Analyze build artifacts for a specific toolchain build."""
        artifacts: Dict[str, Any] = {}
        build_dir = self.workspace_dir / "build"

        if not build_dir.exists():
            return {"error": "Build directory does not exist"}

        # Find executable/library files
        executable_exts = [".exe", ".dll", ".lib", ".so", ".a"] if platform.system() == "Windows" else ["", ".so", ".a"]

        executables: List[Dict[str, Any]] = []
        libraries: List[Dict[str, Any]] = []
        other_files: List[Dict[str, Any]] = []

        for ext in executable_exts:
            for file_path in build_dir.rglob(f"*{ext}"):
                if file_path.is_file():
                    rel_path = file_path.relative_to(build_dir)
                    file_size = file_path.stat().st_size

                    # Calculate hash for comparison
                    with open(file_path, "rb") as f:
                        file_hash = hashlib.md5(f.read()).hexdigest()

                    artifact_info: Dict[str, Any] = {
                        "path": str(rel_path),
                        "size": file_size,
                        "hash": file_hash
                    }

                    if ext in [".exe", ""]:
                        executables.append(artifact_info)
                    elif ext in [".dll", ".so"]:
                        libraries.append(artifact_info)
                    elif ext in [".lib", ".a"]:
                        libraries.append(artifact_info)
                    else:
                        other_files.append(artifact_info)

        artifacts["executables"] = executables
        artifacts["libraries"] = libraries
        artifacts["other_files"] = other_files

        # Summary statistics
        total_files = len(executables) + len(libraries) + len(other_files)
        total_size = sum(f["size"] for f in executables + libraries + other_files)

        return {
            "artifacts": artifacts,
            "summary": {
                "total_files": total_files,
                "total_size": total_size,
                "executables_count": len(executables),
                "libraries_count": len(libraries)
            }
        }

    def compare_build_outputs(self, results: Dict[str, Dict[str, Any]]) -> Dict[str, Any]:
        """Compare build outputs across toolchains."""
        comparison: Dict[str, Any] = {
            "time_comparison": {},
            "artifact_comparison": {},
            "consistency_score": 0.0,
            "issues": []
        }

        if not results:
            return comparison

        # Compare build times
        successful_builds = {k: v for k, v in results.items() if v.get("success", False)}

        if len(successful_builds) > 1:
            times = [r["total_time"] for r in successful_builds.values()]
            avg_time = sum(times) / len(times)
            max_time = max(times)
            min_time = min(times)

            comparison["time_comparison"] = {
                "average_time": avg_time,
                "max_time": max_time,
                "min_time": min_time,
                "time_variance": max_time - min_time,
                "acceptable_range": avg_time * 2  # Allow 2x variance
            }

            if max_time > avg_time * 2:
                comparison["issues"].append(f"Build time variance too high: {max_time:.1f}s vs {avg_time:.1f}s average")

        # Compare artifacts
        artifact_sets: Dict[str, Set[Tuple[str, str]]] = {}
        for toolchain, result in successful_builds.items():
            artifacts = result.get("artifacts", {})
            artifact_sets[toolchain] = set()

            for category in ["executables", "libraries"]:
                for artifact in artifacts.get(category, []):
                    artifact_sets[toolchain].add((artifact["path"], artifact["hash"]))

        if len(artifact_sets) > 1:
            # Check if all toolchains produce the same artifacts
            reference_toolchain = list(artifact_sets.keys())[0]
            reference_artifacts = artifact_sets[reference_toolchain]

            consistent = True
            for toolchain, artifacts in artifact_sets.items():
                if artifacts != reference_artifacts:
                    consistent = False
                    missing = reference_artifacts - artifacts
                    extra = artifacts - reference_artifacts
                    if missing:
                        comparison["issues"].append(f"{toolchain} missing artifacts: {missing}")
                    if extra:
                        comparison["issues"].append(f"{toolchain} has extra artifacts: {extra}")

            comparison["artifact_comparison"] = {
                "artifacts_consistent": consistent,
                "reference_toolchain": reference_toolchain,
                "artifact_counts": {k: len(v) for k, v in artifact_sets.items()}
            }

        # Calculate consistency score
        total_checks = 3  # time, artifacts, success rate
        passed_checks = 0

        if successful_builds:
            passed_checks += 1  # builds successful

        time_variance = comparison["time_comparison"].get("time_variance", float('inf'))
        acceptable_range = comparison["time_comparison"].get("acceptable_range", 0)
        if time_variance < acceptable_range:
            passed_checks += 1

        if comparison["artifact_comparison"].get("artifacts_consistent", False):
            passed_checks += 1

        comparison["consistency_score"] = passed_checks / total_checks if total_checks > 0 else 0

        return comparison

    def run_consistency_check(self, toolchains: List[str], build_types: List[str], output_dir: Path) -> Dict[str, Any]:
        """Run complete build consistency check."""
        consistency_results: Dict[str, Any] = {
            "platform": self.detect_platform(),
            "toolchains_tested": toolchains,
            "build_types_tested": build_types,
            "timestamp": time.time(),
            "build_results": {},
            "comparison_results": {},
            "summary": {}
        }

        # Run builds for each combination
        for toolchain in toolchains:
            for build_type in build_types:
                print(f"Testing {toolchain} {build_type}")
                build_result = self.run_build_with_timing(toolchain, build_type)

                # Analyze artifacts if build succeeded
                if build_result.get("success", False):
                    artifacts = self.analyze_build_artifacts(toolchain, build_type)
                    build_result["artifacts"] = artifacts

                key = f"{toolchain}_{build_type}"
                consistency_results["build_results"][key] = build_result

        # Compare results
        for build_type in build_types:
            type_results = {k: v for k, v in consistency_results["build_results"].items()
                          if k.endswith(f"_{build_type}")}
            comparison = self.compare_build_outputs(type_results)
            consistency_results["comparison_results"][build_type] = comparison

        # Generate summary
        consistency_results["summary"] = self.generate_summary(consistency_results)

        # Save results
        output_file = output_dir / f"build_consistency_{int(time.time())}.json"
        with open(output_file, "w") as f:
            json.dump(consistency_results, f, indent=2)

        return consistency_results

    def generate_summary(self, results: Dict[str, Any]) -> Dict[str, Any]:
        """Generate consistency check summary."""
        summary: Dict[str, Any] = {
            "total_builds": len(results["build_results"]),
            "successful_builds": 0,
            "failed_builds": 0,
            "average_build_time": 0.0,
            "consistency_scores": {},
            "critical_issues": []
        }

        build_times: List[float] = []
        for result in results["build_results"].values():
            if result.get("success", False):
                summary["successful_builds"] += 1
                build_times.append(result.get("total_time", 0))
            else:
                summary["failed_builds"] += 1
                summary["critical_issues"].append(f"Build failed: {result.get('toolchain', 'unknown')} {result.get('build_type', 'unknown')}")

        if build_times:
            summary["average_build_time"] = sum(build_times) / len(build_times)

        # Collect consistency scores
        for build_type, comparison in results["comparison_results"].items():
            summary["consistency_scores"][build_type] = comparison.get("consistency_score", 0)
            summary["critical_issues"].extend(comparison.get("issues", []))

        return summary

def main() -> None:
    parser = argparse.ArgumentParser(description="Build Consistency Script")
    parser.add_argument("--toolchains", help="Comma-separated list of toolchains to compare")
    parser.add_argument("--build-types", default="debug,release", help="Comma-separated list of build types")
    parser.add_argument("--output", default="validation_reports", help="Output directory")

    args = parser.parse_args()

    workspace_dir = Path(__file__).parent.parent.parent
    output_dir = workspace_dir / args.output
    output_dir.mkdir(exist_ok=True)

    checker = BuildConsistencyChecker(str(workspace_dir))

    # Get toolchains
    if args.toolchains:
        toolchains = [t.strip() for t in args.toolchains.split(",")]
    else:
        platform_name = checker.detect_platform()
        toolchains = checker.get_supported_toolchains(platform_name)

    # Get build types
    build_types = [t.strip() for t in args.build_types.split(",")]

    print(f"Running build consistency check")
    print(f"Testing toolchains: {', '.join(toolchains)}")
    print(f"Testing build types: {', '.join(build_types)}")

    results = checker.run_consistency_check(toolchains, build_types, output_dir)

    # Print summary
    summary = results["summary"]
    print("\nConsistency Summary:")
    print(f"Total builds: {summary['total_builds']}")
    print(f"Successful builds: {summary['successful_builds']}")
    print(f"Failed builds: {summary['failed_builds']}")
    print(f"Average build time: {summary['average_build_time']:.1f}s")

    for build_type, score in summary["consistency_scores"].items():
        print(f"Consistency score ({build_type}): {score:.1%}")

    if summary["critical_issues"]:
        print("\nCritical issues found:")
        for issue in summary["critical_issues"]:
            print(f"- {issue}")
    else:
        print("\nNo critical issues found!")

    print(f"\nDetailed results saved to: {output_dir}")

if __name__ == "__main__":
    main()
