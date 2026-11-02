#pragma once

/**
 * @file vulkan_transient_store.hpp
 * @brief The Vulkan side of R6 (R6b): create/alias/destroy images from the plan.
 *
 * The pure planner (`vulkan_frame_allocator.hpp`) answers "what may alias"
 * given declared lifetimes, and `derive_frame_resources` derives those
 * lifetimes from the recorded `GraphNode` sequence. Neither touches a device.
 * This class closes the loop: given the plan plus one image description per
 * OWNED resource, it creates the VkImages, binds memory (a private range for
 * Dedicated entries, the owner's range for Aliased ones), creates one view
 * per image, and destroys everything on reallocation.
 *
 * ## What it does NOT do
 *
 * It does not decide anything. Sizes, tags, history and the alias pairs all
 * come from the plan; the store materializes them and VERIFIES the one
 * property the planner's soundness rests on but cannot check itself: that an
 * Aliased entry's description is identical to its owner's (format, extent,
 * usage), so sharing the owner's memory satisfies the image's own memory
 * requirements (same alignment, same type bits). A mismatch fails the
 * allocation loudly rather than binding memory the image may not use.
 *
 * ## Lifetime contract (frames in flight)
 *
 * Reallocation happens ONLY at quiescent points (ensure/resize, after the
 * caller waited idle) -- exactly like the hand-managed targets this replaces.
 * Per-frame work is verification, not allocation: `verify_against_nodes`
 * re-derives lifetimes from the nodes actually recorded and checks every
 * planned alias pair is still disjoint. A shape change that breaks an alias
 * fails the frame instead of rendering wrong.
 *
 * ## Why the owner is always allocated first
 *
 * The planner's reuse loop walks backwards, so an Aliased entry's owner is
 * always declared EARLIER (see `planKind_owner_earlier` in
 * proofs/FrameAllocator.lean). Allocation follows declaration order, so the
 * owner's memory exists before any sharer binds it. That ordering is load-
 * bearing, not incidental: bind-before-owner would read a null handle.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <warploom/core/deterministic_runtime.hpp>
#include <warploom/render/vulkan_frame_allocator.hpp>
#include <warploom/render/vulkan_memory_allocator.hpp>
#include <warploom/render/vulkan_types.hpp>

namespace warploom::render {

class VulkanMemoryAllocator;

//! Image creation parameters for one OWNED plan resource. Matched to plan
//! entries by `name`. Resources with no description (caller-owned externals
//! such as the path tracer's accumulation image) are skipped: they
//! participate in planning (a history external blocks aliasing) but the
//! store neither creates nor destroys them.
struct TransientImageDesc {
  std::string name;
  VkFormat format{VK_FORMAT_UNDEFINED};
  std::uint32_t width{0};
  std::uint32_t height{0};
  //! VkImageUsageFlags value (0 when Vulkan-off).
  std::uint32_t usage{0};
  //! Aspect for the view (color or depth). 0 infers from the format.
  std::uint32_t aspect{0};
  //! Cross-queue sharing (loop28 step 1): the compose HDR depth sets this
  //! so the compute queue may later sample it. Deliberately EXCLUDED from
  //! image_descs_equal: sharing is per-image access, not per-memory layout,
  //! so two images with different sharing may still bind the same memory.
  ImageSharing sharing{};
};

//! One materialized image.
struct TransientImage {
  std::string name;
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  //! The memory this image is bound to. Owned by the store when this entry
  //! is Dedicated (released via the allocator); borrowed from the owner's
  //! allocation when Aliased (released with the owner, never here).
  VkDeviceMemory memory{VK_NULL_HANDLE};
  VkDeviceSize offset{0};
  bool aliased{false};
  std::string shares_with;
};

class VulkanTransientStore final {
 public:
  VulkanTransientStore() = default;
  ~VulkanTransientStore();
  VulkanTransientStore(const VulkanTransientStore&) = delete;
  VulkanTransientStore& operator=(const VulkanTransientStore&) = delete;
  VulkanTransientStore(VulkanTransientStore&&) = delete;
  VulkanTransientStore& operator=(VulkanTransientStore&&) = delete;

  //! Materialize `plan` for the owned `descs`. Destroys any previous
  //! materialization first (caller guarantees quiescence). Entries with no
  //! matching desc are skipped (caller-owned). Fails loudly on: an Aliased
  //! entry whose owner is missing/not yet allocated, or whose description
  //! differs from its owner's in anything but name.
  [[nodiscard]] ::warploom::core::Result<void> allocate(
      VkDevice device, VkPhysicalDevice physical_device,
      VulkanMemoryAllocator* allocator,
      const std::vector<FrameResource>& resources,
      const std::vector<ResourceAllocationPlan>& plan,
      const std::vector<TransientImageDesc>& descs);

  void destroy() noexcept;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  //! Bumped on every successful allocate(); lets owners tell a realloc from
  //! an idempotent ensure without comparing images.
  [[nodiscard]] std::uint64_t generation() const noexcept {
    return generation_;
  }

  [[nodiscard]] const TransientImage* find(
      const std::string& name) const noexcept;
  [[nodiscard]] VkImage image(const std::string& name) const noexcept;
  [[nodiscard]] VkImageView view(const std::string& name) const noexcept;
  //! Number of backing owner allocations (one per Dedicated entry). 10
  //! images with 4 elided share 6 allocations; without aliasing it would be
  //! 10. The on-device elision number.
  [[nodiscard]] std::size_t owner_allocation_count() const noexcept {
    return owners_.size();
  }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VulkanMemoryAllocator* allocator_{nullptr};
  std::vector<TransientImage> images_{};
  //! Owner allocations, in declaration order. An Aliased image binds the
  //! memory recorded here for its owner; destroy releases each once.
  struct OwnerMemory {
    std::string name;
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize offset{0};
    Allocation allocation{};
    bool from_allocator{false};
  };
  std::vector<OwnerMemory> owners_{};
  bool ready_{false};
  std::uint64_t generation_{0};
};

}  // namespace warploom::render
