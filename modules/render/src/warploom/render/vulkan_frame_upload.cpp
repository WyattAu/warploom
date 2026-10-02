#include "warploom/render/vulkan_frame_upload.hpp"

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

VulkanFrameUploadArena::~VulkanFrameUploadArena() { cleanup(); }

::warploom::core::Result<void> VulkanFrameUploadArena::initialize(
    VkDevice device, VkPhysicalDevice physical_device,
    std::uint32_t queue_family_index, std::uint32_t frame_count,
    VkDeviceSize bytes_per_frame) {
  cleanup();
  if (frame_count == 0U || bytes_per_frame == 0U) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  queue_ = VK_NULL_HANDLE;
  rings_.reserve(frame_count);
  for (std::uint32_t i = 0; i < frame_count; ++i) {
    auto ring = std::make_unique<VulkanUploadRing>();
    auto result = ring->initialize(device, physical_device, queue_family_index,
                                   bytes_per_frame);
    if (!result.is_ok()) {
      cleanup();
      return result;
    }
    rings_.push_back(std::move(ring));
  }
  return ::warploom::core::Result<void>::ok();
}

void VulkanFrameUploadArena::cleanup() noexcept {
  for (auto& ring : rings_) {
    if (ring) ring->cleanup();
  }
  rings_.clear();
  queue_ = VK_NULL_HANDLE;
  active_frame_ = 0xffffffffU;
}

::warploom::core::Result<void> VulkanFrameUploadArena::begin_frame(
    std::uint32_t frame_index) {
  if (frame_index >= rings_.size()) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  // Wait only for this slot's prior submission. This is the per-frame
  // lifetime boundary callers use to retire staging-dependent resources:
  // once slot N has been begun again, slot N's prior copies are guaranteed
  // complete, so any destination resource referenced by those copies can be
  // retired for the next frame.
  rings_[frame_index]->wait_idle();
  active_frame_ = frame_index;
  return ::warploom::core::Result<void>::ok();
}

::warploom::core::Result<VulkanUploadRing::UploadSpan> VulkanFrameUploadArena::acquire(
    VkDeviceSize size) {
  if (active_frame_ >= rings_.size()) {
    return ::warploom::core::Result<VulkanUploadRing::UploadSpan>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  auto begin_result = rings_[active_frame_]->begin_recording();
  if (!begin_result.is_ok()) {
    return ::warploom::core::Result<VulkanUploadRing::UploadSpan>::error(begin_result.error());
  }
  return rings_[active_frame_]->acquire(size);
}

void VulkanFrameUploadArena::record_copy(
    const VulkanUploadRing::UploadSpan& span, VkBuffer destination,
    VkDeviceSize destination_offset) noexcept {
  if (active_frame_ >= rings_.size()) return;
  // VulkanUploadRing keeps its command buffer private; this public wrapper is
  // intentionally command-buffer-free and records into that owned buffer.
  rings_[active_frame_]->record_copy(rings_[active_frame_]->command_buffer_for_recording(),
                                     span, destination, destination_offset);
}

void VulkanFrameUploadArena::record_copy_image_rgba8(
    const VulkanUploadRing::UploadSpan& span, VkImage image,
    std::uint32_t width, std::uint32_t height) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (active_frame_ >= rings_.size() || image == VK_NULL_HANDLE ||
      width == 0U || height == 0U || span.host_data == nullptr) {
    return;
  }
  auto& ring = *rings_[active_frame_];
  const VkCommandBuffer command_buffer = ring.command_buffer_for_recording();
  if (command_buffer == VK_NULL_HANDLE) return;

  // The arena's rings record buffer copies only; this wrapper records the
  // image layout dance on the ring-owned command buffer (already begun by
  // acquire()) around a vkCmdCopyBufferToImage of the staged span.
  const VkImageSubresourceRange kRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkImageMemoryBarrier to_transfer{};
  to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_transfer.srcAccessMask = 0;
  to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_transfer.image = image;
  to_transfer.subresourceRange = kRange;
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &to_transfer);

  VkBufferImageCopy copy{};
  copy.bufferOffset = span.byte_offset;
  copy.bufferRowLength = 0;  // Tightly packed rows.
  copy.bufferImageHeight = 0;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(command_buffer, ring.ring_buffer(), image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  VkImageMemoryBarrier to_shader_read{};
  to_shader_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_shader_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_shader_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  to_shader_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_shader_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_shader_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_shader_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_shader_read.image = image;
  to_shader_read.subresourceRange = kRange;
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &to_shader_read);
#else
  (void)span;
  (void)image;
  (void)width;
  (void)height;
#endif
}

::warploom::core::Result<void> VulkanFrameUploadArena::submit(VkQueue queue) {
  if (active_frame_ >= rings_.size() || !queue) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  queue_ = queue;
  return rings_[active_frame_]->submit(queue);
}

void VulkanFrameUploadArena::wait_idle() noexcept {
  for (auto& ring : rings_) {
    if (ring) ring->wait_idle();
  }
}

} // namespace warploom::render
