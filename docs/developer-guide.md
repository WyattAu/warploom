# Warploom Game Engine - Developer Guide

## Table of Contents
- [Introduction](#introduction)
- [Development Environment](#development-environment)
- [Building from Source](#building-from-source)
- [Code Structure](#code-structure)
- [Adding Features](#adding-features)
- [Testing](#testing)
- [Debugging](#debugging)
- [Performance Optimization](#performance-optimization)
- [Contributing](#contributing)
- [Release Process](#release-process)

## Introduction

This guide is for developers who want to contribute to the Warploom Game Engine or extend it for their own projects.

### Development Philosophy

- **Modularity**: Keep systems independent and loosely coupled
- **Performance**: Optimize for 60 FPS at 1080p
- **Cross-Platform**: Support Windows, Linux, and macOS
- **Modern C++**: Use C++20 features where appropriate
- **Documentation**: Document all public APIs

## Development Environment

### Required Tools

- **Compiler**: MSVC 2019+, GCC 10+, or Clang 12+
- **CMake**: 3.20 or higher
- **Python**: 3.8 or higher (for build system)
- **Git**: For version control
- **Vulkan SDK**: 1.3 or higher
- **Qt**: 6.5 or higher

### Recommended IDEs

- **Visual Studio 2022**: Windows development
- **Visual Studio Code**: Cross-platform with C++ extension
- **CLion**: Cross-platform with CMake support
- **Qt Creator**: Qt-specific development

### Linux-Specific Setup

For Linux development, especially on CachyOS, the project provides enhanced setup options:

#### Nix Development Environment

The project includes comprehensive Nix support for reproducible builds:

```bash
# Enter Nix development environment
nix develop

# Or use specific toolchain
nix develop .#gcc
nix develop .#clang
nix develop .#cachyos-gcc
nix develop .#cachyos-clang
```

> **Note:** See [`docs/nix-development.md`](nix-development.md:1) for detailed Nix setup instructions.

#### CachyOS Optimization

CachyOS is the primary Linux target with performance-optimized builds:

```bash
# Use CachyOS CMake preset
cmake --preset cachyos-gcc-release

# Or use CachyOS Conan profile
conan install . --profile cachyos
```

> **Note:** See [`docs/cachyos-builds.md`](cachyos-builds.md:1) for CachyOS-specific instructions.

#### Linux Build Tools

The project provides Linux-specific build tools and configurations:

```bash
# Install Linux dependencies
sudo pacman -S gcc cmake ninja python python-pip

# Install Conan
pip install conan

# Configure with Linux preset
cmake --preset linux-gcc-debug

# Build
cmake --build --preset linux-gcc-debug
```

#### VSCode Linux Configuration

VSCode provides Linux-specific tasks and debug configurations:

- **Linux Build Tasks**: GCC and Clang specific build tasks
- **Linux Debug Configurations**: GDB and LLDB debug configurations
- **CMake Tools Integration**: Automatic preset detection

> **Note:** See [`docs/vscode-linux-setup.md`](vscode-linux-setup.md:1) for detailed VSCode Linux setup instructions.

### Setting Up

1. Clone the repository:
```bash
git clone <repository-url>
cd OmniCPP-template
```

2. Install Python dependencies:
```bash
pip install -r requirements-docs.txt
```

3. Install Vulkan SDK:
   - Windows: Download from [LunarG](https://vulkan.lunarg.com/)
   - Linux: `sudo apt install vulkan-sdk`
   - macOS: `brew install vulkan-headers`

4. Install Qt:
   - Windows: Download from [Qt](https://www.qt.io/)
   - Linux: `sudo apt install qt6-base-dev`
   - macOS: `brew install qt`

## Building from Source

### Debug Build

```bash
python WarploomController.py build standalone "Build Project" default debug
```

### Release Build

```bash
python WarploomController.py build standalone "Build Project" default release
```

### Building Specific Components

Build only the engine library:
```bash
python WarploomController.py build targets/qt-vulkan/library "Build Project" default debug
```

Build only tests:
```bash
python WarploomController.py build tests "Build Tests" default debug
```

## Code Structure

### Directory Layout

```
OmniCPP-template/
├── include/           # Public headers
│   ├── engine/       # Engine interfaces
│   └── WarploomLib/  # Library headers
├── src/              # Implementation
│   ├── engine/       # Engine implementation
│   └── game/        # Game implementation
├── tests/            # Test suite
├── warploom_scripts/     # Build system
├── cmake/            # CMake modules
├── docs/             # Documentation
├── examples/          # Example code
└── assets/           # Game assets
```

### Module Organization

#### Engine Core

- `Engine.hpp/cpp`: Main engine interface
- `IRenderer.hpp/cpp`: Renderer interface
- `IInputManager.hpp/cpp`: Input manager interface
- `IAudioManager.hpp/cpp`: Audio manager interface
- `IPhysicsEngine.hpp/cpp`: Physics engine interface
- `IResourceManager.hpp/cpp`: Resource manager interface
- `ILogger.hpp/cpp`: Logger interface
- `IPlatform.hpp/cpp`: Platform abstraction

#### Engine Implementation

- `VulkanRenderer.hpp/cpp`: Vulkan renderer
- `VulkanRenderer.cpp`: Vulkan-specific code
- `InputManager.hpp/cpp`: Input manager implementation
- `AudioManager.hpp/cpp`: Audio manager implementation
- `PhysicsEngine.hpp/cpp`: Physics engine implementation
- `ResourceManager.hpp/cpp`: Resource manager implementation
- `ConsoleLogger.hpp/cpp`: Console logger
- `Platform.hpp/cpp`: Platform-specific code

#### Game

- `Game.hpp/cpp`: Main game class
- `Scene.hpp/cpp`: Scene management
- `Entity.hpp/cpp`: Entity-Component-System
- `Component.hpp/cpp`: Component definitions

## Adding Features

### Adding a New Component

1. Define the component in `include/engine/Component.hpp`:

```cpp
struct HealthComponent : public Component {
    float current_health;
    float max_health;
    
    HealthComponent(float max = 100.0f)
        : current_health(max), max_health(max) {}
};
```

2. Register the component type in `ComponentType` enum:

```cpp
enum class ComponentType {
    Transform,
    Mesh,
    Camera,
    Health  // Add new type
};
```

3. Use the component:

```cpp
Entity entity = scene.create_entity();
HealthComponent health(100.0f);
entity.add_component(health);
```

### Adding a New System

1. Define the system interface:

```cpp
class ISystem {
public:
    virtual ~ISystem() = default;
    virtual void update(float delta_time) = 0;
    virtual void render() {}
};
```

2. Implement the system:

```cpp
class HealthSystem : public ISystem {
public:
    void update(float delta_time) override {
        for (auto& entity : entities) {
            if (entity.has_component<HealthComponent>()) {
                auto& health = entity.get_component<HealthComponent>();
                // Update health logic
            }
        }
    }
};
```

3. Register the system with the engine:

```cpp
engine->register_system<HealthSystem>();
```

### Adding a New Renderer Backend

1. Implement the `IRenderer` interface:

```cpp
class OpenGLRenderer : public IRenderer {
public:
    bool initialize(void* window, void* instance) override {
        // OpenGL initialization
    }
    
    void begin_frame() override {
        // OpenGL begin frame
    }
    
    // ... implement other methods
};
```

2. Add to CMake build:

```cmake
if(WARPLOOM_USE_OPENGL)
    target_sources(warploom_engine PRIVATE
        src/engine/OpenGLRenderer.cpp
    )
    target_link_libraries(warploom_engine PRIVATE
        OpenGL::GL
    )
endif()
```

3. Update engine factory:

```cpp
IRenderer* create_renderer(RendererType type) {
    switch (type) {
        case RendererType::Vulkan:
            return new VulkanRenderer();
        case RendererType::OpenGL:
            return new OpenGLRenderer();
    }
}
```

### Adding a New Platform

1. Implement the `IPlatform` interface:

```cpp
class LinuxPlatform : public IPlatform {
public:
    void* create_window(int width, int height, const char* title) override {
        // Linux window creation using X11 or Wayland
    }
    
    void destroy_window(void* window) override {
        // Linux window destruction
    }
    
    // ... implement other methods
};
```

2. Add platform detection:

```cpp
IPlatform* create_platform() {
    #ifdef _WIN32
        return new WindowsPlatform();
    #elif __linux__
        return new LinuxPlatform();
    #elif __APPLE__
        return new MacOSPlatform();
    #endif
}
```

## Testing

### Unit Tests

Write unit tests for new features:

```cpp
// tests/test_health_system.cpp
#include <catch2/catch_all.hpp>
#include "engine/HealthSystem.hpp"

TEST_CASE("HealthSystem updates health", "[health]") {
    HealthSystem system;
    Entity entity;
    
    HealthComponent health(100.0f);
    entity.add_component(health);
    
    system.update(1.0f);
    
    REQUIRE(entity.get_component<HealthComponent>().current_health == 100.0f);
}
```

### Integration Tests

Test integration between systems:

```cpp
TEST_CASE("Health and Physics integration", "[integration]") {
    Scene scene;
    HealthSystem health_system;
    PhysicsSystem physics_system;
    
    Entity entity = scene.create_entity();
    entity.add_component(HealthComponent(100.0f));
    entity.add_component(RigidBody());
    
    health_system.update(1.0f);
    physics_system.update(1.0f);
    
    // Verify integration
}
```

### Running Tests

```bash
# Run all tests
python tests/run_all_tests.py

# Run specific test suite
python -m unittest tests.test_engine

# Run with coverage
python tests/run_all_tests.py --coverage
```

## Debugging

### Debug Builds

Always use debug builds during development:

```bash
python WarploomController.py build standalone "Build Project" default debug
```

### Logging

Use the logger for debugging:

```cpp
#include "engine/ILogger.hpp"

ILogger* logger = engine->get_logger();
logger->log_info("Initializing system");
logger->log_warning("Low memory");
logger->log_error("Failed to load resource");
```

### Vulkan Validation Layers

Enable Vulkan validation for debugging:

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

### RenderDoc

Use RenderDoc for graphics debugging:

1. Install RenderDoc
2. Launch game through RenderDoc
3. Capture frames
4. Analyze draw calls, resources, and shaders

### Visual Studio Debugger

Configure Visual Studio for debugging:

1. Open `OmniCPP-template.sln`
2. Set startup project to `warploom_tests`
3. Set breakpoints
4. Press F5 to start debugging

### GDB (Linux)

Use GDB for debugging on Linux:

```bash
gdb ./build/debug/warploom
(gdb) break main
(gdb) run
(gdb) step
```

## Performance Optimization

### Profiling

Use built-in profiling:

```cpp
#include "engine/Profiler.hpp"

Profiler profiler("Update");

void Game::update(float delta_time) {
    profiler.start();
    
    // Update logic
    
    profiler.stop();
    profiler.log();
}
```

### Memory Management

Follow memory best practices:

1. **Use smart pointers**:
```cpp
std::unique_ptr<IRenderer> renderer = std::make_unique<VulkanRenderer>();
```

2. **Pool objects**:
```cpp
class EntityPool {
    std::vector<Entity> pool;
    std::vector<size_t> free_list;
    
public:
    Entity* allocate() {
        if (free_list.empty()) {
            pool.emplace_back();
            return &pool.back();
        }
        size_t index = free_list.back();
        free_list.pop_back();
        return &pool[index];
    }
    
    void deallocate(Entity* entity) {
        size_t index = entity - pool.data();
        free_list.push_back(index);
    }
};
```

3. **Cache resources**:
```cpp
std::unordered_map<std::string, Model*> model_cache;

Model* load_model(const std::string& path) {
    auto it = model_cache.find(path);
    if (it != model_cache.end()) {
        return it->second;
    }
    
    Model* model = load_model_from_disk(path);
    model_cache[path] = model;
    return model;
}
```

### Rendering Optimization

1. **Batch draw calls**:
```cpp
renderer.begin_batch();
for (auto& entity : entities) {
    renderer.add_to_batch(entity);
}
renderer.end_batch();
```

2. **Use frustum culling**:
```cpp
void Scene::render(IRenderer* renderer) {
    for (auto& entity : entities) {
        if (frustum.contains(entity.get_bounds())) {
            renderer.render(entity);
        }
    }
}
```

3. **Level of Detail (LOD)**:
```cpp
void Scene::render(IRenderer* renderer, const Camera& camera) {
    float distance = camera.get_distance_to(entity);
    int lod = calculate_lod(distance);
    renderer.render(entity, lod);
}
```

## Contributing

### Code Style

Follow the project's code style:

- **Indentation**: 4 spaces
- **Line length**: 100 characters maximum
- **Naming**:
  - Classes: `PascalCase` (e.g., `ResourceManager`)
  - Functions: `snake_case` (e.g., `load_model`)
  - Variables: `snake_case` (e.g., `model_count`)
  - Constants: `kPascalCase` (e.g., `kMaxEntities`)
- **Braces**: K&R style
- **Pointers**: `Type* name` (not `Type *name`)

### Commit Messages

Follow conventional commits:

```
<type>(<scope>): <subject>

<body>

<footer>
```

**Types**:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation
- `style`: Code style
- `refactor`: Code refactoring
- `test`: Tests
- `chore`: Maintenance

**Examples**:
```
feat(renderer): add support for MSAA

Implement multi-sample anti-aliasing support in Vulkan renderer.
Added MSAA configuration to EngineConfig.

Closes #123
```

### Pull Request Process

1. Fork the repository
2. Create a feature branch:
```bash
git checkout -b feature/my-feature
```
3. Make changes and commit
4. Push to your fork
5. Create a pull request
6. Wait for review
7. Address feedback
8. Merge when approved

### Code Review Checklist

Before submitting a PR, ensure:

- [ ] Code follows style guidelines
- [ ] All tests pass
- [ ] New features have tests
- [ ] Documentation is updated
- [ ] No compiler warnings
- [ ] Memory leaks are fixed
- [ ] Performance is acceptable

## Release Process

### Versioning

Follow semantic versioning: `MAJOR.MINOR.PATCH`

- **MAJOR**: Incompatible API changes
- **MINOR**: Backwards-compatible features
- **PATCH**: Backwards-compatible bug fixes

### Release Checklist

Before releasing:

1. [ ] Update version in `include/WarploomLib/version.h`
2. [ ] Update CHANGELOG.md
3. [ ] Run full test suite
4. [ ] Test on all platforms
5. [ ] Test with all compilers
6. [ ] Update documentation
7. [ ] Create release tag
8. [ ] Build release artifacts
9. [ ] Upload to distribution

### Creating a Release

```bash
# Update version
echo "#define WARPLOOM_VERSION_MAJOR 1" > include/WarploomLib/version.h
echo "#define WARPLOOM_VERSION_MINOR 1" >> include/WarploomLib/version.h
echo "#define WARPLOOM_VERSION_PATCH 0" >> include/WarploomLib/version.h

# Commit changes
git add include/WarploomLib/version.h
git commit -m "chore: bump version to 1.1.0"

# Create tag
git tag -a v1.1.0 -m "Release 1.1.0"
git push origin v1.1.0

# Build release
python WarploomController.py build standalone "Build Project" default release
python WarploomController.py build standalone "Create Distribution Tarballs" default release
```

## Additional Resources

- [User Guide](user-guide-game-engine.md)
- [API Documentation](api-documentation.md)
- [Build System Guide](user-guide-build-system.md)
- [Troubleshooting Guide](troubleshooting.md)
- [Analysis Documents](../impl/debug/analysis/)
- [Error Documentation](../impl/debug/errors/identified_errors.md)

## Support

For developer questions:
- GitHub Discussions: [repository-url]/discussions
- Email: dev@example.com
- Discord: [discord-invite-link]
