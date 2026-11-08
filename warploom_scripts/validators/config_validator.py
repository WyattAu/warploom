# warploom_scripts/validators/config_validator.py
"""
Configuration file validation for Warploom project.

This module provides schema-based validation for:
- CMakePresets.json
- CMakeUserPresets.json
- conanfile.py
- Project configuration files
"""

import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Union
from enum import Enum

from ..error_handler import ValidationError, ErrorSeverity, create_error_context

logger = logging.getLogger(__name__)


class ValidationLevel(Enum):
    """Validation strictness levels"""
    LAX = "lax"
    STRICT = "strict"
    PEDANTIC = "pedantic"


@dataclass
class ValidationResult:
    """Result of a validation operation"""
    is_valid: bool
    errors: List[str]
    warnings: List[str]
    file_path: Path
    schema_version: Optional[str] = None

    def __str__(self) -> str:
        status = "✓ VALID" if self.is_valid else "✗ INVALID"
        result = f"{status}: {self.file_path}"

        if self.errors:
            result += f"\nErrors ({len(self.errors)}):"
            for error in self.errors:
                result += f"\n  - {error}"

        if self.warnings:
            result += f"\nWarnings ({len(self.warnings)}):"
            for warning in self.warnings:
                result += f"\n  - {warning}"

        return result


class ConfigValidator:
    """Configuration file validator with schema validation"""

    def __init__(self, validation_level: ValidationLevel = ValidationLevel.STRICT):
        self.validation_level = validation_level
        self.schemas = self._load_schemas()

    def _load_schemas(self) -> Dict[str, Dict[str, Any]]:
        """Load validation schemas"""
        schemas = {}

        # CMakePresets.json schema (simplified)
        schemas['CMakePresets.json'] = {
            'required_fields': ['version', 'configurePresets'],
            'version': {'min': 3, 'max': 10},
            'configurePresets': {
                'type': 'array',
                'item_schema': {
                    'required': ['name'],
                    'optional': ['description', 'inherits', 'cacheVariables', 'environment']
                }
            }
        }

        # Conanfile schema (simplified)
        schemas['conanfile.py'] = {
            'required_patterns': [
                r'class.*ConanFile',
                r'def\s+configure\s*\(',
                r'def\s+package_info\s*\('
            ]
        }

        return schemas

    def validate_file(self, file_path: Union[str, Path]) -> ValidationResult:
        """Validate a configuration file"""
        file_path = Path(file_path)

        if not file_path.exists():
            return ValidationResult(
                is_valid=False,
                errors=[f"File does not exist: {file_path}"],
                warnings=[],
                file_path=file_path
            )

        try:
            if file_path.name.endswith('.json'):
                return self._validate_json_file(file_path)
            elif file_path.name.endswith('.py'):
                return self._validate_python_file(file_path)
            else:
                return ValidationResult(
                    is_valid=True,
                    errors=[],
                    warnings=[f"No specific validation available for {file_path.suffix} files"],
                    file_path=file_path
                )
        except Exception as e:
            return ValidationResult(
                is_valid=False,
                errors=[f"Validation failed: {str(e)}"],
                warnings=[],
                file_path=file_path
            )

    def _validate_json_file(self, file_path: Path) -> ValidationResult:
        """Validate JSON configuration files"""
        errors: List[str] = []
        warnings: List[str] = []

        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = json.load(f)
        except json.JSONDecodeError as e:
            errors.append(f"Invalid JSON: {str(e)}")
            return ValidationResult(False, errors, warnings, file_path)

        # Get schema for this file
        schema = self.schemas.get(file_path.name)
        if not schema:
            warnings.append(f"No validation schema available for {file_path.name}")
            return ValidationResult(True, errors, warnings, file_path)

        # Validate required fields
        if 'required_fields' in schema:
            for field in schema['required_fields']:
                if field not in content:
                    errors.append(f"Missing required field: {field}")

        # Validate version if present
        if 'version' in schema and 'version' in content:
            version = content['version']
            version_req = schema['version']
            if not (version_req['min'] <= version <= version_req['max']):
                errors.append(f"Version {version} not in supported range {version_req['min']}-{version_req['max']}")

        # Validate configurePresets array
        if 'configurePresets' in schema and 'configurePresets' in content:
            presets = content['configurePresets']
            if not isinstance(presets, list):
                errors.append("configurePresets must be an array")
            else:
                preset_schema = schema['configurePresets']['item_schema']
                for i, preset in enumerate(presets):
                    if not isinstance(preset, dict):
                        errors.append(f"configurePresets[{i}] must be an object")
                        continue

                    # Check required fields
                    for req_field in preset_schema.get('required', []):
                        if req_field not in preset:
                            errors.append(f"configurePresets[{i}] missing required field: {req_field}")

                    # Check for duplicate names
                    name = preset.get('name')
                    if name:
                        duplicates = [p for p in presets if p.get('name') == name]
                        if len(duplicates) > 1:
                            errors.append(f"Duplicate preset name: {name}")

        # Additional validations based on validation level
        if self.validation_level in [ValidationLevel.STRICT, ValidationLevel.PEDANTIC]:
            # Check for unused cache variables
            if 'configurePresets' in content:
                for preset in content['configurePresets']:
                    cache_vars = preset.get('cacheVariables', {})
                    if cache_vars:
                        # This would check for common CMake variables
                        pass

        return ValidationResult(
            is_valid=len(errors) == 0,
            errors=errors,
            warnings=warnings,
            file_path=file_path,
            schema_version=content.get('version')
        )

    def _validate_python_file(self, file_path: Path) -> ValidationResult:
        """Validate Python configuration files"""
        errors: List[str] = []
        warnings: List[str] = []

        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
        except UnicodeDecodeError:
            errors.append("File encoding is not UTF-8")
            return ValidationResult(False, errors, warnings, file_path)

        # Get schema for this file
        schema = self.schemas.get(file_path.name)
        if schema and 'required_patterns' in schema:
            for pattern in schema['required_patterns']:
                if not __import__('re').search(pattern, content):
                    errors.append(f"Missing required pattern: {pattern}")

        # Basic Python syntax check
        try:
            compile(content, str(file_path), 'exec')
        except SyntaxError as e:
            errors.append(f"Syntax error: {str(e)}")

        return ValidationResult(
            is_valid=len(errors) == 0,
            errors=errors,
            warnings=warnings,
            file_path=file_path
        )

    def validate_project_configs(self, project_root: Optional[Path] = None) -> List[ValidationResult]:
        """Validate all configuration files in the project"""
        if project_root is None:
            project_root = Path.cwd()

        config_files = [
            project_root / "CMakePresets.json",
            project_root / "CMakeUserPresets.json",
            project_root / "conan" / "conanfile.py",
            project_root / ".vscode" / "tasks.json",
            project_root / ".vscode" / "launch.json"
        ]

        results = []
        for config_file in config_files:
            if config_file.exists():
                result = self.validate_file(config_file)
                results.append(result)

                if not result.is_valid:
                    logger.warning(f"Configuration validation failed for {config_file}")
                elif result.warnings:
                    logger.info(f"Configuration validation passed with warnings for {config_file}")

        return results

    def validate_with_error_handling(self, file_path: Union[str, Path]) -> ValidationResult:
        """Validate a file with proper error handling"""
        try:
            result = self.validate_file(file_path)
            if not result.is_valid:
                error = ValidationError(
                    f"Configuration validation failed for {file_path}",
                    severity=ErrorSeverity.HIGH,
                    context=create_error_context(
                        file_path=str(file_path),
                        errors=result.errors,
                        warnings=result.warnings
                    )
                )
                # Log the error but don't raise it - validation errors are expected
                logger.error(str(error))
            return result
        except Exception as e:
            error = ValidationError(
                f"Unexpected error during configuration validation: {str(e)}",
                severity=ErrorSeverity.MEDIUM,
                context=create_error_context(file_path=str(file_path))
            )
            logger.error(str(error))
            return ValidationResult(
                is_valid=False,
                errors=[str(e)],
                warnings=[],
                file_path=Path(file_path)
            )
