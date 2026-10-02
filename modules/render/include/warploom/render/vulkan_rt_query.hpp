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

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"

#include <cstdint>

namespace warploom::render {

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

}  // namespace warploom::render

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef WARPLOOM_COMPAT_RENDER_NS
#define WARPLOOM_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // WARPLOOM_COMPAT_RENDER_NS
