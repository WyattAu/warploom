# Installation

## Prerequisites

Before building WarploomLib, ensure you have the following installed:

### Required Tools

- **CMake 4.0+**: Build system generator
- **C++ Compiler**: GCC 11+, Clang 14+, or MSVC 2022+ with C++20 support
- **Git**: For cloning the repository

### Optional Dependencies

- **Qt6**: For Qt/Vulkan components (optional)
- **Vulkan SDK**: For graphics features (optional)
- **Doxygen**: For API documentation generation
- **MkDocs**: For project documentation (requires Python)

## Vulkan SDK Setup

The project supports two approaches for Vulkan SDK:

### Option 1: System-Wide Vulkan SDK (Recommended for Local Development)

For local development, install the Vulkan SDK system-wide and set the `VULKAN_SDK` environment variable. This approach provides faster builds and access to full Vulkan SDK tools.

#### Windows

1. Download Vulkan SDK from [LunarG](https://vulkan.lunarg.com/)
2. Install the SDK (default location: `C:\VulkanSDK\1.3.xxx`)
3. Set environment variable:
```powershell
# PowerShell
$env:VULKAN_SDK="C:\VulkanSDK\1.3.xxx"

# Or permanently via System Properties
setx VULKAN_SDK "C:\VulkanSDK\1.3.xxx"
```

#### Linux (Ubuntu/Debian)

```bash
# Install Vulkan SDK
sudo apt update
sudo apt install vulkan-sdk vulkan-tools

# Set environment variable (add to ~/.bashrc or ~/.zshrc)
export VULKAN_SDK=/usr
```

#### macOS

```bash
# Install Vulkan SDK via Homebrew
brew install vulkan-headers molten-vk

# Set environment variable (add to ~/.zshrc)
export VULKAN_SDK=/opt/homebrew
```

### Option 2: Conan-Provided Vulkan SDK (Recommended for CI/CD)

For CI/CD environments, Conan can automatically fetch and install Vulkan SDK components. This approach ensures reproducible builds without manual SDK installation.

When `VULKAN_SDK` is not set, Conan will automatically download and configure:
- Vulkan headers
- Vulkan loader
- Vulkan validation layers
- Shader compiler (shaderc)
- SPIRV tools
- GLSL to SPIRV compiler (glslang)
- SPIRV cross compiler

### Choosing the Right Approach

| Scenario | Recommended Approach | Reason |
|-----------|---------------------|---------|
| Local Development | System-Wide SDK | Faster builds, access to Vulkan tools (vulkaninfo, etc.) |
| CI/CD Pipelines | Conan-Provided SDK | Reproducible builds, no manual setup |
| Docker Containers | System-Wide SDK | Single installation, shared across containers |

### Verifying Vulkan Installation

```bash
# Check if Vulkan SDK is available
vulkaninfo --summary

# Check VULKAN_SDK environment variable
echo $VULKAN_SDK  # Linux/macOS
echo %VULKAN_SDK%  # Windows
```

## Installing Prerequisites

### Windows

```powershell
# Using Chocolatey
choco install cmake git

# Or download from official sites
# CMake: https://cmake.org/download/
# Git: https://git-scm.com/downloads
```

### Linux (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install cmake git build-essential

# For Qt/Vulkan (optional)
sudo apt install qt6-base-dev vulkan-tools
```

### macOS

```bash
# Using Homebrew
brew install cmake git

# For Qt (optional)
brew install qt6
```

## Cloning the Repository

```bash
git clone https://github.com/your-org/your-repo.git
cd your-repo
```

## Building the Project

```bash
# Configure
cmake -B build -S .

# Build
cmake --build build

# Install (optional)
cmake --install build
```

### Build Options

- `BUILD_LIBRARY`: Build the core library (default: ON)
- `BUILD_STANDALONE`: Build standalone executable (default: ON)
- `BUILD_QT_VULKAN`: Build Qt/Vulkan components (default: OFF)

Example with custom options:

```bash
cmake -B build -S . -DBUILD_QT_VULKAN=ON