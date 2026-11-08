"""
Unit tests for build_optimizer module.
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Any
from datetime import datetime
import pytest

# Add parent directories to path for imports
import sys
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.build_optimizer import (
    OptimizationLevel,
    CacheType,
    BuildPerformanceData,
    OptimizationRecommendation,
    CacheEntry,
    HistoricalPerformanceTracker,
    PredictiveFailurePrevention,
    AdvancedCacheManager,
    BuildOptimizer,
    ClangMsvcReleaseTimeouts,
    MemoryAwareBuildScheduler,
    BuildProgressMonitor,
    get_clang_msvc_release_jobs,
    select_build_strategy_for_clang_msvc_release,
    clang_msvc_fallback_to_msvc,
    apply_clang_msvc_release_optimizations,
    ProcessManager,
    BuildIsolationManager,
    SystemResourceManager,
    switch_to_msvc_compiler,
    retry_build,
    force_clean_operation
)


class TestOptimizationLevel:
    """Unit tests for OptimizationLevel enum."""

    def test_optimization_level_values(self) -> None:
        """Test OptimizationLevel enum values."""
        assert OptimizationLevel.NONE.value == "none"
        assert OptimizationLevel.BASIC.value == "basic"
        assert OptimizationLevel.ADVANCED.value == "advanced"
        assert OptimizationLevel.AGGRESSIVE.value == "aggressive"


class TestCacheType:
    """Unit tests for CacheType enum."""

    def test_cache_type_values(self) -> None:
        """Test CacheType enum values."""
        assert CacheType.DEPENDENCY.value == "dependency"
        assert CacheType.OBJECT.value == "object"
        assert CacheType.PRECOMPILED_HEADER.value == "precompiled_header"
        assert CacheType.BUILD_ARTIFACT.value == "build_artifact"
        assert CacheType.CCACHE.value == "ccache"


class TestBuildPerformanceData:
    """Unit tests for BuildPerformanceData."""

    def test_build_performance_data_creation(self) -> None:
        """Test BuildPerformanceData creation."""
        data = BuildPerformanceData(
            build_id="test-1",
            timestamp=datetime.now(),
            product="Warploom",
            arch="x86_64",
            build_type="Release",
            compiler="msvc",
            duration_seconds=120.5,
            peak_memory_mb=1024.0,
            cpu_usage_percent=75.0,
            success=True
        )
        assert data.build_id == "test-1"
        assert data.product == "Warploom"
        assert data.arch == "x86_64"
        assert data.build_type == "Release"
        assert data.compiler == "msvc"
        assert data.duration_seconds == 120.5
        assert data.peak_memory_mb == 1024.0
        assert data.cpu_usage_percent == 75.0
        assert data.success is True

    def test_build_performance_data_defaults(self) -> None:
        """Test BuildPerformanceData with default values."""
        data = BuildPerformanceData(
            build_id="test-1",
            timestamp=datetime.now(),
            product="Warploom",
            arch="x86_64",
            build_type="Release",
            compiler="msvc",
            duration_seconds=120.5,
            peak_memory_mb=1024.0,
            cpu_usage_percent=75.0,
            success=True
        )
        assert data.error_message is None
        assert data.step_durations == {}
        assert data.system_info == {}
        assert data.optimization_applied == []


class TestOptimizationRecommendation:
    """Unit tests for OptimizationRecommendation."""

    def test_optimization_recommendation_creation(self) -> None:
        """Test OptimizationRecommendation creation."""
        recommendation = OptimizationRecommendation(
            category="memory",
            title="Enable Precompiled Headers",
            description="Precompiled headers can reduce compilation time",
            impact="high",
            confidence=0.8,
            implementation_effort="medium",
            expected_improvement_percent=15.0
        )
        assert recommendation.category == "memory"
        assert recommendation.title == "Enable Precompiled Headers"
        assert recommendation.impact == "high"
        assert recommendation.confidence == 0.8
        assert recommendation.implementation_effort == "medium"
        assert recommendation.expected_improvement_percent == 15.0

    def test_optimization_recommendation_defaults(self) -> None:
        """Test OptimizationRecommendation with default values."""
        recommendation = OptimizationRecommendation(
            category="memory",
            title="Test",
            description="Test",
            impact="high",
            confidence=0.8,
            implementation_effort="medium",
            expected_improvement_percent=15.0
        )
        assert recommendation.applicable_conditions == {}
        assert recommendation.implementation_steps == []


class TestCacheEntry:
    """Unit tests for CacheEntry."""

    def test_cache_entry_creation(self) -> None:
        """Test CacheEntry creation."""
        entry = CacheEntry(
            cache_type=CacheType.DEPENDENCY,
            key="test-key",
            path="/test/cache/entry",
            created_at=datetime.now(),
            last_accessed=datetime.now(),
            size_bytes=1024
        )
        assert entry.cache_type == CacheType.DEPENDENCY
        assert entry.key == "test-key"
        assert entry.path == "/test/cache/entry"
        assert entry.size_bytes == 1024
        assert entry.hits == 0
        assert entry.dependencies == set()


class TestHistoricalPerformanceTracker:
    """Unit tests for HistoricalPerformanceTracker."""

    def test_historical_performance_tracker_initialization(self) -> None:
        """Test HistoricalPerformanceTracker initialization."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            assert tracker.data_dir == Path(tmp_dir)
            assert tracker.performance_file == Path(tmp_dir) / "build_performance_history.json"
            assert tracker._performance_data == []

    def test_record_build_performance(self) -> None:
        """Test record_build_performance method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            data = BuildPerformanceData(
                build_id="test-1",
                timestamp=datetime.now(),
                product="Warploom",
                arch="x86_64",
                build_type="Release",
                compiler="msvc",
                duration_seconds=120.5,
                peak_memory_mb=1024.0,
                cpu_usage_percent=75.0,
                success=True
            )
            tracker.record_build_performance(data)
            assert len(tracker._performance_data) == 1

    def test_get_recent_performance(self) -> None:
        """Test get_recent_performance method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            data = BuildPerformanceData(
                build_id="test-1",
                timestamp=datetime.now(),
                product="Warploom",
                arch="x86_64",
                build_type="Release",
                compiler="msvc",
                duration_seconds=120.5,
                peak_memory_mb=1024.0,
                cpu_usage_percent=75.0,
                success=True
            )
            tracker.record_build_performance(data)
            recent = tracker.get_recent_performance(days=30)
            assert len(recent) == 1

    def test_analyze_performance_trends_no_data(self) -> None:
        """Test analyze_performance_trends with no data."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            result = tracker.analyze_performance_trends("Warploom", "x86_64", "Release")
            assert "error" in result

    def test_analyze_performance_trends_with_data(self) -> None:
        """Test analyze_performance_trends with data."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            data = BuildPerformanceData(
                build_id="test-1",
                timestamp=datetime.now(),
                product="Warploom",
                arch="x86_64",
                build_type="Release",
                compiler="msvc",
                duration_seconds=120.5,
                peak_memory_mb=1024.0,
                cpu_usage_percent=75.0,
                success=True
            )
            tracker.record_build_performance(data)
            result = tracker.analyze_performance_trends("Warploom", "x86_64", "Release")
            assert "total_builds" in result
            assert result["total_builds"] == 1

    def test_calculate_trend_stable(self) -> None:
        """Test _calculate_trend with stable values."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            values = [100.0, 100.0, 100.0]
            trend = tracker._calculate_trend(values)
            assert trend == "stable"

    def test_calculate_trend_increasing(self) -> None:
        """Test _calculate_trend with increasing values."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            values = [100.0, 110.0, 120.0]
            trend = tracker._calculate_trend(values)
            assert trend == "increasing"

    def test_calculate_trend_decreasing(self) -> None:
        """Test _calculate_trend with decreasing values."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            values = [120.0, 110.0, 100.0]
            trend = tracker._calculate_trend(values)
            assert trend == "decreasing"


class TestPredictiveFailurePrevention:
    """Unit tests for PredictiveFailurePrevention."""

    def test_predictive_failure_prevention_initialization(self) -> None:
        """Test PredictiveFailurePrevention initialization."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            prevention = PredictiveFailurePrevention(tracker)
            assert prevention.performance_tracker == tracker
            assert prevention.failure_patterns is not None

    def test_predict_potential_failures(self) -> None:
        """Test predict_potential_failures method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tracker = HistoricalPerformanceTracker(Path(tmp_dir))
            prevention = PredictiveFailurePrevention(tracker)
            build_context = {
                "product": "Warploom",
                "arch": "x86_64",
                "build_type": "Release",
                "compiler": "msvc"
            }
            predictions = prevention.predict_potential_failures(build_context)
            assert isinstance(predictions, list)


class TestAdvancedCacheManager:
    """Unit tests for AdvancedCacheManager."""

    def test_advanced_cache_manager_initialization(self) -> None:
        """Test AdvancedCacheManager initialization."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cache_dir = Path(tmp_dir) / "cache"
            manager = AdvancedCacheManager(cache_dir, max_cache_size_gb=5.0)
            assert manager.cache_dir == cache_dir
            assert manager.max_cache_size_gb == 5.0
            assert manager.metadata_file == cache_dir / "cache_metadata.json"

    def test_generate_cache_key(self) -> None:
        """Test generate_cache_key method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cache_dir = Path(tmp_dir) / "cache"
            manager = AdvancedCacheManager(cache_dir)
            key = manager.generate_cache_key(CacheType.DEPENDENCY, product="Warploom", arch="x86_64")
            assert isinstance(key, str)
            assert len(key) == 16  # SHA256 hash truncated to 16 chars

    def test_store_in_cache_file(self) -> None:
        """Test store_in_cache with file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cache_dir = Path(tmp_dir) / "cache"
            manager = AdvancedCacheManager(cache_dir)

            # Create a test file
            source_file = Path(tmp_dir) / "test.txt"
            source_file.write_text("test content")

            key = manager.generate_cache_key(CacheType.DEPENDENCY)
            result = manager.store_in_cache(CacheType.DEPENDENCY, key, source_file)
            assert result is True

    def test_store_in_cache_nonexistent_file(self) -> None:
        """Test store_in_cache with nonexistent file."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cache_dir = Path(tmp_dir) / "cache"
            manager = AdvancedCacheManager(cache_dir)

            source_file = Path(tmp_dir) / "nonexistent.txt"
            key = manager.generate_cache_key(CacheType.DEPENDENCY)
            result = manager.store_in_cache(CacheType.DEPENDENCY, key, source_file)
            assert result is False

    def test_retrieve_from_cache(self) -> None:
        """Test retrieve_from_cache method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cache_dir = Path(tmp_dir) / "cache"
            manager = AdvancedCacheManager(cache_dir)

            # Create and store a test file
            source_file = Path(tmp_dir) / "test.txt"
            source_file.write_text("test content")
            key = manager.generate_cache_key(CacheType.DEPENDENCY)
            manager.store_in_cache(CacheType.DEPENDENCY, key, source_file)

            # Retrieve the file
            target_file = Path(tmp_dir) / "retrieved.txt"
            result = manager.retrieve_from_cache(CacheType.DEPENDENCY, key, target_file)
            assert result is True
            assert target_file.exists()

    def test_retrieve_from_cache_miss(self) -> None:
        """Test retrieve_from_cache with cache miss."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cache_dir = Path(tmp_dir) / "cache"
            manager = AdvancedCacheManager(cache_dir)

            target_file = Path(tmp_dir) / "retrieved.txt"
            key = manager.generate_cache_key(CacheType.DEPENDENCY)
            result = manager.retrieve_from_cache(CacheType.DEPENDENCY, key, target_file)
            assert result is False

    def test_get_cache_stats(self) -> None:
        """Test get_cache_stats method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            cache_dir = Path(tmp_dir) / "cache"
            manager = AdvancedCacheManager(cache_dir)
            stats = manager.get_cache_stats()
            assert "total_entries" in stats
            assert "total_size_mb" in stats
            assert "total_hits" in stats
            assert "cache_types" in stats
            assert "max_size_gb" in stats


class TestBuildOptimizer:
    """Unit tests for BuildOptimizer."""

    def test_build_optimizer_initialization(self) -> None:
        """Test BuildOptimizer initialization."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            optimizer = BuildOptimizer(workspace_dir)
            assert optimizer.workspace_dir == workspace_dir
            assert optimizer.data_dir == workspace_dir / ".warploom" / "optimization"
            assert optimizer.performance_tracker is not None
            assert optimizer.failure_prevention is not None
            assert optimizer.cache_manager is not None

    def test_generate_optimization_recommendations(self) -> None:
        """Test generate_optimization_recommendations method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            optimizer = BuildOptimizer(workspace_dir)
            build_context = {
                "product": "Warploom",
                "arch": "x86_64",
                "build_type": "Release",
                "compiler": "msvc"
            }
            recommendations = optimizer.generate_optimization_recommendations(build_context)
            assert isinstance(recommendations, list)

    def test_apply_optimization(self) -> None:
        """Test apply_optimization method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            optimizer = BuildOptimizer(workspace_dir)
            recommendation = OptimizationRecommendation(
                category="memory",
                title="Enable Precompiled Headers",
                description="Test",
                impact="high",
                confidence=0.8,
                implementation_effort="medium",
                expected_improvement_percent=15.0
            )
            result = optimizer.apply_optimization(recommendation)
            assert isinstance(result, bool)

    def test_get_continuous_improvements(self) -> None:
        """Test get_continuous_improvements method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            optimizer = BuildOptimizer(workspace_dir)
            improvements = optimizer.get_continuous_improvements()
            assert isinstance(improvements, list)

    def test_optimize_build_pipeline(self) -> None:
        """Test optimize_build_pipeline method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            optimizer = BuildOptimizer(workspace_dir)
            build_context = {
                "product": "Warploom",
                "arch": "x86_64",
                "build_type": "Release",
                "compiler": "msvc"
            }
            optimizations = optimizer.optimize_build_pipeline(build_context)
            assert "cache_utilization" in optimizations
            assert "job_scheduling" in optimizations
            assert "memory_management" in optimizations
            assert "failure_prevention" in optimizations

    def test_get_optimization_report(self) -> None:
        """Test get_optimization_report method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            optimizer = BuildOptimizer(workspace_dir)
            report = optimizer.get_optimization_report()
            assert "performance_summary" in report
            assert "cache_stats" in report
            assert "applied_optimizations" in report
            assert "continuous_improvements" in report


class TestClangMsvcReleaseTimeouts:
    """Unit tests for ClangMsvcReleaseTimeouts."""

    def test_clang_msvc_release_timeouts_defaults(self) -> None:
        """Test ClangMsvcReleaseTimeouts default values."""
        timeouts = ClangMsvcReleaseTimeouts()
        assert timeouts.clean == 60
        assert timeouts.conan == 300
        assert timeouts.configure == 180
        assert timeouts.build == 600
        assert timeouts.total == 1200


class TestMemoryAwareBuildScheduler:
    """Unit tests for MemoryAwareBuildScheduler."""

    def test_memory_aware_build_scheduler_initialization(self) -> None:
        """Test MemoryAwareBuildScheduler initialization."""
        scheduler = MemoryAwareBuildScheduler(max_memory_gb=8.0)
        assert scheduler.max_memory_gb == 8.0
        assert scheduler.memory_per_job_mb == 600

    def test_calculate_safe_job_count(self) -> None:
        """Test calculate_safe_job_count method."""
        scheduler = MemoryAwareBuildScheduler(max_memory_gb=8.0)
        job_count = scheduler.calculate_safe_job_count()
        assert isinstance(job_count, int)
        assert job_count >= 1


class TestBuildProgressMonitor:
    """Unit tests for BuildProgressMonitor."""

    def test_build_progress_monitor_initialization(self) -> None:
        """Test BuildProgressMonitor initialization."""
        monitor = BuildProgressMonitor()
        assert monitor.stall_threshold == 300

    def test_check_for_stalls_no_stall(self) -> None:
        """Test check_for_stalls with no stall."""
        monitor = BuildProgressMonitor()
        result = monitor.check_for_stalls(50.0)
        assert result is False


class TestClangMsvcFunctions:
    """Unit tests for clang-msvc utility functions."""

    def test_get_clang_msvc_release_jobs(self) -> None:
        """Test get_clang_msvc_release_jobs function."""
        jobs = get_clang_msvc_release_jobs()
        assert isinstance(jobs, int)
        assert jobs >= 1
        assert jobs <= 4

    def test_select_build_strategy_for_clang_msvc_release(self) -> None:
        """Test select_build_strategy_for_clang_msvc_release function."""
        strategy = select_build_strategy_for_clang_msvc_release()
        assert strategy == "incremental"

    def test_clang_msvc_fallback_to_msvc_no_fallback(self) -> None:
        """Test clang_msvc_fallback_to_msvc with fast build."""
        result = clang_msvc_fallback_to_msvc(300.0)
        assert result is False

    def test_clang_msvc_fallback_to_msvc_fallback(self) -> None:
        """Test clang_msvc_fallback_to_msvc with slow build."""
        result = clang_msvc_fallback_to_msvc(700.0)
        assert result is True

    def test_apply_clang_msvc_release_optimizations(self) -> None:
        """Test apply_clang_msvc_release_optimizations function."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            build_dir = Path(tmp_dir)
            optimizations = apply_clang_msvc_release_optimizations(build_dir)
            assert "jobs" in optimizations
            assert "safe_jobs" in optimizations
            assert "final_jobs" in optimizations
            assert "strategy" in optimizations
            assert "timeouts" in optimizations


class TestProcessManager:
    """Unit tests for ProcessManager."""

    def test_process_manager_initialization(self) -> None:
        """Test ProcessManager initialization."""
        manager = ProcessManager()
        assert manager.active_processes == {}
        assert manager.kill_timeout == 30

    def test_get_process_status(self) -> None:
        """Test get_process_status method."""
        manager = ProcessManager()
        status = manager.get_process_status()
        assert isinstance(status, dict)


class TestBuildIsolationManager:
    """Unit tests for BuildIsolationManager."""

    def test_build_isolation_manager_initialization(self) -> None:
        """Test BuildIsolationManager initialization."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            manager = BuildIsolationManager(workspace_dir)
            assert manager.workspace_dir == workspace_dir
            assert manager.isolation_dir == workspace_dir / ".warploom" / "isolation"

    def test_acquire_build_lock(self) -> None:
        """Test acquire_build_lock method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            manager = BuildIsolationManager(workspace_dir)
            result = manager.acquire_build_lock("test-build-1")
            assert result is True

    def test_release_build_lock(self) -> None:
        """Test release_build_lock method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            manager = BuildIsolationManager(workspace_dir)
            manager.acquire_build_lock("test-build-1")
            result = manager.release_build_lock("test-build-1")
            assert result is True

    def test_cleanup_stale_locks(self) -> None:
        """Test cleanup_stale_locks method."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            workspace_dir = Path(tmp_dir)
            manager = BuildIsolationManager(workspace_dir)
            count = manager.cleanup_stale_locks()
            assert isinstance(count, int)


class TestSystemResourceManager:
    """Unit tests for SystemResourceManager."""

    def test_system_resource_manager_initialization(self) -> None:
        """Test SystemResourceManager initialization."""
        manager = SystemResourceManager()
        assert manager.min_memory_gb == 2.0
        assert manager.min_disk_gb == 5.0
        assert manager.resource_check_interval == 60

    def test_check_system_resources(self) -> None:
        """Test check_system_resources method."""
        manager = SystemResourceManager()
        status = manager.check_system_resources()
        assert "memory_ok" in status
        assert "disk_ok" in status
        assert "cpu_ok" in status
        assert "warnings" in status
        assert "recommendations" in status

    def test_get_resource_limits(self) -> None:
        """Test get_resource_limits method."""
        manager = SystemResourceManager()
        limits = manager.get_resource_limits()
        assert "max_parallel_jobs" in limits
        assert "memory_limit_mb" in limits
        assert "disk_buffer_gb" in limits


class TestUtilityFunctions:
    """Unit tests for utility functions."""

    def test_switch_to_msvc_compiler(self) -> None:
        """Test switch_to_msvc_compiler function."""
        build_context = {"compiler": "clang-msvc"}
        result = switch_to_msvc_compiler(build_context)
        assert result is True
        assert build_context["compiler"] == "msvc"

    def test_switch_to_msvc_compiler_no_change(self) -> None:
        """Test switch_to_msvc_compiler with MSVC already."""
        build_context = {"compiler": "msvc"}
        result = switch_to_msvc_compiler(build_context)
        assert result is False

    def test_retry_build_success(self) -> None:
        """Test retry_build with success."""
        def build_func() -> bool:
            return True

        result = retry_build(build_func, max_retries=3)
        assert result is True

    def test_retry_build_failure(self) -> None:
        """Test retry_build with failure."""
        def build_func() -> bool:
            return False

        result = retry_build(build_func, max_retries=2)
        assert result is False

    def test_force_clean_operation(self) -> None:
        """Test force_clean_operation function."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            build_dir = Path(tmp_dir) / "build"
            build_dir.mkdir()
            process_manager = ProcessManager()
            result = force_clean_operation(build_dir, process_manager)
            assert result is True


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
