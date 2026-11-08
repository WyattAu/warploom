# Warploom Game Engine - User Guide

## Table of Contents
- [Introduction](#introduction)
- [Getting Started](#getting-started)
- [Engine Architecture](#engine-architecture)
- [Core Concepts](#core-concepts)
- [Creating a Game](#creating-a-game)
- [Rendering](#rendering)
- [Input Handling](#input-handling)
- [Audio](#audio)
- [Physics](#physics)
- [Resources](#resources)
- [Scripting](#scripting)
- [Examples](#examples)

## Introduction

The Warploom Game Engine is a modern, high-performance C++ game engine built with Vulkan and Qt. It provides a comprehensive set of tools and systems for creating 2D and 3D games.

### Key Features

- **Modern C++20**: Leverages latest C++ features
- **Vulkan Rendering**: High-performance graphics API
- **Qt Integration**: Cross-platform UI and windowing
- **ECS Architecture**: Entity-Component-System for flexible game logic
- **Cross-Platform**: Windows, Linux, macOS support
- **Modular Design**: Easy to extend and customize

## Getting Started

### Prerequisites

- C++20 compatible compiler (MSVC 2019+, GCC 10+, Clang 12+)
- Vulkan SDK 1.3+
- Qt 6.5+
- CMake 3.20+
- Python 3.8+ (for build system)

### Building the Engine

1. Clone the repository:
```bash
git clone <repository-url>
cd warploom
```

2. Build the engine library:
```bash
python WarploomController.py build targets/qt-vulkan/library "Build Project" default release
```

3. Build the game executable:
```bash
python WarploomController.py build standalone "Build Project" default release
```

### Your First Game

Create a simple game in 5 steps:

```cpp
#include "game/Game.hpp"
#include "engine/Engine.hpp"

int main() {
    // 1. Create engine
    warploom::EngineConfig config;
    config.renderer = nullptr;  // Use default renderer
    config.logger = nullptr;    // Use default logger
    
    warploom::IEngine* engine = warploom::create_engine(config);
    
    // 2. Create game
    warploom::Game game;
    game.initialize();
    
    // 3. Run game loop
    game.run();
    
    // 4. Cleanup
    game.shutdown();
    warploom::destroy_engine(engine);
    
    return 0;
}
```

## Engine Architecture

### Core Systems

The engine is organized into several core systems:

- **Renderer**: Handles all graphics rendering
- **Input Manager**: Processes keyboard, mouse, and gamepad input
- **Audio Manager**: Plays sounds and music
- **Physics Engine**: Simulates physics and collisions
- **Resource Manager**: Loads and manages assets
- **Scene Manager**: Manages game scenes and entities
- **Script Manager**: Executes game scripts

### Module Structure

```
engine/
├── Engine.hpp          # Main engine interface
├── IRenderer.hpp       # Renderer interface
├── IInputManager.hpp   # Input manager interface
├── IAudioManager.hpp   # Audio manager interface
├── IPhysicsEngine.hpp  # Physics engine interface
├── IResourceManager.hpp # Resource manager interface
├── ILogger.hpp         # Logger interface
└── IPlatform.hpp       # Platform abstraction
```

## Core Concepts

### Entity-Component-System (ECS)

The engine uses an ECS architecture for game logic:

- **Entity**: A unique ID representing a game object
- **Component**: Data attached to entities (position, velocity, mesh)
- **System**: Logic that operates on entities with specific components

#### Example: Creating an Entity

```cpp
#include "engine/Entity.hpp"
#include "engine/Component.hpp"

// Create entity
warploom::Entity entity = scene.create_entity();

// Add components
warploom::TransformComponent transform;
transform.position = {0.0f, 0.0f, 0.0f};
entity.add_component(transform);

warploom::MeshComponent mesh;
mesh.model = resource_manager.load_model("cube.obj");
entity.add_component(mesh);
```

### Scene Graph

The scene graph organizes entities in a hierarchical structure:

```cpp
#include "engine/Scene.hpp"
#include "engine/SceneNode.hpp"

// Create scene
warploom::Scene scene;

// Create root node
warploom::SceneNode* root = scene.get_root();

// Add child nodes
warploom::SceneNode* child1 = root->create_child();
warploom::SceneNode* child2 = root->create_child();

// Attach entities to nodes
child1->attach_entity(entity1);
child2->attach_entity(entity2);
```

## Creating a Game

### Game Class Structure

```cpp
#include "game/Game.hpp"

class MyGame : public warploom::Game {
public:
    bool initialize() override {
        // Initialize game systems
        setup_scene();
        setup_input();
        setup_audio();
        return true;
    }
    
    void update(float delta_time) override {
        // Update game logic
        update_entities(delta_time);
        update_physics(delta_time);
    }
    
    void render() override {
        // Render scene
        renderer->render(scene);
    }
    
    void shutdown() override {
        // Cleanup resources
        cleanup_scene();
    }

private:
    warploom::Scene scene;
    warploom::IRenderer* renderer;
    // ... other members
};
```

### Game Loop

The game loop runs at a fixed timestep:

```cpp
void Game::run() {
    const float target_fps = 60.0f;
    const float target_frame_time = 1.0f / target_fps;
    
    while (is_running) {
        float delta_time = timer.get_delta_time();
        
        // Update
        update(delta_time);
        
        // Render
        render();
        
        // Cap framerate
        float frame_time = timer.get_elapsed_time();
        if (frame_time < target_frame_time) {
            sleep(target_frame_time - frame_time);
        }
    }
}
```

## Rendering

### Setting Up a Renderer

```cpp
#include "engine/IRenderer.hpp"
#include "engine/VulkanRenderer.hpp"

// Create Vulkan renderer
warploom::VulkanRenderer renderer;
renderer.initialize(window, vulkan_instance);

// Set clear color
renderer.set_clear_color({0.1f, 0.1f, 0.1f, 1.0f});

// Enable features
renderer.enable_depth_test(true);
renderer.enable_culling(true);
```

### Rendering a Scene

```cpp
void Game::render() {
    // Begin frame
    renderer.begin_frame();
    
    // Clear screen
    renderer.clear();
    
    // Render scene
    renderer.render_scene(scene);
    
    // End frame
    renderer.end_frame();
    
    // Present
    renderer.present();
}
```

### Shaders

```cpp
// Load shader
warploom::Shader* shader = resource_manager.load_shader(
    "vertex.glsl",
    "fragment.glsl"
);

// Use shader
renderer.use_shader(shader);

// Set uniforms
shader.set_uniform("model", model_matrix);
shader.set_uniform("view", view_matrix);
shader.set_uniform("projection", projection_matrix);
```

## Input Handling

### Keyboard Input

```cpp
#include "engine/IInputManager.hpp"

void Game::update(float delta_time) {
    warploom::IInputManager* input = engine->get_input_manager();
    
    // Check key state
    if (input->is_key_pressed(warploom::Key::W)) {
        camera.move_forward(delta_time);
    }
    if (input->is_key_pressed(warploom::Key::S)) {
        camera.move_backward(delta_time);
    }
}
```

### Mouse Input

```cpp
void Game::update(float delta_time) {
    warploom::IInputManager* input = engine->get_input_manager();
    
    // Get mouse position
    float x, y;
    input->get_mouse_position(x, y);
    
    // Get mouse delta
    float dx, dy;
    input->get_mouse_delta(dx, dy);
    
    // Rotate camera
    camera.rotate(dx, dy);
}
```

### Input Callbacks

```cpp
// Register key callback
input->register_key_callback([](warploom::Key key, bool pressed) {
    if (key == warploom::Key::Escape && pressed) {
        game->quit();
    }
});

// Register mouse callback
input->register_mouse_callback([](float x, float y) {
    camera->look_at(x, y);
});
```

## Audio

### Playing Sounds

```cpp
#include "engine/IAudioManager.hpp"

// Load sound
warploom::Sound* sound = resource_manager.load_sound("explosion.wav");

// Play sound
warploom::IAudioManager* audio = engine->get_audio_manager();
audio->play_sound(sound, 1.0f, 1.0f);  // volume, pitch
```

### Playing Music

```cpp
// Load music
warploom::Music* music = resource_manager.load_music("background.mp3");

// Play music
audio->play_music(music, true);  // loop

// Set volume
audio->set_music_volume(0.5f);
```

### 3D Audio

```cpp
// Play 3D sound
warploom::Vector3 position = {10.0f, 0.0f, 5.0f};
audio->play_sound_3d(sound, position, 1.0f);
```

## Physics

### Creating a Physics World

```cpp
#include "engine/IPhysicsEngine.hpp"

// Create physics engine
warploom::IPhysicsEngine* physics = engine->get_physics_engine();
physics->initialize();

// Set gravity
physics->set_gravity({0.0f, -9.81f, 0.0f});
```

### Creating Rigid Bodies

```cpp
// Create rigid body
warploom::RigidBody body;
body.mass = 1.0f;
body.shape = warploom::Shape::Box;
body.size = {1.0f, 1.0f, 1.0f};
body.position = {0.0f, 10.0f, 0.0f};

// Add to physics world
physics->add_rigid_body(body);
```

### Collision Detection

```cpp
// Check collision
if (physics->check_collision(body1, body2)) {
    // Handle collision
    handle_collision(body1, body2);
}

// Get collision events
physics->set_collision_callback([](const warploom::Collision& collision) {
    std::cout << "Collision detected!" << std::endl;
});
```

## Resources

### Loading Models

```cpp
#include "engine/IResourceManager.hpp"

// Load 3D model
warploom::Model* model = resource_manager.load_model("character.fbx");

// Create entity with model
warploom::Entity entity = scene.create_entity();
warploom::MeshComponent mesh;
mesh.model = model;
entity.add_component(mesh);
```

### Loading Textures

```cpp
// Load texture
warploom::Texture* texture = resource_manager.load_texture("diffuse.png");

// Apply to material
warploom::Material material;
material.diffuse_texture = texture;
material.shininess = 32.0f;
```

### Resource Caching

Resources are automatically cached for performance:

```cpp
// First call loads from disk
warploom::Model* model1 = resource_manager.load_model("cube.obj");

// Second call returns cached version
warploom::Model* model2 = resource_manager.load_model("cube.obj");

// model1 == model2 (same pointer)
```

## Scripting

### Lua Integration

The engine supports Lua scripting:

```lua
-- script.lua
function update(entity, delta_time)
    local transform = entity:get_transform()
    transform.position.y = transform.position.y + delta_time
    entity:set_transform(transform)
end

function on_collision(entity, other)
    print("Collision with " .. other.name)
end
```

### Loading Scripts

```cpp
// Load script
warploom::Script* script = resource_manager.load_script("script.lua");

// Attach to entity
entity.add_script(script);
```

### Calling C++ from Lua

```lua
-- Call C++ function
local position = entity:get_position()
entity:set_position(position.x + 1, position.y, position.z)
```

## Examples

### Rotating Cube

```cpp
#include "game/Game.hpp"

class RotatingCubeGame : public warploom::Game {
public:
    bool initialize() override {
        // Create cube entity
        cube = scene.create_entity();
        
        warploom::MeshComponent mesh;
        mesh.model = resource_manager.load_model("cube.obj");
        cube.add_component(mesh);
        
        warploom::TransformComponent transform;
        transform.position = {0.0f, 0.0f, -5.0f};
        cube.add_component(transform);
        
        return true;
    }
    
    void update(float delta_time) override {
        // Rotate cube
        auto& transform = cube.get_component<warploom::TransformComponent>();
        transform.rotation.y += delta_time * 2.0f;
    }

private:
    warploom::Entity cube;
};
```

### First Person Camera

```cpp
class FirstPersonCamera {
public:
    void update(float delta_time, warploom::IInputManager* input) {
        // Mouse look
        float dx, dy;
        input->get_mouse_delta(dx, dy);
        yaw += dx * sensitivity;
        pitch -= dy * sensitivity;
        
        // Movement
        float speed = 5.0f;
        if (input->is_key_pressed(warploom::Key::W))
            position += forward * speed * delta_time;
        if (input->is_key_pressed(warploom::Key::S))
            position -= forward * speed * delta_time;
        if (input->is_key_pressed(warploom::Key::A))
            position -= right * speed * delta_time;
        if (input->is_key_pressed(warploom::Key::D))
            position += right * speed * delta_time;
    }

private:
    warploom::Vector3 position;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float sensitivity = 0.1f;
};
```

## Best Practices

1. **Use ECS for game logic**: Keep data in components, logic in systems
2. **Cache resources**: Load resources once and reuse
3. **Use object pooling**: Reuse objects instead of creating/destroying
4. **Profile performance**: Use built-in profiling tools
5. **Handle errors gracefully**: Check return values and handle exceptions
6. **Keep scenes small**: Split large scenes into smaller chunks
7. **Use LOD**: Level of detail for distant objects

## Support

For more information:
- API Documentation: [api-documentation.md](api-documentation.md)
- Developer Guide: [developer-guide.md](developer-guide.md)
- Troubleshooting: [troubleshooting.md](troubleshooting.md)
