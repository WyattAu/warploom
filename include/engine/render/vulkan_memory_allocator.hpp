#pragma once

/**
 * @file vulkan_memory_allocator.hpp
 * @brief Block-sub-allocation device memory allocator and staging upload ring.
 *
 * Design goals:
 * - Bounded `vkAllocateMemory` calls (drivers cap total allocations).
 * - First-fit sub-allocation inside large memory blocks with free-list
 *   coalescing on release.
 * - Persistent mapping of host-visible blocks for zero-map-per-allocation
 *   CPU writes.
 * - Explicit, fence-tracked upload ring so CPU->GPU staging never overwrites
 *   a region still in flight.
 */

#include "engine/core/deterministic_runtime.hpp"
#include "engine/render/vulkan_types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace omnicpp::render {

//! A live sub-allocation. `buffer`/`image` record what was bound (one of them).
struct Allocation {
  VkDeviceMemory memory{VK_NULL_HANDLE};
  VkDeviceSize offset{0};
  VkDeviceSize size{0};
  void* mapped{nullptr};          //!< Non-null for host-visible allocations.
  std::uint32_t memory_type{0};
  std::size_t block_index{0};     //!< Owning block (allocator-internal).
  VkBuffer buffer{VK_NULL_HANDLE};
  VkImage image{VK_NULL_HANDLE};
  [[nodiscard]] bool is_valid() const noexcept { return memory != VK_NULL_HANDLE; }
};

//! Aggregate allocator statistics for telemetry and tests.
struct AllocatorStats {
  std::uint64_t reserved_bytes{0};    //!< Sum of all block sizes.
  std::uint64_t used_bytes{0};        //!< Sum of live allocation sizes.
  std::uint32_t block_count{0};
  std::uint32_t allocation_count{0};
};

/**
 * @brief First-fit block sub-allocator over vkAllocateMemory.
 *
 * One coarse scope for this increment: callers choose the memory property
 * flags; the allocator picks the best matching memory type. Blocks are
 * 64 MiB for device-local and 16 MiB for host-visible heaps by default.
 */
class VulkanMemoryAllocator final {
public:
  static constexpr VkDeviceSize kDeviceLocalBlockSize = 64u * 1024u * 1024u;
  static constexpr VkDeviceSize kHostVisibleBlockSize = 16u * 1024u * 1024u;

  VulkanMemoryAllocator() = default;
  ~VulkanMemoryAllocator();
  VulkanMemoryAllocator(const VulkanMemoryAllocator&) = delete;
  VulkanMemoryAllocator& operator=(const VulkanMemoryAllocator&) = delete;
  VulkanMemoryAllocator(VulkanMemoryAllocator&&) = delete;
  VulkanMemoryAllocator& operator=(VulkanMemoryAllocator&&) = delete;

  [[nodiscard]] omnicpp::core::Result<void> initialize(
      VkDevice device, VkPhysicalDevice physical_device);
  void cleanup() noexcept;

  [[nodiscard]] bool is_initialized() const noexcept { return device_ != VK_NULL_HANDLE; }

  //! Create a buffer and bind it to sub-allocated memory of the requested type.
  [[nodiscard]] omnicpp::core::Result<Allocation> create_buffer(
      VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
  //! Bind a caller-created image to sub-allocated memory of the requested type.
  [[nodiscard]] omnicpp::core::Result<Allocation> bind_image(
      VkImage image, VkMemoryPropertyFlags properties);
  //! Release a live allocation (destroys the buffer if owned) and coalesce.
  void destroy_allocation(Allocation& allocation) noexcept;

  [[nodiscard]] AllocatorStats stats() const noexcept;

private:
  struct FreeRange {
    VkDeviceSize offset{0};
    VkDeviceSize size{0};
  };

  // Cross-queue sharing state (set at initialize()).
  bool has_dedicated_compute_{false};
  std::uint32_t compute_family_{0};
  std::uint32_t graphics_family_{0};
  struct Block {
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize size{0};
    VkDeviceSize used{0};
    void* mapped{nullptr};
    std::uint32_t memory_type{0};
    std::vector<FreeRange> free_ranges;
  };

  [[nodiscard]] omnicpp::core::Result<Allocation> allocate_sized(
      VkDeviceSize size, VkDeviceSize alignment, std::uint32_t type_bits,
      VkMemoryPropertyFlags properties);
  [[nodiscard]] std::size_t find_or_create_block(
      VkDeviceSize size, std::uint32_t type_bits, VkMemoryPropertyFlags properties);
  [[nodiscard]] std::uint32_t find_memory_type(
      std::uint32_t type_bits, VkMemoryPropertyFlags properties) const;

  //! Buffer handles owned by live allocations (created by create_buffer);
  //! destroyed by cleanup() so a dropped allocator never leaks VkBuffers.
  //! Images are caller-created via bind_image and stay caller-owned.
  std::vector<VkBuffer> live_buffers_;

  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  std::vector<Block> blocks_;
  std::uint64_t allocation_count_{0};
};

/**
 * @brief Fenced HOST_VISIBLE staging ring for CPU->GPU uploads.
 *
 * One large persistently mapped buffer is partitioned as a ring. Each
 * upload records the fence of the submit that consumed it; acquiring space
 * that would wrap into an in-flight region waits on (and retires) the fences
 * guarding that region, so overwrite is always safe.
 */
class VulkanUploadRing final {
public:
  VulkanUploadRing() = default;
  ~VulkanUploadRing();
  VulkanUploadRing(const VulkanUploadRing&) = delete;
  VulkanUploadRing& operator=(const VulkanUploadRing&) = delete;
  VulkanUploadRing(VulkanUploadRing&&) = delete;
  VulkanUploadRing& operator=(VulkanUploadRing&&) = delete;

  //! span.byte_offset..byte_offset+size is valid host memory for CPU writes.
  struct UploadSpan {
    void* host_data{nullptr};
    VkDeviceSize byte_offset{0};
    VkDeviceSize size{0};
  };

  [[nodiscard]] omnicpp::core::Result<void> initialize(
      VkDevice device, VkPhysicalDevice physical_device,
      std::uint32_t queue_family_index, VkDeviceSize total_size);
  void cleanup() noexcept;

  [[nodiscard]] bool is_initialized() const noexcept { return ring_buffer_ != VK_NULL_HANDLE; }

  //! Reserve host-writable staging space, retiring in-flight regions on wrap.
  [[nodiscard]] omnicpp::core::Result<UploadSpan> acquire(VkDeviceSize size);
  //! Record a copy from the staged span into `dst_buffer` at `dst_offset`.
  void record_copy(VkCommandBuffer command_buffer, const UploadSpan& span,
                   VkBuffer dst_buffer, VkDeviceSize dst_offset = 0) const noexcept;
  //! Expose the ring-owned command buffer for an arena that records copies
  //! without managing a second command buffer. The buffer is begun lazily by
  //! acquire().
  [[nodiscard]] VkCommandBuffer command_buffer_for_recording() const noexcept {
    return command_buffer_;
  }
  //! Begin the ring-owned command buffer for an upload batch.
  [[nodiscard]] omnicpp::core::Result<void> begin_recording();
  //! Submit the ring's one-time command buffer; the fence guards the span.
  [[nodiscard]] omnicpp::core::Result<void> submit(VkQueue queue);

  //! Wait for all in-flight uploads (device quiescent w.r.t. the ring).
  void wait_idle() noexcept;

  [[nodiscard]] VkBuffer ring_buffer() const noexcept { return ring_buffer_; }
  [[nodiscard]] VkDeviceSize capacity() const noexcept { return capacity_; }

private:
  struct InFlight {
    VkDeviceSize start{0};
    VkDeviceSize size{0};
    VkFence fence{VK_NULL_HANDLE};
    bool owns_fence{false};
  };

  [[nodiscard]] omnicpp::core::Result<void> begin_commands();
  void retire_completed() noexcept;
  //! Wait until the region [start, start+size) is free for overwrite.
  void wait_region_free(VkDeviceSize start, VkDeviceSize size) noexcept;

  VkDevice device_{VK_NULL_HANDLE};
  VkBuffer ring_buffer_{VK_NULL_HANDLE};
  VkDeviceMemory ring_memory_{VK_NULL_HANDLE};
  void* mapped_{nullptr};
  VkDeviceSize capacity_{0};
  VkDeviceSize head_{0};                 //!< Next byte to hand out.
  VkCommandPool command_pool_{VK_NULL_HANDLE};
  VkCommandBuffer command_buffer_{VK_NULL_HANDLE};
  bool recording_{false};
  std::vector<InFlight> in_flight_;
  // Ranges handed out by acquire() since the last submit(); submit() attaches
  // them to the batch fence so wrap-around waits cover exactly what is live.
  struct ByteRange { VkDeviceSize offset{0}; VkDeviceSize size{0}; };
  std::vector<ByteRange> staged_ranges_;
};

} // namespace omnicpp::render
