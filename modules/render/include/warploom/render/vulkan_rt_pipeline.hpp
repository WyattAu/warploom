#pragma once

/**
 * @file vulkan_rt_pipeline.hpp
 * @brief Ray-tracing pipeline (vkCmdTraceRaysKHR) with shader binding table.
 *
 * Complements VulkanRtQuery (ray queries in compute shaders): this module
 * owns the full RT pipeline — raygen / miss / closest-hit stages, the SBT,
 * and the traceRays dispatch. The renderer never touches per-pixel ray data;
 * the SBT is host-written once (per scene variant) and submitted by
 * vkCmdTraceRaysKHR.
 *
 * Contract summary:
 * - Stages are SPIR-V modules loaded by the caller (VulkanPipeline's stage
 *   loader or raw vkCreateShaderModule); groups are authored 1:1:
 *   group[0] = raygen (general), group[1..miss_count] = miss (general),
 *   group[1+miss_count..] = triangle hit groups (closest-hit only).
 * - The SBT buffer usage requires SHADER_BINDING_TABLE_BIT and
 *   SHADER_DEVICE_ADDRESS; memory allocation must carry
 *   VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT (VulkanMemoryAllocator does this
 *   whenever the buffer usage includes SHADER_DEVICE_ADDRESS).
 * - trace_rays() records onto the caller's command buffer; the caller owns
 *   submission and synchronization (same rule as the AS builders).
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace omnicpp::render {

//! Host-side description of one traceRays dispatch and its SBT contents.
struct RtTracePlan {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t depth{1};
  std::uint32_t miss_count{0};
  std::uint32_t hit_group_count{0};
  //! SPIR-V entry-point names (default "main" for all).
  std::string raygen_entry{"main"};
  std::string miss_entry{"main"};
  std::string hit_entry{"main"};
};

//! One ray-tracing pipeline + its shader binding table. One scene variant per
//! instance (recreate to change group topology; SBT handles are refetched via
//! write_sbt when modules stay the same).
class VulkanRtPipeline final {
 public:
  VulkanRtPipeline() = default;
  ~VulkanRtPipeline();
  VulkanRtPipeline(const VulkanRtPipeline&) = delete;
  VulkanRtPipeline& operator=(const VulkanRtPipeline&) = delete;
  VulkanRtPipeline(VulkanRtPipeline&&) = delete;
  VulkanRtPipeline& operator=(VulkanRtPipeline&&) = delete;

  //! Creates the RT pipeline from loaded stage modules. Group layout is
  //! 1 raygen + miss_count miss groups + hit_group_count triangle hit
  //! groups (closest-hit only, no any-hit). Max recursion comes from the
  //! pipeline, not the trace call. SBT strides/alignments are sized from
  //! `physical_device`'s ray-tracing pipeline properties.
  [[nodiscard]] omnicpp::core::Result<void> create(
      VkPhysicalDevice physical_device, VkDevice device, VkPipelineLayout layout,
      const std::vector<VkPipelineShaderStageCreateInfo>& stages,
      std::uint32_t miss_count, std::uint32_t hit_group_count,
      std::uint32_t max_recursion_depth);

  //! Fetches SBT group handles (vkGetRayTracingShaderGroupHandlesKHR) into
  //! host memory: raygen + miss + hit group handles back-to-back, each
  //! handle_size bytes. Call after create().
  [[nodiscard]] omnicpp::core::Result<std::vector<std::uint8_t>> fetch_handles(
      VkDevice device) const;

  //! Host-writes the SBT into `sbt` (usage SHADER_BINDING_TABLE_BIT |
  //! SHADER_DEVICE_ADDRESS_BIT, HOST_VISIBLE|HOST_COHERENT memory — valid on
  //! all implementations and simpler than a staging upload at engine scale;
  //! size it with required_sbt_bytes). Layout: raygen region, then miss
  //! region, then hit region, each region start aligned to
  //! shaderGroupBaseAlignment and each stride handle-aligned. Records the
  //! region device addresses for trace_rays.
  [[nodiscard]] omnicpp::core::Result<void> write_sbt(
      VkDevice device, const std::vector<std::uint8_t>& handles,
      Allocation& sbt);

  //! Total SBT bytes for 1 raygen + `miss_count` + `hit_group_count` groups
  //! on this device (region alignment included).
  [[nodiscard]] static VkDeviceSize required_sbt_bytes(
      VkPhysicalDevice physical_device, std::uint32_t miss_count,
      std::uint32_t hit_group_count);

  //! Records vkCmdTraceRaysKHR with the region addresses captured by
  //! write_sbt.
  void trace_rays(VkCommandBuffer cmd, std::uint32_t width,
                  std::uint32_t height, std::uint32_t depth = 1U) const noexcept;

  void cleanup(VkDevice device) noexcept;

  [[nodiscard]] VkPipeline pipeline() const noexcept { return pipeline_; }
  [[nodiscard]] std::uint64_t raygen_device_address() const noexcept {
    return raygen_addr_;
  }
  [[nodiscard]] std::uint64_t miss_device_address() const noexcept {
    return miss_addr_;
  }
  [[nodiscard]] std::uint64_t hit_device_address() const noexcept {
    return hit_addr_;
  }
  [[nodiscard]] VkDeviceSize raygen_stride() const noexcept { return raygen_stride_; }
  [[nodiscard]] VkDeviceSize miss_stride() const noexcept { return miss_stride_; }
  [[nodiscard]] VkDeviceSize hit_stride() const noexcept { return hit_stride_; }

 private:
  VkPipeline pipeline_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  std::uint32_t miss_count_{0};
  std::uint32_t hit_group_count_{0};
  VkDeviceSize handle_size_{32};
  VkDeviceSize raygen_stride_{32};
  VkDeviceSize miss_stride_{32};
  VkDeviceSize hit_stride_{32};
  VkDeviceSize base_alignment_{32};
  std::uint64_t raygen_addr_{0};
  std::uint64_t miss_addr_{0};
  std::uint64_t hit_addr_{0};
};

}  // namespace omnicpp::render
