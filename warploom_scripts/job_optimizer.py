#!/usr/bin/env python3
"""
Job Count Optimizer for WarploomController

This script dynamically calculates optimal parallel job counts for different compilers
and build configurations based on system resources and historical performance data.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import os
import sys
import json
from pathlib import Path
from typing import Dict, Any, Optional
import psutil

class JobOptimizer:
    """Optimizes parallel job counts for build operations."""

    def __init__(self, workspace_dir: Path) -> None:
        self.workspace_dir = workspace_dir
        self.performance_tracker: Optional[Any] = None

    def get_system_resources(self) -> Dict[str, Any]:
        """Get current system resource information."""
        resources = {
            "cpu_count": os.cpu_count() or 4,
            "memory_gb": 0,
            "available_memory_gb": 0,
            "memory_pressure": "low"
        }

        if psutil:
            memory = psutil.virtual_memory()
            resources["memory_gb"] = memory.total / (1024**3)
            resources["available_memory_gb"] = memory.available / (1024**3)

            # Determine memory pressure
            usage_percent = memory.percent
            if usage_percent > 80:
                resources["memory_pressure"] = "high"
            elif usage_percent > 60:
                resources["memory_pressure"] = "medium"
            else:
                resources["memory_pressure"] = "low"

        return resources

    def calculate_optimal_jobs(self, compiler: str, build_type: str, target: str = "windows") -> Dict[str, int]:
        """Calculate optimal job counts for compilation and linking."""
        resources = self.get_system_resources()
        base_jobs = resources["cpu_count"]

        # Compiler-specific adjustments
        if compiler == "clang-msvc":
            # clang-cl is memory-intensive and slower with high parallelism
            if build_type == "release":
                compile_jobs = min(2, base_jobs // 2)
                link_jobs = 1
            else:
                compile_jobs = min(4, base_jobs // 2)
                link_jobs = 1
        elif compiler == "msvc":
            # MSVC handles parallelism well
            compile_jobs = min(base_jobs, 8)
            link_jobs = min(base_jobs // 2, 4)
        elif compiler in ["clang", "gcc"]:
            # Standard compilers
            compile_jobs = min(base_jobs, 8)
            link_jobs = min(base_jobs // 2, 4)
        else:
            # Default
            compile_jobs = min(base_jobs // 2, 4)
            link_jobs = 1

        # Adjust for memory pressure
        if resources["memory_pressure"] == "high":
            compile_jobs = max(1, compile_jobs // 2)
            link_jobs = 1
        elif resources["memory_pressure"] == "medium":
            compile_jobs = max(1, compile_jobs - 1)
            link_jobs = min(link_jobs, 2)

        # Adjust for available memory
        memory_per_job_mb = 600  # Estimate for clang-msvc release
        max_jobs_by_memory = int(resources["available_memory_gb"] * 1024 / memory_per_job_mb)
        compile_jobs = min(compile_jobs, max_jobs_by_memory)

        # Historical performance adjustment
        if self.performance_tracker:
            historical_adjustment = self._get_historical_adjustment(compiler, build_type, target)
            compile_jobs = max(1, int(compile_jobs * historical_adjustment))

        return {
            "compile_jobs": max(1, compile_jobs),
            "link_jobs": max(1, link_jobs),
            "total_jobs": max(1, compile_jobs)
        }

    def _get_historical_adjustment(self, compiler: str, build_type: str, target: str) -> float:
        """Get adjustment factor based on historical performance."""
        if not self.performance_tracker:
            return 1.0

        # Analyze recent performance for similar builds
        recent_data = self.performance_tracker.get_recent_performance(days=30)
        similar_builds = [
            data for data in recent_data
            if (data.compiler == compiler and
                data.build_type == build_type and
                data.arch == target)
        ]

        if len(similar_builds) < 3:
            return 1.0

        # Check if high parallelism caused timeouts or failures
        high_parallel_failures = sum(
            1 for data in similar_builds
            if data.duration_seconds > 600 and not data.success
        )

        if high_parallel_failures > len(similar_builds) * 0.5:
            # High failure rate with current parallelism, reduce jobs
            return 0.5

        return 1.0

    def generate_cmake_job_pools(self, compiler: str, build_type: str, target: str = "windows") -> Dict[str, str]:
        """Generate CMake job pool configuration."""
        jobs = self.calculate_optimal_jobs(compiler, build_type, target)

        return {
            "CMAKE_JOB_POOL_COMPILE": str(jobs["compile_jobs"]),
            "CMAKE_JOB_POOL_LINK": str(jobs["link_jobs"])
        }

    def update_preset_jobs(self, preset_name: str, compiler: str, build_type: str, target: str = "windows") -> Dict[str, Any]:
        """Update a CMake preset with optimized job counts."""
        jobs = self.calculate_optimal_jobs(compiler, build_type, target)

        return {
            "name": preset_name,
            "configurePreset": preset_name.replace("-build", ""),
            "jobs": jobs["total_jobs"],
            "environment": {
                "CMAKE_JOB_POOL_COMPILE": str(jobs["compile_jobs"]),
                "CMAKE_JOB_POOL_LINK": str(jobs["link_jobs"])
            }
        }


def main() -> None:
    """Command-line interface for job optimization."""
    if len(sys.argv) < 3:
        print("Usage: python job_optimizer.py <compiler> <build_type> [target]")
        print("Example: python job_optimizer.py clang-msvc release windows")
        return

    compiler = sys.argv[1]
    build_type = sys.argv[2]
    target = sys.argv[3] if len(sys.argv) > 3 else "windows"

    workspace_dir = Path(__file__).parent.parent
    optimizer = JobOptimizer(workspace_dir)

    jobs = optimizer.calculate_optimal_jobs(compiler, build_type, target)
    cmake_pools = optimizer.generate_cmake_job_pools(compiler, build_type, target)

    print(f"Optimal job counts for {compiler} {build_type} on {target}:")
    print(f"  Compile jobs: {jobs['compile_jobs']}")
    print(f"  Link jobs: {jobs['link_jobs']}")
    print(f"  Total jobs: {jobs['total_jobs']}")
    print()
    print("CMake environment variables:")
    for key, value in cmake_pools.items():
        print(f"  {key}={value}")


if __name__ == "__main__":
    main()
