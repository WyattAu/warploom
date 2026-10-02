#pragma once

/**
 * @file vulkan_descriptors.hpp
 * @brief Minimal SPIR-V reflection and descriptor set management.
 *
 * The reflector parses SPIR-V directly (no external SDK dependency) and
 * extracts the subset needed to build descriptor set layouts: binding
 * number, set, resource kind, count, and stage. The manager then creates
 * layouts, a sized pool, and performs write-once descriptor updates.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>
#include <vector>

namespace warploom::render {

//! Reflected shader resource binding.
struct ReflectedBinding {
  std::uint32_t set{0};
  std::uint32_t binding{0};
  std::uint32_t count{1};       //!< Array dimension (1 for non-arrays).
  VkDescriptorType type{VK_DESCRIPTOR_TYPE_MAX_ENUM};   //!< Kind (0 when Vulkan-off).
  VkShaderStageFlags stage_flags{0};                    //!< Stage bit (0 when Vulkan-off).
};

//! Parse SPIR-V for uniform/storage resources (UBOs, SSBOs, sampled images,
//! storage images, samplers, combined image samplers, and input attachments).
[[nodiscard]] std::vector<ReflectedBinding> reflect_spirv_resources(
    const std::uint32_t* code, std::size_t code_word_count);

//! Convenience overload for file bytes.
[[nodiscard]] std::vector<ReflectedBinding> reflect_spirv_resources(
    const std::uint8_t* code, std::size_t code_byte_count);

//! Descriptor layout handle plus its reflected bindings.
struct DescriptorSetLayoutInfo {
  VkDescriptorSetLayout layout{VK_NULL_HANDLE};
  std::vector<ReflectedBinding> bindings;
  //! Pool backing this layout (one pool per layout).
  VkDescriptorPool pool{VK_NULL_HANDLE};
  //! True when created via `create_layout(..., /*bindless=*/true)`.
  bool bindless{false};
};

/**
 * @brief Creates descriptor layouts from reflected bindings, allocates sets
 * from an auto-sized pool, and applies buffer/image writes.
 */
class VulkanDescriptorManager final {
public:
  VulkanDescriptorManager() = default;
  ~VulkanDescriptorManager();
  VulkanDescriptorManager(const VulkanDescriptorManager&) = delete;
  VulkanDescriptorManager& operator=(const VulkanDescriptorManager&) = delete;
  VulkanDescriptorManager(VulkanDescriptorManager&&) = delete;
  VulkanDescriptorManager& operator=(VulkanDescriptorManager&&) = delete;

  [[nodiscard]] ::warploom::core::Result<void> initialize(VkDevice device);
  void cleanup() noexcept;
  [[nodiscard]] bool is_initialized() const noexcept { return device_ != VK_NULL_HANDLE; }

  //! Build a layout from bindings; sets_to_reserve reserves pool capacity.
  //! With `bindless` (requires device descriptor-indexing support), the layout
  //! is created partially bound with update-after-bind: descriptors may be
  //! bound as null and written any time up to draw/dispatch submission.
  [[nodiscard]] ::warploom::core::Result<VkDescriptorSetLayout> create_layout(
      const std::vector<ReflectedBinding>& bindings, std::uint32_t sets_to_reserve,
      bool bindless = false);

  //! Allocate a descriptor set from the internal pool for `layout`.
  [[nodiscard]] ::warploom::core::Result<VkDescriptorSet> allocate_set(
      VkDescriptorSetLayout layout);

  //! Write one buffer range into `binding` of `set`.
  [[nodiscard]] ::warploom::core::Result<void> write_buffer(
      VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
      VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);

  //! Write one image (sampler + view) into `binding` of `set`.
  [[nodiscard]] ::warploom::core::Result<void> write_image(
      VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
      VkSampler sampler, VkImageView view, VkImageLayout layout,
      std::uint32_t array_element = 0);

  //! Write one acceleration structure into `binding` of `set` (type must be
  //! VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR). Required for ray-query
  //! shaders that traverse a TLAS from graphics or compute stages.
  [[nodiscard]] ::warploom::core::Result<void> write_acceleration_structure(
      VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
      VkAccelerationStructureKHR as, std::uint32_t array_element = 0);

  [[nodiscard]] const std::vector<DescriptorSetLayoutInfo>& layouts() const noexcept {
    return layouts_;
  }

  //! Look up the recorded info for a layout created by this manager.
  [[nodiscard]] const DescriptorSetLayoutInfo* find_layout(
      VkDescriptorSetLayout layout) const noexcept;

private:
  VkDevice device_{VK_NULL_HANDLE};
  std::vector<DescriptorSetLayoutInfo> layouts_;
};

} // namespace warploom::render

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef OMNICPP_COMPAT_RENDER_NS
#define OMNICPP_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // OMNICPP_COMPAT_RENDER_NS
