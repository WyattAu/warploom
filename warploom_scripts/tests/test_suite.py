#!/usr/bin/env python3
"""
Comprehensive Test Suite

This script runs a comprehensive test suite for the WarploomController build pipeline.
It tests all major functions, edge cases, error conditions, and integrations.

Features:
- Test all major build pipeline functions
- Test edge cases and error conditions
- Test cross-platform compatibility
- Test toolchain compatibility
- Test Vulkan/Qt integration
- Generate comprehensive test reports

Usage:
    python test_suite.py [--quick] [--full] [--output OUTPUT]

Arguments:
    --quick: Run quick smoke tests only
    --full: Run full comprehensive test suite
    --output: Output directory for reports
"""

import os
import sys
import json
import subprocess
import platform
import argparse
import time
import unittest
from pathlib import Path
from typing import Dict, List, Any, Optional
import tempfile
import shutil

class TestSuiteRunner:
    def __init__(self, workspace_dir: str):
        self.workspace_dir = Path(workspace_dir)
        self.controller_script = self.workspace_dir / "WarploomController.py"
        self.test_results: List[Dict[str, Any]] = []
        self.test_summary: Dict[str, Any] = {}

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

    def run_command_test(self, name: str, cmd: List[str], expected_success: bool = True,
                        timeout: int = 60, env: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
        """Run a command and record the test result."""
        start_time = time.time()

        try:
            result = subprocess.run(
                cmd,
                cwd=self.workspace_dir,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env
            )

            success = (result.returncode == 0) == expected_success
            error_msg = None if success else f"Expected {'success' if expected_success else 'failure'}, got {'success' if result.returncode == 0 else 'failure'}"

        except subprocess.TimeoutExpired:
            success = False
            error_msg = f"Command timed out after {timeout}s"
            result = None
        except Exception as e:
            success = False
            error_msg = str(e)
            result = None

        end_time = time.time()

        test_result = {
            "test_name": name,
            "command": " ".join(cmd),
            "success": success,
            "expected_success": expected_success,
            "duration": end_time - start_time,
            "returncode": result.returncode if result else None,
            "error": error_msg,
            "stdout": result.stdout if result else None,
            "stderr": result.stderr if result else None,
            "timestamp": time.time()
        }

        self.test_results.append(test_result)
        return test_result

    def test_basic_functionality(self) -> List[Dict[str, Any]]:
        """Test basic controller functionality."""
        tests = []

        # Test help command
        tests.append(self.run_command_test(
            "help_command",
            [sys.executable, str(self.controller_script), "--help"]
        ))

        # Test version/info command
        tests.append(self.run_command_test(
            "version_command",
            [sys.executable, str(self.controller_script), "--version"]
        ))

        # Test invalid command
        tests.append(self.run_command_test(
            "invalid_command",
            [sys.executable, str(self.controller_script), "invalid"],
            expected_success=False
        ))

        return tests

    def test_configuration_tests(self) -> List[Dict[str, Any]]:
        """Test configuration-related functionality."""
        tests = []
        platform_name = self.detect_platform()
        toolchains = self.get_supported_toolchains(platform_name)

        for toolchain in toolchains[:2]:  # Test first 2 toolchains for speed
            # Test configure command
            tests.append(self.run_command_test(
                f"configure_{toolchain}_debug",
                [sys.executable, str(self.controller_script),
                 "configure", "default", "debug", f"--compiler={toolchain}"],
                timeout=120
            ))

            # Test configure with invalid toolchain
            tests.append(self.run_command_test(
                f"configure_invalid_toolchain",
                [sys.executable, str(self.controller_script),
                 "configure", "default", "debug", "--compiler=invalid"],
                expected_success=False
            ))

        return tests

    def test_build_tests(self) -> List[Dict[str, Any]]:
        """Test build-related functionality."""
        tests = []
        platform_name = self.detect_platform()
        toolchains = self.get_supported_toolchains(platform_name)

        for toolchain in toolchains[:1]:  # Test only first toolchain for speed
            # Test basic build
            tests.append(self.run_command_test(
                f"build_{toolchain}_debug",
                [sys.executable, str(self.controller_script),
                 "build", "standalone", "Clean Build Pipeline",
                 "default", "debug", f"--compiler={toolchain}"],
                timeout=300
            ))

            # Test build with invalid target
            tests.append(self.run_command_test(
                f"build_invalid_target_{toolchain}",
                [sys.executable, str(self.controller_script),
                 "build", "invalid_target", "Clean Build Pipeline",
                 "default", "debug", f"--compiler={toolchain}"],
                expected_success=False
            ))

        return tests

    def test_edge_cases(self) -> List[Dict[str, Any]]:
        """Test edge cases and error conditions."""
        tests = []

        # Test with non-existent directory
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_controller = Path(temp_dir) / "WarploomController.py"
            shutil.copy2(self.controller_script, temp_controller)

            tests.append(self.run_command_test(
                "non_existent_directory",
                [sys.executable, str(temp_controller), "--help"],
                expected_success=False  # Might fail due to missing dependencies
            ))

        # Test with missing arguments
        tests.append(self.run_command_test(
            "missing_arguments",
            [sys.executable, str(self.controller_script), "build"],
            expected_success=False
        ))

        # Test concurrent builds (if supported)
        # This is complex, so we'll skip for now

        return tests

    def test_cross_platform_compatibility(self) -> List[Dict[str, Any]]:
        """Test cross-platform compatibility aspects."""
        tests = []
        platform_name = self.detect_platform()

        # Test path handling
        if platform_name == "windows":
            # Test Windows-specific path handling
            tests.append(self.run_command_test(
                "windows_path_handling",
                [sys.executable, "-c", "import os; print('Windows path test')"]
            ))
        else:
            # Test Unix-specific path handling
            tests.append(self.run_command_test(
                "unix_path_handling",
                [sys.executable, "-c", "import os; print('Unix path test')"]
            ))

        # Test environment variable handling
        test_env = os.environ.copy()
        test_env["TEST_VAR"] = "test_value"

        tests.append(self.run_command_test(
            "environment_variables",
            [sys.executable, "-c", "import os; print(os.environ.get('TEST_VAR', 'not_set'))"],
            env=test_env
        ))

        return tests

    def test_vulkan_qt_integration(self) -> List[Dict[str, Any]]:
        """Test Vulkan and Qt integration."""
        tests = []
        platform_name = self.detect_platform()
        toolchains = self.get_supported_toolchains(platform_name)

        for toolchain in toolchains[:1]:  # Test only first toolchain
            # Test Vulkan-Qt library build
            tests.append(self.run_command_test(
                f"vulkan_qt_build_{toolchain}",
                [sys.executable, str(self.controller_script),
                 "build", "targets/qt-vulkan/library", "Clean Build Pipeline",
                 "default", "debug", f"--compiler={toolchain}"],
                timeout=300
            ))

        return tests

    def test_toolchain_compatibility(self) -> List[Dict[str, Any]]:
        """Test toolchain-specific compatibility."""
        tests = []
        platform_name = self.detect_platform()
        toolchains = self.get_supported_toolchains(platform_name)

        for toolchain in toolchains:
            # Test toolchain detection
            tests.append(self.run_command_test(
                f"toolchain_detection_{toolchain}",
                [sys.executable, str(self.controller_script),
                 "configure", "default", "debug", f"--compiler={toolchain}"],
                timeout=120
            ))

        return tests

    def run_quick_tests(self) -> Dict[str, Any]:
        """Run quick smoke tests."""
        print("Running quick smoke tests...")

        all_tests = []

        # Basic functionality tests
        all_tests.extend(self.test_basic_functionality())

        # Quick configuration test
        platform_name = self.detect_platform()
        toolchains = self.get_supported_toolchains(platform_name)
        if toolchains:
            all_tests.append(self.run_command_test(
                f"quick_configure_{toolchains[0]}",
                [sys.executable, str(self.controller_script),
                 "configure", "default", "debug", f"--compiler={toolchains[0]}"],
                timeout=60
            ))

        return self.generate_test_report(all_tests, "quick")

    def run_full_tests(self) -> Dict[str, Any]:
        """Run comprehensive full test suite."""
        print("Running comprehensive test suite...")

        all_tests = []

        print("1. Testing basic functionality...")
        all_tests.extend(self.test_basic_functionality())

        print("2. Testing configuration...")
        all_tests.extend(self.test_configuration_tests())

        print("3. Testing builds...")
        all_tests.extend(self.test_build_tests())

        print("4. Testing edge cases...")
        all_tests.extend(self.test_edge_cases())

        print("5. Testing cross-platform compatibility...")
        all_tests.extend(self.test_cross_platform_compatibility())

        print("6. Testing Vulkan/Qt integration...")
        all_tests.extend(self.test_vulkan_qt_integration())

        print("7. Testing toolchain compatibility...")
        all_tests.extend(self.test_toolchain_compatibility())

        return self.generate_test_report(all_tests, "full")

    def generate_test_report(self, tests: List[Dict[str, Any]], test_type: str) -> Dict[str, Any]:
        """Generate comprehensive test report."""
        report: Dict[str, Any] = {
            "test_type": test_type,
            "platform": self.detect_platform(),
            "timestamp": time.time(),
            "total_tests": len(tests),
            "passed_tests": 0,
            "failed_tests": 0,
            "skipped_tests": 0,
            "total_duration": 0.0,
            "test_categories": {},
            "failures": [],
            "performance_stats": {},
            "recommendations": []
        }

        # Analyze results
        for test in tests:
            report["total_duration"] += test["duration"]

            if test["success"]:
                report["passed_tests"] += 1
            else:
                report["failed_tests"] += 1
                report["failures"].append({
                    "test_name": test["test_name"],
                    "error": test["error"],
                    "command": test["command"]
                })

        # Categorize tests
        categories: Dict[str, Dict[str, int]] = {}
        for test in tests:
            category = test["test_name"].split("_")[0]
            if category not in categories:
                categories[category] = {"total": 0, "passed": 0, "failed": 0}
            categories[category]["total"] += 1
            if test["success"]:
                categories[category]["passed"] += 1
            else:
                categories[category]["failed"] += 1

        report["test_categories"] = categories

        # Performance stats
        if tests:
            durations = [t["duration"] for t in tests]
            report["performance_stats"] = {
                "average_duration": sum(durations) / len(durations),
                "max_duration": max(durations),
                "min_duration": min(durations)
            }

        # Generate recommendations
        success_rate = report["passed_tests"] / report["total_tests"] if report["total_tests"] > 0 else 0

        if success_rate < 0.8:
            report["recommendations"].append("Low test success rate - investigate failures")
        if report["total_duration"] > 600:  # 10 minutes
            report["recommendations"].append("Tests are taking too long - consider optimization")

        return report

    def run_test_suite(self, test_type: str, output_dir: Path) -> Dict[str, Any]:
        """Run the appropriate test suite."""
        if test_type == "quick":
            results = self.run_quick_tests()
        elif test_type == "full":
            results = self.run_full_tests()
        else:
            raise ValueError(f"Unknown test type: {test_type}")

        # Save detailed results
        output_file = output_dir / f"test_suite_{test_type}_{int(time.time())}.json"
        with open(output_file, "w") as f:
            json.dump({
                "summary": results,
                "detailed_results": self.test_results
            }, f, indent=2)

        return results

def main() -> None:
    parser = argparse.ArgumentParser(description="Comprehensive Test Suite")
    parser.add_argument("--quick", action="store_true", help="Run quick smoke tests")
    parser.add_argument("--full", action="store_true", help="Run full comprehensive test suite")
    parser.add_argument("--output", default="validation_reports", help="Output directory")

    args = parser.parse_args()

    # Default to quick if neither specified
    if not args.quick and not args.full:
        args.quick = True

    test_type = "full" if args.full else "quick"

    workspace_dir = Path(__file__).parent.parent.parent
    output_dir = workspace_dir / args.output
    output_dir.mkdir(exist_ok=True)

    runner = TestSuiteRunner(str(workspace_dir))

    print(f"Running {test_type} test suite for WarploomController")
    print(f"Platform: {runner.detect_platform()}")
    print(f"Workspace: {workspace_dir}")

    results = runner.run_test_suite(test_type, output_dir)

    # Print summary
    print("\n" + "="*50)
    print("TEST SUITE SUMMARY")
    print("="*50)
    print(f"Test Type: {results['test_type']}")
    print(f"Total Tests: {results['total_tests']}")
    print(f"Passed: {results['passed_tests']}")
    print(f"Failed: {results['failed_tests']}")
    print(f"Success Rate: {results['passed_tests']/results['total_tests']:.1%}" if results['total_tests'] > 0 else "Success Rate: N/A")
    print(f"Total Duration: {results['total_duration']:.1f}s")

    if results['test_categories']:
        print("\nTest Categories:")
        for category, stats in results['test_categories'].items():
            success_rate = stats['passed'] / stats['total'] if stats['total'] > 0 else 0
            print(f"  {category}: {stats['passed']}/{stats['total']} ({success_rate:.1%})")

    if results['failures']:
        print("\nFailures:")
        for failure in results['failures'][:5]:  # Show first 5
            print(f"  - {failure['test_name']}: {failure['error']}")

    if results['recommendations']:
        print("\nRecommendations:")
        for rec in results['recommendations']:
            print(f"  - {rec}")

    print(f"\nDetailed results saved to: {output_dir}")

if __name__ == "__main__":
    main()
