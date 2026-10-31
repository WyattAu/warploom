# OmniCPP Engine

## Verified Components

### DeterministicRuntime

Fixed-step simulation scheduler with bounded event queues, catch-up policies, replay recording, and portable binary encoding.

- **Time modes**: Floating-point or integer-nanosecond accumulation
- **Event transport**: SPSC (default) or MPSC (multi-producer)
- **Catch-up policies**: `run_all`, `cap_and_drop_time`, `cap_and_report_overrun`
- **State hashing**: FNV-1a over deterministic tick sequence

### ECS (Entity Component System)

Archetype-based SoA layout with deterministic entity iteration.

- **Entity**: 8 bytes (`{id: u32, generation: u32}`) with recycling
- **Components**: Sequential type IDs via atomic counter (no hash collisions)
- **Entity migration**: Automatic archetype transitions with correct swap-remove bookkeeping
- **Query API**: `query<T1,T2>(f)`, `query_if<T>(pred, action)`, `count_if<T>(pred)`
- **Change tracking**: `mark_dirty<T>()`, `set_component<T>()`, `is_dirty<T>()`, `clear_dirties()`
- **Parallel execution**: `run_parallel()`; independent systems run concurrently via ThreadPool
- **Component pools**: `ComponentPool<T>`; pre-allocated slots, zero steady-state allocation
- **System scheduler**: Topological sort with wave-based parallel partitioning

### Vulkan Rendering Pipeline

Complete GPU rendering pipeline extracted from legacy code, Qt-free, headlessly tested.
Full details in [Vulkan Rendering Stack](vulkan-rendering.md).

- **VulkanContext**: Instance creation, device enumeration, queue selection, Vulkan 1.2/1.3 feature negotiation (Synchronization 2, timeline semaphores, descriptor indexing)
- **VulkanSurface**: X11/XCB and Win32 surface creation with headless fallback
- **VulkanSwapchain**: Format selection (SRGB preferred), present mode selection (FIFO/MAILBOX), validated recreation + renderer resync on resize
- **VulkanRenderPass**: Color + depth render pass, per-swapchain-image framebuffers, depth resource management
- **VulkanPipeline**: SPIR-V shader loading, graphics pipeline with dynamic viewport/scissor, explicit descriptor/push-constant layouts, configurable depth/cull/blend
- **VulkanRenderer**: Sync2 (`vkQueueSubmit2`) submission with legacy fallback, timeline-semaphore frame pacing, GPU timestamp telemetry, per-frame command pools
- **VulkanMemoryAllocator**: Block sub-allocation with persistent mapping + staging upload ring
- **VulkanDescriptorManager**: SPIR-V reflection, layout/pool management, buffer and image writes, bindless (update-after-bind, partially bound, runtime-array) sets
- **Render graph**: Automatic image barriers between declarative passes
- **VulkanParallelRecorder**: Multithreaded secondary command-buffer recording

### Software Rasterizer

Deterministic CPU rasterizer for testing and headless validation.

- Triangle rasterization with barycentric coordinates
- Depth buffer with z-interpolation
- Color interpolation across vertices, both winding orders
- Deterministic frame hashing for regression testing

### Concurrency Primitives

All lock-free, header-only, zero-allocation, validated under TSan with `halt_on_error=1`.

| Primitive | Thread Model | Use Case |
|-----------|-------------|----------|
| `BoundedQueue<T,N>` | Single thread | Internal bookkeeping (accumulators) |
| `SpscChannel<T,N>` | 1P / 1C | Runtime event queue (producer thread → main thread) |
| `MpscChannel<T,N>` | NP / 1C | Worker threads posting events to main loop |

### Platform Abstractions

- **`SteadyClock`**: Nanosecond monotonic clock
- **`ManualTimer` / `ScopedTimer`**: Elapsed time measurement
- **`ThreadPool`**: Fixed-size pool with task submission and CPU affinity (Linux)

### Engine

Engine facade with `Result<T>` error model, configurable event transport, and time modes.

## Validation Matrix

| Check | Status |
|-------|--------|
| Headless CTest (unit tests) | ✅ All pass |
| Vulkan validation preset (RTX 2060 + Khronos layer, **183/183**) | ✅ Zero diagnostics |
| Vulkan validation CI job (Mesa lavapipe) | ✅ Wired in `.github/workflows/test.yml` |
| TSan (`halt_on_error=1`) | ✅ Zero data races |
| ASan/UBSan | ✅ No memory or UB errors |
| Documentation links | ✅ Verified by `scripts/check_docs_links.py` |
| Benchmark JSON output | ✅ `--output <file>` for CI regression tracking |

## Architecture Principles

1. **Data-oriented**: SoA layout for cache efficiency
2. **Deterministic**: Fixed-step scheduler with reproducible state hashing
3. **Lock-free**: SPSC/MPSC channels for thread communication
4. **Header-only primitives**: Zero dependency, zero allocation at steady state
5. **Verified**: All concurrency tested under ThreadSanitizer
