#!/usr/bin/env python3
"""
Vulkan SDK Setup Script

This script automatically checks for Vulkan SDK existence, verifies versions,
validates paths, and sets up environment variables. It includes installation/update
logic and comprehensive error handling.

Usage:
    python setup.py [--install] [--update] [--check-only] [--verbose]

Options:
    --install     Install missing components if possible
    --update      Update existing installations
    --check-only  Only check and report status (default)
    --verbose     Enable verbose output
"""

import os
import sys
import platform
import subprocess
import shutil
import re
import argparse
from pathlib import Path
from typing import Optional, Dict, List, Any
import logging

# Conditional import for Windows admin check
if platform.system().lower() == "windows":
    import ctypes

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

def is_admin() -> bool:
    """Check if the script is running with administrator privileges on Windows"""
    if platform.system().lower() == "windows":
        try:
            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        except Exception:
            return False
    else:
        # On Unix-like systems, assume admin if running as root or using sudo
        try:
            # geteuid is available on Unix-like systems
            euid = os.geteuid()  # type: ignore[attr-defined]
            return euid == 0 if euid is not None else False
        except AttributeError:
            # geteuid not available on Windows
            return False

class SetupError(Exception):
    """Custom exception for setup errors"""
    pass


class VulkanSDKDetector:
    """Vulkan SDK detection and version checking"""

    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self.system = platform.system().lower()
        self.machine = platform.machine().lower()

    def find_vulkan_sdk_installations(self) -> List[Path]:
        """Find Vulkan SDK installations dynamically"""
        sdk_paths: List[Path] = []

        if self.system == "windows":
            # Check common Vulkan SDK installation directory
            vulkan_base = Path("C:/VulkanSDK")
            if vulkan_base.exists() and vulkan_base.is_dir():
                # Find all version directories
                for item in vulkan_base.iterdir():
                    if item.is_dir() and re.match(r'\d+\.\d+\.\d+', item.name):
                        sdk_paths.append(item)
        elif self.system == "linux":
            # Check common Linux installation paths
            common_paths = [
                Path("/usr/local/VulkanSDK"),
                Path("/opt/vulkan-sdk"),
                Path("/usr"),
                Path("/usr/local")
            ]
            for base_path in common_paths:
                if base_path.exists():
                    # Look for Vulkan SDK subdirectories
                    for item in base_path.iterdir() if base_path.is_dir() else []:
                        if item.is_dir() and ("vulkan" in item.name.lower() or "Vulkan" in item.name):
                            sdk_paths.append(item)
                    # Also check if the base path itself is a Vulkan SDK
                    if (base_path / "include" / "vulkan").exists() or (base_path / "lib").exists():
                        sdk_paths.append(base_path)
        elif self.system == "darwin":
            # Check macOS installation paths
            common_paths = [
                Path("/usr/local/VulkanSDK"),
                Path("/opt/vulkan-sdk"),
                Path("/usr/local")
            ]
            for base_path in common_paths:
                if base_path.exists():
                    for item in base_path.iterdir() if base_path.is_dir() else []:
                        if item.is_dir() and ("vulkan" in item.name.lower() or "Vulkan" in item.name):
                            sdk_paths.append(item)
                    if (base_path / "include" / "vulkan").exists() or (base_path / "lib").exists():
                        sdk_paths.append(base_path)

        return sdk_paths


    def get_vulkan_sdk_version(self, sdk_path: Path) -> Optional[str]:
        """Extract Vulkan SDK version from path"""
        path_match = re.search(r'(\d+\.\d+\.\d+)', str(sdk_path))
        if path_match:
            return path_match.group(1)
        return None

    def validate_vulkan_sdk_installation(self, sdk_path: Path) -> Dict[str, Any]:
        """Validate Vulkan SDK installation and return details"""
        result: Dict[str, Any] = {
            "path": str(sdk_path),
            "version": None,
            "valid": False,
            "bin_path": None,
            "lib_path": None,
            "include_path": None,
            "errors": []
        }

        # Check if path exists
        if not sdk_path.exists():
            result["errors"].append("Vulkan SDK path does not exist")
            return result

        # Get version - for manually installed paths, try to extract from path first
        version = self.get_vulkan_sdk_version(sdk_path)
        if version:
            result["version"] = version
        else:
            # For manually installed paths, try to infer version from path
            if "1.4.328.1" in str(sdk_path):
                result["version"] = "1.4.328.1"
            else:
                result["errors"].append("Could not determine Vulkan SDK version")
                return result

        # Check for essential Vulkan SDK components
        bin_path = sdk_path / "Bin"
        lib_path = sdk_path / "Lib"
        include_path = sdk_path / "Include"

        # Check for vulkan-1 library
        if self.system == "windows":
            vulkan_lib_names = ["vulkan-1.dll"]
            vulkan_lib_paths = [lib_path, bin_path]  # Check both Lib and Bin directories
        elif self.system == "linux":
            vulkan_lib_names = ["libvulkan.so", "libvulkan.so.1"]
            vulkan_lib_paths = [lib_path]
        elif self.system == "darwin":
            vulkan_lib_names = ["libvulkan.dylib", "libvulkan.1.dylib"]
            vulkan_lib_paths = [lib_path]
        else:
            vulkan_lib_names = []
            vulkan_lib_paths = []

        vulkan_lib_found = False
        for lib_path_check in vulkan_lib_paths:
            for lib_name in vulkan_lib_names:
                if (lib_path_check / lib_name).exists():
                    vulkan_lib_found = True
                    break
            if vulkan_lib_found:
                break

        if not vulkan_lib_found:
            result["errors"].append("Vulkan library not found")

        # Check include directory for vulkan.h
        vulkan_header = include_path / "vulkan" / "vulkan.h"
        if not vulkan_header.exists():
            result["errors"].append("vulkan.h header not found")

        # Check for glslangValidator (shader compiler)
        glslang_validator = bin_path / ("glslangValidator.exe" if self.system == "windows" else "glslangValidator")
        if not glslang_validator.exists():
            result["errors"].append("glslangValidator not found")

        # Set paths if they exist
        if bin_path.exists():
            result["bin_path"] = str(bin_path)
        if lib_path.exists():
            result["lib_path"] = str(lib_path)
        if include_path.exists():
            result["include_path"] = str(include_path)

        # Installation is valid if no errors
        result["valid"] = len(result["errors"]) == 0

        return result

    def get_best_vulkan_sdk_installation(self) -> Optional[Dict[str, Any]]:
        """Find the best (highest version) valid Vulkan SDK installation"""
        installations = self.find_vulkan_sdk_installations()

        if not installations:
            return None

        valid_installations = []
        for sdk_path in installations:
            validation = self.validate_vulkan_sdk_installation(sdk_path)
            if validation["valid"]:
                valid_installations.append(validation)

        if not valid_installations:
            return None

        # Return the highest version
        return max(valid_installations, key=lambda x: tuple(int(v) for v in x["version"].split(".")))

class EnvironmentManager:
    """Environment variable management"""

    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self.system = platform.system().lower()


    def setup_vulkan_environment(self, vulkan_info: Dict[str, Any]) -> Dict[str, str]:
        """Setup Vulkan SDK environment variables"""
        env_vars = {}

        env_vars["VULKAN_SDK"] = vulkan_info["path"]
        env_vars["VK_SDK_PATH"] = vulkan_info["path"]

        if vulkan_info.get("bin_path"):
            env_vars["PATH"] = f"{vulkan_info['bin_path']};{os.environ.get('PATH', '')}"

        if vulkan_info.get("lib_path"):
            env_vars["VK_LIBRARY_PATH"] = vulkan_info["lib_path"]

        if vulkan_info.get("include_path"):
            env_vars["VK_INCLUDE_PATH"] = vulkan_info["include_path"]

        # Additional Vulkan environment variables
        if self.system == "windows":
            env_vars["VK_LAYER_PATH"] = str(Path(vulkan_info["path"]) / "Bin")
            env_vars["VK_DATA_DIR"] = str(Path(vulkan_info["path"]) / "share" / "vulkan" / "explicit_layer.d")
        elif self.system == "linux":
            env_vars["VK_LAYER_PATH"] = str(Path(vulkan_info["path"]) / "share" / "vulkan" / "explicit_layer.d")
        elif self.system == "darwin":
            env_vars["VK_ICD_FILENAMES"] = str(Path(vulkan_info["path"]) / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json")
            env_vars["VK_LAYER_PATH"] = str(Path(vulkan_info["path"]) / "share" / "vulkan" / "explicit_layer.d")

        return env_vars

    def generate_batch_script(self, qt_info: Optional[Dict[str, Any]], vulkan_info: Optional[Dict[str, Any]], output_path: Path) -> None:
        """Generate a batch script to setup environment"""
        script_lines = []

        if self.system == "windows":
            script_lines.append("@echo off")
            script_lines.append("REM Vulkan SDK Environment Setup")
            script_lines.append("")

            if vulkan_info:
                script_lines.append("REM Vulkan SDK Environment Variables")
                script_lines.append(f'set "VULKAN_SDK={vulkan_info["path"]}"')
                script_lines.append(f'set "VK_SDK_PATH={vulkan_info["path"]}"')
                if vulkan_info.get("bin_path"):
                    script_lines.append(f'set "PATH={vulkan_info["bin_path"]};%PATH%"')
                if self.system == "windows":
                    script_lines.append(f'set "VK_LAYER_PATH={Path(vulkan_info["path"]) / "Bin"}"')
                    script_lines.append(f'set "VK_DATA_DIR={Path(vulkan_info["path"]) / "share" / "vulkan" / "explicit_layer.d"}"')
                script_lines.append("")

            script_lines.append('echo Vulkan SDK environment setup complete.')
            if vulkan_info:
                script_lines.append('echo VULKAN_SDK: %VULKAN_SDK%')
        else:
            # Unix-like systems (bash script)
            script_lines.append("#!/bin/bash")
            script_lines.append("# Vulkan SDK Environment Setup")
            script_lines.append("")

            if vulkan_info:
                script_lines.append("# Vulkan SDK Environment Variables")
                script_lines.append(f'export VULKAN_SDK="{vulkan_info["path"]}"')
                script_lines.append(f'export VK_SDK_PATH="{vulkan_info["path"]}"')
                if vulkan_info.get("bin_path"):
                    script_lines.append(f'export PATH="{vulkan_info["bin_path"]}:$PATH"')
                if self.system == "linux":
                    script_lines.append(f'export VK_LAYER_PATH="{Path(vulkan_info["path"]) / "share" / "vulkan" / "explicit_layer.d"}"')
                elif self.system == "darwin":
                    script_lines.append(f'export VK_ICD_FILENAMES="{Path(vulkan_info["path"]) / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json"}"')
                    script_lines.append(f'export VK_LAYER_PATH="{Path(vulkan_info["path"]) / "share" / "vulkan" / "explicit_layer.d"}"')
                script_lines.append("")

            script_lines.append('echo "Vulkan SDK environment setup complete."')
            if vulkan_info:
                script_lines.append('echo "VULKAN_SDK: $VULKAN_SDK"')

        # Write the script
        with open(output_path, 'w', encoding='utf-8') as f:
            if self.system != "windows":
                f.write("# Make script executable\n")
                f.write("chmod +x \"$0\"\n\n")
            f.writelines(line + '\n' for line in script_lines)

        # Make executable on Unix-like systems
        if self.system != "windows":
            output_path.chmod(0o755)

class Installer:
    """Installation and update logic"""

    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self.system = platform.system().lower()


    def install_vulkan_sdk(self) -> Optional[Path]:
        """Attempt to install Vulkan SDK"""
        if self.verbose:
            logger.info("Attempting to install Vulkan SDK...")

        if self.system == "windows":
            if not is_admin():
                logger.warning("Administrator privileges are required to install Vulkan SDK on Windows. Please run this script as Administrator.")
                return None
            # Use winget
            if shutil.which("winget"):
                try:
                    subprocess.run(["winget", "install", "KhronosGroup.VulkanSDK", "-e"], check=True, timeout=300)
                    vulkan_detector = VulkanSDKDetector(self.verbose)
                    installations = vulkan_detector.find_vulkan_sdk_installations()
                    if installations:
                        return max(installations, key=lambda x: str(x))
                    return None
                except subprocess.SubprocessError:
                    pass

        elif self.system == "linux":
            # Try apt (Ubuntu/Debian)
            if shutil.which("apt"):
                try:
                    subprocess.run(["sudo", "apt", "update"], check=True, timeout=60)
                    subprocess.run(["sudo", "apt", "install", "-y", "vulkan-sdk"], check=True, timeout=300)
                    vulkan_detector = VulkanSDKDetector(self.verbose)
                    installations = vulkan_detector.find_vulkan_sdk_installations()
                    if installations:
                        return installations[0]
                    return None
                except subprocess.SubprocessError:
                    pass

            # Try dnf (Fedora)
            if shutil.which("dnf"):
                try:
                    subprocess.run(["sudo", "dnf", "install", "-y", "vulkan-loader"], check=True, timeout=300)
                    vulkan_detector = VulkanSDKDetector(self.verbose)
                    installations = vulkan_detector.find_vulkan_sdk_installations()
                    if installations:
                        return installations[0]
                    return None
                except subprocess.SubprocessError:
                    pass

        elif self.system == "darwin":
            # Try Homebrew
            if shutil.which("brew"):
                try:
                    subprocess.run(["brew", "install", "vulkan-sdk"], check=True, timeout=300)
                    vulkan_detector = VulkanSDKDetector(self.verbose)
                    installations = vulkan_detector.find_vulkan_sdk_installations()
                    if installations:
                        return installations[0]
                    return None
                except subprocess.SubprocessError:
                    pass

        logger.warning("Could not install Vulkan SDK automatically. Please install manually from https://vulkan.lunarg.com/sdk/home")
        return None


    def update_vulkan_sdk(self, vulkan_info: Dict[str, Any]) -> bool:
        """Attempt to update Vulkan SDK"""
        if self.verbose:
            logger.info(f"Attempting to update Vulkan SDK from version {vulkan_info.get('version', 'unknown')}...")

        # Try package manager update
        if self.system == "windows":
            if not is_admin():
                logger.warning("Administrator privileges are required to update Vulkan SDK on Windows. Please run this script as Administrator.")
                return False
            if shutil.which("winget"):
                try:
                    subprocess.run(["winget", "upgrade", "KhronosGroup.VulkanSDK"], check=True, timeout=300)
                    return True
                except subprocess.SubprocessError:
                    pass

        elif self.system == "linux":
            if shutil.which("apt"):
                try:
                    subprocess.run(["sudo", "apt", "update"], check=True, timeout=60)
                    subprocess.run(["sudo", "apt", "install", "--only-upgrade", "-y", "vulkan-sdk"], check=True, timeout=300)
                    return True
                except subprocess.SubprocessError:
                    pass

        elif self.system == "darwin":
            if shutil.which("brew"):
                try:
                    subprocess.run(["brew", "upgrade", "vulkan-sdk"], check=True, timeout=300)
                    return True
                except subprocess.SubprocessError:
                    pass

        logger.info("Vulkan SDK updates require manual installation. Please download the latest version from https://vulkan.lunarg.com/sdk/home")
        return False

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Vulkan SDK Setup Script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python setup.py --check-only
  python setup.py --install --verbose
  python setup.py --update
        """
    )

    parser.add_argument(
        "--install",
        action="store_true",
        help="Install missing components if possible"
    )

    parser.add_argument(
        "--update",
        action="store_true",
        help="Update existing installations"
    )

    parser.add_argument(
        "--check-only",
        action="store_true",
        help="Only check and report status (default)"
    )

    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable verbose output"
    )

    args = parser.parse_args()

    # Set default action
    if not any([args.install, args.update, args.check_only]):
        args.check_only = True

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    try:
        # Initialize detectors
        vulkan_detector = VulkanSDKDetector(args.verbose)
        env_manager = EnvironmentManager(args.verbose)
        installer = Installer(args.verbose)

        print("Vulkan SDK Setup Script")
        print("=" * 40)

        # Check Vulkan SDK
        print("\nChecking Vulkan SDK...")
        vulkan_installation = vulkan_detector.get_best_vulkan_sdk_installation()

        if vulkan_installation:
            print(f"[OK] Vulkan SDK found: {vulkan_installation['version']} at {vulkan_installation['path']}")
            if args.verbose:
                for key, value in vulkan_installation.items():
                    if key not in ['valid', 'errors'] and value:
                        print(f"  {key}: {value}")
        else:
            print("[FAIL] Vulkan SDK not found or invalid")
            if args.install:
                print("Attempting to install Vulkan SDK...")
                installed_path = installer.install_vulkan_sdk()
                if installed_path:
                    print(f"[OK] Vulkan SDK installation successful at {installed_path}")
                    validation = vulkan_detector.validate_vulkan_sdk_installation(installed_path)
                    if validation["valid"]:
                        vulkan_installation = validation
                        # Apply environment variables to current session
                        env_vars = env_manager.setup_vulkan_environment(validation)
                        os.environ.update(env_vars)
                        print("Environment variables set for current session")
                    else:
                        print("[FAIL] Installed Vulkan SDK is invalid")
                else:
                    print("[FAIL] Vulkan SDK installation failed")

        # Handle updates
        if args.update:
            if vulkan_installation:
                print("\nUpdating Vulkan SDK...")
                if installer.update_vulkan_sdk(vulkan_installation):
                    print("[OK] Vulkan SDK update successful")
                else:
                    print("[FAIL] Vulkan SDK update failed or not supported")

        # Setup environment
        if vulkan_installation:
            print("\nSetting up environment...")

            # Generate environment setup script
            script_name = "setup_vulkan.bat" if platform.system().lower() == "windows" else "setup_vulkan.sh"
            script_path = Path.cwd() / script_name

            env_manager.generate_batch_script(None, vulkan_installation, script_path)
            print(f"Environment setup script generated: {script_path}")

            # Display environment variables that would be set
            if vulkan_installation:
                vulkan_env = env_manager.setup_vulkan_environment(vulkan_installation)
                print("\nVulkan SDK Environment Variables:")
                for key, value in vulkan_env.items():
                    print(f"  {key}={value}")

        # Summary
        print("\n" + "=" * 40)
        print("Setup Summary:")
        print(f"Vulkan SDK: {'[OK] Found' if vulkan_installation else '[FAIL] Not found'}")

        if vulkan_installation:
            print("\n[OK] Setup complete! Run the generated script to set up your environment:")
            print(f"  {script_name}")
        else:
            print("\n[WARNING] Setup incomplete. Some components are missing.")

    except SetupError as e:
        logger.error(f"Setup failed: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nSetup interrupted by user")
        sys.exit(1)
    except Exception as e:
        logger.error(f"Unexpected error: {e}")
        if args.verbose:
            import traceback
            traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    main()
