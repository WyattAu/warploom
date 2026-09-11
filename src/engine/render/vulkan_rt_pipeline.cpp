//! @file vulkan_rt_pipeline.cpp
//! @brief Ray-tracing pipeline + SBT (see the header). Extension entry
//!        points (vkCreateRayTracingPipelinesKHR,
//!        vkGetRayTracingShaderGroupHandlesKHR, vkCmdTraceRaysKHR) are
//!        fetched via vkGetDeviceProcAddr so the engine links without an RT
//!        loader. The creating VkDevice is retained for those lookups.

#include "engine/render/vulkan_rt_pipeline.hpp"

#include <cstring>
#include <utility>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace omnicpp::render {

namespace {

#ifdef OMNICPP_HAS_VULKAN

PFN_vkCreateRayTracingPipelinesKHR create_rt_pipelines_fn(VkDevice device) noexcept {
  return reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
      vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
}

PFN_vkGetRayTracingShaderGroupHandlesKHR group_handles_fn(VkDevice device) noexcept {
  return reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
      vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR"));
}

PFN_vkCmdTraceRaysKHR cmd_trace_rays_fn(VkDevice device) noexcept {
  return reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
      vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR"));
}

//! Rounds `v` up to a multiple of `align` (align must be a power of two).
VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize align) noexcept {
  return (v + align - 1) & ~(align - 1);
}

#endif  // OMNICPP_HAS_VULKAN

}  // namespace

VulkanRtPipeline::~VulkanRtPipeline() = default;

#ifdef OMNICPP_HAS_VULKAN

omnicpp::core::Result<void> VulkanRtPipeline::create(
    VkPhysicalDevice physical_device, VkDevice device, VkPipelineLayout layout,
    const std::vector<VkPipelineShaderStageCreateInfo>& stages,
    std::uint32_t miss_count, std::uint32_t hit_group_count,
    std::uint32_t max_recursion_depth) {
  if (physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
      layout == VK_NULL_HANDLE) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }
  if (stages.empty() || miss_count == 0U) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }

  // Group topology: [0] raygen (general), [1..miss_count] miss (general),
  // [1+miss_count .. 1+miss_count+hit_group_count) triangle hit groups.
  std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
  groups.reserve(1U + static_cast<std::size_t>(miss_count) +
                 static_cast<std::size_t>(hit_group_count));
  VkRayTracingShaderGroupCreateInfoKHR gen{};
  gen.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
  gen.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  gen.generalShader = 0U;
  gen.closestHitShader = VK_SHADER_UNUSED_KHR;
  gen.anyHitShader = VK_SHADER_UNUSED_KHR;
  gen.intersectionShader = VK_SHADER_UNUSED_KHR;
  groups.push_back(gen);
  for (std::uint32_t m = 0; m < miss_count; ++m) {
    VkRayTracingShaderGroupCreateInfoKHR g = gen;
    g.generalShader = 1U + m;
    groups.push_back(g);
  }
  for (std::uint32_t h = 0; h < hit_group_count; ++h) {
    VkRayTracingShaderGroupCreateInfoKHR g{};
    g.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    g.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    g.generalShader = VK_SHADER_UNUSED_KHR;
    g.closestHitShader = 1U + miss_count + h;
    g.anyHitShader = VK_SHADER_UNUSED_KHR;
    g.intersectionShader = VK_SHADER_UNUSED_KHR;
    groups.push_back(g);
  }

  VkRayTracingPipelineCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
  info.stageCount = static_cast<std::uint32_t>(stages.size());
  info.pStages = stages.data();
  info.groupCount = static_cast<std::uint32_t>(groups.size());
  info.pGroups = groups.data();
  info.maxPipelineRayRecursionDepth = max_recursion_depth;
  info.layout = layout;

  auto create_fn = create_rt_pipelines_fn(device);
  if (create_fn == nullptr) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  const VkResult vr = create_fn(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1U,
                                &info, nullptr, &pipeline_);
  if (vr != VK_SUCCESS || pipeline_ == VK_NULL_HANDLE) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  miss_count_ = miss_count;
  hit_group_count_ = hit_group_count;
  device_ = device;

  // SBT sizing rules come from THIS device's ray-tracing pipeline
  // properties, not the spec minima: implementations differ (e.g. NVIDIA
  // requires 64-byte shaderGroupBaseAlignment) and vkCmdTraceRaysKHR
  // validates every region address/stride against them.
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_props{};
  rt_props.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &rt_props;
  vkGetPhysicalDeviceProperties2(physical_device, &props2);
  handle_size_ = rt_props.shaderGroupHandleSize;
  base_alignment_ = rt_props.shaderGroupBaseAlignment;
  const VkDeviceSize stride =
      align_up(handle_size_, rt_props.shaderGroupHandleAlignment);
  raygen_stride_ = stride;
  miss_stride_ = stride;
  hit_stride_ = stride;
  return omnicpp::core::Result<void>::ok();
}

omnicpp::core::Result<std::vector<std::uint8_t>>
VulkanRtPipeline::fetch_handles(VkDevice device) const {
  if (pipeline_ == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
    return omnicpp::core::Result<std::vector<std::uint8_t>>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }
  auto handles_fn = group_handles_fn(device);
  if (handles_fn == nullptr) {
    return omnicpp::core::Result<std::vector<std::uint8_t>>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  const std::uint32_t handle_count = 1U + miss_count_ + hit_group_count_;
  std::vector<std::uint8_t> out(
      static_cast<std::size_t>(handle_size_) * handle_count);
  const VkResult vr = handles_fn(device, pipeline_, 0U, handle_count,
                                 out.size(), out.data());
  if (vr != VK_SUCCESS) {
    return omnicpp::core::Result<std::vector<std::uint8_t>>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  return omnicpp::core::Result<std::vector<std::uint8_t>>::ok(std::move(out));
}

omnicpp::core::Result<void> VulkanRtPipeline::write_sbt(
    VkDevice device, const std::vector<std::uint8_t>& handles,
    Allocation& sbt) {
  if (device == VK_NULL_HANDLE || sbt.mapped == nullptr ||
      handles.empty() || handle_size_ == 0) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }
  const std::uint32_t handle_count = 1U + miss_count_ + hit_group_count_;
  if (handles.size() <
      static_cast<std::size_t>(handle_size_) * handle_count) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }

  const VkDeviceSize rg_stride = raygen_stride_;
  const VkDeviceSize miss_size = rg_stride * miss_count_;
  const VkDeviceSize hit_size = rg_stride * hit_group_count_;
  const VkDeviceSize miss_off = align_up(rg_stride, base_alignment_);
  const VkDeviceSize hit_off = align_up(miss_off + miss_size, base_alignment_);

  auto* base = static_cast<std::uint8_t*>(sbt.mapped);
  std::memcpy(base, handles.data(), static_cast<std::size_t>(handle_size_));
  if (miss_count_ != 0U) {
    std::memcpy(base + miss_off, handles.data() + handle_size_,
                static_cast<std::size_t>(miss_size));
  }
  if (hit_group_count_ != 0U) {
    std::memcpy(base + hit_off,
                handles.data() +
                    static_cast<std::size_t>(handle_size_) * (1U + miss_count_),
                static_cast<std::size_t>(hit_size));
  }

  VkBufferDeviceAddressInfo addr_info{};
  addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  addr_info.buffer = sbt.buffer;
  const std::uint64_t base_addr = vkGetBufferDeviceAddress(device, &addr_info);
  raygen_addr_ = base_addr;
  miss_addr_ = base_addr + static_cast<std::uint64_t>(miss_off);
  hit_addr_ = base_addr + static_cast<std::uint64_t>(hit_off);
  raygen_stride_ = rg_stride;
  miss_stride_ = rg_stride;
  hit_stride_ = rg_stride;
  return omnicpp::core::Result<void>::ok();
}

VkDeviceSize VulkanRtPipeline::required_sbt_bytes(
    VkPhysicalDevice physical_device, std::uint32_t miss_count,
    std::uint32_t hit_group_count) {
  // Mirrors write_sbt's layout, sized from the device's real properties.
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_props{};
  rt_props.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &rt_props;
  vkGetPhysicalDeviceProperties2(physical_device, &props2);
  const VkDeviceSize stride = align_up(
      static_cast<VkDeviceSize>(rt_props.shaderGroupHandleSize),
      static_cast<VkDeviceSize>(rt_props.shaderGroupHandleAlignment));
  const VkDeviceSize base = rt_props.shaderGroupBaseAlignment;
  const VkDeviceSize miss_size = stride * miss_count;
  const VkDeviceSize hit_size = stride * hit_group_count;
  const VkDeviceSize miss_off = align_up(stride, base);
  const VkDeviceSize hit_off = align_up(miss_off + miss_size, base);
  return hit_off + hit_size;
}

void VulkanRtPipeline::trace_rays(VkCommandBuffer cmd, std::uint32_t width,
                                  std::uint32_t height,
                                  std::uint32_t depth) const noexcept {
  if (cmd == VK_NULL_HANDLE || pipeline_ == VK_NULL_HANDLE ||
      device_ == VK_NULL_HANDLE) {
    return;
  }
  auto trace_fn = cmd_trace_rays_fn(device_);
  if (trace_fn != nullptr) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline_);
    VkStridedDeviceAddressRegionKHR gen{raygen_addr_, raygen_stride_,
                                        raygen_stride_};
    VkStridedDeviceAddressRegionKHR miss{miss_addr_, miss_stride_,
                                         miss_stride_ * miss_count_};
    VkStridedDeviceAddressRegionKHR hit{hit_addr_, hit_stride_,
                                        hit_stride_ * hit_group_count_};
    VkStridedDeviceAddressRegionKHR callables{0, 0, 0};
    trace_fn(cmd, &gen, &miss, &hit, &callables, width, height, depth);
  }
}

void VulkanRtPipeline::cleanup(VkDevice device) noexcept {
  if (pipeline_ != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
    vkDestroyPipeline(device, pipeline_, nullptr);
  }
  pipeline_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

#endif  // OMNICPP_HAS_VULKAN

}  // namespace omnicpp::render
