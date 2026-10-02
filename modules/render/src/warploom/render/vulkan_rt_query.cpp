//! @file vulkan_rt_query.cpp
//! @brief Ray-query support queries (see the header).

#include "warploom/render/vulkan_rt_query.hpp"

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

#ifdef OMNICPP_HAS_VULKAN

void VulkanRtQuery::get_properties(VkPhysicalDevice physical_device,
                                   RtSbtProperties& out) noexcept {
  out = RtSbtProperties{};
  if (physical_device == VK_NULL_HANDLE) {
    out.handle_size = 32U;
    out.raygen_stride = 32U;
    return;
  }
  // SBT sizing rules come from the physical device's RT pipeline properties,
  // chained onto vkGetPhysicalDeviceProperties2.
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR props{};
  props.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &props;
  vkGetPhysicalDeviceProperties2(physical_device, &props2);
  out.handle_size = props.shaderGroupHandleSize;
  if (out.handle_size == 0U) {
    // Properties not populated (extension absent): spec-valid minimums.
    out.handle_size = 32U;
    out.raygen_stride = 32U;
    return;
  }
  const std::uint32_t alignment = props.shaderGroupHandleAlignment > 0U
                                      ? props.shaderGroupHandleAlignment
                                      : 1U;
  out.raygen_stride =
      ((out.handle_size + alignment - 1U) / alignment) * alignment;
}

#endif  // OMNICPP_HAS_VULKAN

}  // namespace warploom::render
