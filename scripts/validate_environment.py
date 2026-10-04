#!/usr/bin/env python3
"""
OmniCpp Environment Validation Script

This script validates the development environment for OmniCpp builds,
checking for required tools, compilers, and dependencies.
"""

import os
import shutil
import sys
import subprocess
import platform
from pathlib import Path
from typing import List, Tuple, Optional

# Add parent directory to path to import omni_scripts
sys.path.insert(0, str(Path(__file__).parent.parent))

from omni_scripts.validators import ConfigValidator, BuildValidator, DependencyValidator


class ToolchainValidator:
    """Validates and installs cross-compilation toolchains."""

    def __init__(self, arch: str):
        self.arch = arch
        self.system = platform.system().lower()
        self.is_linux = self.system == "linux"
        self.required_tools = self._get_required_tools()

    def _get_required_tools(self) -> List[str]:
        """Get required tools for the target architecture."""
        toolchains = {
            "arm64-linux-gnu": ["aarch64-linux-gnu-gcc", "aarch64-linux-gnu-g++"],
            "x86-linux-gnu": ["x86_64-linux-gnu-gcc", "x86_64-linux-gnu-g++"]
        }
        return toolchains.get(self.arch, [])

    def install_missing_tools(self) -> bool:
        """Attempt to install missing tools automatically."""
        if self.is_linux:
            return self._install_linux_tools()
        return False

    def _install_linux_tools(self) -> bool:
        """Install Linux cross-compilation tools."""
        if not self.required_tools:
            return True

        # Try different package managers
        for pm in ["apt", "dnf", "pacman"]:
            if self._has_package_manager(pm):
                return self._install_with_pm(pm, self.required_tools)
        return False

    def _has_package_manager(self, pm: str) -> bool:
        """Check if package manager is available."""
        try:
            subprocess.run([pm, "--version"], capture_output=True, check=True)
            return True
        except (subprocess.CalledProcessError, FileNotFoundError):
            return False

    def _install_with_pm(self, pm: str, tools: List[str]) -> bool:
        """Install tools using the specified package manager."""
        package_map: dict[str, dict[str, str]] = {
            "apt": {
                "aarch64-linux-gnu-gcc": "gcc-aarch64-linux-gnu",
                "aarch64-linux-gnu-g++": "g++-aarch64-linux-gnu",
                "x86_64-linux-gnu-gcc": "gcc-x86-64-linux-gnu",
                "x86_64-linux-gnu-g++": "g++-x86-64-linux-gnu"
            },
            "dnf": {
                "aarch64-linux-gnu-gcc": "gcc-aarch64-linux-gnu",
                "aarch64-linux-gnu-g++": "gcc-c++-aarch64-linux-gnu",
                "x86_64-linux-gnu-gcc": "gcc-x86_64-linux-gnu",
                "x86_64-linux-gnu-g++": "gcc-c++-x86_64-linux-gnu"
            },
            "pacman": {
                "aarch64-linux-gnu-gcc": "aarch64-linux-gnu-gcc",
                "aarch64-linux-gnu-g++": "aarch64-linux-gnu-gcc",  # pacman often bundles
                "x86_64-linux-gnu-gcc": "x86_64-linux-gnu-gcc",
                "x86_64-linux-gnu-g++": "x86_64-linux-gnu-gcc"
            }
        }

        packages: list[str] = []
        for tool in tools:
            pkg = package_map.get(pm, {}).get(tool)
            if pkg:
                packages.append(pkg)  # type: ignore[arg-type]

        if not packages:
            return False

        try:
            cmd = [pm, "-y", "install"] + packages if pm == "dnf" else [pm, "install", "-y"] + packages if pm == "apt" else [pm, "-S", "--noconfirm"] + packages
            subprocess.run(cmd, check=True)
            return True
        except subprocess.CalledProcessError:
            return False

    def _get_installation_commands(self, arch: str, missing_tools: List[str]) -> str:
        """Get installation commands for missing tools."""
        if not self.is_linux:
            return ""

        commands: list[str] = []
        for pm in ["apt", "dnf", "pacman"]:
            if self._has_package_manager(pm):
                package_map = {
                    "apt": {
                        "aarch64-linux-gnu-gcc": "gcc-aarch64-linux-gnu",
                        "aarch64-linux-gnu-g++": "g++-aarch64-linux-gnu",
                        "x86_64-linux-gnu-gcc": "gcc-x86-64-linux-gnu",
                        "x86_64-linux-gnu-g++": "g++-x86-64-linux-gnu"
                    },
                    "dnf": {
                        "aarch64-linux-gnu-gcc": "gcc-aarch64-linux-gnu",
                        "aarch64-linux-gnu-g++": "gcc-c++-aarch64-linux-gnu",
                        "x86_64-linux-gnu-gcc": "gcc-x86_64-linux-gnu",
                        "x86_64-linux-gnu-g++": "gcc-c++-x86_64-linux-gnu"
                    },
                    "pacman": {
                        "aarch64-linux-gnu-gcc": "aarch64-linux-gnu-gcc",
                        "aarch64-linux-gnu-g++": "aarch64-linux-gnu-gcc",
                        "x86_64-linux-gnu-gcc": "x86_64-linux-gnu-gcc",
                        "x86_64-linux-gnu-g++": "x86_64-linux-gnu-gcc"
                    }
                }
                packages = [pkg for tool in missing_tools if (pkg := package_map[pm].get(tool))]
                if packages:
                    cmd = f"{pm} install {' '.join(packages)}"
                    commands.append(cmd)  # type: ignore[arg-type]

        return " | ".join(commands) if commands else ""  # type: ignore[arg-type]


class EnvironmentValidator:
    """Validates the OmniCpp development environment."""

    def __init__(self):
        self.system = platform.system().lower()
        self.is_windows = self.system == "windows"
        self.is_linux = self.system == "linux"
        self.is_macos = self.system == "darwin"

        self.results = {
            "passed": [],
            "failed": [],
            "warnings": []
        }

        # Initialize validators from omni_scripts
        self.config_validator = ConfigValidator()
        self.build_validator = BuildValidator()
        self.dependency_validator = DependencyValidator()

    def run_command(self, cmd: str, shell: bool = False) -> Tuple[bool, str]:
        """Run a command and return success status and output."""
        try:
            result = subprocess.run(
                cmd if shell else cmd.split(),
                shell=shell,
                capture_output=True,
                text=True,
                timeout=30
            )
            return result.returncode == 0, result.stdout.strip()
        except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
            return False, ""

    def check_command(self, command: str, description: str) -> bool:
        """Check if a command is available."""
        success, _ = self.run_command(f"which {command}" if not self.is_windows else f"where {command}")
        if success:
            self.results["passed"].append(f"✅ {description}: {command}")  # type: ignore[arg-type]
            return True
        else:
            self.results["failed"].append(f"❌ {description}: {command} not found")  # type: ignore[arg-type]
            return False

    def check_version(self, command: str, description: str, min_version: Optional[str] = None) -> bool:
        """Check command version."""
        success, output = self.run_command(f"{command} --version")
        if success:
            version = output.split()[0] if output else "unknown"
            if min_version and version < min_version:
                self.results["warnings"].append(f"⚠️  {description}: {command} version {version} < {min_version}")  # type: ignore[arg-type]
                return False
            else:
                self.results["passed"].append(f"✅ {description}: {command} {version}")  # type: ignore[arg-type]
                return True
        else:
            self.results["failed"].append(f"❌ {description}: {command} not found")  # type: ignore[arg-type]
            return False

    def check_file(self, path: str, description: str) -> bool:
        """Check if a file exists."""
        if os.path.exists(path):
            self.results["passed"].append(f"✅ {description}: {path}")  # type: ignore[arg-type]
            return True
        else:
            self.results["failed"].append(f"❌ {description}: {path} not found")  # type: ignore[arg-type]
            return False

    def check_directory(self, path: str, description: str) -> bool:
        """Check if a directory exists."""
        if os.path.isdir(path):
            self.results["passed"].append(f"✅ {description}: {path}")  # type: ignore[arg-type]
            return True
        else:
            self.results["failed"].append(f"❌ {description}: {path} not found")  # type: ignore[arg-type]
            return False

    def validate_essential_tools(self) -> None:
        """Validate essential development tools."""
        print("🔧 Checking Essential Tools...")

        # Git
        self.check_version("git", "Git version control", "2.25")

        # CMake
        self.check_version("cmake", "CMake build system", "3.31")

        # Python
        self.check_version("python", "Python runtime", "3.8")

        # Ninja
        self.check_command("ninja", "Ninja build tool")

    def validate_compilers(self) -> None:
        """Validate compiler installations."""
        print("🔨 Checking Compilers...")

        compilers = []

        if self.is_windows:
            compilers.extend([  # type: ignore[arg-type]
                ("cl", "MSVC compiler"),
                ("clang-cl", "Clang-MSVC compiler"),
                ("gcc", "MinGW GCC compiler")
            ])
        else:
            compilers.extend([  # type: ignore[arg-type]
                ("gcc", "GCC compiler"),
                ("g++", "G++ compiler"),
                ("clang", "Clang compiler"),
                ("clang++", "Clang++ compiler")
            ])

        for cmd, desc in compilers:  # type: ignore[assignment]
            self.check_command(cmd, desc)  # type: ignore[arg-type]

    def validate_package_managers(self) -> None:
        """Validate package managers.

        Warploom has one dependency story: CPM fetches from the network at
        configure time, so there is no package manager binary to require and
        nothing to install. Conan was removed along with conan/ (commit
        a1a3214); it used to be checked here, which recorded a hard failure on
        any machine without it installed -- a requirement for a tool the build
        no longer uses. vcpkg stays as optional toolchain support, since
        cmake/VcpkgIntegration.cmake still honours a vcpkg toolchain.
        """
        print("📦 Checking Package Managers...")
        self.results["passed"].append(
            "✅ CPM: fetched at configure time, no system package manager required"
        )

        # vcpkg (check if directory exists)
        vcpkg_paths = [
            "/usr/local/vcpkg",
            "~/vcpkg",
            "C:\\vcpkg",
            "C:\\Program Files\\vcpkg"
        ]

        # vcpkg is OPTIONAL: cmake/VcpkgIntegration.cmake only honours a vcpkg
        # toolchain if one is present. Probing with check_directory recorded a
        # hard FAILURE for every location tried, so a normal CPM-only machine
        # reported four failures for a toolchain it never intended to use.
        # Probe quietly and report at most one informational line.
        vcpkg_found = any(
            os.path.isdir(os.path.expanduser(path)) for path in vcpkg_paths
        )
        if vcpkg_found:
            self.results["passed"].append(
                "✅ vcpkg: present, toolchain integration available"
            )
        else:
            self.results["warnings"].append(
                "⚠️  vcpkg not found in common locations (optional: CPM is the "
                "dependency mechanism)"
            )

    def validate_qt_vulkan(self) -> None:
        """Validate Qt and Vulkan development setup."""
        print("🎨 Checking Qt/Vulkan Development...")

        # Vulkan: probe the TOOLS, not an SDK install directory. The old check
        # looked for C:\VulkanSDK / /usr/local/VulkanSDK and recorded a hard
        # failure for each miss, so a machine with a perfectly working Vulkan
        # installed from distro packages (vulkaninfo, glslc and glslangValidator
        # all on PATH) was told its Vulkan was broken. What the build actually
        # needs is the shader compilers and a loader, and those are what is
        # checked now.
        vulkan_tools = ["vulkaninfo", "glslc", "glslangValidator"]
        missing_vulkan = [t for t in vulkan_tools if shutil.which(t) is None]

        if not missing_vulkan:
            self.results["passed"].append(
                "✅ Vulkan toolchain: " + ", ".join(vulkan_tools)
            )
        elif len(missing_vulkan) == len(vulkan_tools):
            self.results["failed"].append(
                "❌ Vulkan toolchain: none of " + ", ".join(vulkan_tools) + " found"
            )
        else:
            self.results["warnings"].append(
                "⚠️  Vulkan toolchain missing: " + ", ".join(missing_vulkan)
            )

    def validate_cross_compilation(self) -> None:
        """Validate cross-compilation toolchains."""
        print("🔄 Checking Cross-Compilation Toolchains...")

        toolchains = {
            "arm64-linux-gnu": ["aarch64-linux-gnu-gcc", "aarch64-linux-gnu-g++"],
            "x86-linux-gnu": ["x86_64-linux-gnu-gcc", "x86_64-linux-gnu-g++"],
            "emscripten": ["emcc", "em++"]
        }

        for arch, tools in toolchains.items():
            toolchain_ok = True
            missing_tools: list[str] = []
            for tool in tools:
                if not self.check_command(tool, f"{arch} toolchain"):
                    toolchain_ok = False
                    missing_tools.append(tool)  # type: ignore[arg-type]

            if not toolchain_ok:
                # Try to install missing tools
                if arch in ["arm64-linux-gnu", "x86-linux-gnu"] and self.is_linux:
                    validator = ToolchainValidator(arch)
                    if validator.install_missing_tools():
                        # Re-check after installation
                        toolchain_ok = True
                        missing_tools: list[str] = []
                        for tool in tools:
                            if not self.check_command(tool, f"{arch} toolchain"):
                                missing_tools.append(tool)  # type: ignore[arg-type]
                        if missing_tools:
                            toolchain_ok = False

                if not toolchain_ok:
                    # Provide installation commands in error messages
                    install_cmds = self._get_installation_commands(arch, missing_tools)
                    error_msg = f"⚠️  {arch} toolchain incomplete - missing: {', '.join(missing_tools)}"  # type: ignore[arg-type]
                    if install_cmds:
                        error_msg += f". Install with: {install_cmds}"
                    self.results["warnings"].append(error_msg)  # type: ignore[arg-type]

    def validate_development_tools(self) -> None:
        """Validate additional development tools."""
        print("🛠️  Checking Development Tools...")

        tools = [
            ("clang-format", "Code formatter"),
            ("clang-tidy", "Static analyzer"),
            ("doxygen", "Documentation generator"),
            ("ccache", "Compilation cache")
        ]

        for cmd, desc in tools:
            self.check_command(cmd, desc)

    def validate_project_configuration(self) -> None:
        """Validate project configuration files using omni_scripts validators."""
        print("📋 Validating Project Configuration...")

        project_root = Path.cwd()

        # Validate configuration files
        config_results = self.config_validator.validate_project_configs(project_root)

        for result in config_results:
            if result.is_valid:
                self.results["passed"].append(f"✅ Configuration valid: {result.file_path.name}")  # type: ignore[arg-type]
            else:
                self.results["failed"].append(f"❌ Configuration invalid: {result.file_path.name}")  # type: ignore[arg-type]
                for error in result.errors:
                    self.results["failed"].append(f"   - {error}")  # type: ignore[arg-type]
            for warning in result.warnings:
                self.results["warnings"].append(f"⚠️  {result.file_path.name}: {warning}")  # type: ignore[arg-type]

    def validate_dependencies(self) -> None:
        """Validate project dependencies using omni_scripts validators."""
        print("📦 Validating Dependencies...")

        project_root = Path.cwd()

        # Validate dependencies
        dep_result = self.dependency_validator.validate_all_dependencies(project_root)

        if dep_result.is_valid:
            self.results["passed"].append(f"✅ All dependencies valid ({len(dep_result.dependencies)} found)")  # type: ignore[arg-type]
        else:
            self.results["failed"].append(f"❌ Dependency validation failed")  # type: ignore[arg-type]
            for error in dep_result.errors:
                self.results["failed"].append(f"   - {error}")  # type: ignore[arg-type]

        for conflict in dep_result.conflicts:
            self.results["warnings"].append(f"⚠️  Dependency conflict: {conflict}")  # type: ignore[arg-type]

        for issue in dep_result.security_issues:
            self.results["failed"].append(f"❌ Security issue: {issue}")  # type: ignore[arg-type]

        for issue in dep_result.license_issues:
            self.results["warnings"].append(f"⚠️  License issue: {issue}")  # type: ignore[arg-type]

        for warning in dep_result.warnings:
            self.results["warnings"].append(f"⚠️  {warning}")  # type: ignore[arg-type]

    def print_summary(self) -> bool:
        """Print validation summary."""
        print("\n" + "="*60)
        print("ENVIRONMENT VALIDATION SUMMARY")
        print("="*60)

        if self.results["passed"]:  # type: ignore[index]
            print(f"\n✅ PASSED ({len(self.results['passed'])}):")  # type: ignore[arg-type]
            for item in self.results["passed"]:  # type: ignore[index]
                print(f"  {item}")  # type: ignore[arg-type]

        if self.results["warnings"]:  # type: ignore[index]
            print(f"\n⚠️  WARNINGS ({len(self.results['warnings'])}):")  # type: ignore[arg-type]
            for item in self.results["warnings"]:  # type: ignore[index]
                print(f"  {item}")  # type: ignore[arg-type]

        if self.results["failed"]:  # type: ignore[index]
            print(f"\n❌ FAILED ({len(self.results['failed'])}):")  # type: ignore[arg-type]
            for item in self.results["failed"]:  # type: ignore[index]
                print(f"  {item}")  # type: ignore[arg-type]

        print(f"\nTotal checks: {len(self.results['passed']) + len(self.results['failed']) + len(self.results['warnings'])}")  # type: ignore[arg-type]
        print(f"Passed: {len(self.results['passed'])}, Warnings: {len(self.results['warnings'])}, Failed: {len(self.results['failed'])}")  # type: ignore[arg-type]

        if self.results["failed"]:  # type: ignore[index]
            print("\n❌ Some essential tools are missing. Please install them before building.")
            print("See docs/troubleshooting.md for installation instructions.")
            return False
        elif self.results["warnings"]:  # type: ignore[index]
            print("\n⚠️  Environment is functional but some optional tools are missing.")
            print("Qt/Vulkan builds may require additional setup.")
            return True
        else:
            print("\n✅ Environment validation passed! Ready for OmniCpp development.")
            return True

    def _get_installation_commands(self, arch: str, missing_tools: List[str]) -> str:
        """Get installation commands for missing tools."""
        if not self.is_linux:
            return ""

        commands: list[str] = []
        for pm in ["apt", "dnf", "pacman"]:
            if self._has_package_manager(pm):
                package_map = {
                    "apt": {
                        "aarch64-linux-gnu-gcc": "gcc-aarch64-linux-gnu",
                        "aarch64-linux-gnu-g++": "g++-aarch64-linux-gnu",
                        "x86_64-linux-gnu-gcc": "gcc-x86-64-linux-gnu",
                        "x86_64-linux-gnu-g++": "g++-x86-64-linux-gnu"
                    },
                    "dnf": {
                        "aarch64-linux-gnu-gcc": "gcc-aarch64-linux-gnu",
                        "aarch64-linux-gnu-g++": "gcc-c++-aarch64-linux-gnu",
                        "x86_64-linux-gnu-gcc": "gcc-x86_64-linux-gnu",
                        "x86_64-linux-gnu-g++": "gcc-c++-x86_64-linux-gnu"
                    },
                    "pacman": {
                        "aarch64-linux-gnu-gcc": "aarch64-linux-gnu-gcc",
                        "aarch64-linux-gnu-g++": "aarch64-linux-gnu-gcc",
                        "x86_64-linux-gnu-gcc": "x86_64-linux-gnu-gcc",
                        "x86_64-linux-gnu-g++": "x86_64-linux-gnu-gcc"
                    }
                }
                packages = [pkg for tool in missing_tools if (pkg := package_map[pm].get(tool))]
                if packages:
                    cmd = f"{pm} install {' '.join(packages)}"
                    commands.append(cmd)  # type: ignore[arg-type]

        return " | ".join(commands) if commands else ""  # type: ignore[arg-type]

    def _has_package_manager(self, pm: str) -> bool:
        """Check if package manager is available."""
        try:
            subprocess.run([pm, "--version"], capture_output=True, check=True)
            return True
        except (subprocess.CalledProcessError, FileNotFoundError):
            return False


def main():
    """Main validation function."""
    print("🚀 OmniCpp Environment Validator")
    print("="*40)

    validator = EnvironmentValidator()

    # Run all validation checks
    validator.validate_essential_tools()
    validator.validate_compilers()
    validator.validate_package_managers()
    validator.validate_qt_vulkan()
    validator.validate_cross_compilation()
    validator.validate_development_tools()
    validator.validate_project_configuration()
    validator.validate_dependencies()

    # Print summary and return exit code
    success = validator.print_summary()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
