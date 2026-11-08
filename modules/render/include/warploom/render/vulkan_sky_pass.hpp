#pragma once

/**
 * @file vulkan_sky_pass.hpp
 * @brief One owned raster analytic-sky pass: pipeline + SkyParams UBO.
 *
 * record_pbr_scene() already draws the sky when VulkanPbrScene::sky_pipeline
 * is set, but every consumer built that pipeline and its params by hand --
 * three copies across the test tree, each with its own SkyParams mirror.
 * This class owns the one construction: sky.vert/sky.frag against a
 * caller-provided render pass, one fragment-stage UBO (std140, 4 vec4,
 * matching sky.frag's SkyParams block), and the per-frame camera view.
 * apply_to() points the scene at all four fields, which record_sky_pre_draw
 * consumes.
 *
 * Convention (same as PathTracerSky): the origin is the surface anchor,
 * because the sky model is a planetary shell and a unit-scale scene origin
 * sits inside the planet. The sky is a direction-only backdrop: the
 * camera basis is the live view's, the origin is fixed.
 */

#include <array>
#include <cstdint>
#include <string>

#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_scene.hpp"

namespace warploom::render {

//! Host-side mirror of sky.frag's `uniform SkyParams` (std140, 4 x vec4).
//! Field order also mirrors PathTracerSky so one fill could serve both.
struct SkyParams {
  std::array<float, 4> sun_direction{{0.0f, 0.7f, 0.7f, 0.0f}};
  std::array<float, 4> rayleigh{{5.8e-6f, 13.5e-6f, 33.1e-6f, 0.0f}};
  std::array<float, 4> mie{{21e-6f, 0.76f, 0.0f, 0.0f}};
  //! x = radiance scale applied in the shader, before the HDR target's own
  //! exposure. 1.0 keeps model parity with the path tracer miss branch
  //! (PathTracerSky::misc); below that is a look choice both paths share.
  std::array<float, 4> misc{{1.0f, 0.0f, 0.0f, 0.0f}};
};

//! The origin sky_eval.glsl demands: on the shell [EARTH_RADIUS, ATMO_RADIUS].
//! Kept beside SkyParams rather than in the convention comment because every
//! caller must pass it and "the docs say so" is how the black-sky mistake
//! happened before (a unit-scale eye extinguishes the whole shell).
inline constexpr std::array<float, 3> kSkySurfaceAnchor{{0.0f, 6360e3f + 100.0f,
                                                         0.0f}};

class VulkanSkyPass final {
 public:
  VulkanSkyPass() = default;
  ~VulkanSkyPass() = default;

  VulkanSkyPass(const VulkanSkyPass&) = delete;
  VulkanSkyPass& operator=(const VulkanSkyPass&) = delete;
  VulkanSkyPass(VulkanSkyPass&&) = delete;
  VulkanSkyPass& operator=(VulkanSkyPass&&) = delete;

  //! Builds the sky pipeline for `render_pass`/`color_format` (normally the
  //! app's scene_render_pass()/scene_color_format(), i.e. the HDR compose
  //! target when composing, the swapchain pass otherwise), one UBO, and its
  //! descriptor set. Writes `params` into the UBO. Either everything
  //! succeeds (ready() true) or nothing is kept, with `error` filled.
  [[nodiscard]] ::warploom::core::Result<void> initialize(
      VkDevice device, VulkanMemoryAllocator& allocator,
      VulkanDescriptorManager& descriptors, VkRenderPass render_pass,
      VkFormat color_format, const std::string& shader_dir,
      const SkyParams& params, std::string& error);

  [[nodiscard]] bool ready() const noexcept {
    return pipeline_.pipeline() != VK_NULL_HANDLE && set_ != VK_NULL_HANDLE;
  }

  //! Replaces the UBO contents (sun/exposure changes). Host-visible and
  //! coherent, so the next recorded frame sees it; never a clock.
  void set_params(const SkyParams& params) noexcept;

  //! Records the view the next record takes: orthonormal camera basis plus
  //! the raster field-of-view terms sky.vert needs (tan(fov_y / 2), aspect).
  //! The origin stays kSkySurfaceAnchor; only the basis comes from the app.
  void set_view(const std::array<float, 3>& forward,
                const std::array<float, 3>& right,
                const std::array<float, 3>& up, float tan_half_fov,
                float aspect) noexcept;

  //! Points `scene` at this pass (pipeline, layout, set, view) for
  //! record_pbr_scene's sky pre-draw. No-op unless ready().
  void apply_to(VulkanPbrScene& scene) const noexcept;

  //! Destroys the pipeline and the UBO (device must be idle). The descriptor
  //! layout and set die with the caller's VulkanDescriptorManager.
  //! Mapped UBO contents (host-visible allocation), or nullptr before
  //! initialize. Exposed so tests can prove set_params() lands where the
  //! shader reads; the renderer path never touches it.
  [[nodiscard]] const SkyParams* mapped_params() const noexcept {
    return static_cast<const SkyParams*>(ubo_.mapped);
  }

  void cleanup(VkDevice device) noexcept;

 private:
  VulkanPipeline pipeline_;
  VulkanMemoryAllocator* allocator_{nullptr};
  VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
  VkDescriptorSet set_{VK_NULL_HANDLE};
  Allocation ubo_{};
  VulkanPbrScene::SkyView view_{};
};

}  // namespace warploom::render