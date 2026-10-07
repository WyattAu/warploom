//! @file vulkan_command.cpp
//! @brief Command pool/buffer allocation (see vulkan_command.hpp).

#include <warploom/render/vulkan_command.hpp>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

::warploom::core::Result<VkCommandPool> create_command_pool(
    VkDevice device, std::uint32_t queue_family_index) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device)
    return ::warploom::core::Result<VkCommandPool>::error(
        ::warploom::core::RuntimeError::vulkan_not_available);

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_index;

  VkCommandPool pool = nullptr;
  const VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
  if (result != VK_SUCCESS || !pool)
    return ::warploom::core::Result<VkCommandPool>::error(
        ::warploom::core::RuntimeError::vulkan_not_available);
  return ::warploom::core::Result<VkCommandPool>::ok(pool);
#else
  (void)device;
  (void)queue_family_index;
  return ::warploom::core::Result<VkCommandPool>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<VkCommandBuffer> allocate_command_buffer(
    VkDevice device, VkCommandPool pool) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !pool)
    return ::warploom::core::Result<VkCommandBuffer>::error(
        ::warploom::core::RuntimeError::vulkan_not_available);

  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;

  VkCommandBuffer cb = nullptr;
  const VkResult result = vkAllocateCommandBuffers(device, &alloc_info, &cb);
  if (result != VK_SUCCESS || !cb)
    return ::warploom::core::Result<VkCommandBuffer>::error(
        ::warploom::core::RuntimeError::vulkan_not_available);
  return ::warploom::core::Result<VkCommandBuffer>::ok(cb);
#else
  (void)device;
  (void)pool;
  return ::warploom::core::Result<VkCommandBuffer>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

}  // namespace warploom::render
