# omni_scripts/validators/dependency_validator.py
"""
Dependency validation for OmniCPP project.

This module validates:
- Dependency integrity and versions
- License compliance
- Security vulnerabilities
- Dependency conflicts
"""

import hashlib
import json
import logging
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple
from urllib.parse import urlparse

from ..error_handler import ValidationError, ErrorSeverity, create_error_context

logger = logging.getLogger(__name__)


@dataclass
class DependencyInfo:
    """Information about a dependency"""
    name: str
    version: str
    source: str  # 'conan', 'cmake', 'system'
    license: Optional[str] = None
    url: Optional[str] = None
    hash_value: Optional[str] = None
    vulnerabilities: Optional[List[str]] = None

    def __post_init__(self) -> None:
        if self.vulnerabilities is None:
            self.vulnerabilities = []


@dataclass
class DependencyValidationResult:
    """Result of dependency validation"""
    is_valid: bool
    dependencies: List[DependencyInfo]
    conflicts: List[str]
    security_issues: List[str]
    license_issues: List[str]
    errors: List[str]
    warnings: List[str]


class DependencyValidator:
    """Dependency validator for integrity, security, and compliance"""

    def __init__(self) -> None:
        self.known_vulnerabilities = self._load_vulnerability_database()
        self.license_whitelist = self._load_license_whitelist()

    def _load_vulnerability_database(self) -> Dict[str, List[str]]:
        """Load known vulnerability database"""
        # This would typically load from a file or API
        # For now, return empty dict
        return {}

    def _load_license_whitelist(self) -> Set[str]:
        """Load approved license list"""
        return {
            'MIT',
            'BSD-2-Clause',
            'BSD-3-Clause',
            'Apache-2.0',
            'ISC',
            'Zlib',
            'Boost-1.0',
            'CC0-1.0',
            'Unlicense'
        }

    def validate_conan_dependencies(self, conanfile_path: Path) -> List[DependencyInfo]:
        """Validate Conan dependencies"""
        dependencies: List[DependencyInfo] = []

        if not conanfile_path.exists():
            return dependencies

        try:
            # Run conan info to get dependency information
            result = subprocess.run(
                ['conan', 'info', str(conanfile_path), '--json'],
                capture_output=True,
                text=True,
                cwd=conanfile_path.parent
            )

            if result.returncode == 0:
                info_data = json.loads(result.stdout)
                for dep in info_data:
                    dep_info = DependencyInfo(
                        name=dep.get('name', 'unknown'),
                        version=dep.get('version', 'unknown'),
                        source='conan',
                        license=dep.get('license'),
                        url=dep.get('url')
                    )
                    dependencies.append(dep_info)

        except (subprocess.SubprocessError, json.JSONDecodeError, FileNotFoundError):
            logger.warning("Failed to analyze Conan dependencies")

        return dependencies

    def validate_cmake_dependencies(self, cmake_file: Path) -> List[DependencyInfo]:
        """Validate CMake dependencies (CPM.cmake)"""
        dependencies: List[DependencyInfo] = []

        if not cmake_file.exists():
            return dependencies

        try:
            with open(cmake_file, 'r', encoding='utf-8') as f:
                content = f.read()

            # Look for CPMAddPackage calls
            import re
            cpm_pattern = r'CPMAddPackage\(\s*NAME\s+(\w+).*?VERSION\s+([^\s)]+)'
            matches = re.findall(cpm_pattern, content, re.DOTALL)

            for name, version in matches:
                dep_info = DependencyInfo(
                    name=name,
                    version=version.strip('"\''),
                    source='cmake'
                )
                dependencies.append(dep_info)

        except Exception as e:
            logger.warning(f"Failed to analyze CMake dependencies: {e}")

        return dependencies

    def validate_system_dependencies(self) -> List[DependencyInfo]:
        """Validate system-level dependencies"""
        dependencies: List[DependencyInfo] = []

        # Check for common system dependencies
        system_deps = [
            ('cmake', 'CMake'),
            ('ninja', 'Ninja'),
            ('gcc', 'GCC'),
            ('clang', 'Clang'),
            ('python3', 'Python 3')
        ]

        for cmd, name in system_deps:
            try:
                result = subprocess.run(
                    [cmd, '--version'],
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                if result.returncode == 0:
                    # Extract version from first line
                    version_line = result.stdout.split('\n')[0]
                    version = version_line.split()[-1] if version_line.split() else 'unknown'

                    dep_info = DependencyInfo(
                        name=name,
                        version=version,
                        source='system'
                    )
                    dependencies.append(dep_info)
            except (subprocess.SubprocessError, subprocess.TimeoutExpired):
                continue

        return dependencies

    def check_dependency_conflicts(self, dependencies: List[DependencyInfo]) -> List[str]:
        """Check for dependency conflicts"""
        conflicts: List[str] = []

        # Group by name
        dep_groups: Dict[str, List[DependencyInfo]] = {}
        for dep in dependencies:
            if dep.name not in dep_groups:
                dep_groups[dep.name] = []
            dep_groups[dep.name].append(dep)

        # Check for version conflicts
        for name, deps in dep_groups.items():
            if len(deps) > 1:
                versions = set(dep.version for dep in deps)
                if len(versions) > 1:
                    conflicts.append(
                        f"Version conflict for {name}: {', '.join(sorted(versions))}"
                    )

        return conflicts

    def check_security_vulnerabilities(self, dependencies: List[DependencyInfo]) -> List[str]:
        """Check for known security vulnerabilities"""
        issues: List[str] = []

        for dep in dependencies:
            vuln_key = f"{dep.name}:{dep.version}"
            if vuln_key in self.known_vulnerabilities:
                vulnerabilities = self.known_vulnerabilities[vuln_key]
                for vuln in vulnerabilities:
                    issues.append(f"Security vulnerability in {dep.name} {dep.version}: {vuln}")

        return issues

    def check_license_compliance(self, dependencies: List[DependencyInfo]) -> List[str]:
        """Check license compliance"""
        issues: List[str] = []

        for dep in dependencies:
            if dep.license and dep.license not in self.license_whitelist:
                issues.append(
                    f"License '{dep.license}' for {dep.name} is not in approved list"
                )
            elif not dep.license:
                issues.append(f"Missing license information for {dep.name}")

        return issues

    def validate_dependency_integrity(self, dep_path: Path) -> Tuple[bool, str]:
        """Validate dependency file integrity"""
        if not dep_path.exists():
            return False, "Dependency file does not exist"

        try:
            # Calculate file hash
            hasher = hashlib.sha256()
            with open(dep_path, 'rb') as f:
                for chunk in iter(lambda: f.read(4096), b""):
                    hasher.update(chunk)

            file_hash = hasher.hexdigest()

            # Check file size
            size = dep_path.stat().st_size
            if size == 0:
                return False, "Dependency file is empty"

            # Basic validation based on file type
            if dep_path.suffix in ['.so', '.dylib', '.dll']:
                # Binary library validation would go here
                pass
            elif dep_path.suffix in ['.a', '.lib']:
                # Static library validation would go here
                pass

            return True, f"File integrity OK (SHA256: {file_hash[:16]}...)"

        except Exception as e:
            return False, f"Integrity check failed: {str(e)}"

    def validate_all_dependencies(self, project_root: Path) -> DependencyValidationResult:
        """Validate all project dependencies"""
        all_dependencies: List[DependencyInfo] = []

        # Collect dependencies from different sources
        conanfile = project_root / "conan" / "conanfile.py"
        cmake_deps = project_root / "dependencies.cmake"

        all_dependencies.extend(self.validate_conan_dependencies(conanfile))
        all_dependencies.extend(self.validate_cmake_dependencies(cmake_deps))
        all_dependencies.extend(self.validate_system_dependencies())

        # Perform validations
        conflicts = self.check_dependency_conflicts(all_dependencies)
        security_issues = self.check_security_vulnerabilities(all_dependencies)
        license_issues = self.check_license_compliance(all_dependencies)

        errors: List[str] = []
        warnings: List[str] = []

        # Check for missing dependencies. System deps are recorded under a
        # DISPLAY name ('Python 3', 'CMake') while this list names the COMMAND
        # ('python3', 'cmake'), so the two were compared directly and never
        # matched -- reporting 'Required system dependency not found: python3'
        # on machines where python3 was demonstrably running the check.
        # Normalising both sides (case, spaces, underscores) makes the
        # comparison mean what it says.
        def _normalize(name: str) -> str:
            return ''.join(ch for ch in name.lower() if ch.isalnum())

        required_deps = ['cmake', 'ninja', 'python3']
        found_system_deps = {
            _normalize(dep.name)
            for dep in all_dependencies
            if dep.source == 'system'
        }

        for req_dep in required_deps:
            if _normalize(req_dep) not in found_system_deps:
                errors.append(f"Required system dependency not found: {req_dep}")

        # Check dependency integrity in _deps directory
        deps_dir = project_root / "_deps"
        if deps_dir.exists():
            for item in deps_dir.rglob("*"):
                if item.is_file() and item.suffix in ['.a', '.so', '.dylib', '.lib', '.dll']:
                    is_valid, message = self.validate_dependency_integrity(item)
                    if not is_valid:
                        warnings.append(f"Dependency integrity issue: {item} - {message}")

        return DependencyValidationResult(
            is_valid=len(errors) == 0 and len(security_issues) == 0,
            dependencies=all_dependencies,
            conflicts=conflicts,
            security_issues=security_issues,
            license_issues=license_issues,
            errors=errors,
            warnings=warnings
        )

    def validate_with_error_handling(self, project_root: Path) -> DependencyValidationResult:
        """Validate dependencies with proper error handling"""
        try:
            result = self.validate_all_dependencies(project_root)

            if not result.is_valid or result.security_issues:
                severity = ErrorSeverity.CRITICAL if result.security_issues else ErrorSeverity.HIGH
                error = ValidationError(
                    f"Dependency validation failed for {project_root}",
                    severity=severity,
                    context=create_error_context(
                        project_root=str(project_root),
                        errors=result.errors,
                        security_issues=result.security_issues,
                        conflicts=result.conflicts,
                        license_issues=result.license_issues
                    )
                )
                logger.error(str(error))

            return result

        except Exception as e:
            error = ValidationError(
                f"Unexpected error during dependency validation: {str(e)}",
                severity=ErrorSeverity.MEDIUM,
                context=create_error_context(project_root=str(project_root))
            )
            logger.error(str(error))

            return DependencyValidationResult(
                is_valid=False,
                dependencies=[],
                conflicts=[],
                security_issues=[],
                license_issues=[],
                errors=[str(e)],
                warnings=[]
            )
