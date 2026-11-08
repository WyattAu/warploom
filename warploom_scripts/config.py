# warploom_scripts/config.py
from dataclasses import dataclass
from pathlib import Path
from typing import Optional
import platform

@dataclass
class ConfigManager:
    platform: str
    vulkan_sdk: Optional[Path] = None
    qt_path: Optional[Path] = None

    @classmethod
    def detect_platform(cls) -> str:
        system = platform.system().lower()
        if system == "windows":
            return "windows"
        elif system == "linux":
            return "linux"
        elif system == "darwin":
            return "macos"
        return "unknown"

    def setup_vulkan(self, required: bool = False) -> bool:
        if not required:
            return True
        # Vulkan setup logic using CPM in CMake
        return True