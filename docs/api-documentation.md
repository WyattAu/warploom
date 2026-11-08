# Warploom Game Engine - API Documentation

## Table of Contents
- [Engine API](#engine-api)
- [Renderer API](#renderer-api)
- [Input Manager API](#input-manager-api)
- [Audio Manager API](#audio-manager-api)
- [Physics Engine API](#physics-engine-api)
- [Resource Manager API](#resource-manager-api)
- [Scene Manager API](#scene-manager-api)
- [Entity-Component-System API](#entity-component-system-api)
- [Types and Enums](#types-and-enums)

## Engine API

### IEngine Interface

Main engine interface providing access to all subsystems.

#### Methods

##### `initialize() -> bool`

Initialize the engine and all subsystems.

**Returns**: `true` if successful, `false` otherwise

**Example**:
```cpp
warploom::IEngine* engine = warploom::create_engine(config);
if (!engine->initialize()) {
    std::cerr << "Failed to initialize engine" << std::endl;
    return -1;
}
```

##### `shutdown() -> void`

Shutdown the engine and cleanup all resources.

**Example**:
```cpp
engine->shutdown();
warploom::destroy_engine(engine);
```

##### `is_initialized() -> bool`

Check if the engine is initialized.

**Returns**: `true` if initialized, `false` otherwise

##### `get_renderer() -> IRenderer*`

Get the renderer subsystem.

**Returns**: Pointer to renderer interface

##### `get_input_manager() -> IInputManager*`

Get the input manager subsystem.

**Returns**: Pointer to input manager interface

##### `get_audio_manager() -> IAudioManager*`

Get the audio manager subsystem.

**Returns**: Pointer to audio manager interface

##### `get_physics_engine() -> IPhysicsEngine*`

Get the physics engine subsystem.

**Returns**: Pointer to physics engine interface

##### `get_resource_manager() -> IResourceManager*`

Get the resource manager subsystem.

**Returns**: Pointer to resource manager interface

##### `get_scene_manager() -> ISceneManager*`

Get the scene manager subsystem.

**Returns**: Pointer to scene manager interface

##### `get_logger() -> ILogger*`

Get the logger subsystem.

**Returns**: Pointer to logger interface

##### `get_platform() -> IPlatform*`

Get the platform abstraction layer.

**Returns**: Pointer to platform interface

#### Functions

##### `create_engine(config: EngineConfig) -> IEngine*`

Create an engine instance with the specified configuration.

**Parameters**:
- `config`: Engine configuration structure

**Returns**: Pointer to created engine instance

##### `destroy_engine(engine: IEngine*) -> void`

Destroy an engine instance and cleanup resources.

**Parameters**:
- `engine`: Pointer to engine instance to destroy

##### `engine_get_version() -> const char*`

Get the engine version string.

**Returns**: Version string (e.g., "1.0.0")

##### `engine_get_version_major() -> int`

Get the engine major version number.

**Returns**: Major version number

##### `engine_get_version_minor() -> int`

Get the engine minor version number.

**Returns**: Minor version number

##### `engine_get_version_patch() -> int`

Get the engine patch version number.

**Returns**: Patch version number

### EngineConfig Structure

Configuration structure for engine initialization.

```cpp
struct EngineConfig {
    IRenderer* renderer;
    IInputManager* input_manager;
    IAudioManager* audio_manager;
    IPhysicsEngine* physics_engine;
    IResourceManager* resource_manager;
    ILogger* logger;
    IPlatform* platform;
};
```

**Members**:
- `renderer`: Renderer instance (or `nullptr` for default)
- `input_manager`: Input manager instance (or `nullptr` for default)
- `audio_manager`: Audio manager instance (or `nullptr` for default)
- `physics_engine`: Physics engine instance (or `nullptr` for default)
- `resource_manager`: Resource manager instance (or `nullptr` for default)
- `logger`: Logger instance (or `nullptr` for default)
- `platform`: Platform instance (or `nullptr` for default)

## Renderer API

### IRenderer Interface

Renderer interface for graphics operations.

#### Methods

##### `initialize(window: void*, instance: void*) -> bool`

Initialize the renderer with a window and Vulkan instance.

**Parameters**:
- `window`: Native window handle
- `instance`: Vulkan instance handle

**Returns**: `true` if successful, `false` otherwise

##### `shutdown() -> void`

Shutdown the renderer and cleanup resources.

##### `begin_frame() -> void`

Begin a new frame for rendering.

##### `end_frame() -> void`

End the current frame.

##### `present() -> void`

Present the rendered frame to the screen.

##### `clear() -> void`

Clear the render targets with the clear color.

##### `set_clear_color(color: Vector4) -> void`

Set the clear color for the screen.

**Parameters**:
- `color`: RGBA color values (0.0-1.0)

##### `render_scene(scene: Scene) -> void`

Render a scene.

**Parameters**:
- `scene`: Scene to render

##### `use_shader(shader: Shader*) -> void`

Set the active shader.

**Parameters**:
- `shader`: Shader to use

##### `enable_depth_test(enable: bool) -> void`

Enable or disable depth testing.

**Parameters**:
- `enable`: `true` to enable, `false` to disable

##### `enable_culling(enable: bool) -> void`

Enable or disable backface culling.

**Parameters**:
- `enable`: `true` to enable, `false` to disable

### VulkanRenderer Class

Vulkan implementation of the renderer interface.

#### Additional Methods

##### `get_device() -> VkDevice`

Get the Vulkan logical device.

**Returns**: Vulkan device handle

##### `get_physical_device() -> VkPhysicalDevice`

Get the Vulkan physical device.

**Returns**: Vulkan physical device handle

##### `get_queue() -> VkQueue`

Get the Vulkan graphics queue.

**Returns**: Vulkan queue handle

## Input Manager API

### IInputManager Interface

Input manager interface for handling user input.

#### Methods

##### `update() -> void`

Update input state (call once per frame).

##### `is_key_pressed(key: Key) -> bool`

Check if a key is currently pressed.

**Parameters**:
- `key`: Key to check

**Returns**: `true` if pressed, `false` otherwise

##### `is_key_just_pressed(key: Key) -> bool`

Check if a key was just pressed this frame.

**Parameters**:
- `key`: Key to check

**Returns**: `true` if just pressed, `false` otherwise

##### `get_mouse_position(x: float&, y: float&) -> void`

Get the current mouse position.

**Parameters**:
- `x`: Output parameter for X position
- `y`: Output parameter for Y position

##### `get_mouse_delta(dx: float&, dy: float&) -> void`

Get the mouse movement since last frame.

**Parameters**:
- `dx`: Output parameter for X delta
- `dy`: Output parameter for Y delta

##### `is_mouse_button_pressed(button: MouseButton) -> bool`

Check if a mouse button is currently pressed.

**Parameters**:
- `button`: Mouse button to check

**Returns**: `true` if pressed, `false` otherwise

##### `register_key_callback(callback: KeyCallback) -> void`

Register a callback for key events.

**Parameters**:
- `callback`: Function to call on key events

##### `register_mouse_callback(callback: MouseCallback) -> void`

Register a callback for mouse events.

**Parameters**:
- `callback`: Function to call on mouse events

### Key Enum

Enumeration of keyboard keys.

```cpp
enum class Key {
    Unknown = 0,
    Space, Apostrophe, Comma, Minus, Period, Slash,
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    Semicolon, Equal,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    LeftBracket, Backslash, RightBracket,
    GraveAccent,
    World1, World2,
    Escape, Enter, Tab, Backspace, Insert, Delete,
    Right, Left, Down, Up,
    PageUp, PageDown, Home, End, CapsLock, ScrollLock, NumLock,
    PrintScreen, Pause,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24, F25,
    KP_0, KP_1, KP_2, KP_3, KP_4, KP_5, KP_6, KP_7, KP_8, KP_9,
    KP_Decimal, KP_Divide, KP_Multiply, KP_Subtract, KP_Add, KP_Enter, KP_Equal,
    LeftShift, LeftControl, LeftAlt, LeftSuper,
    RightShift, RightControl, RightAlt, RightSuper, Menu
};
```

### MouseButton Enum

Enumeration of mouse buttons.

```cpp
enum class MouseButton {
    Left = 0,
    Right = 1,
    Middle = 2,
    Button4 = 3,
    Button5 = 4,
    Button6 = 5,
    Button7 = 6,
    Button8 = 7
};
```

## Audio Manager API

### IAudioManager Interface

Audio manager interface for sound and music playback.

#### Methods

##### `initialize() -> bool`

Initialize the audio system.

**Returns**: `true` if successful, `false` otherwise

##### `shutdown() -> void`

Shutdown the audio system.

##### `update() -> void`

Update audio state (call once per frame).

##### `play_sound(sound: Sound*, volume: float, pitch: float) -> void`

Play a sound effect.

**Parameters**:
- `sound`: Sound to play
- `volume`: Volume (0.0-1.0)
- `pitch`: Pitch multiplier (1.0 = normal)

##### `play_sound_3d(sound: Sound*, position: Vector3, volume: float) -> void`

Play a 3D positioned sound.

**Parameters**:
- `sound`: Sound to play
- `position`: 3D position in world space
- `volume`: Volume (0.0-1.0)

##### `play_music(music: Music*, loop: bool) -> void`

Play background music.

**Parameters**:
- `music`: Music to play
- `loop`: `true` to loop, `false` to play once

##### `stop_music() -> void`

Stop the currently playing music.

##### `set_music_volume(volume: float) -> void`

Set the master music volume.

**Parameters**:
- `volume`: Volume (0.0-1.0)

##### `set_sound_volume(volume: float) -> void`

Set the master sound volume.

**Parameters**:
- `volume`: Volume (0.0-1.0)

### Sound Class

Represents a sound effect.

#### Methods

##### `get_duration() -> float`

Get the duration of the sound in seconds.

**Returns**: Duration in seconds

### Music Class

Represents background music.

#### Methods

##### `get_duration() -> float`

Get the duration of the music in seconds.

**Returns**: Duration in seconds

## Physics Engine API

### IPhysicsEngine Interface

Physics engine interface for physics simulation.

#### Methods

##### `initialize() -> bool`

Initialize the physics engine.

**Returns**: `true` if successful, `false` otherwise

##### `shutdown() -> void`

Shutdown the physics engine.

##### `update(delta_time: float) -> void`

Update physics simulation.

**Parameters**:
- `delta_time`: Time since last update in seconds

##### `set_gravity(gravity: Vector3) -> void`

Set the global gravity vector.

**Parameters**:
- `gravity`: Gravity vector (m/s²)

##### `add_rigid_body(body: RigidBody) -> void`

Add a rigid body to the physics world.

**Parameters**:
- `body`: Rigid body to add

##### `remove_rigid_body(body: RigidBody) -> void`

Remove a rigid body from the physics world.

**Parameters**:
- `body`: Rigid body to remove

##### `check_collision(body1: RigidBody, body2: RigidBody) -> bool`

Check if two bodies are colliding.

**Parameters**:
- `body1`: First rigid body
- `body2`: Second rigid body

**Returns**: `true` if colliding, `false` otherwise

##### `set_collision_callback(callback: CollisionCallback) -> void`

Set a callback for collision events.

**Parameters**:
- `callback`: Function to call on collisions

### RigidBody Structure

Represents a rigid body in the physics simulation.

```cpp
struct RigidBody {
    float mass;
    Shape shape;
    Vector3 size;
    Vector3 position;
    Vector3 velocity;
    Vector3 angular_velocity;
    bool is_static;
};
```

**Members**:
- `mass`: Mass in kilograms
- `shape`: Collision shape
- `size`: Dimensions for box/cylinder shapes
- `position`: Position in world space
- `velocity`: Linear velocity
- `angular_velocity`: Angular velocity
- `is_static`: `true` if static, `false` if dynamic

### Shape Enum

Enumeration of collision shapes.

```cpp
enum class Shape {
    Box,
    Sphere,
    Cylinder,
    Capsule,
    ConvexHull,
    TriangleMesh
};
```

## Resource Manager API

### IResourceManager Interface

Resource manager interface for loading and managing assets.

#### Methods

##### `initialize() -> bool`

Initialize the resource manager.

**Returns**: `true` if successful, `false` otherwise

##### `shutdown() -> void`

Shutdown the resource manager and cleanup resources.

##### `load_model(path: std::string) -> Model*`

Load a 3D model from file.

**Parameters**:
- `path`: Path to model file

**Returns**: Pointer to loaded model

##### `load_texture(path: std::string) -> Texture*`

Load a texture from file.

**Parameters**:
- `path`: Path to texture file

**Returns**: Pointer to loaded texture

##### `load_shader(vertex_path: std::string, fragment_path: std::string) -> Shader*`

Load a shader from files.

**Parameters**:
- `vertex_path`: Path to vertex shader
- `fragment_path`: Path to fragment shader

**Returns**: Pointer to loaded shader

##### `load_sound(path: std::string) -> Sound*`

Load a sound from file.

**Parameters**:
- `path`: Path to sound file

**Returns**: Pointer to loaded sound

##### `load_music(path: std::string) -> Music*`

Load music from file.

**Parameters**:
- `path`: Path to music file

**Returns**: Pointer to loaded music

##### `load_script(path: std::string) -> Script*`

Load a script from file.

**Parameters**:
- `path`: Path to script file

**Returns**: Pointer to loaded script

##### `unload_resource(resource: Resource*) -> void`

Unload a resource from memory.

**Parameters**:
- `resource`: Resource to unload

##### `clear_cache() -> void`

Clear all cached resources.

### Model Class

Represents a 3D model.

#### Methods

##### `get_mesh_count() -> size_t`

Get the number of meshes in the model.

**Returns**: Number of meshes

##### `get_mesh(index: size_t) -> Mesh*`

Get a mesh by index.

**Parameters**:
- `index`: Mesh index

**Returns**: Pointer to mesh

### Texture Class

Represents a texture.

#### Methods

##### `get_width() -> uint32_t`

Get the texture width in pixels.

**Returns**: Width in pixels

##### `get_height() -> uint32_t`

Get the texture height in pixels.

**Returns**: Height in pixels

##### `get_format() -> TextureFormat`

Get the texture format.

**Returns**: Texture format

### Shader Class

Represents a GPU shader program.

#### Methods

##### `use() -> void`

Set this shader as active.

##### `set_uniform(name: std::string, value: int) -> void`

Set an integer uniform.

**Parameters**:
- `name`: Uniform name
- `value`: Integer value

##### `set_uniform(name: std::string, value: float) -> void`

Set a float uniform.

**Parameters**:
- `name`: Uniform name
- `value`: Float value

##### `set_uniform(name: std::string, value: Vector3) -> void`

Set a Vector3 uniform.

**Parameters**:
- `name`: Uniform name
- `value`: Vector3 value

##### `set_uniform(name: std::string, value: Matrix4) -> void`

Set a Matrix4 uniform.

**Parameters**:
- `name`: Uniform name
- `value`: Matrix4 value

## Scene Manager API

### ISceneManager Interface

Scene manager interface for managing game scenes.

#### Methods

##### `initialize() -> bool`

Initialize the scene manager.

**Returns**: `true` if successful, `false` otherwise

##### `shutdown() -> void`

Shutdown the scene manager.

##### `create_scene(name: std::string) -> Scene*`

Create a new scene.

**Parameters**:
- `name`: Scene name

**Returns**: Pointer to created scene

##### `load_scene(scene: Scene*) -> void`

Load a scene as the active scene.

**Parameters**:
- `scene`: Scene to load

##### `unload_scene() -> void`

Unload the current scene.

##### `get_active_scene() -> Scene*`

Get the currently active scene.

**Returns**: Pointer to active scene

### Scene Class

Represents a game scene.

#### Methods

##### `create_entity() -> Entity`

Create a new entity in the scene.

**Returns**: Created entity

##### `destroy_entity(entity: Entity) -> void`

Destroy an entity from the scene.

**Parameters**:
- `entity`: Entity to destroy

##### `get_root() -> SceneNode*`

Get the root scene node.

**Returns**: Pointer to root node

### SceneNode Class

Represents a node in the scene graph.

#### Methods

##### `create_child() -> SceneNode*`

Create a child node.

**Returns**: Pointer to created child node

##### `attach_entity(entity: Entity) -> void`

Attach an entity to this node.

**Parameters**:
- `entity`: Entity to attach

##### `detach_entity(entity: Entity) -> void`

Detach an entity from this node.

**Parameters**:
- `entity`: Entity to detach

## Entity-Component-System API

### Entity Class

Represents a game entity.

#### Methods

##### `get_id() -> uint64_t`

Get the unique entity ID.

**Returns**: Entity ID

##### `add_component(component: Component) -> void`

Add a component to this entity.

**Parameters**:
- `component`: Component to add

##### `remove_component(type: ComponentType) -> void`

Remove a component from this entity.

**Parameters**:
- `type`: Type of component to remove

##### `get_component(type: ComponentType) -> Component*`

Get a component from this entity.

**Parameters**:
- `type`: Type of component to get

**Returns**: Pointer to component, or `nullptr` if not found

##### `has_component(type: ComponentType) -> bool`

Check if this entity has a component.

**Parameters**:
- `type`: Type of component to check

**Returns**: `true` if has component, `false` otherwise

### Component Base Class

Base class for all components.

#### Methods

##### `get_type() -> ComponentType`

Get the component type.

**Returns**: Component type

### TransformComponent

Component for position, rotation, and scale.

```cpp
struct TransformComponent : public Component {
    Vector3 position;
    Quaternion rotation;
    Vector3 scale;
};
```

### MeshComponent

Component for rendering a mesh.

```cpp
struct MeshComponent : public Component {
    Model* model;
    Material* material;
};
```

### CameraComponent

Component for camera view.

```cpp
struct CameraComponent : public Component {
    float fov;
    float near_plane;
    float far_plane;
    bool is_active;
};
```

## Types and Enums

### Vector3

3D vector type.

```cpp
struct Vector3 {
    float x, y, z;
    
    Vector3(float x = 0.0f, float y = 0.0f, float z = 0.0f);
    
    float length() const;
    Vector3 normalized() const;
    Vector3 cross(const Vector3& other) const;
    float dot(const Vector3& other) const;
};
```

### Vector4

4D vector type (RGBA color).

```cpp
struct Vector4 {
    float r, g, b, a;
    
    Vector4(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f);
};
```

### Matrix4

4x4 matrix type.

```cpp
struct Matrix4 {
    float data[16];
    
    static Matrix4 identity();
    static Matrix4 translation(const Vector3& translation);
    static Matrix4 rotation(const Quaternion& rotation);
    static Matrix4 scale(const Vector3& scale);
    static Matrix4 perspective(float fov, float aspect, float near, float far);
    static Matrix4 look_at(const Vector3& eye, const Vector3& center, const Vector3& up);
};
```

### Quaternion

Quaternion type for rotations.

```cpp
struct Quaternion {
    float x, y, z, w;
    
    Quaternion(float x = 0.0f, float y = 0.0f, float z = 0.0f, float w = 1.0f);
    
    Quaternion normalized() const;
    Matrix4 to_matrix() const;
};
```

## Error Handling

### Exceptions

The engine uses C++ exceptions for error handling.

```cpp
try {
    engine->initialize();
} catch (const warploom::EngineException& e) {
    std::cerr << "Engine error: " << e.what() << std::endl;
}
```

### EngineException

Base exception class for engine errors.

### RendererException

Exception for renderer errors.

### AudioException

Exception for audio errors.

### PhysicsException

Exception for physics errors.

### ResourceException

Exception for resource loading errors.
