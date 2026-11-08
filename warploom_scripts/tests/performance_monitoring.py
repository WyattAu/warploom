#!/usr/bin/env python3
"""
Performance Monitoring Script

This script monitors build performance across different toolchains and configurations.
It tracks build times, resource usage, success rates, and generates performance reports.

Features:
- Track build times across toolchains
- Monitor resource usage during builds
- Track build success/failure rates
- Monitor dependency resolution times
- Track packaging and installation times

Usage:
    python performance_monitoring.py [--toolchains TOOLCHAINS] [--iterations N] [--output OUTPUT]

Arguments:
    --toolchains: Comma-separated list of toolchains to monitor
    --iterations: Number of iterations to run for each test
    --output: Output directory for reports
"""

import os
import sys
import json
import subprocess
import platform
import argparse
import time
import psutil
from pathlib import Path
from typing import Dict, List, Any, Optional
import threading
import statistics

class PerformanceMonitor:
    def __init__(self, workspace_dir: str):
        self.workspace_dir = Path(workspace_dir)
        self.controller_script = self.workspace_dir / "WarploomController.py"
        self.monitoring_active = False
        self.resource_data: List[Any] = []

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

    def start_resource_monitoring(self, interval: float = 1.0) -> None:
        """Start monitoring system resources."""
        self.monitoring_active = True
        self.resource_data.clear()

        def monitor() -> None:
            while self.monitoring_active:
                try:
                    cpu_percent = psutil.cpu_percent(interval=None)
                    memory = psutil.virtual_memory()
                    disk = psutil.disk_usage(str(self.workspace_dir))

                    self.resource_data.append({
                        "timestamp": time.time(),
                        "cpu_percent": cpu_percent,
                        "memory_percent": memory.percent,
                        "memory_used_gb": memory.used / 1024 / 1024 / 1024,
                        "disk_percent": disk.percent,
                        "disk_used_gb": disk.used / 1024 / 1024 / 1024
                    })
                    time.sleep(interval)
                except Exception as e:
                    print(f"Resource monitoring error: {e}")
                    break

        self.monitor_thread = threading.Thread(target=monitor, daemon=True)
        self.monitor_thread.start()

    def stop_resource_monitoring(self) -> None:
        """Stop monitoring system resources."""
        self.monitoring_active = False
        if hasattr(self, 'monitor_thread'):
            self.monitor_thread.join(timeout=2)

    def run_build_with_monitoring(self, toolchain: str, build_type: str) -> Dict[str, Any]:
        """Run a build with performance monitoring."""
        try:
            # Clean build directory
            build_dir = self.workspace_dir / "build"
            if build_dir.exists():
                import shutil
                shutil.rmtree(build_dir)

            # Start resource monitoring
            self.start_resource_monitoring()

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
                timeout=600  # 10 minutes
            )
            end_time = time.time()

            # Stop monitoring
            self.stop_resource_monitoring()

            # Analyze resource usage
            resource_stats = self.analyze_resource_usage()

            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": result.returncode == 0,
                "total_time": end_time - start_time,
                "returncode": result.returncode,
                "stdout_lines": len(result.stdout.splitlines()) if result.stdout else 0,
                "stderr_lines": len(result.stderr.splitlines()) if result.stderr else 0,
                "resource_usage": resource_stats,
                "timestamp": time.time()
            }
        except subprocess.TimeoutExpired:
            self.stop_resource_monitoring()
            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": False,
                "error": "Build timeout",
                "total_time": 600
            }
        except Exception as e:
            self.stop_resource_monitoring()
            return {
                "toolchain": toolchain,
                "build_type": build_type,
                "success": False,
                "error": str(e)
            }

    def analyze_resource_usage(self) -> Dict[str, Any]:
        """Analyze collected resource usage data."""
        if not self.resource_data:
            return {"error": "No resource data collected"}

        cpu_percents = [d["cpu_percent"] for d in self.resource_data]
        memory_percents = [d["memory_percent"] for d in self.resource_data]
        memory_used = [d["memory_used_gb"] for d in self.resource_data]

        return {
            "duration_monitored": len(self.resource_data),
            "cpu_avg": statistics.mean(cpu_percents) if cpu_percents else 0,
            "cpu_max": max(cpu_percents) if cpu_percents else 0,
            "memory_avg_percent": statistics.mean(memory_percents) if memory_percents else 0,
            "memory_max_percent": max(memory_percents) if memory_percents else 0,
            "memory_avg_gb": statistics.mean(memory_used) if memory_used else 0,
            "memory_max_gb": max(memory_used) if memory_used else 0
        }

    def run_performance_tests(self, toolchains: List[str], build_types: List[str], iterations: int, output_dir: Path) -> Dict[str, Any]:
        """Run comprehensive performance tests."""
        performance_results: Dict[str, Any] = {
            "platform": self.detect_platform(),
            "toolchains_tested": toolchains,
            "build_types_tested": build_types,
            "iterations_per_test": iterations,
            "timestamp": time.time(),
            "individual_runs": {},
            "aggregated_results": {},
            "performance_trends": {},
            "summary": {}
        }

        print(f"Running performance monitoring with {iterations} iterations")
        print(f"Testing toolchains: {', '.join(toolchains)}")
        print(f"Testing build types: {', '.join(build_types)}")

        # Run tests multiple times for statistical analysis
        for toolchain in toolchains:
            for build_type in build_types:
                test_key = f"{toolchain}_{build_type}"
                performance_results["individual_runs"][test_key] = []

                print(f"\nTesting {toolchain} {build_type}...")

                for i in range(iterations):
                    print(f"  Iteration {i+1}/{iterations}")
                    result = self.run_build_with_monitoring(toolchain, build_type)
                    performance_results["individual_runs"][test_key].append(result)

                    # Small delay between runs
                    time.sleep(2)

        # Aggregate results
        performance_results["aggregated_results"] = self.aggregate_results(performance_results["individual_runs"])

        # Analyze trends
        performance_results["performance_trends"] = self.analyze_trends(performance_results["aggregated_results"])

        # Generate summary
        performance_results["summary"] = self.generate_performance_summary(performance_results)

        # Save results
        output_file = output_dir / f"performance_monitoring_{int(time.time())}.json"
        with open(output_file, "w") as f:
            json.dump(performance_results, f, indent=2)

        return performance_results

    def aggregate_results(self, individual_runs: Dict[str, List[Dict[str, Any]]]) -> Dict[str, Any]:
        """Aggregate individual run results."""
        aggregated = {}

        for test_key, runs in individual_runs.items():
            successful_runs = [r for r in runs if r.get("success", False)]
            failed_runs = [r for r in runs if not r.get("success", False)]

            if successful_runs:
                build_times = [r["total_time"] for r in successful_runs]
                cpu_avgs = [r["resource_usage"]["cpu_avg"] for r in successful_runs if "resource_usage" in r]
                memory_avgs = [r["resource_usage"]["memory_avg_gb"] for r in successful_runs if "resource_usage" in r]

                aggregated[test_key] = {
                    "total_runs": len(runs),
                    "successful_runs": len(successful_runs),
                    "failed_runs": len(failed_runs),
                    "success_rate": len(successful_runs) / len(runs),
                    "build_time_stats": {
                        "mean": statistics.mean(build_times),
                        "median": statistics.median(build_times),
                        "min": min(build_times),
                        "max": max(build_times),
                        "stdev": statistics.stdev(build_times) if len(build_times) > 1 else 0
                    },
                    "resource_stats": {
                        "cpu_avg_mean": statistics.mean(cpu_avgs) if cpu_avgs else 0,
                        "cpu_avg_max": max(cpu_avgs) if cpu_avgs else 0,
                        "memory_avg_mean": statistics.mean(memory_avgs) if memory_avgs else 0,
                        "memory_avg_max": max(memory_avgs) if memory_avgs else 0
                    }
                }
            else:
                aggregated[test_key] = {
                    "total_runs": len(runs),
                    "successful_runs": 0,
                    "failed_runs": len(failed_runs),
                    "success_rate": 0.0,
                    "error": "No successful runs"
                }

        return aggregated

    def analyze_trends(self, aggregated_results: Dict[str, Any]) -> Dict[str, Any]:
        """Analyze performance trends across toolchains."""
        trends: Dict[str, Any] = {
            "fastest_toolchain": None,
            "most_reliable_toolchain": None,
            "most_efficient_toolchain": None,
            "performance_comparison": {}
        }

        if not aggregated_results:
            return trends

        # Find fastest toolchain (lowest mean build time)
        build_times: Dict[str, List[float]] = {}
        for test_key, results in aggregated_results.items():
            if "build_time_stats" in results:
                toolchain = test_key.split("_")[0]
                if toolchain not in build_times:
                    build_times[toolchain] = []
                build_times[toolchain].append(results["build_time_stats"]["mean"])

        if build_times:
            avg_times = {tc: statistics.mean(times) for tc, times in build_times.items()}
            trends["fastest_toolchain"] = min(avg_times, key=avg_times.get)  # type: ignore[arg-type]

        # Find most reliable toolchain (highest success rate)
        success_rates: Dict[str, List[float]] = {}
        for test_key, results in aggregated_results.items():
            toolchain = test_key.split("_")[0]
            if toolchain not in success_rates:
                success_rates[toolchain] = []
            success_rates[toolchain].append(results["success_rate"])

        if success_rates:
            avg_success = {tc: statistics.mean(rates) for tc, rates in success_rates.items()}
            trends["most_reliable_toolchain"] = max(avg_success, key=avg_success.get)  # type: ignore[arg-type]

        # Find most efficient toolchain (lowest resource usage)
        resource_usage: Dict[str, List[float]] = {}
        for test_key, results in aggregated_results.items():
            if "resource_stats" in results:
                toolchain = test_key.split("_")[0]
                if toolchain not in resource_usage:
                    resource_usage[toolchain] = []
                cpu_usage = results["resource_stats"]["cpu_avg_mean"]
                mem_usage = results["resource_stats"]["memory_avg_mean"]
                # Combined efficiency score (lower is better)
                efficiency_score = cpu_usage + (mem_usage * 10)  # Weight memory more
                resource_usage[toolchain].append(efficiency_score)

        if resource_usage:
            avg_efficiency = {tc: statistics.mean(scores) for tc, scores in resource_usage.items()}
            trends["most_efficient_toolchain"] = min(avg_efficiency, key=avg_efficiency.get)  # type: ignore[arg-type]

        # Performance comparison
        trends["performance_comparison"] = {
            "build_times": avg_times if 'avg_times' in locals() else {},
            "success_rates": avg_success if 'avg_success' in locals() else {},
            "efficiency_scores": avg_efficiency if 'avg_efficiency' in locals() else {}
        }

        return trends

    def generate_performance_summary(self, results: Dict[str, Any]) -> Dict[str, Any]:
        """Generate performance monitoring summary."""
        summary: Dict[str, Any] = {
            "total_tests": len(results["individual_runs"]),
            "total_runs": sum(len(runs) for runs in results["individual_runs"].values()),
            "overall_success_rate": 0.0,
            "average_build_time": 0.0,
            "performance_insights": [],
            "recommendations": []
        }

        # Calculate overall metrics
        all_runs = []
        successful_runs = 0
        total_runs = 0
        build_times = []

        for runs in results["individual_runs"].values():
            all_runs.extend(runs)
            successful_runs += sum(1 for r in runs if r.get("success", False))
            total_runs += len(runs)
            build_times.extend([r["total_time"] for r in runs if r.get("success", False)])

        summary["overall_success_rate"] = successful_runs / total_runs if total_runs > 0 else 0
        summary["average_build_time"] = statistics.mean(build_times) if build_times else 0

        # Generate insights
        trends = results["performance_trends"]

        if trends["fastest_toolchain"]:
            summary["performance_insights"].append(f"Fastest toolchain: {trends['fastest_toolchain']}")

        if trends["most_reliable_toolchain"]:
            summary["performance_insights"].append(f"Most reliable toolchain: {trends['most_reliable_toolchain']}")

        if trends["most_efficient_toolchain"]:
            summary["performance_insights"].append(f"Most efficient toolchain: {trends['most_efficient_toolchain']}")

        # Generate recommendations
        if summary["overall_success_rate"] < 0.8:
            summary["recommendations"].append("Low success rate detected - investigate build failures")

        if summary["average_build_time"] > 300:  # 5 minutes
            summary["recommendations"].append("Build times are high - consider optimization strategies")

        return summary

def main() -> None:
    parser = argparse.ArgumentParser(description="Performance Monitoring Script")
    parser.add_argument("--toolchains", help="Comma-separated list of toolchains to monitor")
    parser.add_argument("--build-types", default="debug,release", help="Comma-separated list of build types")
    parser.add_argument("--iterations", type=int, default=3, help="Number of iterations per test")
    parser.add_argument("--output", default="validation_reports", help="Output directory")

    args = parser.parse_args()

    workspace_dir = Path(__file__).parent.parent.parent
    output_dir = workspace_dir / args.output
    output_dir.mkdir(exist_ok=True)

    monitor = PerformanceMonitor(str(workspace_dir))

    # Get toolchains
    if args.toolchains:
        toolchains = [t.strip() for t in args.toolchains.split(",")]
    else:
        platform_name = monitor.detect_platform()
        toolchains = monitor.get_supported_toolchains(platform_name)

    # Get build types
    build_types = [t.strip() for t in args.build_types.split(",")]

    results = monitor.run_performance_tests(toolchains, build_types, args.iterations, output_dir)

    # Print summary
    summary = results["summary"]
    print("\nPerformance Monitoring Summary:")
    print(f"Total tests: {summary['total_tests']}")
    print(f"Total runs: {summary['total_runs']}")
    print(f"Overall success rate: {summary['overall_success_rate']:.1%}")
    print(f"Average build time: {summary['average_build_time']:.1f}s")

    trends = results["performance_trends"]
    if trends["fastest_toolchain"]:
        print(f"Fastest toolchain: {trends['fastest_toolchain']}")
    if trends["most_reliable_toolchain"]:
        print(f"Most reliable toolchain: {trends['most_reliable_toolchain']}")
    if trends["most_efficient_toolchain"]:
        print(f"Most efficient toolchain: {trends['most_efficient_toolchain']}")

    if summary["performance_insights"]:
        print("\nPerformance Insights:")
        for insight in summary["performance_insights"]:
            print(f"- {insight}")

    if summary["recommendations"]:
        print("\nRecommendations:")
        for rec in summary["recommendations"]:
            print(f"- {rec}")

    print(f"\nDetailed results saved to: {output_dir}")

if __name__ == "__main__":
    main()
