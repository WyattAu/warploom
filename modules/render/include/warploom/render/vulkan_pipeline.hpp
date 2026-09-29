#pragma once

/**
 * @file vulkan_pipeline.hpp
 * @brief Vulkan graphics pipeline and shader module management.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace omnicpp::render {

struct VertexDescription {
  struct Attribute {
    std::uint32_t location{0};
    VkFormat format{VK_FORMAT_UNDEFINED};
    std::uint32_t offset{0};
  };
  std::uint32_t stride{0};
  std::vector<Attribute> attributes;
  [[nodiscard]] std::uint32_t attribute_count() const noexcept {
    return static_cast<std::uint32_t>(attributes.size());
  }
};

class VulkanPipeline final {
public:
  VulkanPipeline() = default;
  ~VulkanPipeline();

  VulkanPipeline(const VulkanPipeline&) = delete;
  VulkanPipeline& operator=(const VulkanPipeline&) = delete;
  VulkanPipeline(VulkanPipeline&&) = delete;
  VulkanPipeline& operator=(VulkanPipeline&&) = delete;

  [[nodiscard]] omnicpp::core::Result<void> load_shader_file(
      VkDevice device, const std::string& path);
  [[nodiscard]] omnicpp::core::Result<void> load_shader(
      VkDevice device, const std::uint32_t* code, std::size_t code_size_bytes);
  [[nodiscard]] omnicpp::core::Result<void> load_shader_from_bytes(
      VkDevice device, const std::vector<std::uint8_t>& spirv_bytes);
  //! Explicitly assign a SPIR-V module to a named pipeline stage.
  [[nodiscard]] omnicpp::core::Result<void> load_shader_stage_file(
      VkDevice device, const std::string& path, const std::string& stage);
  [[nodiscard]] bool has_stage(const std::string& stage) const noexcept;
  [[nodiscard]] bool all_core_stages_loaded() const noexcept {
    return vertex_shader_ != VK_NULL_HANDLE && fragment_shader_ != VK_NULL_HANDLE;
  }

  [[nodiscard]] omnicpp::core::Result<void> create_graphics_pipeline(
      VkDevice device, VkRenderPass render_pass,
      VkFormat vertex_format,
      VkPipelineLayout pipeline_layout = VK_NULL_HANDLE,
      bool enable_depth_test = true,
      bool enable_depth_write = true,
      bool enable_backface_cull = true,
      //! Depth-bias slope factor (shadow-map acne relief). When nonzero the
      //! pipeline enables depth bias and exposes it as a DYNAMIC state so the
      //! recorder sets per-draw values with vkCmdSetDepthBias; a zero factor
      //! leaves the pipeline bias-free and the dynamic state unregistered.
      float depth_bias_slope = 0.0f);

  //! Create a compute pipeline from the "compute" stage module (load with
  //! load_shader_stage_file(device, path, "compute") first). Requires a
  //! caller-created layout (create_pipeline_layout).
  [[nodiscard]] omnicpp::core::Result<void> create_compute_pipeline(
      VkDevice device, VkPipelineLayout pipeline_layout = VK_NULL_HANDLE);

  [[nodiscard]] omnicpp::core::Result<void> create_pipeline_layout(
      VkDevice device, const void* push_constant_range = nullptr);
  //! Layout with descriptor set layouts (type-safe overload).
  [[nodiscard]] omnicpp::core::Result<void> create_pipeline_layout(
      VkDevice device, const VkDescriptorSetLayout* set_layouts,
      std::uint32_t set_layout_count, const void* push_constant_range = nullptr);

  void cleanup(VkDevice device) noexcept;

  [[nodiscard]] VkPipeline pipeline() const noexcept { return pipeline_; }
  [[nodiscard]] VkPipelineLayout pipeline_layout() const noexcept { return layout_; }

private:
  VkShaderModule vertex_shader_{VK_NULL_HANDLE};
  VkShaderModule fragment_shader_{VK_NULL_HANDLE};
  VkShaderModule compute_shader_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  VkPipelineLayout layout_{VK_NULL_HANDLE};
  bool owns_layout_{false};
};

} // namespace omnicpp::render
