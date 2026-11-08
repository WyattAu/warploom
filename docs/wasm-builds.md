# WebAssembly (WASM) Builds

This document outlines the requirements and expected behavior for WebAssembly (WASM) builds in the Warploom project using Emscripten.

## Prerequisites

### Emscripten SDK Installation

Emscripten SDK installation is **required** for WebAssembly builds. The project uses Emscripten to cross-compile C++ code to WebAssembly that runs in web browsers.

#### Recommended Installation: Official Emscripten SDK

1. **Install Emscripten SDK:**
   ```bash
   # Clone the Emscripten repository
   git clone https://github.com/emscripten-core/emsdk.git
   cd emsdk

   # Download and install the latest SDK
   ./emsdk install latest

   # Activate the SDK
   ./emsdk activate latest

   # Set up environment variables (add to your shell profile)
   source ./emsdk_env.sh
   ```

2. **Verify Installation:**
   ```bash
   emcc --version
   em++ --version
   ```

   Expected output should show Emscripten version 3.1.45 or later.

#### Alternative: Package Manager Installation

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install emscripten

# Arch Linux
sudo pacman -S emscripten

# macOS with Homebrew
brew install emscripten
```

### Additional Requirements

- **Python 3.8+:** For build automation scripts
- **CMake 3.31+:** Build system
- **Conan 2.0+:** Package management
- **Node.js:** For running the development server (optional)
- **Web Browser:** Modern browser with WebAssembly support (Chrome, Firefox, Safari, Edge)

## Build Pipeline Behavior

When Emscripten is available, the build pipeline follows these steps:

### 1. Environment Setup

The build system automatically detects Emscripten:

- **Detection:** Checks for `emcc` and `em++` in PATH
- **EMSDK Environment:** Sources `emsdk_env.sh` if `EMSDK` environment variable is set
- **Default Configuration:** Uses Emscripten toolchain when `CMAKE_SYSTEM_NAME` is "Emscripten"
- **Cache Setup:** Configures `EM_CACHE` for Emscripten ports and cache

### 2. Conan Profile Configuration

WebAssembly builds use a specialized Conan profile for Emscripten:

```ini
[settings]
os=Emscripten
arch=wasm32
compiler=clang
compiler.version=18
compiler.libcxx=libc++
build_type=Release

[buildenv]
CC=emcc
CXX=em++
```

### 3. CMake Configuration

WebAssembly builds use Emscripten-specific CMake configuration:

- **Generator:** Ninja (preferred) or Unix Makefiles
- **Toolchain:** Emscripten toolchain file (`Emscripten.cmake`)
- **Build Directory:** `build/debug/emscripten` or `build/release/emscripten`
- **Compiler Flags:** Emscripten-specific flags for WebAssembly compilation

### 4. Build Process

The build pipeline executes:

1. **Dependency Installation:** Conan installs Emscripten-compatible packages
2. **Emscripten Ports:** Pre-compiles SDL2, FreeType, and other ports
3. **CMake Generation:** Configures build system with Emscripten toolchain
4. **Compilation:** Emscripten compiles C++ to WebAssembly
5. **Asset Packaging:** Preloads assets into virtual filesystem
6. **HTML Generation:** Creates web-ready HTML file with embedded JavaScript

## Expected Build Outputs

### Directory Structure

```
build/
├── debug/emscripten/
│   ├── bin/                    # Web deployment files
│   │   ├── WarploomStandalone.html    # Main HTML file
│   │   ├── WarploomStandalone.js      # JavaScript wrapper
│   │   ├── WarploomStandalone.wasm    # WebAssembly binary
│   │   ├── WarploomStandalone.data    # Preloaded assets
│   │   └── WarploomStandalone.wasm.map # Source map (debug builds)
│   ├── CMakeFiles/             # Build artifacts
│   └── conan/                  # Conan artifacts
└── release/emscripten/
    └── [same structure]
```

### Build Artifacts

- **HTML File (.html):** Complete web application with embedded JavaScript and WebAssembly
- **JavaScript File (.js):** Emscripten-generated JavaScript wrapper and runtime
- **WebAssembly File (.wasm):** Compiled WebAssembly binary
- **Data File (.data):** Preloaded assets in Emscripten virtual filesystem
- **Source Map (.wasm.map):** Debug information for browser debugging (debug builds only)

### Runtime Dependencies

WebAssembly builds run entirely in the browser:

- **WebAssembly Support:** Modern browser with WebAssembly enabled
- **WebGL2:** For graphics rendering (replaces Vulkan)
- **Web Audio API:** For audio playback
- **Canvas API:** For rendering output
- **Fetch API:** For asset loading

## WebAssembly-Specific Configuration

### Emscripten Toolchain

The build uses specialized Emscripten flags:

- **WebAssembly Output:** `-s WASM=1`
- **Memory Management:** `-s ALLOW_MEMORY_GROWTH=1`
- **Threading:** `-s USE_PTHREADS=1` (when enabled)
- **Async Operations:** `-s ASYNCIFY=1`
- **WebGL2 Support:** `-s USE_WEBGL2=1 -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2`

### SDL2 Integration

WebAssembly builds use SDL2 for cross-platform compatibility:

- **Graphics:** SDL2 with WebGL2 backend
- **Audio:** SDL2_mixer with Web Audio API
- **Input:** SDL2 event system mapped to browser events
- **Windowing:** Canvas-based rendering

### Asset Management

Assets are preloaded into Emscripten's virtual filesystem:

```cmake
# Preload assets command
--preload-file ${CMAKE_SOURCE_DIR}/assets@share/WarploomStandalone/assets
```

Assets are accessed at runtime via:
```cpp
// Load file from virtual filesystem
std::ifstream file("share/WarploomStandalone/assets/WarploomLogo.svg");
```

### HTML Shell File

Uses a custom HTML template (`assets/ems-mini.html`) with:

- **Canvas Setup:** Full-screen canvas for WebGL2 rendering
- **Console Output:** JavaScript console for C++ stdout/stderr
- **Asset Loading:** SVG logo loaded from virtual filesystem
- **Responsive Design:** Adapts to different screen sizes
- **Dark/Light Theme:** Automatic theme detection

## Qt6 and Vulkan Integration in Web Environments

### Qt6 in WebAssembly

Qt6 is **not used** in Emscripten builds due to web-specific requirements:

- **Native Qt6:** Used for desktop platforms (Windows, Linux, macOS)
- **WebAssembly Qt6:** Requires Qt for WebAssembly, which has different APIs
- **SDL2 Replacement:** SDL2 provides cross-platform graphics/audio APIs that work in browsers
- **WebGL2 Backend:** Direct WebGL2 usage instead of Qt's Vulkan abstraction

### Vulkan in Web Environments

Vulkan is **replaced with WebGL2** in web builds:

- **Native Vulkan:** Used for desktop GPU acceleration via Vulkan API
- **Web Vulkan:** WebGPU is emerging but not yet stable across browsers
- **WebGL2 Fallback:** Mature, widely-supported graphics API in browsers
- **SDL2 Abstraction:** SDL2 provides unified graphics API across platforms

### Graphics Pipeline Differences

| Feature | Native (Qt6 + Vulkan) | WebAssembly (SDL2 + WebGL2) |
|---------|----------------------|----------------------------|
| GUI Framework | Qt6 Widgets | SDL2 with custom UI |
| Graphics API | Vulkan | WebGL2 |
| Windowing | Native windows | HTML5 Canvas |
| Input Handling | Qt events | SDL2 events |
| Audio | Qt Multimedia | SDL2_mixer |
| Threading | Native threads | Emscripten pthreads |

## Build Commands

### VSCode Keyboard Workflow

1. **Setup:** Shift+F7 → "Zero to Hero" (complete setup)
2. **Build:** F7 (quick rebuild)
3. **Serve:** Ctrl+Alt+E (build and launch development server)
4. **Debug:** Browser developer tools for WebAssembly debugging

### Python Controller

```bash
# Complete WebAssembly build
python WarploomController.py build both "Zero to Hero" emscripten Debug

# Individual steps
python WarploomController.py build standalone "Conan install" emscripten Debug
python WarploomController.py build standalone "CMake configure" emscripten Debug
python WarploomController.py build standalone "Build" emscripten Debug

# Launch development server
python WarploomController.py build standalone "Launch Emscripten Server" noNeedArch
```

### CMake Direct

```bash
# Configure
cmake --preset emscripten-debug -DBUILD_LIBRARY=ON -DBUILD_STANDALONE=ON

# Build
cmake --build build/debug/emscripten -j $(nproc)

# Serve locally
emrun build/debug/emscripten/bin/WarploomStandalone.html
```

### Makefile

```bash
# Complete build
make ARCH=emscripten build

# Individual targets
make conan-install
make cmake-configure
make build-only

# Serve
make emscripten-serve
```

## Development Server

### Emscripten Development Server

The build system includes a development server:

```bash
# Launch server (Ctrl+Alt+E in VSCode)
python WarploomController.py build standalone "Launch Emscripten Server" noNeedArch
```

Features:
- **Auto-reload:** Refreshes browser on code changes
- **Console Output:** Displays C++ stdout/stderr in browser
- **Debug Support:** Source maps for debugging
- **Cross-Origin:** Handles CORS for asset loading

### Alternative: Python HTTP Server

```bash
cd build/debug/emscripten/bin
python3 -m http.server 8000
# Access at http://localhost:8000/WarploomStandalone.html
```

## Troubleshooting

### Common Issues

#### 1. "Emscripten not found" Error

**Symptoms:** Build fails with "emcc/em++ not found"

**Solutions:**
- Install Emscripten SDK as described above
- Source `emsdk_env.sh`: `source /path/to/emsdk/emsdk_env.sh`
- Add Emscripten to PATH
- Verify installation: `emcc --version`

#### 2. "EMSDK environment variable not set"

**Symptoms:** CMake fails with EMSDK error

**Solutions:**
- Set EMSDK environment variable: `export EMSDK=/path/to/emsdk`
- Source environment: `source $EMSDK/emsdk_env.sh`
- Add to shell profile for persistence

#### 3. WebGL2 Context Creation Failed

**Symptoms:** Application fails to start with WebGL errors

**Solutions:**
- Enable WebGL2 in browser settings
- Update browser to latest version
- Check GPU drivers and browser compatibility
- Try different browser (Chrome recommended)

#### 4. Asset Loading Failures

**Symptoms:** Assets not found or corrupted

**Solutions:**
- Verify asset paths in virtual filesystem
- Check preload-file CMake configuration
- Ensure assets directory exists and is accessible
- Clear browser cache and Emscripten cache

#### 5. Memory Growth Issues

**Symptoms:** Runtime crashes with memory errors

**Solutions:**
- Increase initial memory: `-s INITIAL_MEMORY=268435456`
- Enable memory growth: `-s ALLOW_MEMORY_GROWTH=1`
- Profile memory usage in browser dev tools
- Optimize asset sizes and loading

### Debug Information

Enable verbose output for troubleshooting:

```bash
# CMake verbose build
cmake --build build/debug/emscripten --verbose

# Emscripten verbose compilation
export EMCC_DEBUG=1
cmake --build build/debug/emscripten

# Browser debugging
# Open browser dev tools → Sources → WebAssembly
# Use source maps for C++ debugging
```

### Environment Verification

Run this script to verify Emscripten environment:

```bash
#!/bin/bash
echo "Emscripten Environment Check"
echo "============================"

echo "Checking EMSDK..."
if [ -n "$EMSDK" ]; then
    echo "✓ EMSDK set: $EMSDK"
    if [ -f "$EMSDK/emsdk_env.sh" ]; then
        echo "✓ emsdk_env.sh found"
    else
        echo "✗ emsdk_env.sh not found"
    fi
else
    echo "✗ EMSDK not set"
fi

echo -e "\nChecking compilers..."
if command -v emcc &> /dev/null; then
    echo "✓ emcc found: $(emcc --version | head -n1)"
else
    echo "✗ emcc not found"
fi

if command -v em++ &> /dev/null; then
    echo "✓ em++ found: $(em++ --version | head -n1)"
else
    echo "✗ em++ not found"
fi

echo -e "\nChecking tools..."
for tool in cmake ninja python3 conan; do
    if command -v $tool &> /dev/null; then
        echo "✓ $tool found: $($tool --version | head -n1)"
    else
        echo "✗ $tool not found"
    fi
done

echo -e "\nChecking Emscripten cache..."
if [ -n "$EM_CACHE" ]; then
    echo "✓ EM_CACHE set: $EM_CACHE"
    if [ -d "$EM_CACHE" ]; then
        echo "✓ Cache directory exists"
    else
        echo "⚠ Cache directory does not exist (will be created)"
    fi
else
    echo "⚠ EM_CACHE not set (using default)"
fi
```

## Performance Considerations

### Build Performance

- **Emscripten Cache:** Use `EM_CACHE` to speed up rebuilds
- **Parallel Jobs:** Limit to `-j1` for Emscripten to avoid issues
- **Incremental Builds:** Only rebuild changed files
- **Port Pre-compilation:** Pre-compile SDL2 and other ports

### Runtime Performance

- **WebAssembly Optimization:** `-O3` for release builds
- **Memory Management:** Use memory growth for large applications
- **Asset Compression:** Minimize asset sizes for faster loading
- **WebGL2 Optimization:** Efficient graphics rendering

## Integration with CI/CD

WebAssembly builds are supported in GitHub Actions:

```yaml
- name: Setup Emscripten
  uses: mymindstorm/setup-emsdk@v14
  with:
    version: '3.1.45'
    actions-cache-folder: 'emsdk-cache'

- name: Build WebAssembly
  run: |
    source $EMSDK/emsdk_env.sh
    export EM_CACHE=${{ github.workspace }}/.emscripten_cache
    python WarploomController.py build both "Zero to Hero" emscripten Release
```

## Browser Compatibility

### Supported Browsers

- **Chrome:** Full support (recommended)
- **Firefox:** Full support
- **Safari:** Full support (iOS 15+)
- **Edge:** Full support

### Required Features

- **WebAssembly:** `WebAssembly` global object
- **WebGL2:** `WebGL2RenderingContext`
- **SharedArrayBuffer:** For threading (requires secure context)
- **Cross-Origin Isolation:** For shared memory (optional)

## Best Practices

1. **Use Emscripten SDK:** Official SDK over package managers for latest features
2. **Set Up Caching:** Use `EM_CACHE` for faster builds
3. **Test in Multiple Browsers:** Verify compatibility across target browsers
4. **Optimize Assets:** Compress and minimize asset sizes
5. **Enable Source Maps:** For debugging in development
6. **Use Development Server:** For iterative development
7. **Monitor Memory Usage:** Profile and optimize memory consumption
8. **Handle Async Operations:** Use Emscripten's ASYNCIFY for synchronous-looking async code

## Limitations

1. **No Qt6 Integration:** Qt6 for WebAssembly has different APIs than desktop Qt6
2. **Vulkan Replacement:** WebGL2 instead of Vulkan for graphics
3. **Threading Constraints:** Shared memory requires cross-origin isolation
4. **File System Access:** Limited to Emscripten virtual filesystem
5. **Native APIs:** No access to native OS APIs
6. **Testing:** No unit testing in WebAssembly builds
7. **Code Coverage:** Not supported for WebAssembly targets

## Support

For WebAssembly-specific issues:

1. Check Emscripten documentation: https://emscripten.org/docs/
2. Review WebAssembly MDN docs: https://developer.mozilla.org/en-US/docs/WebAssembly
3. Verify browser compatibility: https://caniuse.com/wasm
4. Test with Emscripten examples to isolate issues
5. Check browser developer console for runtime errors

The build system provides comprehensive logging to help diagnose WebAssembly-specific problems.