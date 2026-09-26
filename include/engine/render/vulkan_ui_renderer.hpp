#pragma once

//! @file vulkan_ui_renderer.hpp
//! @brief M4 UI rendering: draws the M2 paint list as GPU quads.
//!
//! One pipeline, one vertex buffer, one draw call per frame:
//!   - `upload_paint_list` converts PaintList rects + text runs into a quad
//!     vertex stream (pixel-space positions, atlas UVs, packed colors) and
//!     writes it into a host-visible buffer;
//!   - the glyph atlas is a texture built once by `ui::build_atlas` (the
//!     solid cell at atlas origin powers solid rects);
//!   - the fragment shader multiplies the vertex color by the atlas alpha,
//!     so text runs render as glyphs and rects render as solid fills.
//!
//! The renderer owns its modules/pipeline/atlas/buffers and releases them
//! in cleanup(). Recording is caller-driven (tests/viewport own submission):
//! `record()` binds everything and draws into the caller's active render
//! pass. SPIR-V is loaded directly from `shader_dir` (ui_quad.vert/frag).

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/deterministic_runtime.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "warploom/ui/widget.hpp"

namespace omnicpp::render {
namespace ui = ::warploom::ui;  // S1

class VulkanUiRenderer final {
 public:
  VulkanUiRenderer() = default;
  ~VulkanUiRenderer();
  VulkanUiRenderer(const VulkanUiRenderer&) = delete;
  VulkanUiRenderer& operator=(const VulkanUiRenderer&) = delete;

  //! Creates the pipeline (pixel-space quads, no depth, no cull), layout
  //! (atlas sampler + resolution push constants), and the glyph atlas
  //! texture. `render_pass` must be compatible with the UI pass format.
  [[nodiscard]] omnicpp::core::Result<void> initialize(
      VkDevice device, VkPhysicalDevice physical_device,
      VkRenderPass render_pass, VulkanMemoryAllocator& allocator,
      const std::string& shader_dir);

  //! Converts the paint list to quad vertices and uploads them. Returns the
  //! recorded quad count. Call every frame before record().
  [[nodiscard]] omnicpp::core::Result<std::uint32_t> upload_paint_list(
      const warploom::ui::PaintList& paint, float viewport_w, float viewport_h);

  //! Records the one-time atlas layout barrier (host-written -> shader
  //! read). MUST be called on `cmd` BEFORE the render pass begins — layout
  //! transitions are illegal inside a render pass.
  void ensure_layout(VkCommandBuffer cmd);

  //! Records the UI draw (bind pipeline + atlas + draw) into `cmd` inside
  //! an active render pass.
  void record(VkCommandBuffer cmd, std::uint32_t width, std::uint32_t height,
              std::uint32_t quad_count);

  void cleanup(VkDevice device) noexcept;

  [[nodiscard]] bool is_initialized() const noexcept {
    return pipeline_ != VK_NULL_HANDLE;
  }
  [[nodiscard]] VkPipeline pipeline() const noexcept { return pipeline_; }
  //! Glyph-atlas dimensions (for tests asserting atlas contents).
  [[nodiscard]] std::uint32_t atlas_width() const noexcept {
    return atlas_w_;
  }
  [[nodiscard]] std::uint32_t atlas_height() const noexcept {
    return atlas_h_;
  }

 //! Quad vertex: pixel-space position, atlas UV, packed color (shader
  //! expands 0xAABBGGRR). Public so the TU's helpers can build streams.
 public:
  struct UiVertex {
    float pos[2];
    float uv[2];
    std::uint32_t color;
  };

 private:
  [[nodiscard]] VkShaderModule load_module(VkDevice device,
                                           const std::string& path);

  VkDevice device_{VK_NULL_HANDLE};
  VulkanMemoryAllocator* allocator_{nullptr};
  VkShaderModule vert_module_{VK_NULL_HANDLE};
  VkShaderModule frag_module_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  VkPipelineLayout layout_{VK_NULL_HANDLE};
  VkDescriptorPool pool_{VK_NULL_HANDLE};
  VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
  VkDescriptorSet atlas_set_{VK_NULL_HANDLE};
  VkImage atlas_image_{VK_NULL_HANDLE};
  VkImageView atlas_view_{VK_NULL_HANDLE};
  VkDeviceMemory atlas_memory_{VK_NULL_HANDLE};
  VkSampler sampler_{VK_NULL_HANDLE};
  bool atlas_layout_ready_{false};  //!< one-time UNDEFINED -> SHADER_READ barrier
  std::uint32_t atlas_w_{0};
  std::uint32_t atlas_h_{0};
  Allocation quad_allocation_{};
  std::vector<UiVertex> vertices_{};
};

}  // namespace omnicpp::render
