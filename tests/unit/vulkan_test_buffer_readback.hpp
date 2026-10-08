#pragma once

//! @file vulkan_test_buffer_readback.hpp
//! @brief Read a device-local buffer's contents to the host in tests.
//!
//! Exists for the packed H-Z pyramid (B5b): the cull shader consumes the
//! pyramid as packed float-bit words from a device-local storage buffer, and
//! verifying that data required mapping it, which device-local memory does not
//! offer. Mirrors readback_swapchain_image's structure: own command pool,
//! one-shot copy, fence, host-visible landing buffer.

#include <cstdint>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>


namespace omnicpp_test {

struct BufferReadback {
  std::vector<std::uint32_t> words;  //!< whole 32-bit words, buffer order
};

//! Copies `size_bytes` from `src_buffer` (any location) to the host. Requires
//! size_bytes % 4 == 0. Returns empty words on any failure; the caller asserts.
[[nodiscard]] inline BufferReadback readback_buffer(
    VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
    std::uint32_t queue_family, VkBuffer src_buffer, VkDeviceSize size_bytes) {
  BufferReadback out;
  if (device == VK_NULL_HANDLE || src_buffer == VK_NULL_HANDLE ||
      queue == VK_NULL_HANDLE || (size_bytes % 4U) != 0U) {
    return out;
  }

  // Host-visible landing buffer.
  VkBufferCreateInfo dst_info{};
  dst_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  dst_info.size = size_bytes;
  dst_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  dst_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer dst = VK_NULL_HANDLE;
  if (vkCreateBuffer(device, &dst_info, nullptr, &dst) != VK_SUCCESS) {
    return out;
  }
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, dst, &req);

  std::uint32_t mem_type = UINT32_MAX;
  VkPhysicalDeviceMemoryProperties mem_props{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
  for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    const bool host_visible =
        (mem_props.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
    const bool host_coherent =
        (mem_props.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
    if (host_visible && host_coherent &&
        (req.memoryTypeBits & (1U << i)) != 0U) {
      mem_type = i;
      break;
    }
  }
  VkDeviceMemory dst_memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  const auto fail = [&]() {
    if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
    if (cmd != VK_NULL_HANDLE && pool != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device, pool, 1U, &cmd);
    }
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
    if (mapped != nullptr) vkUnmapMemory(device, dst_memory);
    if (dst_memory != VK_NULL_HANDLE) vkFreeMemory(device, dst_memory, nullptr);
    if (dst != VK_NULL_HANDLE) vkDestroyBuffer(device, dst, nullptr);
    return out;
  };

  if (mem_type == UINT32_MAX) return fail();
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = mem_type;
  if (vkAllocateMemory(device, &alloc, nullptr, &dst_memory) != VK_SUCCESS) {
    return fail();
  }
  if (vkBindBufferMemory(device, dst, dst_memory, 0) != VK_SUCCESS) {
    return fail();
  }
  if (vkMapMemory(device, dst_memory, 0, size_bytes, 0, &mapped) != VK_SUCCESS) {
    return fail();
  }

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family;
  if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    return fail();
  }
  VkCommandBufferAllocateInfo cmd_info{};
  cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmd_info.commandPool = pool;
  cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_info.commandBufferCount = 1U;
  if (vkAllocateCommandBuffers(device, &cmd_info, &cmd) != VK_SUCCESS) {
    return fail();
  }

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    return fail();
  }

  if (vkResetCommandBuffer(cmd, 0U) != VK_SUCCESS) return fail();
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return fail();
  VkBufferCopy copy{};
  copy.srcOffset = 0;
  copy.dstOffset = 0;
  copy.size = size_bytes;
  vkCmdCopyBuffer(cmd, src_buffer, dst, 1U, &copy);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &cmd;
  if (vkQueueSubmit(queue, 1U, &submit, fence) != VK_SUCCESS) return fail();
  if (vkWaitForFences(device, 1U, &fence, VK_TRUE, 5'000'000'000ULL) !=
      VK_SUCCESS) {
    return fail();
  }

  out.words.resize(static_cast<std::size_t>(size_bytes / 4U));
  std::memcpy(out.words.data(), mapped, size_bytes);
  return fail();  // frees everything; `out` already holds the words
}

}  // namespace omnicpp_test
