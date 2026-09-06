#include "engine/render/vulkan_memory_allocator.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace omnicpp::render {

namespace {
constexpr VkDeviceSize round_up(VkDeviceSize value, VkDeviceSize alignment) noexcept {
  return (alignment <= 1) ? value : ((value + alignment - 1) / alignment) * alignment;
}
} // namespace

// =============================================================================
// VulkanMemoryAllocator
// =============================================================================

VulkanMemoryAllocator::~VulkanMemoryAllocator() { cleanup(); }

omnicpp::core::Result<void> VulkanMemoryAllocator::initialize(
    VkDevice device, VkPhysicalDevice physical_device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !physical_device) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  if (device_) cleanup();
  device_ = device;
  physical_device_ = physical_device;
  // Detect a dedicated COMPUTE-only family so cross-queue buffers can use
  // CONCURRENT sharing (no ownership-transfer ping-pong on handoff).
  has_dedicated_compute_ = false;
  compute_family_ = graphics_family_ = 0;
  std::uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
  if (family_count > 0) {
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
    for (std::uint32_t i = 0; i < family_count; ++i) {
      const bool gfx = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
      const bool comp = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
      if (gfx && graphics_family_ == 0 && families[i].queueCount > 0) {
        graphics_family_ = i;
      }
      if (comp && !gfx) {
        has_dedicated_compute_ = true;
        compute_family_ = i;
        break;
      }
    }
  }
  return omnicpp::core::Result<void>::ok();
#else
  (void)device; (void)physical_device;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanMemoryAllocator::cleanup() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device_) {
    for (auto& block : blocks_) {
      if (block.memory) vkFreeMemory(device_, block.memory, nullptr);
    }
  }
#endif
  blocks_.clear();
  allocation_count_ = 0;
  device_ = VK_NULL_HANDLE;
  physical_device_ = VK_NULL_HANDLE;
  has_dedicated_compute_ = false;
  compute_family_ = 0;
  graphics_family_ = 0;
}

std::uint32_t VulkanMemoryAllocator::find_memory_type(
    std::uint32_t type_bits, VkMemoryPropertyFlags properties) const {
#ifdef OMNICPP_HAS_VULKAN
  VkPhysicalDeviceMemoryProperties memory_properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
  for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
    const bool type_allowed = (type_bits & (1U << i)) != 0U;
    const bool has_properties =
        (memory_properties.memoryTypes[i].propertyFlags & properties) == properties;
    if (type_allowed && has_properties) return i;
  }
#else
  (void)type_bits; (void)properties;
#endif
  return UINT32_MAX;
}

std::size_t VulkanMemoryAllocator::find_or_create_block(
    VkDeviceSize size, std::uint32_t type_bits, VkMemoryPropertyFlags properties) {
#ifdef OMNICPP_HAS_VULKAN
  // Prefer an existing block that supports the resource's memory types AND
  // carries every requested property. A DEVICE_LOCAL-only block must never
  // serve a HOST_VISIBLE request: its range would be unmapped (mapped=null).
  VkPhysicalDeviceMemoryProperties memory_properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
  for (std::size_t i = 0; i < blocks_.size(); ++i) {
    if ((type_bits & (1U << blocks_[i].memory_type)) == 0U) continue;
    const auto block_flags = memory_properties.memoryTypes[blocks_[i].memory_type].propertyFlags;
    if ((block_flags & properties) != properties) continue;
    // A block is mapped only when its ORIGINAL request included HOST_VISIBLE.
    // On drivers where a device-local type is also host-visible (llvmpipe), a
    // flag-only check would serve host-visible allocations from an unmapped
    // block, yielding mapped=nullptr and a crash on first CPU write.
    if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U &&
        blocks_[i].mapped == nullptr) {
      continue;
    }
    for (const auto& range : blocks_[i].free_ranges) {
      if (range.size >= size) return i;
    }
  }

  const VkDeviceSize block_size = (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U
      ? kDeviceLocalBlockSize : kHostVisibleBlockSize;

  // The block's memory type must satisfy both the properties and the
  // resource's required type bits (VUID-vkBindImageMemory-memory-01047).
  const std::uint32_t memory_type = find_memory_type(type_bits, properties);
  if (memory_type == UINT32_MAX) return SIZE_MAX;

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = std::max(block_size, size);
  alloc_info.memoryTypeIndex = memory_type;

  Block block;
  block.size = alloc_info.allocationSize;
  block.memory_type = memory_type;
  if (vkAllocateMemory(device_, &alloc_info, nullptr, &block.memory) != VK_SUCCESS) {
    return SIZE_MAX;
  }
  if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
    if (vkMapMemory(device_, block.memory, 0, block.size, 0, &block.mapped) != VK_SUCCESS) {
      vkFreeMemory(device_, block.memory, nullptr);
      block.memory = VK_NULL_HANDLE;
      return SIZE_MAX;
    }
  }
  block.free_ranges.push_back({0, block.size});
  blocks_.push_back(std::move(block));
  return blocks_.size() - 1;
#else
  (void)size; (void)type_bits; (void)properties;
  return SIZE_MAX;
#endif
}

omnicpp::core::Result<Allocation> VulkanMemoryAllocator::allocate_sized(
    VkDeviceSize size, VkDeviceSize alignment, std::uint32_t type_bits,
    VkMemoryPropertyFlags properties) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || size == 0) {
    return omnicpp::core::Result<Allocation>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  const std::size_t block_index = find_or_create_block(size, type_bits, properties);
  if (block_index == SIZE_MAX) {
    return omnicpp::core::Result<Allocation>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  Block& block = blocks_[block_index];

  // First-fit with alignment: carve [aligned_start, aligned_start + size).
  // NOTE: no references into block.free_ranges may be held across the mutations
  // below — insert() can reallocate the vector and leave the reference dangling
  // (this was a real heap-corruption bug). Everything is computed from a value
  // copy first, then applied by index.
  for (std::size_t r = 0; r < block.free_ranges.size(); ++r) {
    const FreeRange original = block.free_ranges[r];
    const VkDeviceSize aligned_start = round_up(original.offset, alignment);
    const VkDeviceSize pad = aligned_start - original.offset;
    if (pad >= original.size || original.size - pad < size) continue;

    const VkDeviceSize remainder_offset = aligned_start + size;
    const VkDeviceSize remainder_size = original.size - pad - size;

    Allocation allocation;
    allocation.memory = block.memory;
    allocation.offset = aligned_start;
    allocation.size = size;
    allocation.mapped = block.mapped
        ? static_cast<std::byte*>(block.mapped) + aligned_start : nullptr;
    allocation.memory_type = block.memory_type;
    allocation.block_index = block_index;

    if (pad > 0 && remainder_size == 0) {
      // Whole range minus the pad: slot becomes the pad range.
      block.free_ranges[r] = FreeRange{original.offset, pad};
    } else if (pad > 0) {
      // Slot becomes the remainder, pad is inserted before it.
      block.free_ranges[r] = FreeRange{remainder_offset, remainder_size};
      block.free_ranges.insert(
          block.free_ranges.begin() + static_cast<std::ptrdiff_t>(r),
          FreeRange{original.offset, pad});
    } else if (remainder_size == 0) {
      block.free_ranges.erase(block.free_ranges.begin() +
                              static_cast<std::ptrdiff_t>(r));
    } else {
      block.free_ranges[r] = FreeRange{remainder_offset, remainder_size};
    }
    block.used += size;
    ++allocation_count_;
    return omnicpp::core::Result<Allocation>::ok(allocation);
  }
  return omnicpp::core::Result<Allocation>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#else
  (void)size; (void)alignment; (void)type_bits; (void)properties;
  return omnicpp::core::Result<Allocation>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<Allocation> VulkanMemoryAllocator::create_buffer(
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || size == 0) {
    return omnicpp::core::Result<Allocation>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  // Buffers shared between compute and graphics queues: when the device has a
  // dedicated COMPUTE-only family, mark CONCURRENT (family pair {compute,
  // graphics}) so cross-family handoffs need no release/acquire ping-pong.
  std::array<std::uint32_t, 2> sharing_families{};
  if (has_dedicated_compute_ && compute_family_ != graphics_family_) {
    sharing_families[0] = compute_family_;
    sharing_families[1] = graphics_family_;
    buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    buffer_info.queueFamilyIndexCount = 2;
    buffer_info.pQueueFamilyIndices = sharing_families.data();
  }
  VkBuffer buffer = VK_NULL_HANDLE;
  if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
    return omnicpp::core::Result<Allocation>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device_, buffer, &requirements);

  auto allocation = allocate_sized(requirements.size, requirements.alignment,
                                   requirements.memoryTypeBits, properties);
  if (!allocation.is_ok()) {
    vkDestroyBuffer(device_, buffer, nullptr);
    return allocation;
  }
  Allocation alloc = allocation.value();
  if (vkBindBufferMemory(device_, buffer, alloc.memory, alloc.offset) != VK_SUCCESS) {
    destroy_allocation(alloc);
    vkDestroyBuffer(device_, buffer, nullptr);
    return omnicpp::core::Result<Allocation>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  alloc.buffer = buffer;
  return omnicpp::core::Result<Allocation>::ok(alloc);
#else
  (void)size; (void)usage; (void)properties;
  return omnicpp::core::Result<Allocation>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<Allocation> VulkanMemoryAllocator::bind_image(
    VkImage image, VkMemoryPropertyFlags properties) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || !image) {
    return omnicpp::core::Result<Allocation>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, image, &requirements);
  auto allocation = allocate_sized(requirements.size, requirements.alignment,
                                   requirements.memoryTypeBits, properties);
  if (!allocation.is_ok()) return allocation;
  Allocation alloc = allocation.value();
  if (vkBindImageMemory(device_, image, alloc.memory, alloc.offset) != VK_SUCCESS) {
    destroy_allocation(alloc);
    return omnicpp::core::Result<Allocation>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  alloc.image = image;
  return omnicpp::core::Result<Allocation>::ok(alloc);
#else
  (void)image; (void)properties;
  return omnicpp::core::Result<Allocation>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanMemoryAllocator::destroy_allocation(Allocation& allocation) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || !allocation.is_valid()) return;
  if (allocation.buffer) {
    vkDestroyBuffer(device_, allocation.buffer, nullptr);
    allocation.buffer = VK_NULL_HANDLE;
  }
  if (allocation.block_index < blocks_.size()) {
    Block& block = blocks_[allocation.block_index];
    FreeRange freed{allocation.offset, allocation.size};
    // Coalesce with a preceding free range.
    for (std::size_t i = 0; i < block.free_ranges.size(); ++i) {
      if (block.free_ranges[i].offset + block.free_ranges[i].size == freed.offset) {
        block.free_ranges[i].size += freed.size;
        freed = block.free_ranges[i];
        block.free_ranges.erase(block.free_ranges.begin() +
                                static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    // Coalesce with a following free range.
    bool merged = false;
    for (std::size_t i = 0; i < block.free_ranges.size(); ++i) {
      if (freed.offset + freed.size == block.free_ranges[i].offset) {
        block.free_ranges[i].offset = freed.offset;
        block.free_ranges[i].size += freed.size;
        merged = true;
        break;
      }
    }
    if (!merged) block.free_ranges.push_back(freed);
    block.used = (block.used > allocation.size) ? block.used - allocation.size : 0;
    if (allocation_count_ > 0) --allocation_count_;
  }
  allocation.memory = VK_NULL_HANDLE;
  allocation.mapped = nullptr;
  allocation.offset = 0;
  allocation.size = 0;
#else
  (void)allocation;
#endif
}

AllocatorStats VulkanMemoryAllocator::stats() const noexcept {
  AllocatorStats out;
  out.allocation_count = static_cast<std::uint32_t>(allocation_count_);
  out.block_count = static_cast<std::uint32_t>(blocks_.size());
  for (const auto& block : blocks_) {
    out.reserved_bytes += block.size;
    out.used_bytes += block.used;
  }
  return out;
}

// =============================================================================
// VulkanUploadRing
// =============================================================================

VulkanUploadRing::~VulkanUploadRing() { cleanup(); }

omnicpp::core::Result<void> VulkanUploadRing::initialize(
    VkDevice device, VkPhysicalDevice physical_device,
    std::uint32_t queue_family_index, VkDeviceSize total_size) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !physical_device || total_size == 0) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  device_ = device;

  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = total_size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &buffer_info, nullptr, &ring_buffer_) != VK_SUCCESS) {
    cleanup();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device_, ring_buffer_, &requirements);
  VkPhysicalDeviceMemoryProperties memory_properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
  std::uint32_t memory_type = UINT32_MAX;
  for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
    const bool allowed = (requirements.memoryTypeBits & (1U << i)) != 0U;
    const auto flags = memory_properties.memoryTypes[i].propertyFlags;
    if (allowed && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U &&
        (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U) {
      memory_type = i;
      break;
    }
  }
  if (memory_type == UINT32_MAX) {
    cleanup();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = requirements.size;
  alloc_info.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device_, &alloc_info, nullptr, &ring_memory_) != VK_SUCCESS) {
    cleanup();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  if (vkBindBufferMemory(device_, ring_buffer_, ring_memory_, 0) != VK_SUCCESS ||
      vkMapMemory(device_, ring_memory_, 0, requirements.size, 0, &mapped_) != VK_SUCCESS) {
    cleanup();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  capacity_ = requirements.size;

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_index;
  if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
    cleanup();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkCommandBufferAllocateInfo cb_info{};
  cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_info.commandPool = command_pool_;
  cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device_, &cb_info, &command_buffer_) != VK_SUCCESS) {
    cleanup();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  return omnicpp::core::Result<void>::ok();
#else
  (void)device; (void)physical_device; (void)queue_family_index; (void)total_size;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanUploadRing::retire_completed() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  for (auto it = in_flight_.begin(); it != in_flight_.end();) {
    if (it->fence == VK_NULL_HANDLE ||
        vkGetFenceStatus(device_, it->fence) != VK_SUCCESS) {
      ++it;
      continue;
    }
    const VkFence completed = it->fence;
    for (auto erase_it = in_flight_.begin(); erase_it != in_flight_.end();) {
      if (erase_it->fence == completed) erase_it = in_flight_.erase(erase_it);
      else ++erase_it;
    }
    vkDestroyFence(device_, completed, nullptr);
    it = in_flight_.begin();
  }
#endif
}

void VulkanUploadRing::wait_region_free(VkDeviceSize start, VkDeviceSize size) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  const VkDeviceSize end = start + size;
  for (auto it = in_flight_.begin(); it != in_flight_.end();) {
    const VkDeviceSize f_start = it->start;
    const VkDeviceSize f_end = it->start + it->size;
    const bool overlaps = f_start < end && start < f_end;
    if (overlaps && it->fence != VK_NULL_HANDLE) {
      const VkFence completed = it->fence;
      vkWaitForFences(device_, 1, &completed, VK_TRUE, UINT64_MAX);
      for (auto erase_it = in_flight_.begin(); erase_it != in_flight_.end();) {
        if (erase_it->fence == completed) erase_it = in_flight_.erase(erase_it);
        else ++erase_it;
      }
      vkDestroyFence(device_, completed, nullptr);
      it = in_flight_.begin();
    } else {
      ++it;
    }
  }
#else
  (void)start; (void)size;
#endif
}

omnicpp::core::Result<VulkanUploadRing::UploadSpan> VulkanUploadRing::acquire(
    VkDeviceSize size) {
#ifdef OMNICPP_HAS_VULKAN
  if (!is_initialized() || size == 0 || size > capacity_) {
    return omnicpp::core::Result<UploadSpan>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  retire_completed();
  if (head_ + size > capacity_) {
    // Wrap: retire the tail region [head_, capacity) before reusing the start.
    wait_region_free(head_, capacity_ - head_);
    head_ = 0;
  }
  wait_region_free(head_, size);

  UploadSpan span;
  span.host_data = static_cast<std::byte*>(mapped_) + head_;
  span.byte_offset = head_;
  span.size = size;
  // Record the exact handed-out range; submit() attaches it to the batch fence.
  staged_ranges_.push_back({head_, size});
  head_ += size;
  return omnicpp::core::Result<UploadSpan>::ok(span);
#else
  (void)size;
  return omnicpp::core::Result<UploadSpan>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanUploadRing::begin_commands() {
#ifdef OMNICPP_HAS_VULKAN
  if (recording_) return omnicpp::core::Result<void>::ok();
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command_buffer_, &begin) != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  recording_ = true;
  return omnicpp::core::Result<void>::ok();
#else
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanUploadRing::begin_recording() {
  return begin_commands();
}
void VulkanUploadRing::record_copy(VkCommandBuffer command_buffer,
                                   const UploadSpan& span,
                                   VkBuffer dst_buffer,
                                   VkDeviceSize dst_offset) const noexcept {
#ifdef OMNICPP_HAS_VULKAN
  VkBufferCopy copy{};
  copy.srcOffset = span.byte_offset;
  copy.dstOffset = dst_offset;
  copy.size = span.size;
  vkCmdCopyBuffer(command_buffer, ring_buffer_, dst_buffer, 1, &copy);
#else
  (void)command_buffer; (void)span; (void)dst_buffer; (void)dst_offset;
#endif
}

omnicpp::core::Result<void> VulkanUploadRing::submit(VkQueue queue) {
#ifdef OMNICPP_HAS_VULKAN
  if (!is_initialized() || !queue) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  if (!recording_) return omnicpp::core::Result<void>::ok();

  if (vkEndCommandBuffer(command_buffer_) != VK_SUCCESS) {
    recording_ = false;
    staged_ranges_.clear();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  recording_ = false;

  VkFence fence = VK_NULL_HANDLE;
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vkCreateFence(device_, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    staged_ranges_.clear();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer_;
  const VkResult result = vkQueueSubmit(queue, 1, &submit, fence);
  if (result != VK_SUCCESS) {
    vkDestroyFence(device_, fence, nullptr);
    staged_ranges_.clear();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  if (staged_ranges_.empty()) {
    vkDestroyFence(device_, fence, nullptr);
    return omnicpp::core::Result<void>::ok();
  }
  // Every range handed out since the last submit is guarded by this fence.
  for (std::size_t i = 0; i < staged_ranges_.size(); ++i) {
    const auto& range = staged_ranges_[i];
    in_flight_.push_back({range.offset, range.size, fence, i == 0U});
  }
  staged_ranges_.clear();
  return omnicpp::core::Result<void>::ok();
#else
  (void)queue;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanUploadRing::wait_idle() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  for (auto& f : in_flight_) {
    if (f.fence) vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
  }
  if (recording_) {
    vkEndCommandBuffer(command_buffer_);
    recording_ = false;
  }
  if (device_ && command_buffer_) {
    vkResetCommandBuffer(command_buffer_, 0);
  }
  std::vector<VkFence> fences;
  for (const auto& f : in_flight_) {
    if (f.fence && std::find(fences.begin(), fences.end(), f.fence) == fences.end()) {
      fences.push_back(f.fence);
    }
  }
  for (const auto fence : fences) vkDestroyFence(device_, fence, nullptr);
#endif
  in_flight_.clear();
  staged_ranges_.clear();
  head_ = 0;
}

void VulkanUploadRing::cleanup() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device_) {
    wait_idle();
    std::vector<VkFence> destroyed;
    for (const auto& f : in_flight_) {
      if (f.fence && std::find(destroyed.begin(), destroyed.end(), f.fence) == destroyed.end()) {
        vkDestroyFence(device_, f.fence, nullptr);
        destroyed.push_back(f.fence);
      }
    }
    in_flight_.clear();
    if (ring_memory_) vkFreeMemory(device_, ring_memory_, nullptr);
    if (ring_buffer_) vkDestroyBuffer(device_, ring_buffer_, nullptr);
    if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
  }
#endif
  ring_buffer_ = VK_NULL_HANDLE;
  ring_memory_ = VK_NULL_HANDLE;
  mapped_ = nullptr;
  capacity_ = 0;
  head_ = 0;
  command_pool_ = VK_NULL_HANDLE;
  command_buffer_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

} // namespace omnicpp::render
