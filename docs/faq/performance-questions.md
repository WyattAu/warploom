# Performance Questions

Questions and solutions related to performance optimization, profiling, and debugging.

## Table of Contents

- [Profiling](#profiling)
- [Memory Management](#memory-management)
- [Rendering Performance](#rendering-performance)
- [Physics Performance](#physics-performance)
- [Build Performance](#build-performance)

## Profiling

### How do I profile the application?

**TL;DR:** Use platform-specific profilers: Visual Studio Profiler (Windows), perf (Linux), or Instruments (macOS).

**The Deep Dive:**

**Windows - Visual Studio Profiler:**
```bash
# Build with debug symbols
python WarploomController.py build standalone "Build Project" default relwithdebinfo

# Run with profiler
# In Visual Studio: Debug > Performance Profiler
```

**Linux - perf:**
```bash
# Install perf
sudo apt install linux-tools-generic

# Profile application
perf record -g ./build/release/bin/warploom

# Analyze results
perf report
```

**macOS - Instruments:**
```bash
# Build with debug symbols
python WarploomController.py build standalone "Build Project" default relwithdebinfo

# Open Instruments
# Applications > Xcode > Open Developer Tool > Instruments
```

**Key Build Type:** `relwithdebinfo` provides optimization with debug symbols.

### How do I profile GPU performance?

**TL;DR:** Use GPU-specific tools: NVIDIA Nsight, AMD Radeon GPU Profiler, or Vulkan Validation Layers.

**The Deep Dive:**

**Vulkan Validation Layers:**
```cpp
// Enable validation layers in debug builds
#ifdef DEBUG
const char* validationLayers[] = {
    "VK_LAYER_KHRONOS_validation"
};

VkInstanceCreateInfo createInfo = {};
createInfo.enabledLayerCount = 1;
createInfo.ppEnabledLayerNames = validationLayers;
#endif
```

**NVIDIA Nsight Graphics:**
```bash
# Install Nsight Graphics
# Download from NVIDIA Developer website

# Profile application
nsight -- ./build/release/bin/warploom
```

**AMD Radeon GPU Profiler:**
```bash
# Install Radeon GPU Profiler
# Download from AMD Developer website

# Profile application
rgp -- ./build/release/bin/warploom
```

## Memory Management

### How do I detect memory leaks?

**TL;DR:** Use Valgrind (Linux), Dr. Memory (Windows), or AddressSanitizer.

**The Deep Dive:**

**Linux - Valgrind:**
```bash
# Install Valgrind
sudo apt install valgrind

# Run with Valgrind
valgrind --leak-check=full --show-leak-kinds=all ./build/debug/bin/warploom
```

**Windows - Dr. Memory:**
```bash
# Install Dr. Memory
# Download from drmemory.org

# Run with Dr. Memory
drmemory -- ./build/debug/bin/warploom.exe
```

**AddressSanitizer (Cross-Platform):**
```cmake
# Enable in CMakeLists.txt
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
```

**Key File:** [`cmake/user/tmplt-sanitizer.cmake`](../cmake/user/tmplt-sanitizer.cmake:1)

### How do I optimize memory usage?

**TL;DR:** Use object pools, smart pointers, and avoid unnecessary allocations.

**The Deep Dive:**

**1. Object Pools:**
```cpp
// include/engine/memory/ObjectPool.hpp
template<typename T, size_t PoolSize>
class ObjectPool {
private:
    std::array<T, PoolSize> pool;
    std::bitset<PoolSize> used;
    
public:
    T* allocate() {
        size_t index = used.find_first();
        if (index != std::bitset<PoolSize>::npos) {
            used.set(index);
            return &pool[index];
        }
        return nullptr;
    }
    
    void deallocate(T* obj) {
        size_t index = obj - pool.data();
        used.reset(index);
    }
};
```

**2. Smart Pointers:**
```cpp
// Use std::unique_ptr for exclusive ownership
std::unique_ptr<Engine> engine = std::make_unique<Engine>();

// Use std::shared_ptr for shared ownership
std::shared_ptr<Texture> texture = std::make_shared<Texture>();

// Use std::weak_ptr to avoid cycles
std::weak_ptr<Texture> weakTexture = texture;
```

**3. Avoid Unnecessary Allocations:**
```cpp
// Bad: Creates new string each iteration
for (const auto& item : items) {
    std::string name = item.getName();  // Allocation
    process(name);
}

// Good: Reuses string
std::string name;
for (const auto& item : items) {
    name = item.getName();  // No allocation
    process(name);
}
```

## Rendering Performance

### How do I optimize rendering performance?

**TL;DR:** Use instancing, batching, frustum culling, and level-of-detail (LOD).

**The Deep Dive:**

**1. Instancing:**
```cpp
// Render multiple objects with one draw call
struct InstanceData {
    glm::mat4 modelMatrix;
    glm::vec4 color;
};

std::vector<InstanceData> instances;
for (const auto& object : objects) {
    instances.push_back({object.modelMatrix, object.color});
}

glDrawArraysInstanced(GL_TRIANGLES, 0, vertexCount, instanceCount);
```

**2. Batching:**
```cpp
// Group similar objects for fewer draw calls
struct RenderBatch {
    Material* material;
    std::vector<Mesh*> meshes;
};

std::vector<RenderBatch> batches;
for (const auto& mesh : meshes) {
    auto& batch = findOrCreateBatch(mesh->material);
    batch.meshes.push_back(mesh);
}

// Render each batch
for (const auto& batch : batches) {
    batch.material->bind();
    for (const auto& mesh : batch.meshes) {
        mesh->draw();
    }
}
```

**3. Frustum Culling:**
```cpp
// include/engine/render/Frustum.hpp
class Frustum {
public:
    bool intersects(const BoundingBox& box) const {
        for (const auto& plane : planes) {
            if (plane.distance(box.center) < -box.radius) {
                return false;
            }
        }
        return true;
    }
};

// Cull objects outside view
for (const auto& object : objects) {
    if (frustum.intersects(object->boundingBox)) {
        object->render();
    }
}
```

**Key File:** [`include/engine/render/RenderPipeline.hpp`](../include/engine/render/RenderPipeline.hpp:1)

### How do I reduce draw calls?

**TL;DR:** Use instancing, batching, and texture atlases to minimize draw calls.

**The Deep Dive:**

**Draw Call Reduction Techniques:**

| Technique | Reduction | Complexity |
|-----------|-------------|-------------|
| **Instancing** | 100-1000x | Medium |
| **Batching** | 10-100x | Low |
| **Texture Atlases** | 5-10x | Medium |
| **GPU Culling** | 2-5x | High |

**Example - Texture Atlas:**
```cpp
// Pack multiple textures into one
struct TextureAtlas {
    GLuint textureId;
    std::unordered_map<std::string, UVRect> regions;
};

struct UVRect {
    glm::vec2 min;
    glm::vec2 max;
};

// Use UV coordinates from atlas
void drawSprite(const std::string& spriteName) {
    auto uv = atlas.regions[spriteName];
    shader->setUniform("u_uvMin", uv.min);
    shader->setUniform("u_uvMax", uv.max);
    drawQuad();
}
```

## Physics Performance

### How do I optimize physics calculations?

**TL;DR:** Use spatial partitioning, fixed time steps, and broad-phase collision detection.

**The Deep Dive:**

**1. Spatial Partitioning:**
```cpp
// include/engine/physics/SpatialHash.hpp
class SpatialHash {
private:
    std::unordered_map<uint64_t, std::vector<Collider*>> cells;
    float cellSize;
    
public:
    uint64_t hashPosition(const glm::vec3& pos) {
        int x = static_cast<int>(pos.x / cellSize);
        int y = static_cast<int>(pos.y / cellSize);
        int z = static_cast<int>(pos.z / cellSize);
        return static_cast<uint64_t>(x) << 42 |
               static_cast<uint64_t>(y) << 21 |
               static_cast<uint64_t>(z);
    }
    
    void insert(Collider* collider) {
        uint64_t hash = hashPosition(collider->position);
        cells[hash].push_back(collider);
    }
    
    std::vector<Collider*> query(const glm::vec3& pos, float radius) {
        std::vector<Collider*> results;
        // Query nearby cells
        return results;
    }
};
```

**2. Fixed Time Steps:**
```cpp
// include/engine/physics/PhysicsEngine.hpp
class PhysicsEngine {
private:
    const float fixedDeltaTime = 1.0f / 60.0f;
    float accumulator = 0.0f;
    
public:
    void update(float deltaTime) {
        accumulator += deltaTime;
        
        while (accumulator >= fixedDeltaTime) {
            stepPhysics(fixedDeltaTime);
            accumulator -= fixedDeltaTime;
        }
        
        // Interpolate for rendering
        float alpha = accumulator / fixedDeltaTime;
        interpolateState(alpha);
    }
};
```

**3. Broad-Phase Collision:**
```cpp
// First pass: quick rejection
bool broadPhaseCollision(const Collider& a, const Collider& b) {
    return a.boundingBox.intersects(b.boundingBox);
}

// Second pass: detailed check
bool narrowPhaseCollision(const Collider& a, const Collider& b) {
    // SAT (Separating Axis Theorem)
    // GJK (Gilbert-Johnson-Keerthi)
    return detailedCollisionCheck(a, b);
}
```

**Key File:** [`include/engine/physics/PhysicsEngine.hpp`](../include/engine/physics/PhysicsEngine.hpp:1)

## Build Performance

### How do I measure build time?

**TL;DR:** Use `time` command on Linux/macOS or Measure-Command in PowerShell on Windows.

**The Deep Dive:**

**Linux/macOS:**
```bash
# Measure build time
time python WarploomController.py build standalone "Build Project" default release

# Output:
# real    2m15.345s
# user    8m45.123s
# sys     0m23.456s
```

**Windows (PowerShell):**
```powershell
# Measure build time
Measure-Command { python WarploomController.py build standalone "Build Project" default release }

# Output:
# Days              : 0
# Hours             : 0
# Minutes           : 2
# Seconds           : 15
# Milliseconds      : 345
```

### How do I identify slow compilation units?

**TL;DR:** Use CMake's `--profile` option or build-time analysis tools.

**The Deep Dive:**

**CMake Build Profiling:**
```bash
# Enable build profiling
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release --profiling-output=build_profile.json

# Build
cmake --build build/release

# Analyze profile
python -m json.tool build_profile.json
```

**Clang Build Time Analysis:**
```bash
# Add to CMake flags
add_compile_options(-ftime-trace)

# Build
cmake --build build/release

# Analyze
find build/release -name "*.json" -exec cat {} \; | jq '.'
```

**Common Slow Compilation Causes:**
1. **Large header files** - Split into smaller headers
2. **Template-heavy code** - Use explicit instantiations
3. **Recursive includes** - Use forward declarations

**Key File:** [`cmake/CompilerFlags.cmake`](../cmake/CompilerFlags.cmake:1)
