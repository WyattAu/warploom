//! @file vulkan_sky_pass.cpp
//! @brief One owned raster analytic-sky pass: pipeline + SkyParams UBO.
//! Direct port of the proven harness sequence (test_pt_sky_parity.cpp):
//! same shader stages, same set-0 UBO binding, same depth-test-on /
//! depth-write-off / no-cull pipeline, same 64-byte vertex|fragment push.

#include "warploom/render/vulkan_sky_pass.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

namespace {
#ifdef OMNICPP_HAS_VULKAN
static_assert(sizeof(SkyParams) == 64U,
              "SkyParams must be exactly the 4-vec4 SkyParams UBO");
static_assert(offsetof(SkyParams, sun_direction) == 0U, "UBO word order");
static_assert(offsetof(SkyParams, rayleigh) == 16U, "UBO word order");
static_assert(offsetof(SkyParams, mie) == 32U, "UBO word order");
static_assert(offsetof(SkyParams, misc) == 48U, "UBO word order");
#endif
}  // namespace

::warploom::core::Result<void> VulkanSkyPass::initialize(
    VkDevice device, VulkanMemoryAllocator& allocator,
    VulkanDescriptorManager& descriptors, VkRenderPass render_pass,
    VkFormat color_format, const std::string& shader_dir,
    const SkyParams& params, std::string& error) {
#ifdef OMNICPP_HAS_VULKAN
  if (device == VK_NULL_HANDLE || render_pass == VK_NULL_HANDLE ||
      color_format == VK_FORMAT_UNDEFINED) {
    error = "sky pass: null device, render pass, or color format";
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  allocator_ = &allocator;

  const std::vector<ReflectedBinding> sky_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto layout_result = descriptors.create_layout(sky_bindings, 4U);
  if (!layout_result.is_ok()) {
    error = "sky pass: descriptor layout failed";
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  layout_ = layout_result.value();
  auto set_result = descriptors.allocate_set(layout_);
  if (!set_result.is_ok()) {
    error = "sky pass: descriptor set failed";
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  set_ = set_result.value();
  auto ubo_result = allocator.create_buffer(
      sizeof(SkyParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!ubo_result.is_ok()) {
    error = "sky pass: UBO allocation failed";
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  ubo_ = ubo_result.value();
  if (!descriptors
           .write_buffer(set_, 0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         ubo_.buffer, 0U, VK_WHOLE_SIZE)
           .is_ok()) {
    error = "sky pass: descriptor write failed";
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  set_params(params);

  if (!pipeline_
           .load_shader_stage_file(device, shader_dir + "/sky.vert.spv",
                                   "vertex")
           .is_ok() ||
      !pipeline_
           .load_shader_stage_file(device, shader_dir + "/sky.frag.spv",
                                   "fragment")
           .is_ok()) {
    error = "sky pass: shader load failed (dir: " + shader_dir + ")";
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  const std::vector<VkDescriptorSetLayout> sky_layouts = {layout_};
  const VkPushConstantRange sky_push{VK_SHADER_STAGE_VERTEX_BIT |
                                         VK_SHADER_STAGE_FRAGMENT_BIT,
                                     0U, 64U};
  if (!pipeline_
           .create_pipeline_layout(device, sky_layouts.data(), 1U, &sky_push)
           .is_ok()) {
    error = "sky pass: pipeline layout failed";
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  // Depth test on, depth write off: the pre-draw shades every pixel at the
  // far plane and geometry overdraws it. No culling (one winding-independent
  // full-screen triangle).
  if (!pipeline_
           .create_graphics_pipeline(device, render_pass, color_format,
                                     pipeline_.pipeline_layout(), true, false,
                                     false)
           .is_ok()) {
    error = "sky pass: pipeline creation failed";
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)device;
  (void)allocator;
  (void)descriptors;
  (void)render_pass;
  (void)color_format;
  (void)shader_dir;
  (void)params;
  error = "sky pass: Vulkan unavailable";
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanSkyPass::set_params(const SkyParams& params) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (ubo_.mapped != nullptr) {
    std::memcpy(ubo_.mapped, &params, sizeof(params));
  }
#else
  (void)params;
#endif
}

void VulkanSkyPass::set_view(const std::array<float, 3>& forward,
                             const std::array<float, 3>& right,
                             const std::array<float, 3>& up, float tan_half_fov,
                             float aspect) noexcept {
  view_.camera_position = {kSkySurfaceAnchor[0], kSkySurfaceAnchor[1],
                           kSkySurfaceAnchor[2], tan_half_fov};
  view_.forward = {forward[0], forward[1], forward[2], aspect};
  view_.right = {right[0], right[1], right[2], 0.0f};
  view_.up = {up[0], up[1], up[2], 0.0f};
}

void VulkanSkyPass::apply_to(VulkanPbrScene& scene) const noexcept {
  if (!ready()) return;
  scene.sky_pipeline = pipeline_.pipeline();
  scene.sky_pipeline_layout = pipeline_.pipeline_layout();
  scene.sky_set = set_;
  scene.sky_view = view_;
}

void VulkanSkyPass::cleanup(VkDevice device) noexcept {
  pipeline_.cleanup(device);
  if (allocator_ != nullptr && ubo_.is_valid()) {
    allocator_->destroy_allocation(ubo_);
  }
  allocator_ = nullptr;
  layout_ = VK_NULL_HANDLE;  // owned by the descriptor manager's cleanup
  set_ = VK_NULL_HANDLE;
  ubo_ = Allocation{};
}

}  // namespace warploom::render
