#pragma once

/**
 * @file vulkan_rt_query.hpp
 * @brief Ray-query support queries.
 *
 * Ray queries run inside ordinary COMPUTE pipelines and need no SBT and no
 * vkCmdTraceRays: traversal starts from rayQueryInitializeEXT against the
 * acceleration structure bound as a descriptor. The only host-side input is
 * the SBT sizing rules (needed later for RT pipelines / path tracing).
 */

#include "engine/core/deterministic_runtime.hpp"
#include "engine/render/vulkan_types.hpp"

#include <cstdint>

namespace omnicpp::render {

//! SBT sizing rules from the RT pipeline properties (E3 groundwork).
struct RtSbtProperties {
  std::uint32_t handle_size{0};   //!< shaderGroupHandleSize (bytes).
  std::uint32_t raygen_stride{0}; //!< handle_size rounded up to its alignment.
};

class VulkanRtQuery final {
 public:
  VulkanRtQuery() = default;
  ~VulkanRtQuery() = default;
  VulkanRtQuery(const VulkanRtQuery&) = delete;
  VulkanRtQuery& operator=(const VulkanRtQuery&) = delete;

  //! Fills `out` from the physical device's RT pipeline properties. When the
  //! properties are absent, the spec-valid minimum (32/32) is returned so
  //! callers can size scratch SBTs safely.
  static void get_properties(VkPhysicalDevice physical_device,
                             RtSbtProperties& out) noexcept;
};

}  // namespace omnicpp::render
