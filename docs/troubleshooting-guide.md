# OmniCpp - Troubleshooting Guide

## Table of Contents
- [Build System Issues](#build-system-issues)
- [Compiler Issues](#compiler-issues)
- [Vulkan Issues](#vulkan-issues)
- [Qt Issues](#qt-issues)
- [Engine Issues](#engine-issues)
- [Performance Issues](#performance-issues)
- [Platform-Specific Issues](#platform-specific-issues)
- [Getting Help](#getting-help)

## Build System Issues

### Python Import Errors

**Symptom**: `ImportError: cannot import name 'X' from 'omni_scripts.utils'`

**Cause**: Missing or incorrect imports in Python modules.

**Solution**:
1. Check `omni_scripts/utils/__init__.py` for exported symbols
2. Verify all required functions are defined in `utils.py`
3. Restart Python interpreter

**Example**:
```python
# omni_scripts/utils/__init__.py
from .utils import (
    run_command,
    detect_platform,
    format_duration,
    # ... other functions
)
```

### CMake Configuration Fails

**Symptom**: CMake generation fails with errors

**Cause**: Missing dependencies, incorrect paths, or CMake version issues.

**Solution**:
1. Verify CMake version:
```bash
cmake --version  # Should be 3.20 or higher
```

2. Check CMake cache:
```bash
rm -rf build/CMakeCache.txt
rm -rf build/CMakeFiles
```

3. Verify dependencies:
```bash
vcpkg list
conan list
```

4. Reconfigure with verbose output:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug --verbose
```

### Build Fails with Linker Errors

**Symptom**: `undefined reference to 'X'` or `cannot find -lX`

**Cause**: Missing libraries or incorrect library paths.

**Solution**:
1. Check library installation:
```bash
vcpkg list | grep <library-name>
conan list | grep <library-name>
```

2. Verify CMake library paths:
```bash
cmake --build build --verbose 2>&1 | grep library
```

3. Check `dependencies.cmake` for correct library names
4. Rebuild with clean:
```bash
python OmniCppController.py build standalone "Clean Build Pipeline" default debug
```

### Build Performance Issues

**Symptom**: Build takes too long

**Cause**: Insufficient parallel jobs or system resources.

**Solution**:
1. Increase parallel jobs:
```bash
python OmniCppController.py build standalone "Build Project" default debug --parallel 8
```

2. Use ccache for faster rebuilds:
```bash
# Install ccache
sudo apt install ccache  # Linux
choco install ccache  # Windows

# Configure CMake to use ccache
cmake -DCMAKE_C_COMPILER_LAUNCHER=ccache ..
```

3. Disable unnecessary features:
```bash
cmake -DOMNICPP_BUILD_TESTS=OFF ..
```

## Compiler Issues

### MSVC Not Found

**Symptom**: `'cl' is not recognized as an internal or external command`

**Cause**: MSVC not in PATH or not installed.

**Solution**:
1. Open "Developer Command Prompt for VS" from Start Menu
2. Or add MSVC to PATH:
```bash
# Add to PATH (Windows)
C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.xx\bin\Hostx64\x64
```

3. Verify installation:
```bash
cl --version
```

### GCC Not Found

**Symptom**: `'gcc' is not recognized as an internal or external command`

**Cause**: GCC not in PATH or not installed.

**Solution**:
1. Install MinGW-w64:
```bash
# Windows (using Chocolatey)
choco install mingw

# Linux
sudo apt install build-essential
```

2. Add to PATH:
```bash
# Windows
C:\mingw64\bin

# Linux
/usr/bin
```

3. Verify installation:
```bash
gcc --version
```

### Clang Not Found

**Symptom**: `'clang' is not recognized as an internal or external command`

**Cause**: Clang not in PATH or not installed.

**Solution**:
1. Install Clang:
```bash
# Windows (using Chocolatey)
choco install llvm

# Linux
sudo apt install clang
```

2. Add to PATH:
```bash
# Windows
C:\Program Files\LLVM\bin

# Linux
/usr/bin
```

3. Verify installation:
```bash
clang --version
```

### Compiler Warnings

**Symptom**: Build succeeds but produces warnings

**Cause**: Code quality issues or deprecated features.

**Solution**:
1. Review warnings and fix code:
```cpp
// Warning: unused variable
int x = 5;  // Remove if unused

// Warning: deprecated function
old_function();  // Replace with new_function()
```

2. Enable warnings as errors for strict builds:
```bash
cmake -DWARPLOOM_WARNINGS_AS_ERRORS=ON ..
```

## Vulkan Issues

### Vulkan Not Found

**Symptom**: `Failed to find Vulkan` or `VK_NO_PROTOTYPES` errors

**Cause**: Vulkan SDK not installed or not in PATH.

**Solution**:
1. Install Vulkan SDK:
   - Windows: Download from [LunarG](https://vulkan.lunarg.com/)
   - Linux: `sudo apt install vulkan-sdk`
   - macOS: `brew install vulkan-headers`

2. Set environment variables:
```bash
# Windows
set VULKAN_SDK=C:\VulkanSDK\1.3.xxx

# Linux/macOS
export VULKAN_SDK=/usr/local/vulkansdk
```

3. Verify installation:
```bash
vulkaninfo
```

### VULKAN_SDK Environment Variable Not Recognized

**Symptom**: Conan still downloads Vulkan dependencies despite having Vulkan SDK installed, or build fails to find Vulkan.

**Cause**: `VULKAN_SDK` environment variable not set or not visible to build system.

**Solution**:
1. Verify `VULKAN_SDK` is set:
```bash
# Windows (PowerShell)
echo $env:VULKAN_SDK

# Windows (CMD)
echo %VULKAN_SDK%

# Linux/macOS
echo $VULKAN_SDK
```

2. Set `VULKAN_SDK` permanently:
```bash
# Windows (PowerShell - add to profile)
[System.Environment]::SetEnvironmentVariable('VULKAN_SDK', 'C:\VulkanSDK\1.3.xxx', 'User')

# Linux/macOS - add to ~/.bashrc or ~/.zshrc
echo 'export VULKAN_SDK=/usr/local/vulkansdk' >> ~/.bashrc
source ~/.bashrc
```

3. Reconfigure build:
```bash
# Clean CMake cache
rm -rf build/CMakeCache.txt
rm -rf build/CMakeFiles

# Reconfigure
cmake -B build -S .
```

### Choosing Between System SDK and Conan

**Symptom**: Confusion about which Vulkan SDK approach to use.

**Cause**: Unclear when to use system-wide SDK vs Conan-provided SDK.

**Solution**:

| Scenario | Recommended Approach | How to Configure |
|-----------|---------------------|-------------------|
| Local Development | System-Wide SDK | Set `VULKAN_SDK` environment variable |
| CI/CD Pipelines | Conan-Provided SDK | Do NOT set `VULKAN_SDK` |
| Docker Containers | System-Wide SDK | Set `VULKAN_SDK` in Dockerfile |

**Switching Approaches**:

To switch from Conan to System SDK:
```bash
# Set VULKAN_SDK
export VULKAN_SDK=/path/to/vulkan/sdk

# Clean and rebuild
rm -rf build
cmake -B build -S .
cmake --build build
```

To switch from System SDK to Conan:
```bash
# Unset VULKAN_SDK
unset VULKAN_SDK  # Linux/macOS
set VULKAN_SDK=  # Windows CMD

# Clean and rebuild
rm -rf build
cmake -B build -S .
cmake --build build
```

### Conan Vulkan Build Fails

**Symptom**: Conan fails to build Vulkan packages from source.

**Cause**: Building Vulkan from source can take significant time and may fail on some platforms.

**Solution**:
1. Use system-wide Vulkan SDK instead (recommended):
```bash
# Set VULKAN_SDK to skip Conan Vulkan dependencies
export VULKAN_SDK=/path/to/vulkan/sdk
```

2. Or configure Conan to use pre-built binaries:
```bash
# In Conan profile
[buildenv]
CONAN_PREFER_BINARY=True

# Or when running Conan install
conan install . --build=missing -o "*/*:build=None"
```

3. Check Conan logs for specific errors:
```bash
# Run with verbose output
conan install . --build=missing --verbose
```

### Vulkan Validation Errors

**Symptom**: `VK_ERROR_VALIDATION_FAILED` or validation layer errors

**Cause**: Incorrect Vulkan API usage.

**Solution**:
1. Enable validation layers in debug builds:
```cpp
#ifdef _DEBUG
    const char* validation_layers[] = {
        "VK_LAYER_KHRONOS_validation"
    };
    
    VkInstanceCreateInfo create_info{};
    create_info.enabledLayerCount = 1;
    create_info.ppEnabledLayerNames = validation_layers;
#endif
```

2. Use RenderDoc to debug:
   - Install RenderDoc
   - Capture frames
   - Analyze draw calls

3. Check Vulkan specification:
   - Verify API usage against [Vulkan Spec](https://www.khronos.org/registry/vulkan-spec/)

### Vulkan Device Creation Fails

**Symptom**: `vkCreateDevice` returns `VK_ERROR_INITIALIZATION_FAILED`

**Cause**: No suitable GPU or missing features.

**Solution**:
1. Check available GPUs:
```bash
vulkaninfo --summary
```

2. Request fewer features:
```cpp
VkPhysicalDeviceFeatures features{};
features.samplerAnisotropy = VK_TRUE;  // Only request needed features
```

3. Use integrated GPU if discrete fails:
```cpp
// Prefer integrated GPU for compatibility
uint32_t device_count = 0;
vkEnumeratePhysicalDevices(instance, &device_count, devices);

// Select first device (usually integrated)
VkPhysicalDevice selected_device = devices[0];
```

## Qt Issues

### Qt Not Found

**Symptom**: `Could not find Qt6` or `Qt6::Core` not found

**Cause**: Qt not installed or CMake can't find it.

**Solution**:
1. Install Qt:
   - Windows: Download from [Qt](https://www.qt.io/)
   - Linux: `sudo apt install qt6-base-dev`
   - macOS: `brew install qt`

2. Set CMAKE_PREFIX_PATH:
```bash
cmake -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x.x/gcc_64 ..
```

3. Verify installation:
```bash
qmake --version
```

### Qt Plugin Loading Fails

**Symptom**: `Failed to load Qt platform plugin`

**Cause**: Missing Qt platform plugins or incorrect paths.

**Solution**:
1. Set QT_PLUGIN_PATH:
```bash
# Windows
set QT_PLUGIN_PATH=C:\Qt\6.x.x\plugins

# Linux/macOS
export QT_PLUGIN_PATH=/usr/local/Qt/plugins
```

2. Copy plugins to executable directory:
```bash
cp -r $QT_PLUGIN_PATH/platforms ./build/debug/
```

### Qt Style Issues

**Symptom**: Application looks wrong or has no styling

**Cause**: Missing Qt style plugins or incorrect theme.

**Solution**:
1. Set QT_QPA_PLATFORM:
```bash
# Windows
set QT_QPA_PLATFORM=windows

# Linux
export QT_QPA_PLATFORM=xcb

# macOS
export QT_QPA_PLATFORM=cocoa
```

2. Use Fusion style for cross-platform:
```cpp
#include <QApplication>
#include <QStyleFactory>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    // ...
}
```

## Engine Issues

### Engine Initialization Fails

**Symptom**: `create_engine()` returns `nullptr` or throws exception

**Cause**: Missing subsystems or invalid configuration.

**Solution**:
1. Check engine configuration:
```cpp
omnicpp::EngineConfig config;
config.renderer = nullptr;  // Use default
config.logger = nullptr;    // Use default
// ... other subsystems
```

2. Verify all subsystems can be created:
```cpp
// Test each subsystem individually
auto renderer = omnicpp::create_vulkan_renderer();
auto input = omnicpp::create_input_manager();
// ...
```

3. Check logs for specific error:
```cpp
omnicpp::ILogger* logger = omnicpp::create_console_logger();
logger->set_level(omnicpp::LogLevel::Debug);
```

### Entity Creation Fails

**Symptom**: `create_entity()` returns invalid entity

**Cause**: Entity pool exhausted or scene not initialized.

**Solution**:
1. Check entity pool size:
```cpp
const size_t kMaxEntities = 10000;
if (scene.get_entity_count() >= kMaxEntities) {
    // Pool exhausted
}
```

2. Initialize scene before creating entities:
```cpp
omnicpp::Scene scene;
scene.initialize();  // Must call first

omnicpp::Entity entity = scene.create_entity();
```

### Component Access Fails

**Symptom**: `get_component()` returns `nullptr`

**Cause**: Component not added or wrong component type.

**Solution**:
1. Verify component was added:
```cpp
entity.add_component(TransformComponent{});

if (!entity.has_component<TransformComponent>()) {
    // Component not added
}
```

2. Use correct component type:
```cpp
// Wrong
auto* mesh = entity.get_component<MeshComponent>();

// Correct
auto& mesh = entity.get_component<MeshComponent>();
```

### Resource Loading Fails

**Symptom**: `load_model()` returns `nullptr` or throws exception

**Cause**: File not found, unsupported format, or out of memory.

**Solution**:
1. Check file path:
```cpp
std::string path = "assets/models/cube.obj";
if (!std::filesystem::exists(path)) {
    logger->log_error("File not found: " + path);
}
```

2. Verify file format:
```bash
file assets/models/cube.obj
# Should show: Wavefront OBJ
```

3. Check available memory:
```cpp
size_t available_memory = get_available_memory();
size_t required_memory = estimate_model_size(path);

if (available_memory < required_memory) {
    logger->log_warning("Insufficient memory for model");
}
```

## Performance Issues

### Low Frame Rate

**Symptom**: FPS below 60 at 1080p

**Cause**: Too many draw calls, expensive shaders, or CPU bottleneck.

**Solution**:
1. Profile with built-in profiler:
```cpp
Profiler profiler("Render");
profiler.start();

renderer.render_scene(scene);

profiler.stop();
profiler.log();  // Check output for bottlenecks
```

2. Reduce draw calls:
```cpp
// Bad: individual draw calls
for (auto& entity : entities) {
    renderer.draw(entity);
}

// Good: batched draw calls
renderer.begin_batch();
for (auto& entity : entities) {
    renderer.add_to_batch(entity);
}
renderer.end_batch();
```

3. Use frustum culling:
```cpp
void Scene::render(IRenderer* renderer, const Camera& camera) {
    Frustum frustum = camera.get_frustum();
    
    for (auto& entity : entities) {
        if (frustum.contains(entity.get_bounds())) {
            renderer.render(entity);
        }
    }
}
```

### High Memory Usage

**Symptom**: Application uses excessive memory

**Cause**: Memory leaks, large textures, or uncached resources.

**Solution**:
1. Use memory profiler:
```bash
# Linux
valgrind --leak-check=full ./build/debug/omnicpp

# Windows
drmemory ./build/debug/omnicpp.exe
```

2. Implement resource pooling:
```cpp
class TexturePool {
    std::vector<Texture*> pool;
    std::vector<bool> used;
    
public:
    Texture* acquire() {
        for (size_t i = 0; i < pool.size(); ++i) {
            if (!used[i]) {
                used[i] = true;
                return pool[i];
            }
        }
        return nullptr;
    }
    
    void release(Texture* texture) {
        size_t index = texture - pool.data();
        used[index] = false;
    }
};
```

3. Compress textures:
```bash
# Use texture compression tools
astcenc -c input.png -o output.astc
```

### Stuttering

**Symptom**: Frame time spikes causing stutter

**Cause**: Garbage collection, asset loading, or shader compilation.

**Solution**:
1. Load assets asynchronously:
```cpp
class AsyncLoader {
    std::thread loading_thread;
    std::queue<std::string> load_queue;
    
public:
    void load_async(const std::string& path) {
        load_queue.push(path);
    }
    
    void update() {
        while (!load_queue.empty()) {
            std::string path = load_queue.front();
            load_queue.pop();
            resource_manager.load_async(path);
        }
    }
};
```

2. Pre-compile shaders:
```bash
# Use SPIR-V offline compilation
glslangValidator -V -o shader.vert.spv shader.vert
```

3. Use fixed timestep:
```cpp
const float kFixedDeltaTime = 1.0f / 60.0f;
float accumulator = 0.0f;

void Game::run() {
    while (is_running) {
        float frame_time = timer.get_delta_time();
        accumulator += frame_time;
        
        while (accumulator >= kFixedDeltaTime) {
            update(kFixedDeltaTime);
            accumulator -= kFixedDeltaTime;
        }
        
        render();
    }
}
```

## Platform-Specific Issues

### Windows Issues

#### DLL Not Found

**Symptom**: `The code execution cannot proceed because X.dll was not found`

**Cause**: Missing DLL in PATH or incorrect deployment.

**Solution**:
1. Copy required DLLs to executable directory:
```bash
cp vcpkg_installed/x64-windows/bin/*.dll build/debug/
```

2. Use static linking:
```bash
cmake -DOMNICPP_USE_STATIC_RUNTIME=ON ..
```

#### UAC Prompt

**Symptom**: Application asks for administrator privileges

**Cause**: Application manifest requires elevation.

**Solution**:
1. Remove manifest or set execution level:
```xml
<!-- app.exe.manifest -->
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" />
      </requestedPrivileges>
    </security>
  </trustInfo>
</assembly>
```

### Linux Issues

#### Library Path Issues

**Symptom**: `error while loading shared libraries: libX.so: cannot open shared object file`

**Cause**: Library not in LD_LIBRARY_PATH.

**Solution**:
1. Set LD_LIBRARY_PATH:
```bash
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

2. Use RPATH in CMake:
```cmake
set_target_properties(omnicpp PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
)
```

#### Wayland vs X11

**Symptom**: Application fails to start on Wayland

**Cause**: Qt using X11 backend on Wayland.

**Solution**:
1. Set QT_QPA_PLATFORM:
```bash
export QT_QPA_PLATFORM=wayland
```

2. Install Wayland Qt packages:
```bash
sudo apt install qt6-wayland-client
```

### macOS Issues

#### Code Signing

**Symptom**: Application blocked by Gatekeeper

**Cause**: Unsigned application.

**Solution**:
1. Sign application:
```bash
codesign --deep --force --verify --verbose --sign "Developer ID Application" ./build/release/omnicpp.app
```

2. Disable Gatekeeper (development only):
```bash
sudo spctl --master-disable
```

#### Notarization

**Symptom**: Application damaged and can't be opened

**Cause**: Missing notarization for distribution.

**Solution**:
1. Notarize application:
```bash
xcrun notarytool submit ./omnicpp.dmg --apple-id "com.example.omnicpp" --password "@keychain:altool" --wait
```

2. Staple ticket:
```bash
xcrun stapler staple ./omnicpp.dmg ./omnicpp.dmg
```

## Getting Help

### Log Files

Check log files for detailed error information:

- **Build logs**: `build/build.log`
- **Engine logs**: `omnicpp.log`
- **Test logs**: `tests/test_log.txt`

### Debug Builds

Always use debug builds when troubleshooting:

```bash
python OmniCppController.py build standalone "Build Project" default debug
```

### Verbose Output

Enable verbose output for more information:

```bash
python OmniCppController.py build standalone "Build Project" default debug --verbose
```

### Community Support

Get help from the community:

- **GitHub Issues**: [repository-url]/issues
- **Discord**: [discord-invite-link]
- **Stack Overflow**: Tag questions with `omnicpp`

### Reporting Bugs

When reporting bugs, include:

1. **Platform**: Windows, Linux, or macOS
2. **Compiler**: MSVC, GCC, or Clang
3. **Build Type**: Debug or Release
4. **Error Message**: Full error output
5. **Steps to Reproduce**: Clear reproduction steps
6. **Expected Behavior**: What should happen
7. **Actual Behavior**: What actually happens

**Bug Report Template**:
```markdown
## Bug Report

**Platform**: Windows 11
**Compiler**: MSVC 2022
**Build Type**: Debug

### Error Message
```
[Full error output here]
```

### Steps to Reproduce
1. Step 1
2. Step 2
3. Step 3

### Expected Behavior
[What should happen]

### Actual Behavior
[What actually happens]

### Additional Information
- [ ] I can reproduce this bug
- [ ] I checked the troubleshooting guide
- [ ] I searched existing issues
```
