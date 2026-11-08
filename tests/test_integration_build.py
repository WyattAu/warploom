"""
Integration tests for Warploom build system.

This module contains integration tests that verify the complete build pipeline
from configuration to artifact generation.
"""

import unittest
import tempfile
import os
import sys
import shutil
from pathlib import Path
from unittest.mock import patch, Mock
import json

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.build import BuildManager, BuildContext
from warploom_scripts.cmake import CMakeManager
from warploom_scripts.conan import ConanManager


class TestBuildPipelineIntegration(unittest.TestCase):
    """Integration tests for complete build pipeline."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.temp_dir = tempfile.mkdtemp()
        self.build_dir = os.path.join(self.temp_dir, "build")
        os.makedirs(self.build_dir, exist_ok=True)
        
        self.build_manager = BuildManager()
        self.cmake_manager = CMakeManager()
        self.conan_manager = ConanManager()
    
    def tearDown(self):
        """Clean up test fixtures."""
        if os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)
    
    @patch('warploom_scripts.build.run_command')
    def test_full_build_pipeline(self, mock_run):
        """Test complete build pipeline from configure to install."""
        # Mock successful command execution
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="standalone",
            build_type="debug",
            compiler="msvc",
            preset="default"
        )
        
        # Configure
        configure_result = self.build_manager.configure(context)
        self.assertTrue(configure_result)
        
        # Build
        build_result = self.build_manager.build(context)
        self.assertTrue(build_result)
        
        # Install
        install_result = self.build_manager.install(context)
        self.assertTrue(install_result)
    
    @patch('warploom_scripts.build.run_command')
    def test_clean_build_pipeline(self, mock_run):
        """Test clean build pipeline."""
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="standalone",
            build_type="release",
            compiler="msvc",
            preset="default"
        )
        
        # Clean
        clean_result = self.build_manager.clean(context)
        self.assertTrue(clean_result)
        
        # Configure
        configure_result = self.build_manager.configure(context)
        self.assertTrue(configure_result)
        
        # Build
        build_result = self.build_manager.build(context)
        self.assertTrue(build_result)
    
    @patch('warploom_scripts.cmake.run_command')
    def test_cmake_integration(self, mock_run):
        """Test CMake integration with build system."""
        mock_run.return_value = (0, "", "")
        
        # Generate
        generate_result = self.cmake_manager.generate(
            source_dir=".",
            build_dir=self.build_dir,
            preset="default",
            toolchain=None
        )
        self.assertTrue(generate_result)
        
        # Build
        build_result = self.cmake_manager.build(
            build_dir=self.build_dir,
            target="all",
            config="debug"
        )
        self.assertTrue(build_result)
        
        # Install
        install_result = self.cmake_manager.install(
            build_dir=self.build_dir,
            config="debug"
        )
        self.assertTrue(install_result)
    
    @patch('warploom_scripts.conan.run_command')
    def test_conan_integration(self, mock_run):
        """Test Conan integration with build system."""
        mock_run.return_value = (0, "", "")
        
        # Install dependencies
        install_result = self.conan_manager.install(
            profile="default",
            build_type="debug",
            settings=None
        )
        self.assertTrue(install_result)
        
        # Create profile
        profile_result = self.conan_manager.create_profile(
            name="test_profile",
            settings={"compiler": "msvc", "build_type": "Debug"}
        )
        self.assertTrue(profile_result)


class TestMultiCompilerIntegration(unittest.TestCase):
    """Integration tests for multiple compiler support."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.temp_dir = tempfile.mkdtemp()
        self.build_manager = BuildManager()
    
    def tearDown(self):
        """Clean up test fixtures."""
        if os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)
    
    @patch('warploom_scripts.build.run_command')
    def test_msvc_build(self, mock_run):
        """Test MSVC compiler build."""
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="standalone",
            build_type="debug",
            compiler="msvc",
            preset="default"
        )
        
        result = self.build_manager.build(context)
        self.assertTrue(result)
    
    @patch('warploom_scripts.build.run_command')
    def test_mingw_gcc_build(self, mock_run):
        """Test MinGW-GCC compiler build."""
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="standalone",
            build_type="debug",
            compiler="mingw-gcc",
            preset="default"
        )
        
        result = self.build_manager.build(context)
        self.assertTrue(result)
    
    @patch('warploom_scripts.build.run_command')
    def test_clang_mingw_build(self, mock_run):
        """Test Clang-MinGW compiler build."""
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="standalone",
            build_type="debug",
            compiler="mingw-clang",
            preset="default"
        )
        
        result = self.build_manager.build(context)
        self.assertTrue(result)


class TestBuildTypeIntegration(unittest.TestCase):
    """Integration tests for different build types."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.temp_dir = tempfile.mkdtemp()
        self.build_manager = BuildManager()
    
    def tearDown(self):
        """Clean up test fixtures."""
        if os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)
    
    @patch('warploom_scripts.build.run_command')
    def test_debug_build(self, mock_run):
        """Test debug build configuration."""
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="standalone",
            build_type="debug",
            compiler="msvc",
            preset="default"
        )
        
        result = self.build_manager.build(context)
        self.assertTrue(result)
    
    @patch('warploom_scripts.build.run_command')
    def test_release_build(self, mock_run):
        """Test release build configuration."""
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="standalone",
            build_type="release",
            compiler="msvc",
            preset="default"
        )
        
        result = self.build_manager.build(context)
        self.assertTrue(result)


class TestTargetIntegration(unittest.TestCase):
    """Integration tests for different build targets."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.temp_dir = tempfile.mkdtemp()
        self.build_manager = BuildManager()
    
    def tearDown(self):
        """Clean up test fixtures."""
        if os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)
    
    @patch('warploom_scripts.build.run_command')
    def test_standalone_target(self, mock_run):
        """Test standalone target build."""
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="standalone",
            build_type="debug",
            compiler="msvc",
            preset="default"
        )
        
        result = self.build_manager.build(context)
        self.assertTrue(result)
    
    @patch('warploom_scripts.build.run_command')
    def test_library_target(self, mock_run):
        """Test library target build."""
        mock_run.return_value = (0, "", "")
        
        context = BuildContext(
            target="targets/qt-vulkan/library",
            build_type="debug",
            compiler="msvc",
            preset="default"
        )
        
        result = self.build_manager.build(context)
        self.assertTrue(result)


class TestErrorHandlingIntegration(unittest.TestCase):
    """Integration tests for error handling."""
    
    def setUp(self):
        """Set up test fixtures."""
        self.temp_dir = tempfile.mkdtemp()
        self.build_manager = BuildManager()
    
    def tearDown(self):
        """Clean up test fixtures."""
        if os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)
    
    @patch('warploom_scripts.build.run_command')
    def test_build_failure_handling(self, mock_run):
        """Test handling of build failures."""
        # Mock build failure
        mock_run.return_value = (1, "", "Build error")
        
        context = BuildContext(
            target="standalone",
            build_type="debug",
            compiler="msvc",
            preset="default"
        )
        
        result = self.build_manager.build(context)
        self.assertFalse(result)
    
    @patch('warploom_scripts.build.run_command')
    def test_configure_failure_handling(self, mock_run):
        """Test handling of configuration failures."""
        # Mock configure failure
        mock_run.return_value = (1, "", "Configure error")
        
        context = BuildContext(
            target="standalone",
            build_type="debug",
            compiler="msvc",
            preset="default"
        )
        
        result = self.build_manager.configure(context)
        self.assertFalse(result)


if __name__ == '__main__':
    unittest.main()
