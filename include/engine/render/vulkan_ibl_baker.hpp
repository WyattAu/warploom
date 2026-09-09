#pragma once

/**
 * @file vulkan_ibl_baker.hpp
 * @brief GPU-side image-based-lighting bake from an analytic sky.
 *
 * One-shot compute sequence (all inside a single submitted command buffer):
 *   1. ibl_sky.comp        fills an HDR equirectangular sky (sun disc +
 *                          ambient ground colour),
 *   2. ibl_prefilter.comp  convolves it into a mip-chained prefiltered cube
 *                          (one dispatch per roughness mip),
 *   3. ibl_irradiance.comp cosine-convolves a diffuse irradiance cube,
 *   4. ibl_brdf_lut.comp   bakes the split-sum BRDF LUT,
 * then transitions every image to SHADER_READ_ONLY for the fragment stage.
 *
 * `descriptor_set()` feeds VulkanPbrScene::ibl_set (record_pbr_scene binds it
 * at set 3 for the pbr_ibl variants, or set 5 for the composed pbr_full
 * variant — the caller owns slot selection).
 *
 * The bake is a direct port of the proven test harness bake (test_pbr_ibl):
 * identical image formats, mip counts, dispatch sizes, barriers, and push
 * layouts, so its pixel-level guarantees carry over.
 */

#include <array>
#include <cstdint>
#include <string>

#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_pipeline.hpp"

namespace omnicpp::render {

//! Sky description consumed by ibl_sky.comp (push-constant layout match).
struct IblBakeParams {
  std::array<float, 3> sun_direction{0.45f, 0.7f, 0.55f};
  float sun_radius_rad{0.14f};
  std::array<float, 3> sun_color{6.0f, 6.0f, 6.0f};
  std::array<float, 3> sky_color{0.12f, 0.16f, 0.24f};
  float sky_gain{1.0f};
};

class VulkanIblBaker final {
 public:
  VulkanIblBaker() = default;
  ~VulkanIblBaker() = default;

  VulkanIblBaker(const VulkanIblBaker&) = delete;
  VulkanIblBaker& operator=(const VulkanIblBaker&) = delete;

  //! Creates images, views, samplers, bake descriptor layouts/sets, compute
  //! pipelines, and the set-3-style IBL descriptor set. Shader SPIR-V is
  //! loaded from `shader_dir` (ibl_*.comp.spv). Returns false with `error`
  //! filled when anything fails (all or nothing).
  [[nodiscard]] bool initialize(
      VkDevice device, VkQueue graphics_queue, std::uint32_t queue_family,
      VulkanMemoryAllocator& allocator, VulkanDescriptorManager& descriptors,
      const std::string& shader_dir, std::string& error);

  //! Runs the bake for `params` (one command buffer, fence-waited). Safe to
  //! call again to re-bake (sky changes); images return to READ_ONLY after.
  [[nodiscard]] bool bake(const IblBakeParams& params);

  //! Descriptor set (binding 0 prefiltered cube, 1 irradiance cube, 2 BRDF
  //! LUT) for VulkanPbrScene::ibl_set. Valid after initialize(). The layout
  //! declares the bindings at set index 3 (the pbr_ibl*.frag convention);
  //! callers binding the composed pbr_full variant create their own set-5
  //! layout and write these views/samplers into it.
  [[nodiscard]] VkDescriptorSet descriptor_set() const noexcept {
    return ibl_set_;
  }
  [[nodiscard]] VkImageView prefiltered_cube_view() const noexcept {
    return prefiltered_cube_view_;
  }
  [[nodiscard]] VkImageView irradiance_cube_view() const noexcept {
    return irradiance_cube_view_;
  }
  [[nodiscard]] VkImageView brdf_lut_view() const noexcept {
    return lut_view_;
  }
  [[nodiscard]] VkSampler cube_sampler() const noexcept {
    return sampler_cube_;
  }
  [[nodiscard]] VkSampler flat_sampler() const noexcept {
    return sampler_flat_;
  }

  void cleanup(VkDevice device) noexcept;

 private:
  [[nodiscard]] bool create_image(VkImage& image, Allocation& memory,
                                  std::uint32_t width, std::uint32_t height,
                                  std::uint32_t mips, std::uint32_t layers,
                                  VkImageUsageFlags usage,
                                  VkImageCreateFlags flags);
  [[nodiscard]] bool create_view(VkImageView& view, VkImage image,
                                 VkImageViewType type, std::uint32_t base_mip,
                                 std::uint32_t mip_count,
                                 std::uint32_t layer_count);
  void transition(VkCommandBuffer cb, VkImage image, std::uint32_t mips,
                  std::uint32_t layers, VkImageLayout old_layout,
                  VkImageLayout new_layout, VkPipelineStageFlags src_stage,
                  VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                  VkAccessFlags dst_access);

  VkDevice device_{VK_NULL_HANDLE};
  VulkanMemoryAllocator* allocator_{nullptr};
  VulkanDescriptorManager* descriptors_{nullptr};
  VkQueue queue_{VK_NULL_HANDLE};
  std::uint32_t queue_family_{0};

  VkImage sky_image_{VK_NULL_HANDLE};
  Allocation sky_memory_{};
  VkImage prefiltered_image_{VK_NULL_HANDLE};
  Allocation prefiltered_memory_{};
  VkImage irradiance_image_{VK_NULL_HANDLE};
  Allocation irradiance_memory_{};
  VkImage lut_image_{VK_NULL_HANDLE};
  Allocation lut_memory_{};

  VkImageView sky_view_{VK_NULL_HANDLE};
  std::array<VkImageView, 7> prefilter_mip_views_{};
  VkImageView irradiance_mip_view_{VK_NULL_HANDLE};
  VkImageView prefiltered_cube_view_{VK_NULL_HANDLE};
  VkImageView irradiance_cube_view_{VK_NULL_HANDLE};
  VkImageView lut_view_{VK_NULL_HANDLE};

  VkSampler sampler_sky_{VK_NULL_HANDLE};
  VkSampler sampler_cube_{VK_NULL_HANDLE};
  VkSampler sampler_flat_{VK_NULL_HANDLE};

  VkDescriptorSetLayout bake_layouts_[4]{};
  VkDescriptorSet bake_sets_[4]{};   //!< [1] unused (per-mip sets below)
  VkDescriptorSet prefilter_sets_[7]{};
  VkDescriptorSetLayout ibl_layout_{VK_NULL_HANDLE};
  VkDescriptorSet ibl_set_{VK_NULL_HANDLE};

  VulkanPipeline sky_pipeline_;
  VulkanPipeline prefilter_pipeline_;
  VulkanPipeline irradiance_pipeline_;
  VulkanPipeline lut_pipeline_;

  VkCommandPool pool_{VK_NULL_HANDLE};
  VkFence fence_{VK_NULL_HANDLE};
};

}  // namespace omnicpp::render
