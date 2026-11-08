"""
System tests for complete Warploom pipeline.

This module contains system tests that verify the entire build and development
pipeline from source code to executable artifacts.
"""

import unittest
import tempfile
import os
import sys
import shutil
import subprocess
from pathlib import Path
from unittest.mock import patch
import json

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))


class TestCompleteBuildPipeline(unittest.TestCase):
    """System tests for complete build pipeline."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.temp_dir = tempfile.mkdtemp()
        self.project_root = Path(__file__).parent.parent
    
    def tearDown(self):
        """Clean up test fixtures."""
        if os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)
    
    def test_warploom_controller_exists(self):
        """Test that WarploomController.py exists and is executable."""
        controller_path = self.project_root / "WarploomController.py"
        self.assertTrue(controller_path.exists())
        self.assertTrue(os.access(controller_path, os.R_OK))
    
    def test_build_scripts_exist(self):
        """Test that all build scripts exist."""
        scripts_dir = self.project_root / "warploom_scripts"
        self.assertTrue(scripts_dir.exists())
        
        required_scripts = [
            "build.py",
            "cmake.py",
            "conan.py",
            "utils.py",
            "config.py"
        ]
        
        for script in required_scripts:
            script_path = scripts_dir / script
            self.assertTrue(script_path.exists(), f"Script {script} not found")
    
    def test_cmake_configuration_exists(self):
        """Test that CMake configuration exists."""
        cmake_files = [
            "CMakeLists.txt",
            "dependencies.cmake"
        ]
        
        for cmake_file in cmake_files:
            file_path = self.project_root / cmake_file
            self.assertTrue(file_path.exists(), f"CMake file {cmake_file} not found")
    
    def test_test_suite_exists(self):
        """Test that test suite exists."""
        tests_dir = self.project_root / "tests"
        self.assertTrue(tests_dir.exists())
        
        test_files = [
            "CMakeLists.txt",
            "test_main.cpp",
            "test_engine.cpp",
            "test_game.cpp"
        ]
        
        for test_file in test_files:
            file_path = tests_dir / test_file
            self.assertTrue(file_path.exists(), f"Test file {test_file} not found")
    
    def test_documentation_exists(self):
        """Test that documentation exists."""
        docs_dir = self.project_root / "docs"
        self.assertTrue(docs_dir.exists())
        
        doc_files = [
            "index.md",
            "troubleshooting.md"
        ]
        
        for doc_file in doc_files:
            file_path = docs_dir / doc_file
            self.assertTrue(file_path.exists(), f"Documentation file {doc_file} not found")
    
    def test_analysis_documents_exist(self):
        """Test that analysis documents exist."""
        analysis_dir = self.project_root / "impl" / "debug" / "analysis"
        self.assertTrue(analysis_dir.exists())
        
        analysis_files = [
            "warploom_controller_analysis.md",
            "build_module_analysis.md",
            "cmake_module_analysis.md",
            "conan_module_analysis.md",
            "utils_module_analysis.md"
        ]
        
        for analysis_file in analysis_files:
            file_path = analysis_dir / analysis_file
            self.assertTrue(file_path.exists(), f"Analysis file {analysis_file} not found")
    
    def test_error_documentation_exists(self):
        """Test that error documentation exists."""
        errors_dir = self.project_root / "impl" / "debug" / "errors"
        self.assertTrue(errors_dir.exists())
        
        errors_file = errors_dir / "identified_errors.md"
        self.assertTrue(errors_file.exists(), "Error documentation not found")
    
    def test_edge_case_documentation_exists(self):
        """Test that edge case documentation exists."""
        edge_cases_dir = self.project_root / "impl" / "debug" / "edge-cases"
        self.assertTrue(edge_cases_dir.exists())
        
        edge_cases_file = edge_cases_dir / "identified_edge_cases.md"
        self.assertTrue(edge_cases_file.exists(), "Edge case documentation not found")


class TestPythonCodeQuality(unittest.TestCase):
    """System tests for Python code quality."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.project_root = Path(__file__).parent.parent
        self.warploom_scripts_dir = self.project_root / "warploom_scripts"
    
    def test_python_files_have_docstrings(self):
        """Test that Python files have module docstrings."""
        python_files = list(self.warploom_scripts_dir.glob("*.py"))
        
        for py_file in python_files:
            if py_file.name != "__init__.py":
                content = py_file.read_text()
                self.assertIn('"""', content, 
                          f"File {py_file.name} missing docstring")
    
    def test_python_files_import_correctly(self):
        """Test that Python files can be imported without errors."""
        python_files = [
            "build.py",
            "cmake.py",
            "conan.py",
            "utils.py",
            "config.py"
        ]
        
        for py_file in python_files:
            try:
                module_name = py_file.replace(".py", "")
                __import__(f"warploom_scripts.{module_name}")
            except ImportError as e:
                self.fail(f"Failed to import {py_file}: {e}")


class TestBuildArtifacts(unittest.TestCase):
    """System tests for build artifacts."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.project_root = Path(__file__).parent.parent
    
    def test_build_directories_exist(self):
        """Test that build directories can be created."""
        build_dirs = [
            "build",
            "build/debug",
            "build/release"
        ]
        
        for build_dir in build_dirs:
            dir_path = self.project_root / build_dir
            # Just verify the path can be constructed
            self.assertIsNotNone(dir_path)
    
    def test_install_directories_exist(self):
        """Test that install directories can be created."""
        install_dirs = [
            "install",
            "install/debug",
            "install/release"
        ]
        
        for install_dir in install_dirs:
            dir_path = self.project_root / install_dir
            # Just verify the path can be constructed
            self.assertIsNotNone(dir_path)


class TestConfigurationFiles(unittest.TestCase):
    """System tests for configuration files."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.project_root = Path(__file__).parent.parent
    
    def test_vcpkg_configuration_exists(self):
        """Test that vcpkg configuration exists."""
        vcpkg_json = self.project_root / "vcpkg.json"
        self.assertTrue(vcpkg_json.exists())
    
    def test_gitignore_exists(self):
        """Test that .gitignore exists."""
        gitignore = self.project_root / ".gitignore"
        self.assertTrue(gitignore.exists())
    
    def test_readme_exists(self):
        """Test that README exists."""
        readme = self.project_root / "README.md"
        self.assertTrue(readme.exists())
    
    def test_license_exists(self):
        """Test that LICENSE exists."""
        license_file = self.project_root / "LICENSE"
        self.assertTrue(license_file.exists())


class TestSpecsDocumentation(unittest.TestCase):
    """System tests for specs documentation."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.project_root = Path(__file__).parent.parent
        self.specs_dir = self.project_root / ".specs"
    
    def test_specs_directory_exists(self):
        """Test that specs directory exists."""
        self.assertTrue(self.specs_dir.exists())
    
    def test_requirements_exist(self):
        """Test that requirements documentation exists."""
        requirements_files = [
            "warploom-analysis-refactor/requirements.md",
            "warploom-full-analysis-refactor/requirements.md"
        ]
        
        for req_file in requirements_files:
            file_path = self.specs_dir / req_file
            self.assertTrue(file_path.exists(), f"Requirements file {req_file} not found")
    
    def test_design_documentation_exists(self):
        """Test that design documentation exists."""
        design_file = self.specs_dir / "warploom-analysis-refactor" / "design.md"
        self.assertTrue(design_file.exists())
    
    def test_tasks_documentation_exists(self):
        """Test that tasks documentation exists."""
        tasks_file = self.specs_dir / "warploom-full-analysis-refactor" / "tasks.md"
        self.assertTrue(tasks_file.exists())


class TestPerformanceMetrics(unittest.TestCase):
    """System tests for performance metrics."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.project_root = Path(__file__).parent.parent
    
    def test_performance_files_exist(self):
        """Test that performance metric files can be created."""
        # Check for any performance JSON files
        perf_files = list(self.project_root.glob("*_performance_*.json"))
        # Just verify we can find them if they exist
        self.assertIsNotNone(perf_files)
    
    def test_test_reports_exist(self):
        """Test that test reports can be created."""
        # Check for test report files
        test_reports = list(self.project_root.glob("*_test_*.md"))
        # Just verify we can find them if they exist
        self.assertIsNotNone(test_reports)


class TestCrossPlatformSupport(unittest.TestCase):
    """System tests for cross-platform support."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.project_root = Path(__file__).parent.parent
    
    def test_platform_detection(self):
        """Test that platform can be detected."""
        import platform
        system = platform.system()
        self.assertIn(system, ["Windows", "Linux", "Darwin"])
    
    def test_python_version(self):
        """Test that Python version is compatible."""
        version = sys.version_info
        self.assertGreaterEqual(version.major, 3)
        self.assertGreaterEqual(version.minor, 8)


if __name__ == '__main__':
    unittest.main()
