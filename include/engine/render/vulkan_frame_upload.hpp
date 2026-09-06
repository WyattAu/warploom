//! @file vulkan_frame_upload.hpp
//! @brief Per-frame ownership for persistently mapped Vulkan upload staging.

#pragma once

#include "engine/core/deterministic_runtime.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace omnicpp::render {

//! A fixed ring of upload arenas, one per CPU/GPU frame slot.
//!
//! Beginning a slot waits only for that slot's prior submission. This gives
//! callers an explicit lifetime boundary: all destination resources referenced
//! by copies recorded through the slot are safe to retire after the slot has
//! been begun again (or after wait_idle()).
class VulkanFrameUploadArena final {
public:
  VulkanFrameUploadArena() = default;
  ~VulkanFrameUploadArena();
  VulkanFrameUploadArena(const VulkanFrameUploadArena&) = delete;
  VulkanFrameUploadArena& operator=(const VulkanFrameUploadArena&) = delete;

  [[nodiscard]] omnicpp::core::Result<void> initialize(
      VkDevice device, VkPhysicalDevice physical_device,
      std::uint32_t queue_family_index, std::uint32_t frame_count,
      VkDeviceSize bytes_per_frame);
  void cleanup() noexcept;

  //! Wait for and activate one frame slot before writing its staging memory.
  [[nodiscard]] omnicpp::core::Result<void> begin_frame(std::uint32_t frame_index);
  [[nodiscard]] bool is_initialized() const noexcept { return !rings_.empty(); }
  [[nodiscard]] std::uint32_t active_frame() const noexcept { return active_frame_; }

  [[nodiscard]] omnicpp::core::Result<VulkanUploadRing::UploadSpan> acquire(
      VkDeviceSize size);
  //! Record into the active slot's private command buffer.
  void record_copy(const VulkanUploadRing::UploadSpan& span,
                   VkBuffer destination, VkDeviceSize destination_offset = 0) noexcept;
  //! Record a tightly packed RGBA8 upload into a fresh 2D image.
  //!
  //! The image must have been created with usage TRANSFER_DST | SAMPLED and
  //! initialLayout UNDEFINED, and bound to device-local memory before the
  //! frame is submitted. This records UNDEFINED -> TRANSFER_DST_OPTIMAL
  //! (copy) -> SHADER_READ_ONLY_OPTIMAL with access/stage masks for a
  //! fragment-shader consumer; the image is ready to sample after submit.
  void record_copy_image_rgba8(const VulkanUploadRing::UploadSpan& span,
                               VkImage image, std::uint32_t width,
                               std::uint32_t height) noexcept;
  //! Submit the active slot and transfer staging lifetime to the GPU fence.
  [[nodiscard]] omnicpp::core::Result<void> submit(VkQueue queue);
  void wait_idle() noexcept;

private:
  std::vector<std::unique_ptr<VulkanUploadRing>> rings_;
  VkQueue queue_{VK_NULL_HANDLE};
  std::uint32_t active_frame_{0xffffffffU};
};

} // namespace omnicpp::render
