#include "warploom/render/vulkan_parallel_recorder.hpp"

#include <algorithm>
#include <thread>

#include "warploom/core/job_system.hpp"

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

namespace {

#ifdef OMNICPP_HAS_VULKAN
//! Function-pointer trampoline: records one band's secondary command buffer.
void run_band_job(void* raw) {
  auto* job = static_cast<VulkanParallelRecorder::BandJob*>(raw);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
  VkCommandBufferInheritanceInfo inheritance{};
  inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
  inheritance.renderPass = job->render_pass;
  inheritance.framebuffer = job->framebuffer;
  inheritance.subpass = 0;
  begin.pInheritanceInfo = &inheritance;
  vkBeginCommandBuffer(job->buffer, &begin);

  VkRect2D scissor{};
  scissor.offset = {0, static_cast<std::int32_t>(job->band_offset)};
  scissor.extent = {job->width, job->extent_height};
  (*job->fn)(job->buffer, scissor);

  vkEndCommandBuffer(job->buffer);
}
#endif // OMNICPP_HAS_VULKAN

} // namespace

VulkanParallelRecorder::~VulkanParallelRecorder() { cleanup(); }

::warploom::core::Result<void> VulkanParallelRecorder::initialize(
    VkDevice device, std::uint32_t queue_family_index, std::uint32_t band_count) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || band_count == 0) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  if (device_) cleanup();
  device_ = device;
  queue_family_index_ = queue_family_index;
  band_count_ = band_count;
  bands_.resize(band_count);
  band_jobs_.resize(band_count);
  for (auto& band : bands_) {
    band.pool = VK_NULL_HANDLE;
    band.buffer = VK_NULL_HANDLE;
  }
  for (auto& band : bands_) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_index;
    if (vkCreateCommandPool(device_, &pool_info, nullptr, &band.pool) != VK_SUCCESS) {
      cleanup();
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::vulkan_not_available);
    }
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = band.pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &alloc_info, &band.buffer) != VK_SUCCESS) {
      cleanup();
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::vulkan_not_available);
    }
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)queue_family_index; (void)band_count;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanParallelRecorder::cleanup() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device_) {
    for (auto& band : bands_) {
      if (band.pool) vkDestroyCommandPool(device_, band.pool, nullptr);
    }
  }
#endif
  bands_.clear();
  band_jobs_.clear();
  band_count_ = 0;
  device_ = VK_NULL_HANDLE;
}

::warploom::core::Result<std::vector<VkCommandBuffer>> VulkanParallelRecorder::record_parallel(
    std::uint32_t width, std::uint32_t height, const RecordBandFn& record_band,
    VkRenderPass compatible_pass, VkFramebuffer framebuffer) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || !record_band || width == 0 || height == 0 || !compatible_pass) {
    return ::warploom::core::Result<std::vector<VkCommandBuffer>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  if (height < band_count_) {
    return ::warploom::core::Result<std::vector<VkCommandBuffer>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  const std::size_t band_count = bands_.size();
  const std::uint32_t band_height = height / band_count_;

  // Fill the reused per-band job payloads (no allocation: band_jobs_ was
  // sized at initialize()).
  for (std::size_t i = 0; i < band_count; ++i) {
    BandJob& job = band_jobs_[i];
    job.fn = &record_band;
    job.buffer = bands_[i].buffer;
    job.render_pass = compatible_pass;
    job.framebuffer = framebuffer;
    job.band_offset = static_cast<std::uint32_t>(i) * band_height;
    job.band_height = band_height;
    job.width = width;
    job.extent_height = (i + 1 == band_count) ? height - job.band_offset : band_height;
  }

  if (job_system_ != nullptr && job_system_->is_running()) {
    // Persistent-workers path: fork/join through the job system. No thread
    // creation and no heap allocation per frame. Band 0 runs on the calling
    // thread (it has nothing better to do), bands 1..n-1 as raw jobs.
    ::warploom::core::JobCounter counter;
    counter.add(static_cast<int>(band_count - 1));
    for (std::size_t i = 1; i < band_count; ++i) {
      job_system_->submit_raw(::warploom::core::JobPriority::render, &counter,
                              &run_band_job, &band_jobs_[i]);
    }
    run_band_job(&band_jobs_[0]);
    counter.wait();
  } else {
    // Fallback: ad-hoc threads (original behavior; per-frame thread creation).
    // Fallback: ad-hoc threads (original behavior; per-frame thread creation).
    auto record_band_buffer = [&](std::size_t index) {
      run_band_job(&band_jobs_[index]);
    };
    std::vector<std::thread> workers;
    workers.reserve(band_count - 1);
    for (std::size_t i = 1; i < band_count; ++i) {
      workers.emplace_back(record_band_buffer, i);
    }
    record_band_buffer(0);
    for (auto& worker : workers) {
      worker.join();
    }
  }

  std::vector<VkCommandBuffer> buffers;
  buffers.reserve(band_count);
  for (const auto& band : bands_) {
    buffers.push_back(band.buffer);
  }
  return ::warploom::core::Result<std::vector<VkCommandBuffer>>::ok(buffers);
#else
  (void)width; (void)height; (void)record_band;
  (void)compatible_pass; (void)framebuffer;
  return ::warploom::core::Result<std::vector<VkCommandBuffer>>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanParallelRecorder::reset() {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  for (auto& band : bands_) {
    vkResetCommandBuffer(band.buffer, 0);
  }
  return ::warploom::core::Result<void>::ok();
#else
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

} // namespace warploom::render
