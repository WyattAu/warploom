/**
 * @file vulkan_compute.cpp
 * @brief Compute dispatch and event-based GPU synchronization.
 *
 * Uses core events (vkCmdSetEvent/vkCmdWaitEvents with explicit stage masks)
 * so the helpers work on any Vulkan 1.0+ device; the render graph's sync2
 * submission is orthogonal to event ordering.
 */

#include "warploom/render/vulkan_compute.hpp"

#include "warploom/core/contract.hpp"


#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

void cmd_signal_event([[maybe_unused]] VkCommandBuffer command_buffer,
                      [[maybe_unused]] VkEvent event,
                      [[maybe_unused]] std::uint32_t src_stage) {
#ifdef OMNICPP_HAS_VULKAN
  OMNICPP_CONTRACT(event != VK_NULL_HANDLE);
  vkCmdSetEvent(command_buffer, event, src_stage);
#endif
}

void cmd_acquire_shared_image(VkCommandBuffer command_buffer, VkEvent event,
                              const QueueImageState& state) {
#ifdef OMNICPP_HAS_VULKAN
  OMNICPP_CONTRACT(event != VK_NULL_HANDLE && state.image != VK_NULL_HANDLE);

  // Concurrent-usage images: queue ownership transfers via RELEASE (producer
  // side) then ACQUIRE (this call). With single-queue testing the acquire
  // barrier still performs the layout transition + visibility; the event
  // provides producer->consumer ordering without CPU round-trips.
  VkImageMemoryBarrier acquire{};
  acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  acquire.srcAccessMask = 0;  // The wait provides producer visibility.
  acquire.dstAccessMask = state.consumer_access;
  acquire.oldLayout = state.current_layout;
  acquire.newLayout = state.consumer_layout;
  acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  acquire.image = state.image;
  acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  vkCmdWaitEvents(
      command_buffer, 1, &event,
      state.last_stage, state.consumer_stage,
      0, nullptr, 0, nullptr, 1, &acquire);
#else
  (void)command_buffer;
  (void)event;
  (void)state;
#endif
}

void cmd_release_image_to_family(VkCommandBuffer command_buffer, VkImage image,
                                 std::uint32_t src_family, std::uint32_t dst_family,
                                 VkImageLayout layout, std::uint32_t src_access,
                                 std::uint32_t src_stage, std::uint32_t dst_stage) {
#ifdef OMNICPP_HAS_VULKAN
  VkImageMemoryBarrier release{};
  release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  release.srcAccessMask = src_access;
  release.dstAccessMask = 0;  // Acquire side supplies its own dst access.
  release.oldLayout = layout;
  release.newLayout = layout;  // Ownership transfer does not change layout.
  release.srcQueueFamilyIndex = src_family;
  release.dstQueueFamilyIndex = dst_family;
  release.image = image;
  release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0,
                       0, nullptr, 0, nullptr, 1, &release);
#else
  (void)command_buffer; (void)image; (void)src_family; (void)dst_family;
  (void)layout; (void)src_access; (void)src_stage; (void)dst_stage;
#endif
}

void cmd_acquire_image_from_family(VkCommandBuffer command_buffer, VkImage image,
                                   std::uint32_t src_family, std::uint32_t dst_family,
                                   VkImageLayout layout, std::uint32_t dst_access,
                                   std::uint32_t src_stage, std::uint32_t dst_stage) {
#ifdef OMNICPP_HAS_VULKAN
  VkImageMemoryBarrier acquire{};
  acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  acquire.srcAccessMask = 0;  // Producer's release + semaphore ordering.
  acquire.dstAccessMask = dst_access;
  acquire.oldLayout = layout;
  acquire.newLayout = layout;
  acquire.srcQueueFamilyIndex = src_family;
  acquire.dstQueueFamilyIndex = dst_family;
  acquire.image = image;
  acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0,
                       0, nullptr, 0, nullptr, 1, &acquire);
#else
  (void)command_buffer; (void)image; (void)src_family; (void)dst_family;
  (void)layout; (void)dst_access; (void)src_stage; (void)dst_stage;
#endif
}

void cmd_release_buffer_to_family(VkCommandBuffer command_buffer, VkBuffer buffer,
                                  VkDeviceSize offset, VkDeviceSize size,
                                  std::uint32_t src_family, std::uint32_t dst_family,
                                  std::uint32_t src_access, std::uint32_t src_stage,
                                  std::uint32_t dst_stage) {
#ifdef OMNICPP_HAS_VULKAN
  VkBufferMemoryBarrier release{};
  release.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  release.srcAccessMask = src_access;
  release.dstAccessMask = 0;
  release.srcQueueFamilyIndex = src_family;
  release.dstQueueFamilyIndex = dst_family;
  release.buffer = buffer;
  release.offset = offset;
  release.size = size;
  vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0,
                       0, nullptr, 1, &release, 0, nullptr);
#else
  (void)command_buffer; (void)buffer; (void)offset; (void)size;
  (void)src_family; (void)dst_family; (void)src_access; (void)src_stage; (void)dst_stage;
#endif
}

void cmd_acquire_buffer_from_family(VkCommandBuffer command_buffer, VkBuffer buffer,
                                    VkDeviceSize offset, VkDeviceSize size,
                                    std::uint32_t src_family, std::uint32_t dst_family,
                                    std::uint32_t dst_access, std::uint32_t src_stage,
                                    std::uint32_t dst_stage) {
#ifdef OMNICPP_HAS_VULKAN
  VkBufferMemoryBarrier acquire{};
  acquire.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  acquire.srcAccessMask = 0;
  acquire.dstAccessMask = dst_access;
  acquire.srcQueueFamilyIndex = src_family;
  acquire.dstQueueFamilyIndex = dst_family;
  acquire.buffer = buffer;
  acquire.offset = offset;
  acquire.size = size;
  vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0,
                       0, nullptr, 1, &acquire, 0, nullptr);
#else
  (void)command_buffer; (void)buffer; (void)offset; (void)size;
  (void)src_family; (void)dst_family; (void)dst_access; (void)src_stage; (void)dst_stage;
#endif
}

// =============================================================================
// AsyncComputeQueue
// =============================================================================

AsyncComputeQueue::~AsyncComputeQueue() { cleanup(); }

::warploom::core::Result<void> AsyncComputeQueue::initialize(VkDevice device,
                                                          VkQueue compute_queue,
                                                          std::uint32_t compute_family) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !compute_queue) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  if (device_) cleanup();
  device_ = device;
  queue_ = compute_queue;
  family_ = compute_family;

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = compute_family;
  if (vkCreateCommandPool(device_, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
    cleanup();
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = pool_;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = kInFlight;
  if (vkAllocateCommandBuffers(device_, &alloc_info, buffers_) != VK_SUCCESS) {
    cleanup();
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  buffer_ = buffers_[0];

  VkSemaphoreTypeCreateInfo timeline_info{};
  timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  timeline_info.initialValue = 0;
  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  sem_info.pNext = &timeline_info;
  if (vkCreateSemaphore(device_, &sem_info, nullptr, &timeline_) != VK_SUCCESS) {
    cleanup();
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  for (auto& fence : fences_) {
    if (vkCreateFence(device_, &fence_info, nullptr, &fence) != VK_SUCCESS) {
      cleanup();
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)compute_queue; (void)compute_family;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void AsyncComputeQueue::cleanup() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device_) {
    for (auto& fence : fences_) {
      if (fence) vkDestroyFence(device_, fence, nullptr);
    }
    if (timeline_) vkDestroySemaphore(device_, timeline_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);  // frees all buffers_
  }
#endif
  device_ = nullptr;
  queue_ = nullptr;
  family_ = 0;
  pool_ = nullptr;
  for (auto& b : buffers_) b = nullptr;
  buffer_ = nullptr;
  timeline_ = nullptr;
  for (auto& fence : fences_) fence = nullptr;
  timeline_value_ = 0;
  recording_ = false;
}

void AsyncComputeQueue::begin() {
#ifdef OMNICPP_HAS_VULKAN
  OMNICPP_CONTRACT(device_ && pool_);
  // Submission N (0-based) uses ring slot N % kInFlight: its own command
  // buffer and fence. A slot in use still carries submission N - kInFlight;
  // retire it before re-recording. Other slots remain in flight — that is
  // the overlap the ring exists for.
  const std::uint64_t n = timeline_value_;
  buffer_ = buffers_[n % kInFlight];
  VkFence slot_fence = fences_[n % kInFlight];
  if (n >= kInFlight) {
    const VkResult waited = vkWaitForFences(device_, 1, &slot_fence, VK_TRUE, 1'000'000'000ULL);
    OMNICPP_CONTRACT(waited == VK_SUCCESS);
    (void)waited;
  }
  vkResetFences(device_, 1, &slot_fence);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(buffer_, &begin);
  recording_ = true;
#else
#endif
}

void AsyncComputeQueue::record(void (*record_fn)(VkCommandBuffer, void*), void* user_data) {
#ifdef OMNICPP_HAS_VULKAN
  OMNICPP_CONTRACT(recording_ && record_fn);
  record_fn(buffer_, user_data);
#else
  (void)record_fn; (void)user_data;
#endif
}

::warploom::core::Result<std::uint64_t> AsyncComputeQueue::submit() {
#ifdef OMNICPP_HAS_VULKAN
  if (!recording_) {
    return ::warploom::core::Result<std::uint64_t>::error(::warploom::core::RuntimeError::invalid_config);
  }
  recording_ = false;
  if (vkEndCommandBuffer(buffer_) != VK_SUCCESS) {
    return ::warploom::core::Result<std::uint64_t>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  const std::uint64_t signal_value = timeline_value_ + 1;
  VkFence slot_fence = fences_[timeline_value_ % kInFlight];

  VkTimelineSemaphoreSubmitInfo timeline_submit{};
  timeline_submit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline_submit.signalSemaphoreValueCount = 1;
  timeline_submit.pSignalSemaphoreValues = &signal_value;

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.pNext = &timeline_submit;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &buffer_;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &timeline_;
  if (vkQueueSubmit(queue_, 1, &submit, slot_fence) != VK_SUCCESS) {
    return ::warploom::core::Result<std::uint64_t>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  timeline_value_ = signal_value;
  return ::warploom::core::Result<std::uint64_t>::ok(signal_value);
#else
  return ::warploom::core::Result<std::uint64_t>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

bool AsyncComputeQueue::wait_done(std::uint64_t timeout_ns) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || fences_[0] == nullptr) return false;
  // Wait only the slots that carried submissions: never-submitted fences are
  // unsignaled by definition and would make a whole-array wait time out.
  const std::uint64_t submitted = timeline_value_ < kInFlight ? timeline_value_ : kInFlight;
  if (submitted == 0) return true;
  VkFence used[kInFlight];
  for (std::uint64_t i = 0; i < submitted; ++i) used[i] = fences_[i];
  return vkWaitForFences(device_, static_cast<std::uint32_t>(submitted), used,
                         VK_TRUE, timeout_ns) == VK_SUCCESS;
#else
  (void)timeout_ns;
  return false;
#endif
}

}  // namespace warploom::render
