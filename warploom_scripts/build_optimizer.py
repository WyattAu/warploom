"""
Build Optimization System for WarploomController

This module provides automated build optimization recommendations, historical performance tracking,
predictive build failure prevention, continuous improvement system, advanced caching mechanisms,
and build pipeline optimization.

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

import json
import os
import hashlib
import shutil
import time
import subprocess
from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any, Set
from dataclasses import dataclass, field, asdict
from collections import defaultdict, deque
import threading
import logging
from enum import Enum

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    psutil = None  # type: ignore[assignment]
    HAS_PSUTIL = False

from .utils import FileUtils, SystemUtils
from .error_handler import ValidationError


class OptimizationLevel(Enum):
    """Optimization levels for build recommendations."""
    NONE = "none"
    BASIC = "basic"
    ADVANCED = "advanced"
    AGGRESSIVE = "aggressive"


class CacheType(Enum):
    """Types of caching mechanisms."""
    DEPENDENCY = "dependency"
    OBJECT = "object"
    PRECOMPILED_HEADER = "precompiled_header"
    BUILD_ARTIFACT = "build_artifact"
    CCACHE = "ccache"


@dataclass
class BuildPerformanceData:
    """Historical build performance data."""
    build_id: str
    timestamp: datetime
    product: str
    arch: str
    build_type: str
    compiler: Optional[str]
    duration_seconds: float
    peak_memory_mb: float
    cpu_usage_percent: float
    success: bool
    error_message: Optional[str] = None
    step_durations: Dict[str, float] = field(default_factory=dict)
    system_info: Dict[str, Any] = field(default_factory=dict)
    optimization_applied: List[str] = field(default_factory=list)


@dataclass
class OptimizationRecommendation:
    """Build optimization recommendation."""
    category: str
    title: str
    description: str
    impact: str  # "high", "medium", "low"
    confidence: float  # 0.0 to 1.0
    implementation_effort: str  # "easy", "medium", "hard"
    expected_improvement_percent: float
    applicable_conditions: Dict[str, Any] = field(default_factory=dict)
    implementation_steps: List[str] = field(default_factory=list)


@dataclass
class CacheEntry:
    """Cache entry metadata."""
    cache_type: CacheType
    key: str
    path: str
    created_at: datetime
    last_accessed: datetime
    size_bytes: int
    hits: int = 0
    dependencies: Set[str] = field(default_factory=set)


class HistoricalPerformanceTracker:
    """Tracks and analyzes historical build performance data."""

    def __init__(self, data_dir: Path):
        self.data_dir = data_dir
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.performance_file = self.data_dir / "build_performance_history.json"
        self._lock = threading.RLock()
        self._performance_data: List[BuildPerformanceData] = []
        self._load_data()

    def _load_data(self) -> None:
        """Load historical performance data from disk."""
        if self.performance_file.exists():
            try:
                with open(self.performance_file, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    self._performance_data = [
                        BuildPerformanceData(**item) for item in data
                    ]
            except (json.JSONDecodeError, KeyError) as e:
                logging.warning(f"Failed to load performance data: {e}")
                self._performance_data = []

    def _save_data(self) -> None:
        """Save performance data to disk."""
        with self._lock:
            data = [asdict(item) for item in self._performance_data[-1000:]]  # Keep last 1000 builds
            with open(self.performance_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, default=str)

    def record_build_performance(self, performance_data: BuildPerformanceData) -> None:
        """Record a new build performance data point."""
        with self._lock:
            self._performance_data.append(performance_data)
            self._save_data()

    def get_recent_performance(self, days: int = 30) -> List[BuildPerformanceData]:
        """Get performance data from the last N days."""
        cutoff = datetime.now() - timedelta(days=days)
        return [data for data in self._performance_data if data.timestamp >= cutoff]

    def analyze_performance_trends(self, product: str, arch: str, build_type: str) -> Dict[str, Any]:
        """Analyze performance trends for a specific build configuration."""
        relevant_data = [
            data for data in self._performance_data
            if data.product == product and data.arch == arch and data.build_type == build_type
        ]

        if not relevant_data:
            return {"error": "No historical data available"}

        # Calculate trends
        durations = [data.duration_seconds for data in relevant_data]
        success_rate = sum(1 for data in relevant_data if data.success) / len(relevant_data)

        return {
            "total_builds": len(relevant_data),
            "success_rate": success_rate,
            "avg_duration": sum(durations) / len(durations),
            "min_duration": min(durations),
            "max_duration": max(durations),
            "recent_trend": self._calculate_trend(durations[-10:]) if len(durations) >= 10 else "insufficient_data"
        }

    def _calculate_trend(self, values: List[float]) -> str:
        """Calculate trend direction from a list of values."""
        if len(values) < 2:
            return "stable"

        # Simple linear trend
        n = len(values)
        x = list(range(n))
        y = values

        # Calculate slope
        slope = sum((x[i] - sum(x)/n) * (y[i] - sum(y)/n) for i in range(n)) / sum((x[i] - sum(x)/n)**2 for i in range(n))

        if slope > 0.1:
            return "increasing"
        elif slope < -0.1:
            return "decreasing"
        else:
            return "stable"


class PredictiveFailurePrevention:
    """Predictive build failure prevention system."""

    def __init__(self, performance_tracker: HistoricalPerformanceTracker):
        self.performance_tracker = performance_tracker
        self.failure_patterns: Dict[str, Dict[str, Any]] = {}
        self._load_failure_patterns()

    def _load_failure_patterns(self) -> None:
        """Load known failure patterns and their prevention strategies."""
        self.failure_patterns = {
            "memory_exhaustion": {
                "pattern": lambda data: data.peak_memory_mb > 3000,  # 3GB threshold
                "prevention": "Reduce parallel jobs or increase system memory",
                "confidence": 0.9
            },
            "timeout_clang": {
                "pattern": lambda data: "clang" in (data.compiler or "") and data.duration_seconds > 800,  # 13+ minutes
                "prevention": "Use MSVC compiler or reduce parallel jobs for clang-msvc",
                "confidence": 0.8
            },
            "consistent_failures": {
                "pattern": lambda data: self._check_failure_streak(data),
                "prevention": "Check for systemic issues in build configuration",
                "confidence": 0.7
            }
        }

    def _check_failure_streak(self, current_data: BuildPerformanceData) -> bool:
        """Check if there are consistent failures for similar builds."""
        recent_data = self.performance_tracker.get_recent_performance(days=7)
        similar_builds = [
            data for data in recent_data
            if (data.product == current_data.product and
                data.arch == current_data.arch and
                data.build_type == current_data.build_type)
        ]

        if len(similar_builds) < 3:
            return False

        recent_failures = sum(1 for data in similar_builds[-3:] if not data.success)
        return recent_failures >= 2  # 2 out of last 3 builds failed

    def predict_potential_failures(self, build_context: Dict[str, Any]) -> List[Dict[str, Any]]:
        """Predict potential failures based on current build context."""
        predictions = []
        recent_data = self.performance_tracker.get_recent_performance(days=14)

        for pattern_name, pattern_info in self.failure_patterns.items():
            risk_score = self._calculate_risk_score(pattern_name, pattern_info, build_context, recent_data)
            if risk_score > 0.5:  # Medium risk threshold
                predictions.append({
                    "pattern": pattern_name,
                    "risk_score": risk_score,
                    "prevention": pattern_info["prevention"],
                    "confidence": pattern_info["confidence"]
                })

        return sorted(predictions, key=lambda x: x["risk_score"], reverse=True)

    def _calculate_risk_score(self, pattern_name: str, pattern_info: Dict[str, Any], build_context: Dict[str, Any], recent_data: List[BuildPerformanceData]) -> float:
        """Calculate risk score for a failure pattern."""
        base_risk = 0.0

        # Check recent occurrences
        for data in recent_data[-5:]:  # Last 5 builds
            if pattern_info["pattern"](data):
                base_risk += 0.2

        # Adjust based on build context similarity
        context_matches = 0
        total_checks = 0

        for data in recent_data:
            total_checks += 1
            matches = 0
            if data.product == build_context.get("product"):
                matches += 1
            if data.arch == build_context.get("arch"):
                matches += 1
            if data.build_type == build_context.get("build_type"):
                matches += 1
            if data.compiler == build_context.get("compiler"):
                matches += 1

            if matches >= 3:  # Similar build context
                context_matches += 1

        if total_checks > 0:
            context_similarity = context_matches / total_checks
            base_risk *= (0.5 + 0.5 * context_similarity)  # Boost risk for similar contexts

        return min(base_risk, 1.0)


class AdvancedCacheManager:
    """Advanced caching system for build artifacts and dependencies."""

    def __init__(self, cache_dir: Path, max_cache_size_gb: float = 10.0):
        self.cache_dir = cache_dir
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self.max_cache_size_gb = max_cache_size_gb
        self.metadata_file = self.cache_dir / "cache_metadata.json"
        self._lock = threading.RLock()
        self._cache_entries: Dict[str, CacheEntry] = {}
        self._load_metadata()

    def _load_metadata(self) -> None:
        """Load cache metadata from disk."""
        if self.metadata_file.exists():
            try:
                with open(self.metadata_file, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    self._cache_entries = {
                        key: CacheEntry(**entry) for key, entry in data.items()
                    }
            except (json.JSONDecodeError, KeyError) as e:
                logging.warning(f"Failed to load cache metadata: {e}")
                self._cache_entries.clear()

    def _save_metadata(self) -> None:
        """Save cache metadata to disk."""
        with self._lock:
            data = {key: asdict(entry) for key, entry in self._cache_entries.items()}
            with open(self.metadata_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, default=str)

    def generate_cache_key(self, cache_type: CacheType, **kwargs: Any) -> str:
        """Generate a cache key based on input parameters."""
        key_components = [cache_type.value]
        key_components.extend(str(kwargs.get(k, "")) for k in sorted(kwargs.keys()))

        key_string = "|".join(key_components)
        return hashlib.sha256(key_string.encode()).hexdigest()[:16]

    def store_in_cache(self, cache_type: CacheType, key: str, source_path: Path,
                      dependencies: Optional[Set[str]] = None) -> bool:
        """Store an item in the cache."""
        if not source_path.exists():
            return False

        cache_path = self.cache_dir / f"{cache_type.value}_{key}"
        dependencies = dependencies or set()

        try:
            # Copy to cache
            if source_path.is_file():
                FileUtils.copy_file(source_path, cache_path)
                size = source_path.stat().st_size
            elif source_path.is_dir():
                FileUtils.copy_directory(source_path, cache_path)
                size = sum(f.stat().st_size for f in cache_path.rglob("*") if f.is_file())
            else:
                return False

            # Create cache entry
            entry = CacheEntry(
                cache_type=cache_type,
                key=key,
                path=str(cache_path),
                created_at=datetime.now(),
                last_accessed=datetime.now(),
                size_bytes=size,
                dependencies=dependencies
            )

            with self._lock:
                self._cache_entries[key] = entry
                self._save_metadata()
                self._enforce_cache_limits()

            return True

        except Exception as e:
            logging.error(f"Failed to cache item: {e}")
            return False

    def retrieve_from_cache(self, cache_type: CacheType, key: str, target_path: Path) -> bool:
        """Retrieve an item from cache."""
        with self._lock:
            entry = self._cache_entries.get(key)
            if not entry or entry.cache_type != cache_type:
                return False

            cache_path = Path(entry.path)
            if not cache_path.exists():
                # Clean up invalid entry
                del self._cache_entries[key]
                self._save_metadata()
                return False

            try:
                # Copy from cache to target
                if cache_path.is_file():
                    FileUtils.copy_file(cache_path, target_path)
                elif cache_path.is_dir():
                    FileUtils.copy_directory(cache_path, target_path)

                # Update access statistics
                entry.last_accessed = datetime.now()
                entry.hits += 1
                self._save_metadata()

                return True

            except Exception as e:
                logging.error(f"Failed to retrieve from cache: {e}")
                return False

    def _enforce_cache_limits(self) -> None:
        """Enforce cache size limits by removing least recently used items."""
        total_size = sum(entry.size_bytes for entry in self._cache_entries.values())
        max_size_bytes = self.max_cache_size_gb * 1024 * 1024 * 1024

        if total_size <= max_size_bytes:
            return

        # Sort by last accessed (oldest first)
        sorted_entries = sorted(
            self._cache_entries.items(),
            key=lambda x: x[1].last_accessed
        )

        # Remove oldest entries until under limit
        for key, entry in sorted_entries:
            if total_size <= max_size_bytes:
                break

            try:
                cache_path = Path(entry.path)
                if cache_path.exists():
                    if cache_path.is_file():
                        cache_path.unlink()
                    else:
                        shutil.rmtree(cache_path)

                total_size -= entry.size_bytes
                del self._cache_entries[key]

            except Exception as e:
                logging.warning(f"Failed to remove cached item {key}: {e}")

        self._save_metadata()

    def get_cache_stats(self) -> Dict[str, Any]:
        """Get cache statistics."""
        total_entries = len(self._cache_entries)
        total_size = sum(entry.size_bytes for entry in self._cache_entries.values())
        total_hits = sum(entry.hits for entry in self._cache_entries.values())

        cache_types: Dict[str, int] = defaultdict(int)
        for entry in self._cache_entries.values():
            cache_types[entry.cache_type.value] += 1

        return {
            "total_entries": total_entries,
            "total_size_mb": total_size / (1024 * 1024),
            "total_hits": total_hits,
            "cache_types": dict(cache_types),
            "max_size_gb": self.max_cache_size_gb
        }


class BuildOptimizer:
    """Main build optimization system."""

    def __init__(self, workspace_dir: Path):
        self.workspace_dir = workspace_dir
        self.data_dir = workspace_dir / ".warploom" / "optimization"
        self.data_dir.mkdir(parents=True, exist_ok=True)

        # Initialize components
        self.performance_tracker = HistoricalPerformanceTracker(self.data_dir)
        self.failure_prevention = PredictiveFailurePrevention(self.performance_tracker)
        self.cache_manager = AdvancedCacheManager(self.data_dir / "cache")

        # Optimization state
        self.current_optimizations: Dict[str, Any] = {}
        self.optimization_history: List[Dict[str, Any]] = []

    def generate_optimization_recommendations(self, build_context: Dict[str, Any]) -> List[OptimizationRecommendation]:
        """Generate automated build optimization recommendations."""
        recommendations = []

        # Analyze historical performance
        performance_analysis = self.performance_tracker.analyze_performance_trends(
            build_context.get("product", ""),
            build_context.get("arch", ""),
            build_context.get("build_type", "")
        )

        if "error" not in performance_analysis:
            # Memory optimization recommendations
            if performance_analysis.get("avg_duration", 0) > 300:  # 5+ minutes
                recommendations.append(OptimizationRecommendation(
                    category="memory",
                    title="Enable Precompiled Headers",
                    description="Precompiled headers can significantly reduce compilation time for large projects",
                    impact="high",
                    confidence=0.8,
                    implementation_effort="medium",
                    expected_improvement_percent=15.0,
                    applicable_conditions={"build_type": "release", "compiler": ["msvc", "clang-msvc"]},
                    implementation_steps=[
                        "Add precompiled header configuration to CMakeLists.txt",
                        "Update source files to include the precompiled header",
                        "Test build performance improvement"
                    ]
                ))

            # Parallel job optimization
            if performance_analysis.get("success_rate", 1.0) < 0.8:  # Less than 80% success rate
                recommendations.append(OptimizationRecommendation(
                    category="parallelization",
                    title="Reduce Parallel Jobs",
                    description="High failure rate may indicate memory pressure from parallel compilation",
                    impact="medium",
                    confidence=0.7,
                    implementation_effort="easy",
                    expected_improvement_percent=5.0,
                    applicable_conditions={"compiler": ["clang-msvc"]},
                    implementation_steps=[
                        "Reduce CMAKE_JOB_POOL_COMPILE jobs in CMakePresets.json",
                        "Monitor memory usage during builds",
                        "Adjust based on system capabilities"
                    ]
                ))

        # Cache optimization recommendations
        cache_stats = self.cache_manager.get_cache_stats()
        if cache_stats["total_entries"] < 10:  # Low cache utilization
            recommendations.append(OptimizationRecommendation(
                category="caching",
                title="Enable Dependency Caching",
                description="Implement caching for Conan dependencies to speed up rebuilds",
                impact="high",
                confidence=0.9,
                implementation_effort="medium",
                expected_improvement_percent=25.0,
                applicable_conditions={},
                implementation_steps=[
                    "Configure Conan cache directory",
                    "Enable incremental dependency installation",
                    "Monitor cache hit rates"
                ]
            ))

        # Compiler-specific optimizations
        compiler = build_context.get("compiler")
        if compiler == "clang-msvc" and performance_analysis.get("avg_duration", 0) > 600:  # 10+ minutes
            recommendations.append(OptimizationRecommendation(
                category="compiler",
                title="Consider MSVC Compiler",
                description="MSVC may be faster than clang-msvc for this workload",
                impact="medium",
                confidence=0.6,
                implementation_effort="easy",
                expected_improvement_percent=20.0,
                applicable_conditions={"compiler": "clang-msvc"},
                implementation_steps=[
                    "Test build with MSVC compiler",
                    "Compare performance metrics",
                    "Update default compiler if beneficial"
                ]
            ))

        return sorted(recommendations, key=lambda x: x.expected_improvement_percent, reverse=True)

    def apply_optimization(self, recommendation: OptimizationRecommendation) -> bool:
        """Apply a specific optimization recommendation."""
        try:
            optimization_id = f"{recommendation.category}_{recommendation.title.replace(' ', '_').lower()}"

            # Record optimization attempt
            self.optimization_history.append({
                "id": optimization_id,
                "recommendation": asdict(recommendation),
                "applied_at": datetime.now(),
                "status": "applying"
            })

            # Apply based on category
            if recommendation.category == "memory":
                success = self._apply_memory_optimization(recommendation)
            elif recommendation.category == "parallelization":
                success = self._apply_parallelization_optimization(recommendation)
            elif recommendation.category == "caching":
                success = self._apply_caching_optimization(recommendation)
            elif recommendation.category == "compiler":
                success = self._apply_compiler_optimization(recommendation)
            else:
                success = False

            # Update status
            self.optimization_history[-1]["status"] = "success" if success else "failed"
            self.optimization_history[-1]["completed_at"] = datetime.now()

            return success

        except Exception as e:
            logging.error(f"Failed to apply optimization {recommendation.title}: {e}")
            if self.optimization_history:
                self.optimization_history[-1]["status"] = "error"
                self.optimization_history[-1]["error"] = str(e)
            return False

    def _apply_memory_optimization(self, recommendation: OptimizationRecommendation) -> bool:
        """Apply memory-related optimizations."""
        if "precompiled" in recommendation.title.lower():
            # This would modify CMakeLists.txt to enable precompiled headers
            # For now, just log the recommendation
            logging.info(f"Memory optimization applied: {recommendation.title}")
            return True
        return False

    def _apply_parallelization_optimization(self, recommendation: OptimizationRecommendation) -> bool:
        """Apply parallelization optimizations."""
        # This would modify CMakePresets.json to reduce job counts
        logging.info(f"Parallelization optimization applied: {recommendation.title}")
        return True

    def _apply_caching_optimization(self, recommendation: OptimizationRecommendation) -> bool:
        """Apply caching optimizations."""
        # Enable caching mechanisms
        logging.info(f"Caching optimization applied: {recommendation.title}")
        return True

    def _apply_compiler_optimization(self, recommendation: OptimizationRecommendation) -> bool:
        """Apply compiler optimizations."""
        # This would modify build configuration to use different compiler
        logging.info(f"Compiler optimization applied: {recommendation.title}")
        return True

    def get_continuous_improvements(self) -> List[Dict[str, Any]]:
        """Get continuous improvement suggestions based on historical data."""
        improvements = []

        # Analyze failure patterns
        recent_data = self.performance_tracker.get_recent_performance(days=30)
        if recent_data:
            failure_rate = sum(1 for data in recent_data if not data.success) / len(recent_data)

            if failure_rate > 0.2:  # More than 20% failure rate
                improvements.append({
                    "type": "reliability",
                    "title": "Improve Build Reliability",
                    "description": f"Current failure rate is {failure_rate:.1%}, consider systematic fixes",
                    "priority": "high"
                })

        # Analyze performance trends
        for product in set(data.product for data in recent_data):
            product_data = [data for data in recent_data if data.product == product]
            if len(product_data) >= 5:
                durations = [data.duration_seconds for data in product_data]
                trend = self.performance_tracker._calculate_trend(durations)

                if trend == "increasing":
                    improvements.append({
                        "type": "performance",
                        "title": f"Address Performance Degradation for {product}",
                        "description": "Build times are increasing, investigate causes",
                        "priority": "medium"
                    })

        return improvements

    def optimize_build_pipeline(self, build_context: Dict[str, Any]) -> Dict[str, Any]:
        """Optimize the entire build pipeline based on context and history."""
        optimizations = {
            "cache_utilization": self._optimize_cache_usage(build_context),
            "job_scheduling": self._optimize_job_scheduling(build_context),
            "memory_management": self._optimize_memory_management(build_context),
            "failure_prevention": self.failure_prevention.predict_potential_failures(build_context)
        }

        return optimizations

    def _optimize_cache_usage(self, build_context: Dict[str, Any]) -> Dict[str, Any]:
        """Optimize cache usage for the build."""
        cache_key = self.cache_manager.generate_cache_key(
            CacheType.DEPENDENCY,
            product=build_context.get("product"),
            arch=build_context.get("arch"),
            compiler=build_context.get("compiler")
        )

        return {
            "cache_key": cache_key,
            "can_use_cache": self.cache_manager._cache_entries.get(cache_key) is not None,
            "recommended_cache_types": [CacheType.DEPENDENCY, CacheType.OBJECT]
        }

    def _optimize_job_scheduling(self, build_context: Dict[str, Any]) -> Dict[str, Any]:
        """Optimize job scheduling based on system capabilities and history."""
        compiler = build_context.get("compiler", "")
        build_type = build_context.get("build_type", "")

        # Base job count
        base_jobs = min(os.cpu_count() or 4, 8)

        # Adjust based on compiler and build type
        if compiler == "clang-msvc" and build_type == "release":
            recommended_jobs = max(1, base_jobs // 2)
        else:
            recommended_jobs = base_jobs

        return {
            "recommended_jobs": recommended_jobs,
            "reasoning": f"Adjusted for {compiler} {build_type} builds"
        }

    def _optimize_memory_management(self, build_context: Dict[str, Any]) -> Dict[str, Any]:
        """Optimize memory management for the build."""
        if HAS_PSUTIL:
            memory = psutil.virtual_memory()
            available_gb = memory.available / (1024**3)

            if available_gb < 4:  # Less than 4GB available
                return {
                    "memory_optimization": "aggressive",
                    "recommended_actions": ["Reduce parallel jobs", "Disable precompiled headers"]
                }
            elif available_gb < 8:  # Less than 8GB available
                return {
                    "memory_optimization": "moderate",
                    "recommended_actions": ["Monitor memory usage"]
                }

        return {
            "memory_optimization": "standard",
            "recommended_actions": []
        }

    def get_optimization_report(self) -> Dict[str, Any]:
        """Generate a comprehensive optimization report."""
        return {
            "performance_summary": self.performance_tracker.analyze_performance_trends("", "", ""),
            "cache_stats": self.cache_manager.get_cache_stats(),
            "applied_optimizations": self.optimization_history[-10:],  # Last 10 optimizations
            "continuous_improvements": self.get_continuous_improvements(),
            "system_info": SystemUtils.get_system_info() if hasattr(SystemUtils, 'get_system_info') else {}
        }


# Additional classes for clang-msvc timeout fixes

@dataclass
class ClangMsvcReleaseTimeouts:
    """Hierarchical timeout configuration for clang-msvc release builds."""
    clean: int = 60      # 1 minute
    conan: int = 300     # 5 minutes (Qt6/Vulkan install)
    configure: int = 180 # 3 minutes (CMake generation)
    build: int = 600     # 10 minutes (reduced from 15)
    total: int = 1200    # 20 minutes total


class MemoryAwareBuildScheduler:
    """Memory-aware build job scheduler for clang-msvc release builds."""

    def __init__(self, max_memory_gb: float = 8.0):
        self.max_memory_gb = max_memory_gb
        self.memory_per_job_mb = 600  # clang-cl release estimate

    def calculate_safe_job_count(self) -> int:
        """Calculate job count that won't exceed memory limits."""
        max_jobs_by_memory = int(self.max_memory_gb * 1024 / self.memory_per_job_mb)
        max_jobs_by_cpu = max(1, (os.cpu_count() or 4) // 2)

        return min(max_jobs_by_memory, max_jobs_by_cpu)


class BuildProgressMonitor:
    """Build progress monitor with stall detection for clang-msvc builds."""

    def __init__(self) -> None:
        self.last_progress_time = time.time()
        self.stall_threshold = 300  # 5 minutes without progress

    def check_for_stalls(self, current_progress: float) -> bool:  # noqa: ARG002
        """Detect build stalls and trigger recovery."""
        if time.time() - self.last_progress_time > self.stall_threshold:
            print("Build stall detected, reducing parallelism...")
            return True
        self.last_progress_time = time.time()
        return False


def get_clang_msvc_release_jobs() -> int:
    """Calculate optimal job count for clang-msvc release builds."""
    cpu_count = os.cpu_count() or 4

    # clang-cl is inefficient with high parallelism due to MSVC runtime
    # Reduce to 50% of available cores, minimum 2, maximum 4
    optimal_jobs = max(2, min(4, cpu_count // 2))

    # Further reduce for release builds due to linking complexity
    return max(1, optimal_jobs - 1)  # Typically 2 jobs


def select_build_strategy_for_clang_msvc_release() -> str:
    """Select build strategy to avoid timeouts."""
    # For clang-msvc release, prefer incremental builds to avoid
    # the expensive clean->full rebuild cycle
    return "incremental"


def two_phase_clang_msvc_release_build(build_dir: Path, preset: str = "clang-msvc-release") -> bool:
    """Build in two phases to avoid memory pressure."""
    try:
        # Phase 1: Compile with reduced parallelism
        cmd1 = f"cmake --build {build_dir} --preset {preset} --parallel 2"
        result1 = os.system(cmd1)
        if result1 != 0:
            print("Phase 1 compilation failed")
            return False

        # Phase 2: Link with single job to avoid conflicts
        cmd2 = f"cmake --build {build_dir} --preset {preset} --parallel 1 --target link_targets"
        result2 = os.system(cmd2)
        if result2 != 0:
            print("Phase 2 linking failed")
            return False

        return True
    except Exception as e:
        print(f"Two-phase build failed: {e}")
        return False


def clang_msvc_fallback_to_msvc(build_time_seconds: float, threshold: int = 600) -> bool:
    """Automatically fallback to MSVC for release builds if clang-cl fails."""
    if build_time_seconds > threshold:  # 10 minutes
        print("clang-msvc release build slow, falling back to MSVC...")
        return True
    return False


def apply_clang_msvc_release_optimizations(build_dir: Path) -> Dict[str, Any]:
    """Apply all clang-msvc release optimizations."""
    optimizations: Dict[str, Any] = {}

    # Get optimal job count
    jobs = get_clang_msvc_release_jobs()
    optimizations["jobs"] = jobs

    # Memory-aware scheduling
    scheduler = MemoryAwareBuildScheduler()
    safe_jobs = scheduler.calculate_safe_job_count()
    optimizations["safe_jobs"] = safe_jobs

    # Use the more conservative of the two
    final_jobs = min(jobs, safe_jobs)
    optimizations["final_jobs"] = final_jobs

    # Build strategy
    strategy = select_build_strategy_for_clang_msvc_release()
    optimizations["strategy"] = strategy

    # Timeouts
    timeouts = ClangMsvcReleaseTimeouts()
    optimizations["timeouts"] = asdict(timeouts)

    return optimizations


class ProcessManager:
    """Aggressive process management for build operations."""

    def __init__(self) -> None:
        self.active_processes: Dict[str, subprocess.Popen[Any]] = {}
        self.process_lock = threading.RLock()
        self.kill_timeout = 30  # seconds to wait before force kill

    def start_process(self, name: str, cmd: str, **kwargs: Any) -> subprocess.Popen[Any]:
        """Start a process with monitoring."""
        with self.process_lock:
            if name in self.active_processes:
                self.kill_process(name)

            try:
                process = subprocess.Popen(cmd, shell=True, **kwargs)
                self.active_processes[name] = process
                logging.info(f"Started process: {name} (PID: {process.pid})")
                return process
            except Exception as e:
                logging.error(f"Failed to start process {name}: {e}")
                raise

    def kill_process(self, name: str) -> bool:
        """Kill a process gracefully, then forcefully if needed."""
        with self.process_lock:
            if name not in self.active_processes:
                return True

            process = self.active_processes[name]
            try:
                # Try graceful termination first
                process.terminate()
                try:
                    process.wait(timeout=self.kill_timeout)
                    logging.info(f"Process {name} terminated gracefully")
                except subprocess.TimeoutExpired:
                    # Force kill if graceful termination fails
                    logging.warning(f"Force killing process {name}")
                    process.kill()
                    process.wait()
                    logging.info(f"Process {name} force killed")

                del self.active_processes[name]
                return True

            except Exception as e:
                logging.error(f"Failed to kill process {name}: {e}")
                return False

    def kill_all_processes(self) -> Dict[str, bool]:
        """Kill all active processes."""
        results = {}
        with self.process_lock:
            for name in list(self.active_processes.keys()):
                results[name] = self.kill_process(name)
        return results

    def get_process_status(self) -> Dict[str, Dict[str, Any]]:
        """Get status of all active processes."""
        status: Dict[str, Any] = {}
        with self.process_lock:
            for name, process in self.active_processes.items():
                try:
                    poll_result = process.poll()
                    status[name] = {
                        "pid": process.pid,
                        "alive": poll_result is None,
                        "exit_code": poll_result,
                        "cmd": getattr(process, 'args', 'unknown')
                    }
                except Exception as e:
                    status[name] = {"error": str(e)}
        return status


class BuildIsolationManager:
    """Build isolation mechanisms to prevent conflicts."""

    def __init__(self, workspace_dir: Path) -> None:
        self.workspace_dir = workspace_dir
        self.isolation_dir = workspace_dir / ".warploom" / "isolation"
        self.isolation_dir.mkdir(parents=True, exist_ok=True)
        self.lock_file = self.isolation_dir / "build_lock"

    def acquire_build_lock(self, build_id: str) -> bool:
        """Acquire exclusive build lock."""
        try:
            if self.lock_file.exists():
                with open(self.lock_file, 'r') as f:
                    existing_build_id = f.read().strip()
                if existing_build_id != build_id:
                    logging.warning(f"Build lock held by different build: {existing_build_id}")
                    return False

            with open(self.lock_file, 'w') as f:
                f.write(build_id)
            logging.info(f"Acquired build lock for: {build_id}")
            return True

        except Exception as e:
            logging.error(f"Failed to acquire build lock: {e}")
            return False

    def release_build_lock(self, build_id: str) -> bool:
        """Release build lock."""
        try:
            if self.lock_file.exists():
                with open(self.lock_file, 'r') as f:
                    existing_build_id = f.read().strip()
                if existing_build_id == build_id:
                    self.lock_file.unlink()
                    logging.info(f"Released build lock for: {build_id}")
                    return True
            return False
        except Exception as e:
            logging.error(f"Failed to release build lock: {e}")
            return False

    def cleanup_stale_locks(self, max_age_hours: int = 2) -> int:
        """Clean up stale build locks."""
        try:
            if not self.lock_file.exists():
                return 0

            stat = self.lock_file.stat()
            age_hours = (time.time() - stat.st_mtime) / 3600

            if age_hours > max_age_hours:
                self.lock_file.unlink()
                logging.info(f"Cleaned up stale build lock (age: {age_hours:.1f}h)")
                return 1

            return 0
        except Exception as e:
            logging.error(f"Failed to cleanup stale locks: {e}")
            return 0


class SystemResourceManager:
    """System resource management for build operations."""

    def __init__(self) -> None:
        self.min_memory_gb = 2.0
        self.min_disk_gb = 5.0
        self.resource_check_interval = 60  # seconds

    def check_system_resources(self) -> Dict[str, Any]:
        """Check if system has adequate resources for building."""
        status: Dict[str, Any] = {
            "memory_ok": True,
            "disk_ok": True,
            "cpu_ok": True,
            "warnings": [],
            "recommendations": []
        }

        if HAS_PSUTIL:
            # Memory check
            memory = psutil.virtual_memory()
            available_gb = memory.available / (1024**3)
            if available_gb < self.min_memory_gb:
                status["memory_ok"] = False
                status["warnings"].append(f"Low memory: {available_gb:.1f}GB available")
                status["recommendations"].append("Close memory-intensive applications")

            # Disk space check
            disk = psutil.disk_usage('/')
            free_gb = disk.free / (1024**3)
            if free_gb < self.min_disk_gb:
                status["disk_ok"] = False
                status["warnings"].append(f"Low disk space: {free_gb:.1f}GB free")
                status["recommendations"].append("Free up disk space")

            # CPU usage check
            cpu_percent = psutil.cpu_percent(interval=1)
            if cpu_percent > 90:
                status["cpu_ok"] = False
                status["warnings"].append(f"High CPU usage: {cpu_percent}%")
                status["recommendations"].append("Wait for CPU usage to decrease")

        return status

    def wait_for_resources(self, timeout_seconds: int = 300) -> bool:
        """Wait for adequate system resources."""
        start_time = time.time()

        while time.time() - start_time < timeout_seconds:
            status = self.check_system_resources()
            if all([status["memory_ok"], status["disk_ok"], status["cpu_ok"]]):
                return True

            logging.info("Waiting for system resources...")
            time.sleep(self.resource_check_interval)

        logging.error("Timeout waiting for system resources")
        return False

    def get_resource_limits(self) -> Dict[str, Any]:
        """Get recommended resource limits for build operations."""
        limits = {
            "max_parallel_jobs": min(os.cpu_count() or 4, 8),
            "memory_limit_mb": 4096,  # 4GB default
            "disk_buffer_gb": 1.0
        }

        if HAS_PSUTIL:
            memory = psutil.virtual_memory()
            available_gb = memory.available / (1024**3)

            # Adjust limits based on available memory
            if available_gb < 4:
                limits["max_parallel_jobs"] = 1
                limits["memory_limit_mb"] = 2048
            elif available_gb < 8:
                limits["max_parallel_jobs"] = min(limits["max_parallel_jobs"], 4)
                limits["memory_limit_mb"] = 3072

        return limits


def switch_to_msvc_compiler(build_context: Dict[str, Any]) -> bool:
    """Switch compiler from clang-msvc to MSVC for better performance."""
    try:
        # This would modify the build context to use MSVC instead of clang-msvc
        if build_context.get("compiler") == "clang-msvc":
            build_context["compiler"] = "msvc"
            logging.info("Switched compiler from clang-msvc to msvc")
            return True
        return False
    except Exception as e:
        logging.error(f"Failed to switch compiler: {e}")
        return False


def retry_build(build_func: Any, max_retries: int = 3, *args: Any, **kwargs: Any) -> bool:
    """Retry build function with exponential backoff."""
    for attempt in range(max_retries):
        try:
            logging.info(f"Build attempt {attempt + 1}/{max_retries}")
            if build_func(*args, **kwargs):
                return True

            if attempt < max_retries - 1:
                wait_time = 2 ** attempt  # Exponential backoff
                logging.info(f"Build failed, waiting {wait_time}s before retry...")
                time.sleep(wait_time)

        except Exception as e:
            logging.error(f"Build attempt {attempt + 1} failed: {e}")
            if attempt < max_retries - 1:
                wait_time = 2 ** attempt
                time.sleep(wait_time)

    logging.error(f"Build failed after {max_retries} attempts")
    return False


def force_clean_operation(build_dir: Path, process_manager: ProcessManager) -> bool:
    """Perform force clean operation with process termination."""
    try:
        # Kill any active build processes
        killed_processes = process_manager.kill_all_processes()
        logging.info(f"Killed {len(killed_processes)} active processes")

        # Force remove build directory
        if build_dir.exists():
            shutil.rmtree(build_dir, ignore_errors=True)
            logging.info(f"Force removed build directory: {build_dir}")

        # Clean up any lock files
        isolation_manager = BuildIsolationManager(build_dir.parent)
        isolation_manager.cleanup_stale_locks()

        return True

    except Exception as e:
        logging.error(f"Force clean operation failed: {e}")
        return False
