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

namespace warploom::render {

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

  [[nodiscard]] ::warploom::core::Result<void> load_shader_file(
      VkDevice device, const std::string& path);
  [[nodiscard]] ::warploom::core::Result<void> load_shader(
      VkDevice device, const std::uint32_t* code, std::size_t code_size_bytes);
  [[nodiscard]] ::warploom::core::Result<void> load_shader_from_bytes(
      VkDevice device, const std::vector<std::uint8_t>& spirv_bytes);
  //! Explicitly assign a SPIR-V module to a named pipeline stage.
  [[nodiscard]] ::warploom::core::Result<void> load_shader_stage_file(
      VkDevice device, const std::string& path, const std::string& stage);
  [[nodiscard]] bool has_stage(const std::string& stage) const noexcept;
  [[nodiscard]] bool all_core_stages_loaded() const noexcept {
    return vertex_shader_ != VK_NULL_HANDLE && fragment_shader_ != VK_NULL_HANDLE;
  }

  [[nodiscard]] ::warploom::core::Result<void> create_graphics_pipeline(
      VkDevice device, VkRenderPass render_pass,
      VkFormat vertex_format,
      VkPipelineLayout pipeline_layout = VK_NULL_HANDLE,
      bool enable_depth_test = true,
      bool enable_depth_write = true,
      bool enable_backface_cull = true,
      //! Static rasterizer depth-bias slope factor (shadow-map acne relief),
      //! baked into VkPipelineRasterizationStateCreateInfo::depthBiasSlopeFactor.
      float depth_bias_slope = 0.0f,
  //! Declare VK_DYNAMIC_STATE_DEPTH_BIAS on this pipeline. Set this
  //! whenever the recorder will call vkCmdSetDepthBias: without the
  //! dynamic state registered, that call is both a validation error
  //! (VUID-vkCmdDrawIndexed-None-08608) and a silent no-op, so the bias
  //! it sets never reaches the rasterizer. It is deliberately separate
  //! from depth_bias_slope above -- a pipeline can carry a static bias,
  //! a dynamic bias, or both.
  bool dynamic_depth_bias = false);

  //! Create a pipeline for dynamic rendering (Blade): no render pass,
  //! formats declared inline via VkPipelineRenderingCreateInfo. Stages
  //! 1-2 (bloom, tonemap) use the color-only default: no depth attachment,
  //! depth test/write off. Stage 3 (the scene) passes its depth format
  //! with test/write on, mirroring the baked scene pipeline's depth state
  //! so the same draw shades identically under either framing.
  [[nodiscard]] ::warploom::core::Result<void> create_graphics_pipeline_dynamic(
      VkDevice device, VkFormat color_format,
      VkPipelineLayout pipeline_layout = VK_NULL_HANDLE,
      bool enable_backface_cull = true,
      VkFormat depth_format = VK_FORMAT_UNDEFINED,
      bool enable_depth_test = false,
      bool enable_depth_write = false);

  //! Create a compute pipeline from the "compute" stage module (load with
  //! load_shader_stage_file(device, path, "compute") first). Requires a
  //! caller-created layout (create_pipeline_layout).
  [[nodiscard]] ::warploom::core::Result<void> create_compute_pipeline(
      VkDevice device, VkPipelineLayout pipeline_layout = VK_NULL_HANDLE);

  [[nodiscard]] ::warploom::core::Result<void> create_pipeline_layout(
      VkDevice device, const void* push_constant_range = nullptr);
  //! Layout with descriptor set layouts (type-safe overload).
  [[nodiscard]] ::warploom::core::Result<void> create_pipeline_layout(
      VkDevice device, const VkDescriptorSetLayout* set_layouts,
      std::uint32_t set_layout_count, const void* push_constant_range = nullptr);

  void cleanup(VkDevice device = VK_NULL_HANDLE) noexcept;

  [[nodiscard]] VkPipeline pipeline() const noexcept { return pipeline_; }
  [[nodiscard]] VkPipelineLayout pipeline_layout() const noexcept { return layout_; }

private:
  //! Shared core behind create_graphics_pipeline (baked render pass) and
  //! create_graphics_pipeline_dynamic (VkPipelineRenderingCreateInfo).
  //! Exactly one of `render_pass`, `rendering_create_info` must be set;
  //! the struct is `void*` so the header compiles without Vulkan.
  [[nodiscard]] ::warploom::core::Result<void> create_graphics_pipeline_impl(
      VkDevice device, VkRenderPass render_pass,
      const void* rendering_create_info, VkFormat vertex_format,
      VkPipelineLayout pipeline_layout, bool enable_depth_test,
      bool enable_depth_write, bool enable_backface_cull,
      float depth_bias_slope, bool dynamic_depth_bias);

  //! Device that owns every handle below, recorded so cleanup() works from
  //! the destructor. Previously the destructor passed VK_NULL_HANDLE, which
  //! cleanup() ignores, so every pipeline leaked its VkPipeline, layout and
  //! shader modules.
  VkDevice device_{VK_NULL_HANDLE};
  VkShaderModule vertex_shader_{VK_NULL_HANDLE};
  VkShaderModule fragment_shader_{VK_NULL_HANDLE};
  VkShaderModule compute_shader_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  VkPipelineLayout layout_{VK_NULL_HANDLE};
  bool owns_layout_{false};
};

} // namespace warploom::render