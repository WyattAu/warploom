# Warploom

*(formerly OmniCPP)*

A data-oriented C++ engine with deterministic simulation, lock-free concurrency, Vulkan rendering, and an ECS foundation.

## Architecture

```
include/engine/core/
├── deterministic_runtime.hpp  — Fixed-step scheduler, SPSC/MPSC, replay, state hash
├── ecs.hpp                    — Archetype ECS, queries, change tracking, entity recycling,
│                                component pools, parallel system execution, bump allocator
├── clock.hpp                  — SteadyClock, ManualTimer, ScopedTimer
├── thread_pool.hpp            — Fixed-size pool with CPU affinity
├── replay.hpp                 — Versioned portable binary replay
└── engine.hpp                 — Engine facade (Result<T> error model)

include/engine/render/
├── vulkan_context.hpp         — Vulkan instance/device abstraction (Qt-free)
├── vulkan_surface.hpp         — Platform surface creation (X11/Wayland/Win32)
├── vulkan_swapchain.hpp       — Swapchain management (format/present mode selection)
├── vulkan_render_pass.hpp     — Render pass + framebuffers + depth resources
├── vulkan_pipeline.hpp        — Graphics pipeline + SPIR-V shader loading
├── vulkan_renderer.hpp        — Command buffers, frame sync, draw loop
└── software_rasterizer.hpp    — Deterministic CPU rasterizer for testing

Concurrency Primitives
├── BoundedQueue<T,N>          — Single-thread ring buffer
├── SpscChannel<T,N>           — Lock-free SPSC (1P/1C)
└── MpscChannel<T,N>           — Lock-free MPSC (Vyukov, NP/1C)
```

## Build

```bash
cmake --preset headless-debug
cmake --build build/headless-debug -j$(nproc)
ctest --test-dir build/headless-debug
```

### Presets

| Preset | Flags | Purpose |
|--------|-------|---------|
| `headless-debug` | `-O0 -g` | Development + all tests |
| `headless-release` | `-O3` | Optimized build |
| `tsan` | `-fsanitize=thread` | ThreadSanitizer |
| `asan-ubsan` | `-fsanitize=address,undefined` | Memory/UB checks |

## Rendering Pipeline

### Vulkan Context (`vulkan_context.hpp`)
- Instance creation with validation layers
- Physical device selection and scoring (prefers discrete GPU)
- Logical device creation with graphics + present queue families
- Graceful degradation when Vulkan is unavailable

### Vulkan Surface (`vulkan_surface.hpp`)
- X11/XCB surface creation
- Win32 surface creation
- Headless fallback for CI

### Vulkan Swapchain (`vulkan_swapchain.hpp`)
- Surface capability querying (min/max images, extent, transforms)
- Format selection (prefers SRGB B8G8R8A8 for correct gamma)
- Present mode selection (FIFO for vsync, MAILBOX for low latency)
- Image view creation for all swapchain images
- Swapchain recreation on window resize

### Vulkan Render Pass (`vulkan_render_pass.hpp`)
- Color + depth render pass with subpass dependencies
- Per-swapchain-image framebuffers
- Depth image/view creation with memory allocation
- Depth format selection from physical device capabilities

### Vulkan Pipeline (`vulkan_pipeline.hpp`)
- SPIR-V shader loading from file and memory
- Graphics pipeline with configurable state:
  - Dynamic viewport and scissor
  - Configurable depth test/write
  - Configurable face culling
  - Blend state
- Pipeline layout creation (ready for uniform buffers / push constants)

### Vulkan Renderer (`vulkan_renderer.hpp`)
- Per-frame command buffer pool (configurable frame count)
- Frame synchronization: fences + semaphores (double/triple buffering)
- Acquire → Record → Submit → Present cycle
- Static utilities: command pool creation, command buffer allocation

### Software Rasterizer (`software_rasterizer.hpp`)
- Triangle rasterization with barycentric coordinates
- Depth buffer with z-interpolation
- Color interpolation across vertices
- Both winding orders supported
- Deterministic frame hashing for regression testing

## ECS

Archetype-based SoA layout:

- **Entity**: 8 bytes with recycling and generation counter
- **Query API**: `query<T1,T2>(f)`, `query_if<T>(pred, action)`, `count_if<T>(pred)`
- **Change tracking**: `mark_dirty<T>()`, `set_component<T>()`, `is_dirty<T>()`, `clear_dirties()`
- **Parallel execution**: `run_parallel()` — independent systems run concurrently via ThreadPool
- **Component pools**: `ComponentPool<T>` — pre-allocated slots, zero steady-state allocation
- **Bump allocator**: Zero-allocation simulation phase
- **System scheduler**: Topological ordering with wave-based parallel partitioning

## Concurrency

All lock-free, header-only, TSan-validated with `halt_on_error=1`:

| Primitive | Thread Model | Use Case |
|-----------|-------------|----------|
| `BoundedQueue<T,N>` | Single thread | Internal bookkeeping |
| `SpscChannel<T,N>` | 1P / 1C | Runtime event queue |
| `MpscChannel<T,N>` | NP / 1C | Worker thread events |

## Benchmark

```bash
build/headless-debug/bin/warploom_deterministic_runtime_benchmark
build/headless-debug/bin/warploom_deterministic_runtime_benchmark --output results.json
```

## Validation

| Check | Status |
|-------|--------|
| Headless CTest (**134 unit tests**) | ✅ |
| TSan (`halt_on_error=1`) | ✅ Zero data races |
| ASan/UBSan | ✅ No memory/UB errors |
| Documentation links | ✅ 29 files |
| Benchmark JSON output | ✅ p50=56ns, p99=71ns |

## Project Structure

```
include/engine/core/          — 6 verified core headers
include/engine/render/        — 7 rendering headers (complete pipeline)
src/engine/core/              — 1 engine implementation
src/engine/render/            — 6 Vulkan implementations
tests/unit/                   — Unit tests (Google Test)
tests/performance/            — Benchmark (JSON output)
tests/archive/                — Archived legacy subsystem tests
legacy/archive/               — Archived legacy code (138 files)
.github/workflows/            — CI matrix (debug, release, TSan, ASan, docs)
```
