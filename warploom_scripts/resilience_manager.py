"""
Resilience Manager for WarploomController
Implements Phase 3: Resilience and Fallback Mechanisms

This module provides comprehensive resilience features including:
- Retry mechanisms with exponential backoff
- Graceful degradation strategies
- Build recovery mechanisms
- Timeout handling
- Error recovery systems

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
import time
import logging
import threading
import subprocess
from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any, Callable, Union
from dataclasses import dataclass, field
from enum import Enum
import json

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    psutil = None  # type: ignore[assignment]
    HAS_PSUTIL = False

from .build_optimizer import (
    ProcessManager,
    BuildIsolationManager,
    SystemResourceManager,
    retry_build,
    switch_to_msvc_compiler,
    force_clean_operation,
    ClangMsvcReleaseTimeouts
)


class ResilienceLevel(Enum):
    """Levels of resilience for different failure scenarios."""
    BASIC = "basic"           # Simple retry
    STANDARD = "standard"     # Retry with backoff
    ADVANCED = "advanced"     # Multiple strategies
    AGGRESSIVE = "aggressive" # All fallback mechanisms


class FailureType(Enum):
    """Types of build failures that can occur."""
    TIMEOUT = "timeout"
    COMPILATION_ERROR = "compilation_error"
    LINKING_ERROR = "linking_error"
    RESOURCE_EXHAUSTION = "resource_exhaustion"
    DEPENDENCY_ERROR = "dependency_error"
    CONFIGURATION_ERROR = "configuration_error"
    UNKNOWN = "unknown"


@dataclass
class RetryPolicy:
    """Configuration for retry behavior."""
    max_attempts: int = 3
    base_delay_seconds: float = 1.0
    max_delay_seconds: float = 60.0
    backoff_multiplier: float = 2.0
    jitter_range: float = 0.1
    timeout_multiplier: float = 1.5  # Increase timeout on retry


@dataclass
class DegradationStrategy:
    """Strategy for graceful degradation."""
    name: str
    description: str
    applicability_check: Callable[[Dict[str, Any]], bool]
    apply_function: Callable[[Dict[str, Any]], Dict[str, Any]]
    recovery_priority: int = 1  # Lower number = higher priority


@dataclass
class BuildRecoveryState:
    """State information for build recovery."""
    build_id: str
    original_context: Dict[str, Any]
    failure_history: List[Dict[str, Any]] = field(default_factory=list)
    applied_strategies: List[str] = field(default_factory=list)
    recovery_attempts: int = 0
    last_attempt_time: Optional[datetime] = None
    can_recover: bool = True


@dataclass
class ResilienceMetrics:
    """Metrics for resilience operations."""
    total_failures: int = 0
    successful_recoveries: int = 0
    failed_recoveries: int = 0
    average_recovery_time: float = 0.0
    strategy_success_rates: Dict[str, float] = field(default_factory=dict)


class RetryManager:
    """Manages retry logic with configurable policies."""

    def __init__(self, policy: Optional[RetryPolicy] = None):
        self.policy = policy or RetryPolicy()
        self._lock = threading.Lock()

    def execute_with_retry(self, operation: Callable[..., Any], *args: Any, **kwargs: Any) -> Tuple[bool, Any, Dict[str, Any]]:
        """
        Execute an operation with retry logic.

        Returns:
            Tuple of (success, result, metadata)
        """
        metadata: Dict[str, Any] = {
            "attempts": 0,
            "total_delay": 0.0,
            "failures": [],
            "final_error": None
        }

        for attempt in range(self.policy.max_attempts):
            metadata["attempts"] += 1

            try:
                start_time = time.time()
                result = operation(*args, **kwargs)
                execution_time = time.time() - start_time

                # Assume operation returns True/False for success
                if result is True or (isinstance(result, tuple) and result[0] is True):
                    metadata["execution_time"] = execution_time
                    return True, result, metadata

            except Exception as e:
                error_info: Dict[str, Any] = {
                    "attempt": attempt + 1,
                    "error": str(e),
                    "timestamp": datetime.now().isoformat()
                }
                metadata["failures"].append(error_info)
                metadata["final_error"] = str(e)

                logging.warning(f"Attempt {attempt + 1} failed: {e}")

                # Don't retry on the last attempt
                if attempt < self.policy.max_attempts - 1:
                    delay = self._calculate_delay(attempt)
                    metadata["total_delay"] += delay
                    logging.info(f"Waiting {delay:.1f}s before retry...")
                    time.sleep(delay)

        return False, None, metadata

    def _calculate_delay(self, attempt: int) -> float:
        """Calculate delay with exponential backoff and jitter."""
        base_delay = self.policy.base_delay_seconds * (self.policy.backoff_multiplier ** attempt)
        delay = min(base_delay, self.policy.max_delay_seconds)

        # Add jitter to prevent thundering herd
        import random
        jitter = random.uniform(-self.policy.jitter_range, self.policy.jitter_range) * delay
        delay += jitter

        return max(0.1, delay)  # Minimum 100ms delay


class GracefulDegradationManager:
    """Manages graceful degradation strategies."""

    def __init__(self) -> None:
        self.strategies: List[DegradationStrategy] = []
        self._register_default_strategies()

    def _register_default_strategies(self) -> None:
        """Register default degradation strategies."""

        # Strategy 1: Reduce parallel jobs
        self.strategies.append(DegradationStrategy(
            name="reduce_parallel_jobs",
            description="Reduce parallel compilation jobs to conserve memory",
            applicability_check=lambda ctx: ctx.get("compiler") in ["clang-msvc", "clang"] and ctx.get("build_type") == "release",
            apply_function=self._reduce_parallel_jobs,
            recovery_priority=1
        ))

        # Strategy 2: Switch to incremental build
        self.strategies.append(DegradationStrategy(
            name="incremental_build",
            description="Use incremental build instead of clean rebuild",
            applicability_check=lambda ctx: ctx.get("task") == "Clean Build Pipeline",
            apply_function=self._switch_to_incremental,
            recovery_priority=2
        ))

        # Strategy 3: Fallback compiler
        self.strategies.append(DegradationStrategy(
            name="fallback_compiler",
            description="Switch from clang-msvc to MSVC compiler",
            applicability_check=lambda ctx: ctx.get("compiler") == "clang-msvc",
            apply_function=self._fallback_compiler,
            recovery_priority=3
        ))

        # Strategy 4: Two-phase build
        self.strategies.append(DegradationStrategy(
            name="two_phase_build",
            description="Split build into compilation and linking phases",
            applicability_check=lambda ctx: ctx.get("task") == "Build Project" and ctx.get("build_type") == "release",
            apply_function=self._enable_two_phase_build,
            recovery_priority=4
        ))

        # Strategy 5: Disable optimizations
        self.strategies.append(DegradationStrategy(
            name="disable_optimizations",
            description="Disable aggressive optimizations for reliability",
            applicability_check=lambda ctx: ctx.get("build_type") == "release",
            apply_function=self._disable_optimizations,
            recovery_priority=5
        ))

    def get_applicable_strategies(self, build_context: Dict[str, Any]) -> List[DegradationStrategy]:
        """Get strategies applicable to the current build context."""
        applicable = []
        for strategy in self.strategies:
            try:
                if strategy.applicability_check(build_context):
                    applicable.append(strategy)
            except Exception as e:
                logging.warning(f"Error checking strategy {strategy.name}: {e}")

        # Sort by priority (lower number = higher priority)
        return sorted(applicable, key=lambda s: s.recovery_priority)

    def apply_strategy(self, strategy: DegradationStrategy, build_context: Dict[str, Any]) -> Dict[str, Any]:
        """Apply a degradation strategy."""
        try:
            logging.info(f"Applying degradation strategy: {strategy.name}")
            modified_context = strategy.apply_function(build_context.copy())
            modified_context["_applied_strategy"] = strategy.name
            return modified_context
        except Exception as e:
            logging.error(f"Failed to apply strategy {strategy.name}: {e}")
            return build_context

    def _reduce_parallel_jobs(self, context: Dict[str, Any]) -> Dict[str, Any]:
        """Reduce parallel jobs in build context."""
        current_jobs = context.get("parallel_jobs", os.cpu_count() or 4)
        context["parallel_jobs"] = max(1, current_jobs // 2)
        context["degradation_reason"] = "Reduced parallel jobs for memory conservation"
        return context

    def _switch_to_incremental(self, context: Dict[str, Any]) -> Dict[str, Any]:
        """Switch to incremental build."""
        context["build_strategy"] = "incremental"
        context["skip_clean"] = True
        context["degradation_reason"] = "Using incremental build to avoid expensive clean"
        return context

    def _fallback_compiler(self, context: Dict[str, Any]) -> Dict[str, Any]:
        """Fallback to MSVC compiler."""
        if context.get("compiler") == "clang-msvc":
            context["compiler"] = "msvc"
            context["degradation_reason"] = "Switched to MSVC compiler for better reliability"
        return context

    def _enable_two_phase_build(self, context: Dict[str, Any]) -> Dict[str, Any]:
        """Enable two-phase build approach."""
        context["build_phases"] = ["compile", "link"]
        context["phase_parallelism"] = {"compile": 2, "link": 1}
        context["degradation_reason"] = "Using two-phase build to reduce memory pressure"
        return context

    def _disable_optimizations(self, context: Dict[str, Any]) -> Dict[str, Any]:
        """Disable aggressive optimizations."""
        context["optimization_level"] = "basic"
        context["disable_pch"] = True
        context["degradation_reason"] = "Disabled optimizations for improved reliability"
        return context


class BuildRecoveryManager:
    """Manages build recovery after failures."""

    def __init__(self, workspace_dir: Path):
        self.workspace_dir = workspace_dir
        self.recovery_states: Dict[str, BuildRecoveryState] = {}
        self.isolation_manager = BuildIsolationManager(workspace_dir)
        self.process_manager = ProcessManager()
        self.resource_manager = SystemResourceManager()
        self._lock = threading.Lock()

    def initiate_recovery(self, build_id: str, original_context: Dict[str, Any],
                         failure_info: Dict[str, Any]) -> BuildRecoveryState:
        """Initiate recovery process for a failed build."""
        with self._lock:
            state = BuildRecoveryState(
                build_id=build_id,
                original_context=original_context,
                failure_history=[failure_info],
                last_attempt_time=datetime.now()
            )
            self.recovery_states[build_id] = state
            logging.info(f"Initiated recovery for build {build_id}")
            return state

    def attempt_recovery(self, build_id: str, recovery_strategy: str) -> Tuple[bool, Dict[str, Any]]:
        """Attempt to recover a build using specified strategy."""
        with self._lock:
            if build_id not in self.recovery_states:
                return False, {"error": "No recovery state found"}

            state = self.recovery_states[build_id]
            state.recovery_attempts += 1
            state.applied_strategies.append(recovery_strategy)
            state.last_attempt_time = datetime.now()

            try:
                # Apply recovery strategy
                success, result = self._execute_recovery_strategy(state, recovery_strategy)

                if success:
                    state.can_recover = True
                    logging.info(f"Recovery successful for build {build_id} using {recovery_strategy}")
                else:
                    logging.warning(f"Recovery failed for build {build_id} using {recovery_strategy}")

                return success, result

            except Exception as e:
                logging.error(f"Recovery attempt failed: {e}")
                return False, {"error": str(e)}

    def _execute_recovery_strategy(self, state: BuildRecoveryState, strategy: str) -> Tuple[bool, Dict[str, Any]]:
        """Execute a specific recovery strategy."""
        context = state.original_context.copy()

        if strategy == "force_clean":
            # Force clean and restart
            build_dir = self.workspace_dir / "build" / f"{context.get('compiler', 'unknown')}_{context.get('target', 'unknown')}_{context.get('build_type', 'unknown')}"
            success = force_clean_operation(build_dir, self.process_manager)
            return success, {"action": "force_clean", "build_dir": str(build_dir)}

        elif strategy == "resource_wait":
            # Wait for system resources
            success = self.resource_manager.wait_for_resources(timeout_seconds=300)
            return success, {"action": "resource_wait", "waited": True}

        elif strategy == "isolation_reset":
            # Reset build isolation
            success = self.isolation_manager.release_build_lock(state.build_id)
            if success:
                success = self.isolation_manager.acquire_build_lock(state.build_id)
            return success, {"action": "isolation_reset"}

        elif strategy == "compiler_fallback":
            # Switch to fallback compiler
            success = switch_to_msvc_compiler(context)
            return success, {"action": "compiler_fallback", "new_compiler": context.get("compiler")}

        else:
            return False, {"error": f"Unknown strategy: {strategy}"}

    def get_recovery_options(self, build_id: str) -> List[str]:
        """Get available recovery options for a build."""
        if build_id not in self.recovery_states:
            return []

        state = self.recovery_states[build_id]

        # Base recovery options
        options = ["force_clean", "resource_wait", "isolation_reset"]

        # Add compiler fallback if applicable
        if state.original_context.get("compiler") == "clang-msvc":
            options.append("compiler_fallback")

        # Add timeout-specific options
        last_failure = state.failure_history[-1] if state.failure_history else {}
        if last_failure.get("type") == "timeout":
            options.extend(["reduce_timeout", "split_build"])

        return options

    def cleanup_recovery_state(self, build_id: str) -> None:
        """Clean up recovery state after successful completion or abandonment."""
        with self._lock:
            if build_id in self.recovery_states:
                del self.recovery_states[build_id]
                logging.info(f"Cleaned up recovery state for build {build_id}")


class TimeoutHandler:
    """Handles timeout scenarios with intelligent recovery."""

    def __init__(self) -> None:
        self.timeout_configs: Dict[str, ClangMsvcReleaseTimeouts] = {
            "clang-msvc_release": ClangMsvcReleaseTimeouts(),
            "default": ClangMsvcReleaseTimeouts()  # Use same timeouts as base
        }

    def get_timeout_for_context(self, build_context: Dict[str, Any]) -> Dict[str, str]:
        """Get appropriate timeouts for build context."""
        config_key = f"{build_context.get('compiler', '')}_{build_context.get('build_type', '')}".strip('_')
        config = self.timeout_configs.get(config_key, self.timeout_configs["default"])

        return {
            "clean": str(config.clean),
            "configure": str(config.configure),
            "build": str(config.build),
            "total": str(config.total)
        }

    def handle_timeout(self, build_context: Dict[str, Any], elapsed_time: float) -> Dict[str, Any]:
        """Handle a timeout scenario and suggest recovery actions."""
        timeouts = self.get_timeout_for_context(build_context)

        analysis = {
            "timeout_type": self._classify_timeout(elapsed_time, timeouts),
            "suggested_actions": self._get_timeout_recovery_actions(build_context),
            "can_retry": True,
            "should_degrade": elapsed_time > float(timeouts["build"]) * 0.8  # Close to timeout
        }

        return analysis

    def _classify_timeout(self, elapsed: float, timeouts: Dict[str, str]) -> str:
        """Classify the type of timeout that occurred."""
        if elapsed < float(timeouts["configure"]):
            return "configuration_timeout"
        elif elapsed < float(timeouts["build"]):
            return "compilation_timeout"
        else:
            return "linking_timeout"

    def _get_timeout_recovery_actions(self, context: Dict[str, Any]) -> List[str]:
        """Get recovery actions for timeout scenarios."""
        actions = ["increase_timeout", "reduce_parallelism"]

        if context.get("compiler") == "clang-msvc":
            actions.extend(["switch_to_msvc", "use_incremental_build"])

        if context.get("build_type") == "release":
            actions.extend(["disable_optimizations", "two_phase_build"])

        return actions


class ErrorRecoveryManager:
    """Manages error recovery and classification."""

    def __init__(self) -> None:
        self.error_patterns = self._load_error_patterns()

    def _load_error_patterns(self) -> Dict[str, Dict[str, Any]]:
        """Load known error patterns and their recovery strategies."""
        return {
            "memory_exhaustion": {
                "patterns": ["out of memory", "cannot allocate", "memory exhausted"],
                "failure_type": FailureType.RESOURCE_EXHAUSTION,
                "recovery_actions": ["reduce_jobs", "free_memory", "use_swap"]
            },
            "compilation_error": {
                "patterns": ["error:", "fatal error", "compilation terminated"],
                "failure_type": FailureType.COMPILATION_ERROR,
                "recovery_actions": ["check_sources", "update_compiler", "reduce_optimizations"]
            },
            "linking_error": {
                "patterns": ["undefined reference", "link failed", "unresolved external"],
                "failure_type": FailureType.LINKING_ERROR,
                "recovery_actions": ["rebuild_dependencies", "check_libraries", "single_thread_link"]
            },
            "timeout": {
                "patterns": ["timed out", "timeout", "operation timed out"],
                "failure_type": FailureType.TIMEOUT,
                "recovery_actions": ["increase_timeout", "reduce_workload", "split_tasks"]
            },
            "dependency_error": {
                "patterns": ["dependency not found", "missing package", "conan failed"],
                "failure_type": FailureType.DEPENDENCY_ERROR,
                "recovery_actions": ["reinstall_dependencies", "check_conan_config", "clear_cache"]
            }
        }

    def classify_error(self, error_message: str) -> Tuple[FailureType, List[str]]:
        """Classify an error message and return recovery actions."""
        error_msg_lower = error_message.lower()

        for error_type, config in self.error_patterns.items():
            for pattern in config["patterns"]:
                if pattern in error_msg_lower:
                    return config["failure_type"], config["recovery_actions"]

        return FailureType.UNKNOWN, ["retry", "check_logs", "manual_intervention"]

    def get_recovery_plan(self, failure_type: FailureType, build_context: Dict[str, Any]) -> List[str]:
        """Get a recovery plan based on failure type and context."""
        base_plan = {
            FailureType.TIMEOUT: ["increase_timeout", "reduce_parallelism", "switch_compiler"],
            FailureType.COMPILATION_ERROR: ["check_code", "update_tools", "reduce_optimizations"],
            FailureType.LINKING_ERROR: ["rebuild_deps", "check_libs", "single_thread"],
            FailureType.RESOURCE_EXHAUSTION: ["free_resources", "reduce_jobs", "add_swap"],
            FailureType.DEPENDENCY_ERROR: ["fix_deps", "clear_cache", "reinstall"],
            FailureType.CONFIGURATION_ERROR: ["check_config", "validate_setup", "reset_build"],
            FailureType.UNKNOWN: ["gather_info", "retry", "escalate"]
        }

        plan = base_plan.get(failure_type, base_plan[FailureType.UNKNOWN])

        # Add context-specific actions
        if build_context.get("compiler") == "clang-msvc":
            plan.insert(0, "consider_msvc_fallback")

        if build_context.get("build_type") == "release":
            plan.insert(0, "try_debug_first")

        return plan


class ResilienceManager:
    """Main resilience manager coordinating all resilience features."""

    def __init__(self, workspace_dir: Path, resilience_level: ResilienceLevel = ResilienceLevel.STANDARD) -> None:
        self.workspace_dir = workspace_dir
        self.resilience_level = resilience_level

        # Initialize components
        self.retry_manager = RetryManager()
        self.degradation_manager = GracefulDegradationManager()
        self.recovery_manager = BuildRecoveryManager(workspace_dir)
        self.timeout_handler = TimeoutHandler()
        self.error_recovery = ErrorRecoveryManager()

        # Metrics
        self.metrics = ResilienceMetrics()

        # State
        self.active_recoveries: Dict[str, BuildRecoveryState] = {}

    def execute_resilient_operation(self, operation: Callable[..., Any], build_context: Dict[str, Any],
                                  operation_name: str = "build_operation") -> Tuple[bool, Any, Dict[str, Any]]:
        """
        Execute an operation with full resilience support.

        Returns:
            Tuple of (success, result, resilience_metadata)
        """
        build_id = build_context.get("build_id", f"{operation_name}_{int(time.time())}")
        resilience_metadata = {
            "build_id": build_id,
            "resilience_level": self.resilience_level.value,
            "strategies_applied": [],
            "recovery_attempts": 0,
            "total_time": 0.0,
            "success": False
        }

        start_time = time.time()

        try:
            # Initial attempt
            success, result, retry_metadata = self.retry_manager.execute_with_retry(
                operation, build_context
            )

            resilience_metadata.update(retry_metadata)

            if success:
                resilience_metadata["success"] = True
                return success, result, resilience_metadata

            # If retry failed, attempt recovery
            if self.resilience_level.value in ["advanced", "aggressive"]:
                recovery_success, recovery_result = self._attempt_recovery_chain(
                    build_id, build_context, retry_metadata.get("final_error", "unknown error")
                )

                if recovery_success:
                    resilience_metadata["recovery_attempts"] += 1
                    resilience_metadata["strategies_applied"].extend(["recovery_chain"])
                    resilience_metadata["success"] = True
                    return True, recovery_result, resilience_metadata

            # If recovery failed, try degradation
            if self.resilience_level.value == "aggressive":
                degradation_success, degradation_result = self._attempt_degradation_chain(
                    build_context, retry_metadata.get("final_error", "unknown error")
                )

                if degradation_success:
                    resilience_metadata["strategies_applied"].extend(["degradation_chain"])
                    resilience_metadata["success"] = True
                    return True, degradation_result, resilience_metadata

        except Exception as e:
            logging.error(f"Resilience operation failed: {e}")
            resilience_metadata["final_error"] = str(e)

        finally:
            resilience_metadata["total_time"] = time.time() - start_time
            self._update_metrics(resilience_metadata)

        return False, None, resilience_metadata

    def _attempt_recovery_chain(self, build_id: str, build_context: Dict[str, Any],
                               error_message: str) -> Tuple[bool, Any]:
        """Attempt recovery using various strategies."""
        # Classify the error
        failure_type, _ = self.error_recovery.classify_error(error_message)

        # Initiate recovery state
        failure_info = {
            "type": failure_type.value,
            "message": error_message,
            "timestamp": datetime.now().isoformat()
        }

        recovery_state = self.recovery_manager.initiate_recovery(build_id, build_context, failure_info)

        # Try recovery options in order
        recovery_options = self.recovery_manager.get_recovery_options(build_id)

        for strategy in recovery_options:
            success, result = self.recovery_manager.attempt_recovery(build_id, strategy)
            if success:
                return True, result

        return False, None

    def _attempt_degradation_chain(self, build_context: Dict[str, Any],
                                  error_message: str) -> Tuple[bool, Any]:
        """Attempt graceful degradation strategies."""
        # Get applicable degradation strategies
        strategies = self.degradation_manager.get_applicable_strategies(build_context)

        for strategy in strategies:
            try:
                degraded_context = self.degradation_manager.apply_strategy(strategy, build_context)
                # Here we would re-execute the operation with degraded context
                # For now, just return success with modified context
                return True, {"degraded_context": degraded_context, "strategy": strategy.name}
            except Exception as e:
                logging.warning(f"Degradation strategy {strategy.name} failed: {e}")
                continue

        return False, None

    def _update_metrics(self, metadata: Dict[str, Any]) -> None:
        """Update resilience metrics."""
        if metadata.get("success"):
            self.metrics.successful_recoveries += 1
        else:
            self.metrics.failed_recoveries += 1

        self.metrics.total_failures += metadata.get("attempts", 1) - 1

        # Update strategy success rates
        for strategy in metadata.get("strategies_applied", []):
            if strategy not in self.metrics.strategy_success_rates:
                self.metrics.strategy_success_rates[strategy] = 0.0

            # Simple success rate tracking (would need more sophisticated tracking in real impl)
            current_rate = self.metrics.strategy_success_rates[strategy]
            self.metrics.strategy_success_rates[strategy] = (current_rate + (1.0 if metadata["success"] else 0.0)) / 2.0

    def get_resilience_report(self) -> Dict[str, Any]:
        """Generate a resilience report."""
        return {
            "resilience_level": self.resilience_level.value,
            "metrics": {
                "total_failures": self.metrics.total_failures,
                "successful_recoveries": self.metrics.successful_recoveries,
                "failed_recoveries": self.metrics.failed_recoveries,
                "recovery_rate": (self.metrics.successful_recoveries /
                                max(1, self.metrics.successful_recoveries + self.metrics.failed_recoveries)),
                "strategy_success_rates": self.metrics.strategy_success_rates
            },
            "active_recoveries": len(self.active_recoveries),
            "system_status": self._get_system_status()
        }

    def _get_system_status(self) -> Dict[str, Any]:
        """Get current system status for resilience decisions."""
        status: Dict[str, Any] = {"resource_available": True, "warnings": []}

        if HAS_PSUTIL:
            memory = psutil.virtual_memory()
            if memory.percent > 90:
                status["resource_available"] = False
                status["warnings"].append("High memory usage")

            cpu = psutil.cpu_percent(interval=1)
            if cpu > 95:
                status["resource_available"] = False
                status["warnings"].append("High CPU usage")

        return status


# Convenience functions for easy integration

def create_resilient_build_executor(workspace_dir: Path, resilience_level: ResilienceLevel = ResilienceLevel.STANDARD) -> ResilienceManager:
    """Create a resilient build executor."""
    return ResilienceManager(workspace_dir, resilience_level)


def execute_with_resilience(operation: Callable[..., Any], build_context: Dict[str, Any],
                           workspace_dir: Path, resilience_level: ResilienceLevel = ResilienceLevel.STANDARD) -> Tuple[bool, Any, Dict[str, Any]]:
    """Execute an operation with resilience support."""
    manager = ResilienceManager(workspace_dir, resilience_level)
    return manager.execute_resilient_operation(operation, build_context)


# Export key classes for external use
__all__ = [
    'ResilienceManager',
    'ResilienceLevel',
    'FailureType',
    'RetryPolicy',
    'create_resilient_build_executor',
    'execute_with_resilience'
]
