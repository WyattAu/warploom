"""
Comprehensive test runner for Warploom project.

This script runs all test suites and generates a comprehensive test report.
"""

import unittest
import sys
import os
from pathlib import Path
import json
from datetime import datetime
import traceback


class TestRunner:
    """Test runner for all test suites."""
    
    def __init__(self):
        """Initialize test runner."""
        self.results = {
            "timestamp": datetime.now().isoformat(),
            "suites": {},
            "summary": {
                "total": 0,
                "passed": 0,
                "failed": 0,
                "errors": 0,
                "skipped": 0
            }
        }
    
    def run_suite(self, suite_name, test_module):
        """Run a test suite and collect results."""
        print(f"\n{'='*60}")
        print(f"Running {suite_name}")
        print(f"{'='*60}")
        
        loader = unittest.TestLoader()
        suite = loader.loadTestsFromModule(test_module)
        runner = unittest.TextTestRunner(verbosity=2)
        result = runner.run(suite)
        
        suite_results = {
            "tests_run": result.testsRun,
            "failures": len(result.failures),
            "errors": len(result.errors),
            "skipped": len(result.skipped),
            "success": result.wasSuccessful()
        }
        
        self.results["suites"][suite_name] = suite_results
        self.results["summary"]["total"] += result.testsRun
        self.results["summary"]["passed"] += result.testsRun - len(result.failures) - len(result.errors)
        self.results["summary"]["failed"] += len(result.failures)
        self.results["summary"]["errors"] += len(result.errors)
        self.results["summary"]["skipped"] += len(result.skipped)
        
        return suite_results
    
    def generate_report(self):
        """Generate test report."""
        print(f"\n{'='*60}")
        print("TEST SUMMARY")
        print(f"{'='*60}")
        print(f"Total Tests: {self.results['summary']['total']}")
        print(f"Passed: {self.results['summary']['passed']}")
        print(f"Failed: {self.results['summary']['failed']}")
        print(f"Errors: {self.results['summary']['errors']}")
        print(f"Skipped: {self.results['summary']['skipped']}")
        
        success_rate = (self.results['summary']['passed'] / self.results['summary']['total'] * 100) if self.results['summary']['total'] > 0 else 0
        print(f"Success Rate: {success_rate:.2f}%")
        
        # Save JSON report
        report_path = Path(__file__).parent / "test_report.json"
        with open(report_path, 'w') as f:
            json.dump(self.results, f, indent=2)
        
        print(f"\nDetailed report saved to: {report_path}")
        
        return self.results['summary']['failed'] == 0 and self.results['summary']['errors'] == 0


def main():
    """Main entry point."""
    print("Warploom Comprehensive Test Suite")
    print(f"Started at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    
    runner = TestRunner()
    
    # Add parent directory to path
    sys.path.insert(0, str(Path(__file__).parent.parent))
    
    # Run system tests
    try:
        import test_system
        runner.run_suite("System Tests", test_system)
    except Exception as e:
        print(f"Error running system tests: {e}")
        traceback.print_exc()
    
    # Run build system tests
    try:
        import test_build_system
        runner.run_suite("Build System Unit Tests", test_build_system)
    except Exception as e:
        print(f"Error running build system tests: {e}")
        traceback.print_exc()
    
    # Run integration tests
    try:
        import test_integration_build
        runner.run_suite("Build System Integration Tests", test_integration_build)
    except Exception as e:
        print(f"Error running integration tests: {e}")
        traceback.print_exc()
    
    # Generate final report
    success = runner.generate_report()
    
    print(f"\n{'='*60}")
    if success:
        print("ALL TESTS PASSED!")
    else:
        print("SOME TESTS FAILED!")
    print(f"{'='*60}")
    
    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
