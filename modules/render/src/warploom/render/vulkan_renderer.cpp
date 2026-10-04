/**
 * @file vulkan_renderer.cpp
 * @brief Vulkan renderer implementation: command buffers, frame sync, draw loop.
 */

#include "warploom/render/vulkan_renderer.hpp"
#include "warploom/core/clock.hpp"
#include <algorithm>
#include <cstring>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

void FrameResources::cleanup(VkDevice device) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device) {
    if (in_flight_fence) vkDestroyFence(device, in_flight_fence, nullptr);
    if (image_available_semaphore) vkDestroySemaphore(device, image_available_semaphore, nullptr);
    if (render_finished_semaphore) vkDestroySemaphore(device, render_finished_semaphore, nullptr);
  }
  command_buffer = nullptr;
  in_flight_fence = nullptr;
  image_available_semaphore = nullptr;
  render_finished_semaphore = nullptr;
  frame_in_flight = false;
#else
  (void)device;
#endif
}

VulkanRenderer::~VulkanRenderer() { cleanup(nullptr); }

::warploom::core::Result<void> VulkanRenderer::initialize(
    VulkanContext& context,
    const VulkanSwapchain& swapchain,
    const VulkanRenderPass& render_pass,
    const RendererConfig& config) {
#ifdef OMNICPP_HAS_VULKAN
  if (!context.is_initialized() || !context.device()) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  device_ = context.device();
  physical_device_ = context.physical_device();
  graphics_queue_ = context.graphics_queue();
  present_queue_ = context.present_queue();
  render_pass_ = render_pass.render_pass();
  render_pass_resource_ = &render_pass;
  swapchain_ = &swapchain;
  config_ = config;
  // Captured for the HDR compose chain: its tonemap pipeline must be built
  // for the swapchain's render pass and format, which is a different
  // attachment format from the HDR intermediate.
  present_pass_ = render_pass.render_pass();
  present_format_ = swapchain.image_format();

  auto pool_result = create_command_pool(
      device_, static_cast<std::uint32_t>(context.queue_families().graphics_family));
  if (!pool_result.is_ok()) {
    return ::warploom::core::Result<void>::error(pool_result.error());
  }
  command_pool_ = pool_result.value();

  if (config.max_frames_in_flight == 0 || swapchain.image_count() == 0) {
    cleanup(device_);
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }

  frames_.resize(config.max_frames_in_flight);
  images_in_flight_.assign(swapchain.image_count(), VK_NULL_HANDLE);
  render_finished_semaphores_.assign(swapchain.image_count(), VK_NULL_HANDLE);
  // GPU timestamp queries: 2 per frame slot (frame start, main-pass end).
  // Availability is a queue-family property; the pool is created regardless
  // so record paths can be unconditional, and results are only resolved
  // when the device reports support.
  gpu_timing_enabled_ = config.enable_gpu_timing;
  timestamp_valid_.assign(frames_.size(), false);
  if (gpu_timing_enabled_) {
    VkQueryPoolCreateInfo qp_info{};
    qp_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qp_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qp_info.queryCount = static_cast<std::uint32_t>(frames_.size()) * 2U;
    VkQueueFamilyProperties queue_props{};
    std::uint32_t qcount = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qcount, nullptr);
    if (qcount > static_cast<std::uint32_t>(
                     context.queue_families().graphics_family)) {
      std::vector<VkQueueFamilyProperties> props(qcount);
      vkGetPhysicalDeviceQueueFamilyProperties(
          physical_device_, &qcount, props.data());
      queue_props = props[static_cast<std::size_t>(
          context.queue_families().graphics_family)];
    }
    const bool timestamp_supported =
        qcount > 0U &&
        queue_props.timestampValidBits > 0U &&
        (queue_props.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U;
    gpu_timing_.available = timestamp_supported;
    gpu_timing_.timestamp_period_ns =
        context.device_properties().timestamp_period_ns;
    timestamp_period_ns_ = gpu_timing_.timestamp_period_ns;
    if (vkCreateQueryPool(device_, &qp_info, nullptr, &timestamp_pool_) !=
        VK_SUCCESS) {
      cleanup(device_);
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::vulkan_not_available);
    }
  }
  // Timeline pacing decision up front: it changes which resources are created.
  timeline_pacing_ = timeline_pacing_requested_ && context.has_timeline_semaphores();
  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    auto cb_result = allocate_command_buffer(device_, command_pool_);
    if (!cb_result.is_ok()) {
      cleanup(device_);
      return ::warploom::core::Result<void>::error(cb_result.error());
    }
    frames_[i].command_buffer = cb_result.value();

    if (!timeline_pacing_) {
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      if (vkCreateFence(device_, &fence_info, nullptr, &frames_[i].in_flight_fence) != VK_SUCCESS) {
        cleanup(device_);
        return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
      }
    }
    // In timeline mode the per-frame fence is omitted entirely: the timeline
    // semaphore paces CPU-GPU, and a never-reset fence would be submitted in
    // the SIGNALED state (VUID-vkQueueSubmit-fence-00063).

    if (vkCreateSemaphore(device_, &sem_info, nullptr, &frames_[i].image_available_semaphore) != VK_SUCCESS) {
      cleanup(device_);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
  }
  for (auto& semaphore : render_finished_semaphores_) {
    const VkResult result = vkCreateSemaphore(device_, &sem_info, nullptr, &semaphore);
    if (result != VK_SUCCESS) {
      cleanup(device_);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
  }

  // Timeline semaphore for frame pacing (decision made before resource creation).
  if (timeline_pacing_) {
    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 0;
    VkSemaphoreCreateInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timeline_info.pNext = &type_info;
    if (vkCreateSemaphore(device_, &timeline_info, nullptr, &timeline_semaphore_) != VK_SUCCESS) {
      cleanup(device_);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
    image_last_frame_.assign(swapchain.image_count(), 0);
  }

  if (config_.enable_hiz &&
      (!render_pass_resource_ || !render_pass_resource_->depth_image() ||
       !render_pass_resource_->depth_is_sampleable())) {
    cleanup(device_);
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  initialized_ = true;
  if (config_.enable_hiz) {
    auto hiz_result = recreate_hiz_resources(swapchain.extent_width(),
                                              swapchain.extent_height());
    if (!hiz_result.is_ok()) {
      cleanup(device_);
      return hiz_result;
    }
  }
  // Compose eagerly, at the swapchain extent, rather than on the first
  // record_commands(): an application builds its scene pipelines against the
  // HDR intermediate (see hdr_render_pass()), so the target has to exist
  // before the caller gets control back from initialize().
  if (config_.enable_hdr_compose) {
    (void)ensure_compose_resources(swapchain.extent_width(),
                                   swapchain.extent_height());
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)context; (void)swapchain; (void)render_pass; (void)config;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::record_scene(
    VkCommandBuffer command_buffer, const VulkanScene& scene,
    std::uint32_t width, std::uint32_t height) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || !scene.pipeline || !scene.pipeline_layout ||
      width == 0U || height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  VkViewport viewport{};
  viewport.width = static_cast<float>(width);
  viewport.height = static_cast<float>(height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = {width, height};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    scene.pipeline);

  // 160-byte material ABI: view_projection (64) + model (64) + base_color
  // (16) + albedo bindless index (4) + padding (12), matching the push
  // blocks in indexed_scene_material.{vert,frag}.
  struct PushConstants {
    SceneMatrix view_projection;
    SceneMatrix model;
    std::array<float, 4> base_color;
    std::uint32_t albedo_index{0};
    std::uint32_t pad[3]{0, 0, 0};
  } push{};
  push.view_projection = scene.camera.view_projection;

  // Bind the bindless albedo-texture array once when the caller provided it
  // (set 1). The material fragment samples albedos[albedo_index]; element 0
  // is the opaque-white fallback for untextured materials.
  if (scene.material_push_constants && scene.texture_set != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scene.pipeline_layout, 1, 1, &scene.texture_set, 0,
                            nullptr);
  }

  for (const SceneObject& object : scene.objects) {
    const SceneMesh* mesh_ptr = object.effective_mesh();
    if (mesh_ptr == nullptr || !mesh_ptr->is_drawable()) continue;
    const SceneMesh& mesh = *mesh_ptr;
    push.model = object.model;
    push.base_color = object.has_material
        ? object.material_value.base_color
        : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
    push.albedo_index = object.has_albedo ? object.albedo_value.bindless_index : 0U;
    const std::uint32_t push_size = scene.material_push_constants
        ? static_cast<std::uint32_t>(sizeof(push))
        : static_cast<std::uint32_t>(sizeof(SceneMatrix) * 2U);
    // The material fragment stage reads the push block (albedo_index), so the
    // material layout's range covers VERTEX|FRAGMENT. The legacy unlit layout
    // declares a VERTEX-only range, so keep that stage set there.
    const VkShaderStageFlags push_stages =
        scene.material_push_constants
        ? static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                          VK_SHADER_STAGE_FRAGMENT_BIT)
        : static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scene.pipeline_layout, 0, 1,
                            &mesh.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer, scene.pipeline_layout,
                       push_stages, 0,
                       push_size, &push);
    vkCmdBindIndexBuffer(command_buffer, mesh.index_buffer,
                         mesh.index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(command_buffer, mesh.index_count, 1, 0, 0, 0);
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)scene;
  (void)width;
  (void)height;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::record_pbr_scene(
    VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
    std::uint32_t width, std::uint32_t height) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || !scene.pipeline || !scene.pipeline_layout ||
      width == 0U || height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  VkViewport viewport{};
  viewport.width = static_cast<float>(width);
  viewport.height = static_cast<float>(height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = {width, height};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  // ---- Analytic sky (optional) ------------------------------------------
  // Drawn first inside the caller's render pass: the full-screen triangle
  // shades every pixel analytically, then geometry overdraws it via the
  // depth test (sky writes no depth; LEQUAL against cleared 1.0 lets it
  // through on empty pixels and behind geometry it simply loses).
  if (scene.sky_pipeline != VK_NULL_HANDLE) {
    if (!record_sky_pre_draw(command_buffer, scene).is_ok()) {
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
    // Main pipeline rebind follows below; sky state does not leak.
  }

  // ---- Main PBR lit pass ------------------------------------------------
  // 160-byte PBR ABI: view_projection (64) + model (64) + camera_position
  // (16) + material_index (4) + padding (12), matching the push blocks in
  // pbr_scene.{vert,frag}. Texture indices and shading factors are read by
  // the fragment stage from the set-2 material SSBO.
  struct PushConstants {
    SceneMatrix view_projection;
    SceneMatrix model;
    std::array<float, 4> camera_position;
    std::uint32_t material_index{0xffffffffU};
    std::uint32_t joint_base{0U};
    std::uint32_t pad[2]{0, 0};
  } push{};
  push.view_projection = scene.camera.view_projection;
  push.camera_position = scene.camera_position;

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    scene.pipeline);
  // Per-pass pipeline state for the lit pass. The draw loop below may swap to
  // the scene's skinned variant for rigged objects.
  //
  // Vulkan invalidates descriptor bindings whenever the bound pipeline layout
  // changes, so the two are tracked together: a layout change forces every
  // descriptor set to be re-issued. When the caller gives the skinned variant
  // the same layout as the default (the recommended shape), the layout never
  // changes mid-loop and no set is re-bound.
  VkPipeline bound_pipeline = scene.pipeline;
  VkPipelineLayout bound_layout = scene.pipeline_layout;
  constexpr std::uint32_t kInvalidMaterial = 0xffffffffU;

  // As in the shadow pass: the bone SSBO belongs to the skinned layout, and
  // the default lit layout may not have a set 3 at all.
  const auto bind_scene_descriptors = [&](VkPipelineLayout layout,
                                          bool has_bone_slot) {
    if (scene.texture_set != VK_NULL_HANDLE) {
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, 1, 1, &scene.texture_set, 0, nullptr);
    }
    if (scene.material_set != VK_NULL_HANDLE) {
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, 2, 1, &scene.material_set, 0, nullptr);
    }
    if (scene.ibl_set != VK_NULL_HANDLE) {
      // IBL variant (pbr_ibl.frag): prefiltered env cube, irradiance cube and
      // split-sum BRDF LUT live at set 3 by default; scene.ibl_set_slot
      // relocates them (5 for the composed pbr_full variant whose set 3 is
      // the skinning bones). Non-IBL pipelines leave it null.
      const std::uint32_t ibl_slot =
          scene.ibl_set_slot != 0U ? scene.ibl_set_slot : 3U;
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, ibl_slot, 1, &scene.ibl_set, 0, nullptr);
    }
    if (scene.shadow_set != VK_NULL_HANDLE) {
      // Shadow map sampled in the fragment stage for PCF. Slot matches the
      // pipeline variant: 4 for IBL+shadow (pbr_ibl_shadow.frag), 3 for
      // shadow-only (pbr_shadow.frag); scene.shadow_set_slot == 0 keeps the
      // historical default of 4.
      const std::uint32_t shadow_slot =
          scene.shadow_set_slot != 0U ? scene.shadow_set_slot : 4U;
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, shadow_slot, 1, &scene.shadow_set, 0,
                              nullptr);
    }
    if (scene.rt_set != VK_NULL_HANDLE) {
      // Ray-query shadow variant (pbr_rt_full.frag): set 4 is the scene TLAS
      // the fragment stage traces occlusion rays against.
      const std::uint32_t rt_slot =
          scene.rt_set_slot != 0U ? scene.rt_set_slot : 4U;
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, rt_slot, 1, &scene.rt_set, 0, nullptr);
    }
    if (has_bone_slot && scene.bone_set != VK_NULL_HANDLE) {
      // Skinned variant (skinned_scene.vert): one 64-byte joint matrix per
      // joint at set 3.
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, 3, 1, &scene.bone_set, 0, nullptr);
    }
    if (scene.lights_set != VK_NULL_HANDLE) {
      // Many-light variant (pbr_full_ml / pbr_rt_full_ml): dynamic point
      // lights SSBO at set 6 by default (scene.lights_set_slot relocates).
      const std::uint32_t lights_slot =
          scene.lights_set_slot != 0U ? scene.lights_set_slot : 6U;
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, lights_slot, 1, &scene.lights_set, 0,
                              nullptr);
    }
  };

  bind_scene_descriptors(bound_layout, scene.skinned_pipeline == VK_NULL_HANDLE);

  constexpr VkShaderStageFlags kPushStages =
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT);

  // ---- GPU LOD resolution (optional) -------------------------------------
  // The app maps the selection buffer (HOST_VISIBLE|HOST_COHERENT) and hands
  // record_pbr_scene the host pointer; per-object uvec2(lod, visible) results
  // start at word 26 + 4*N, matching lod_select.comp's output layout.
  const bool lod_active = scene.lod_results != nullptr &&
                          scene.lod_object_count > 0U &&
                          scene.lod_object_count == scene.objects.size();
  const std::uint32_t lod_result_base = 26U + 4U * scene.lod_object_count;

  for (const ScenePbrObject& object : scene.objects) {
    const SceneMesh* mesh_ptr = object.effective_mesh();
    if (mesh_ptr == nullptr || !mesh_ptr->is_drawable() ||
        object.material_index == kInvalidMaterial) {
      continue;
    }
    // Resolve the GPU-selected LOD level for this object (index order must
    // match the selection dispatch). Objects without a chain always draw
    // level 0; culled objects are skipped.
    if (lod_active) {
      const std::size_t obj = &object - scene.objects.data();
      const std::uint32_t lod = scene.lod_results[lod_result_base + 2U * obj];
      const std::uint32_t visible =
          scene.lod_results[lod_result_base + 2U * obj + 1U];
      if (visible == 0U) continue;
      mesh_ptr = object.mesh_for_lod(lod);
      if (mesh_ptr == nullptr || !mesh_ptr->is_drawable()) continue;
    }
    const SceneMesh& mesh = *mesh_ptr;
    push.model = object.model;
    push.material_index = object.material_index;
    push.joint_base = object.joint_base;
    // Skinned objects need a vertex stage that samples the bone SSBO, so
    // they bind the scene's skinned variant. Track the current binding and
    // only re-bind on a change: the pipeline state is expensive and the draw
    // list usually alternates in runs, not per object.
    const bool want_skinned = object.skinned &&
                              scene.skinned_pipeline != VK_NULL_HANDLE;
    const VkPipeline want_pipeline =
        want_skinned ? scene.skinned_pipeline : scene.pipeline;
    if (bound_pipeline != want_pipeline) {
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        want_pipeline);
      bound_pipeline = want_pipeline;
    }
    const VkPipelineLayout want_layout =
        want_skinned && scene.skinned_pipeline_layout != VK_NULL_HANDLE
            ? scene.skinned_pipeline_layout
            : scene.pipeline_layout;
    if (bound_layout != want_layout) {
      // A different layout drops every previously bound set.
      bind_scene_descriptors(want_layout, want_skinned);
      bound_layout = want_layout;
    }
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            want_layout, 0, 1, &mesh.descriptor_set, 0,
                            nullptr);
    vkCmdPushConstants(command_buffer, want_layout, kPushStages, 0,
                       sizeof(push), &push);
    vkCmdBindIndexBuffer(command_buffer, mesh.index_buffer,
                         mesh.index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(command_buffer, mesh.index_count, 1, 0, 0, 0);
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)scene;
  (void)width;
  (void)height;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

namespace {

//! Graph record shim: execute_graph's callbacks are plain function pointers,
//! so the frame context (renderer + scene + which pass) travels through
//! GraphPass::user_data and dispatches back into the public record methods.
struct PbrFrameRecordCtx {
  const VulkanRenderer* self;
  const VulkanPbrScene* scene;
  bool shadow;
};

void pbr_frame_render_cb(VkCommandBuffer cb, const GraphPass& pass,
                         void* user_data) {
  auto& ctx = *static_cast<PbrFrameRecordCtx*>(user_data);
  if (ctx.shadow) {
    (void)ctx.self->record_shadow_pre_pass(cb, *ctx.scene, pass.width,
                                           pass.height);
  } else {
    (void)ctx.self->record_pbr_scene(cb, *ctx.scene, pass.width, pass.height);
  }
}

}  // namespace

::warploom::core::Result<void> VulkanRenderer::record_shadow_pre_pass(
    VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
    std::uint32_t width, std::uint32_t height) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || scene.shadow_pipeline == VK_NULL_HANDLE ||
      scene.shadow_pipeline_layout == VK_NULL_HANDLE || width == 0U ||
      height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  VkViewport viewport{};
  viewport.width = static_cast<float>(width);
  viewport.height = static_cast<float>(height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = {width, height};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);

  // Depth-only pre-pass: light VP + model push (128 bytes), one set 0 per
  // mesh. Shadows are cast from the full-detail mesh; LOD selection and the
  // sky do not participate. When the pipeline's vertex stage is the skinned
  // shadow variant (shadow_skinned.vert), scene.bone_set carries the joint
  // matrices at set 3; static pipelines leave it null and nothing is bound.
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    scene.shadow_pipeline);
  VkPipeline bound_shadow_pipeline = scene.shadow_pipeline;
  // Depth bias relieves shadow-map acne: with a diagonal light the stored
  // depth varies across a face, so unbiased fragments fail LESS_OR_EQUAL
  // against neighboring texels and everything reads shadowed. NOTE: the
  // constant factor is scaled by r (min representable depth delta, ~2^-23
  // for D32), so it is nearly inert at small values; the slope factor does
  // the real work (slope 128 covers ~45-degree faces in light UV space).
  vkCmdSetDepthBias(command_buffer, 8.0f, 0.0f, 128.0f);
  // Set 3 carries the joint matrices for the skinned vertex stage. Tracked
  // per-layout: swapping to a layout that differs drops all bound sets.
  VkPipelineLayout bound_shadow_layout = scene.shadow_pipeline_layout;
  // The bone SSBO lives at set 3 of the *skinned* shadow layout only. The
  // static shadow layout is often a single-set layout, and binding set 3
  // against it is out of range -- which faults the driver rather than
  // failing validation. So the caller's choice of layout decides whether
  // bones are bound, not merely whether bone_set is non-null.
  const auto bind_shadow_descriptors = [&](VkPipelineLayout layout,
                                           bool has_bone_slot) {
    if (has_bone_slot && scene.bone_set != VK_NULL_HANDLE) {
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, 3, 1, &scene.bone_set, 0, nullptr);
    }
  };
  bind_shadow_descriptors(bound_shadow_layout,
                         scene.shadow_skinned_pipeline == VK_NULL_HANDLE);

  // 144 bytes: light VP + model + joint base. Matches shadow_skinned.vert
  // exactly, and shadow.vert ignores the trailing word, so one push-constant
  // range -- and therefore one pipeline layout -- serves both vertex stages.
  struct ShadowPush {
    SceneMatrix light_view_projection;
    SceneMatrix model;
    std::array<std::uint32_t, 4> joint_base{{0U, 0U, 0U, 0U}};
  } push{};
  push.light_view_projection = scene.shadow_light_vp;
  for (const ScenePbrObject& object : scene.objects) {
    const SceneMesh* mesh_ptr = object.effective_mesh();
    if (mesh_ptr == nullptr || !mesh_ptr->is_drawable() ||
        object.material_index == 0xffffffffU) {
      continue;
    }
    const SceneMesh& mesh = *mesh_ptr;
    push.model = object.model;
    push.joint_base[0] = object.joint_base;
    // A rigged actor's shadow needs the skinned vertex stage, exactly as the
    // lit pass does. See ScenePbrObject::skinned.
    const bool want_skinned =
        object.skinned && scene.shadow_skinned_pipeline != VK_NULL_HANDLE;
    if (bound_shadow_pipeline !=
        (want_skinned ? scene.shadow_skinned_pipeline
                      : scene.shadow_pipeline)) {
      vkCmdBindPipeline(
          command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
          want_skinned ? scene.shadow_skinned_pipeline
                       : scene.shadow_pipeline);
      bound_shadow_pipeline =
          want_skinned ? scene.shadow_skinned_pipeline : scene.shadow_pipeline;
    }
    const VkPipelineLayout want_layout =
        want_skinned && scene.shadow_skinned_pipeline_layout != VK_NULL_HANDLE
            ? scene.shadow_skinned_pipeline_layout
            : scene.shadow_pipeline_layout;
    if (bound_shadow_layout != want_layout) {
      bind_shadow_descriptors(want_layout, want_skinned);
      bound_shadow_layout = want_layout;
    }
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            want_layout, 0, 1, &mesh.descriptor_set, 0,
                            nullptr);
    vkCmdPushConstants(command_buffer, want_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(command_buffer, mesh.index_buffer,
                         mesh.index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(command_buffer, mesh.index_count, 1, 0, 0, 0);
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)scene;
  (void)width;
  (void)height;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

// --- HDR compose chain -------------------------------------------------
//
// Owned by the renderer so an application opts in with two config fields
// instead of hand-rolling an intermediate target, a sampler, a descriptor
// set and a fullscreen pass. The pass sequence becomes:
//
//   [optional] shadow pre-pass      (application hook)
//   [optional] GPU-driven cull      (application hook)
//   HDR pass                       -> hdr_target_   (application callback)
//   [optional] bloom down           -> bloom_target_
//   [optional] bloom up             -> hdr_target_
//   tonemap + FXAA + bloom          -> swapchain image
//
// The last three are the engine's, which is what stops the application from
// owning presentation.

::warploom::core::Result<void> VulkanRenderer::ensure_compose_resources(
    std::uint32_t width, std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!config_.enable_hdr_compose) {
    return ::warploom::core::Result<void>::ok();
  }
  if (compose_failed_) {
    // Already refused once; do not retry every frame.
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  if (width == 0U || height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  const auto refuse = [this](const char* why) -> ::warploom::core::Result<void> {
    compose_failed_ = true;
    std::fprintf(stderr,
                 "Warploom: HDR compose disabled (%s). The scene will render "
                 "straight to the swapchain with clipped highlights.\n", why);
    return ::warploom::core::Result<void>::ok();
  };

  // Pick the best supported HDR format.
  VkFormat hdr_format = config_.hdr_format != VK_FORMAT_UNDEFINED
                            ? config_.hdr_format
                            : VK_FORMAT_R16G16B16A16_SFLOAT;
  {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, hdr_format, &props);
    const bool sampled = (props.optimalTilingFeatures &
                          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U &&
                         (props.optimalTilingFeatures &
                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U;
    if (!sampled) {
      hdr_format = VK_FORMAT_B8G8R8A8_UNORM;
      vkGetPhysicalDeviceFormatProperties(physical_device_, hdr_format, &props);
      if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0U ||
          (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U) {
        return refuse("no colour-renderable + sampleable HDR format");
      }
    }
  }
  compose_hdr_format_ = hdr_format;

  // One-time setup: allocator, descriptors, samplers, pipelines, black
  // texture. Size-dependent resources are (re)created below.
  if (!compose_allocator_) {
    compose_allocator_ = std::make_unique<VulkanMemoryAllocator>();
    if (!compose_allocator_->initialize(device_, physical_device_).is_ok()) {
      return refuse("allocator init failed");
    }
  }
  if (!compose_descriptor_manager_) {
    compose_descriptor_manager_ = std::make_unique<VulkanDescriptorManager>();
    if (!compose_descriptor_manager_->initialize(device_).is_ok()) {
      return refuse("descriptor manager init failed");
    }
    // ReflectedBinding is {set, binding, count, type, stage}: both samplers
    // live at set 0, bindings 0 (HDR input) and 1 (additive bloom input).
    auto layout = compose_descriptor_manager_->create_layout({
        {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT},
        {0, 1, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT}}, 1U);
    if (!layout.is_ok()) return refuse("compose descriptor layout failed");
    compose_layout_ = layout.value();
    auto set = compose_descriptor_manager_->allocate_set(compose_layout_);
    if (!set.is_ok()) return refuse("compose descriptor set failed");
    compose_set_ = set.value();

    // Only when bloom is on: these sets are otherwise dead weight, and
    // allocating them unconditionally exhausted the pool and silently
    // disabled compose altogether.
    if (config_.enable_bloom) {
      // Capacity 2: one set per bloom stage, sharing this single-binding
      // layout. create_layout sizes the pool from sets_to_reserve, so
      // allocating two sets from a layout reserved for one fails.
      auto stage_layout = compose_descriptor_manager_->create_layout({
          {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           VK_SHADER_STAGE_FRAGMENT_BIT}}, 2U);
      if (!stage_layout.is_ok()) return refuse("bloom stage layout failed");
      bloom_stage_layout_ = stage_layout.value();
      auto down_set =
          compose_descriptor_manager_->allocate_set(bloom_stage_layout_);
      if (!down_set.is_ok()) return refuse("bloom downsample set failed");
      bloom_down_set_ = down_set.value();
      auto up_set =
          compose_descriptor_manager_->allocate_set(bloom_stage_layout_);
      if (!up_set.is_ok()) return refuse("bloom upsample set failed");
      bloom_up_set_ = up_set.value();
    }
  }

  if (hdr_sampler_ == VK_NULL_HANDLE) {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1.0f;
    if (vkCreateSampler(device_, &si, nullptr, &hdr_sampler_) != VK_SUCCESS) {
      return refuse("sampler creation failed");
    }
    linear_sampler_ = hdr_sampler_;
  }

  if (black_texture_.image == VK_NULL_HANDLE) {
    // 1x1 opaque black, sampled by the bloom slot when bloom is off.
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {1, 1, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    // initialLayout must be UNDEFINED or PREINITIALIZED
    // (VUID-VkImageCreateInfo-initialLayout-00993), so the move to
    // SHADER_READ_ONLY_OPTIMAL is a one-time barrier below rather than an
    // initial layout.
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ii, nullptr, &black_texture_.image) != VK_SUCCESS) {
      return refuse("black texture creation failed");
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, black_texture_.image, &req);
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    const std::uint32_t bits = req.memoryTypeBits;
    std::uint32_t type = 0U;
    bool found = false;
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((bits & (1U << i)) == 0U) continue;
      const VkMemoryPropertyFlags flags = mem_props.memoryTypes[i].propertyFlags;
      if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
        type = i; found = true; break;
      }
    }
    if (!found) return refuse("no device-local memory for the black texture");
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &ai, nullptr, &black_texture_.memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, black_texture_.image,
                          black_texture_.memory, 0U) != VK_SUCCESS) {
      return refuse("black texture allocation failed");
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = black_texture_.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &black_texture_.view) != VK_SUCCESS) {
      return refuse("black texture view failed");
    }
    // One-time transition into the layout it is always sampled in. Without
    // it the submit sees the image still UNDEFINED.
    if (auto pool = VulkanRenderer::create_command_pool(
            device_, static_cast<std::uint32_t>(
                         [&] {
                           std::uint32_t n = 0U;
                           vkGetPhysicalDeviceQueueFamilyProperties(
                               physical_device_, &n, nullptr);
                           return n;
                         }() > 0U
                         ? 0U
                         : 0U));
        pool.is_ok()) {
      if (auto cb = VulkanRenderer::allocate_command_buffer(device_, pool.value());
          cb.is_ok()) {
        VkCommandBuffer cmd = cb.value();
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS) {
          VkImageMemoryBarrier b{};
          b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          b.image = black_texture_.image;
          b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          b.srcAccessMask = 0;
          b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U,
                               0U, nullptr, 0U, nullptr, 1U, &b);
          if (vkEndCommandBuffer(cmd) == VK_SUCCESS) {
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1U;
            si.pCommandBuffers = &cmd;
            vkQueueSubmit(graphics_queue_, 1U, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(graphics_queue_);
          }
        }
      }
      vkDestroyCommandPool(device_, pool.value(), nullptr);
    }
  }

  const std::string dir = config_.compose_shader_dir;
  if (dir.empty()) return refuse("compose_shader_dir is empty");

  // Load the compose shaders and build their layouts. Idempotent: a resize
  // must redo this, because VulkanPipeline::cleanup() destroys the shader
  // modules AND the (owned) layout, not just the VkPipeline. Skipping this on
  // a rebuild leaves create_graphics_pipeline with no vertex or fragment
  // module, which fails its argument check with no diagnostic.
  const auto build_pipelines = [&]() -> bool {
    VkDescriptorSetLayout layouts[1] = {compose_layout_};
    VkDescriptorSetLayout stage_layouts[1] = {bloom_stage_layout_};
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0U, 16U};
    if (compose_pipeline_ == nullptr) compose_pipeline_ = std::make_unique<VulkanPipeline>();
    compose_pipeline_->cleanup();
    if (!compose_pipeline_
             ->load_shader_stage_file(device_, dir + "/fullscreen.vert.spv", "vertex").is_ok() ||
        !compose_pipeline_
             ->load_shader_stage_file(device_, dir + "/tonemap_fxaa.frag.spv", "fragment").is_ok()) {
      return false;
    }
    if (!compose_pipeline_->create_pipeline_layout(device_, layouts, 1U, &push).is_ok()) {
      return false;
    }
    if (!config_.enable_bloom) return true;

    if (bloom_down_pipeline_ == nullptr) {
      bloom_down_pipeline_ = std::make_unique<VulkanPipeline>();
    }
    bloom_down_pipeline_->cleanup();
    if (!bloom_down_pipeline_
             ->load_shader_stage_file(device_, dir + "/fullscreen.vert.spv", "vertex").is_ok() ||
        !bloom_down_pipeline_
             ->load_shader_stage_file(device_, dir + "/bloom_downsample.frag.spv", "fragment").is_ok()) {
      return false;
    }
    if (!bloom_down_pipeline_->create_pipeline_layout(device_, stage_layouts, 1U, nullptr).is_ok()) {
      return false;
    }
    if (bloom_up_pipeline_ == nullptr) bloom_up_pipeline_ = std::make_unique<VulkanPipeline>();
    bloom_up_pipeline_->cleanup();
    if (!bloom_up_pipeline_
             ->load_shader_stage_file(device_, dir + "/fullscreen.vert.spv", "vertex").is_ok() ||
        !bloom_up_pipeline_
             ->load_shader_stage_file(device_, dir + "/bloom_upsample.frag.spv", "fragment").is_ok()) {
      return false;
    }
    return bloom_up_pipeline_->create_pipeline_layout(device_, stage_layouts, 1U, nullptr).is_ok();
  };

  // ---- size-dependent resources ---------------------------------------
  if (compose_ready_ && compose_width_ == width && compose_height_ == height) {
    return ::warploom::core::Result<void>::ok();
  }

  // Pipelines bake in the render pass and format, so they are rebuilt with
  // the targets rather than reused across a resize.
  const std::uint32_t bw = std::max(1U, width / std::max(1U, config_.bloom_downscale));
  const std::uint32_t bh = std::max(1U, height / std::max(1U, config_.bloom_downscale));

  hdr_target_.cleanup();
  bloom_target_.cleanup();
  // The pipelines below bake in a render pass and attachment formats, so they
  // are rebuilt here. The old ones must be released first: recreating over a
  // live VkPipeline leaks it and then destroys it twice, which the driver
  // reports as vkDestroyShaderModule: Invalid device at teardown.
  //
  // cleanup() forgets the caller's layout handle (it does not own it, so it
  // does not destroy it), so the layouts are captured first and handed back
  // explicitly -- otherwise the rebuild asks for a null layout, the engine
  // synthesises an empty one, and vkCreateGraphicsPipelines fails because the
  // shader uses set 0.
  if (!build_pipelines()) {
    return refuse("compose shader/pipeline setup failed");
  }
  const VkPipelineLayout compose_layout = compose_pipeline_->pipeline_layout();
  const VkPipelineLayout bloom_down_layout =
      bloom_down_pipeline_ != nullptr ? bloom_down_pipeline_->pipeline_layout()
                                      : VK_NULL_HANDLE;
  const VkPipelineLayout bloom_up_layout =
      bloom_up_pipeline_ != nullptr ? bloom_up_pipeline_->pipeline_layout()
                                    : VK_NULL_HANDLE;
  compose_ready_ = false;
  compose_width_ = width;
  compose_height_ = height;

  // SAMPLED: the compose chain samples this image, and so does anything the
  // application reads back through the capture path.
  if (!hdr_target_.create(device_, physical_device_, compose_hdr_format_, width,
                          height, compose_allocator_.get(),
                          VK_IMAGE_USAGE_SAMPLED_BIT)
           .is_ok() ||
      !hdr_target_.create_depth(device_, physical_device_, VK_FORMAT_D32_SFLOAT)
           .is_ok() ||
      // COLOR_ATTACHMENT_OPTIMAL: this target is sampled immediately, and
      // letting the compose chain own the transition to SHADER_READ_ONLY keeps
      // one explicit barrier per image.
      !hdr_target_
           .create_render_pass(device_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
           .is_ok() ||
      !hdr_target_.create_framebuffer(device_).is_ok()) {
    return refuse("HDR target creation failed");
  }

  // Tonemap targets the swapchain image: built for the presentation render
  // pass and format, NOT the HDR intermediate's.
  if (present_pass_ == VK_NULL_HANDLE ||
      present_format_ == VK_FORMAT_UNDEFINED) {
    return refuse("presentation target unknown");
  }
  if (!compose_pipeline_
           ->create_graphics_pipeline(device_, present_pass_, present_format_,
                                     compose_layout, false, false, false)
           .is_ok()) {
    return refuse("compose pipeline creation failed");
  }

  if (config_.enable_bloom) {
    if (!bloom_target_
             .create(device_, physical_device_, compose_hdr_format_, bw, bh,
                     compose_allocator_.get(), VK_IMAGE_USAGE_SAMPLED_BIT)
             .is_ok() ||
        !bloom_target_
             .create_render_pass(device_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
             .is_ok() ||
        !bloom_target_.create_framebuffer(device_).is_ok()) {
      return refuse("bloom target creation failed");
    }
    if (!bloom_down_pipeline_
             ->create_graphics_pipeline(device_, bloom_target_.render_pass(),
                                       compose_hdr_format_,
                                       bloom_down_layout, false, false, false)
             .is_ok() ||
        !bloom_up_pipeline_
             ->create_graphics_pipeline(device_, hdr_target_.render_pass(),
                                       compose_hdr_format_,
                                       bloom_up_layout, false, false, false)
             .is_ok()) {
      return refuse("bloom pipeline creation failed");
    }
  }

  // Bind the two sampler slots: HDR input and the bloom input (the black
  // texture when bloom is off).
  if (!compose_descriptor_manager_
           ->write_image(compose_set_, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         hdr_sampler_, hdr_target_.image_view(),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    return refuse("HDR descriptor write failed");
  }
  const VkImageView bloom_view =
      config_.enable_bloom ? bloom_target_.image_view() : black_texture_.view;
  if (!compose_descriptor_manager_
           ->write_image(compose_set_, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         linear_sampler_, bloom_view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    return refuse("bloom descriptor write failed");
  }
  // Each bloom stage reads its own input at binding 0. Only when bloom is on:
  // the sets do not exist otherwise, and writing to a null set fails.
  if (config_.enable_bloom &&
      (!compose_descriptor_manager_
           ->write_image(bloom_down_set_, 0U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, hdr_sampler_,
                         hdr_target_.image_view(),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok() ||
       !compose_descriptor_manager_
           ->write_image(bloom_up_set_, 0U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         linear_sampler_, bloom_view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok())) {
    return refuse("bloom stage descriptor write failed");
  }

  compose_ready_ = true;
  ++compose_generation_;
  return ::warploom::core::Result<void>::ok();
#else
  (void)width; (void)height;
  return ::warploom::core::Result<void>::ok();
#endif
}


namespace {

//! One compose stage: the pipeline, the target it renders into, and what it
//! samples. `target == nullptr` means the stage writes the presentation target
//! (the swapchain), whose layout the render pass owns.
struct ComposeStage {
  const VulkanPipeline* pipeline{nullptr};
  VulkanOffscreenTarget* target{nullptr};
  VkRenderPass pass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  std::uint32_t width{0};
  std::uint32_t height{0};
  const void* push{nullptr};
  std::uint32_t push_size{0};
  VkDescriptorSet set{VK_NULL_HANDLE};
  //! Images this stage samples, in descriptor-slot order.
  std::array<GraphSampledImage, 2> samples{};
  std::uint32_t sample_count{0};
};

}  // namespace

//! Records the chain as render-graph nodes and lets compile_graph compute the
//! barriers, rather than hand-writing a layout transition per stage. Every
//! stage declares what it reads and what it writes; the compiler tracks each
//! image's layout and access across the sequence and emits exactly the
//! transitions required.
void VulkanRenderer::record_compose_chain(VkCommandBuffer command_buffer,
                                          VkRenderPass target_pass,
                                          VkFramebuffer target_framebuffer,
                                          std::uint32_t width,
                                          std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!compose_ready_ || hdr_sampler_ == VK_NULL_HANDLE) return;

  // Where the scene pass left the HDR image. It is recorded outside this graph,
  // so each stage's first sample of it declares this as the producer state.
  const VkImageLayout hdr_from_scene = hdr_target_.color_final_layout();

  VkClearValue clear[2]{};
  clear[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clear[1].depthStencil = {1.0f, 0U};

  struct ComposePush {
    float exposure;
    float pad[3];
  } push{};
  push.exposure = config_.exposure;

  // --- Declare the stages. ---
  std::vector<ComposeStage> stages;
  if (config_.enable_bloom && bloom_down_pipeline_ && bloom_up_pipeline_) {
    ComposeStage down{};
    down.pipeline = bloom_down_pipeline_.get();
    down.target = &bloom_target_;
    down.pass = bloom_target_.render_pass();
    down.framebuffer = bloom_target_.framebuffer();
    down.width = bloom_target_.width();
    down.height = bloom_target_.height();
    down.set = bloom_down_set_;
    down.samples[0].image = hdr_target_.image();
    down.samples[0].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    down.samples[0].initial_layout = hdr_from_scene;
    down.samples[0].initial_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    down.samples[0].initial_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    down.sample_count = 1;
    stages.push_back(down);

    ComposeStage up{};
    up.pipeline = bloom_up_pipeline_.get();
    up.target = &hdr_target_;
    up.pass = hdr_target_.render_pass();
    up.framebuffer = hdr_target_.framebuffer();
    up.width = width;
    up.height = height;
    up.set = bloom_up_set_;
    up.samples[0].image = bloom_target_.image();
    up.samples[0].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    up.samples[0].initial_layout = bloom_target_.color_final_layout();
    up.samples[0].initial_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    up.samples[0].initial_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    up.sample_count = 1;
    stages.push_back(up);
  }

  // The tonemap always runs: it samples the HDR scene and the bloom buffer
  // (slot 1 is the 1x1 black texture when bloom is off) and writes the
  // presentation target.
  ComposeStage tonemap{};
  tonemap.pipeline = compose_pipeline_.get();
  tonemap.target = nullptr;
  tonemap.pass = target_pass;
  tonemap.framebuffer = target_framebuffer;
  tonemap.width = width;
  tonemap.height = height;
  tonemap.push = &push;
  tonemap.push_size = sizeof(push);
  tonemap.set = compose_set_;
  tonemap.samples[0].image = hdr_target_.image();
  tonemap.samples[0].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // With no bloom stage, the scene pass is this stage's only producer. With
  // bloom, the upsample rewrote hdr and the compiler already tracked it.
  tonemap.samples[0].initial_layout = hdr_from_scene;
  tonemap.samples[0].initial_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  tonemap.samples[0].initial_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  tonemap.samples[1].image = bloom_target_.image();
  tonemap.samples[1].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (bloom_target_.image() != VK_NULL_HANDLE) {
    tonemap.samples[1].initial_layout = bloom_target_.color_final_layout();
    tonemap.samples[1].initial_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    tonemap.samples[1].initial_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  }
  tonemap.sample_count = config_.enable_bloom ? 2U : 1U;
  stages.push_back(tonemap);

  // --- Build graph nodes. Each stage writes its target as a real attachment,
  // so the compiler tracks write-after-write and write-after-read edges too.
  std::vector<GraphPass> passes;
  std::vector<ComposeStage> stage_copy;
  passes.reserve(stages.size());
  for (const ComposeStage& stage : stages) {
    GraphPass pass{};
    pass.name = "compose";
    pass.render_pass = stage.pass;
    pass.framebuffer = stage.framebuffer;
    pass.width = stage.width;
    pass.height = stage.height;
    pass.clear_values = clear;
    // Two entries always: pClearValues is indexed by attachment number, and
    // too few is VUID-VkRenderPassBeginInfo-clearValueCount-00902.
    pass.clear_value_count = 2U;
    pass.sampled_images.assign(stage.samples.begin(),
                               stage.samples.begin() + stage.sample_count);
    if (stage.target != nullptr) {
      RenderPassAttachment attachment{};
      attachment.image = stage.target->image();
      attachment.view = stage.target->image_view();
      attachment.format = stage.target->format();
      attachment.used_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachment.final_layout = stage.target->color_final_layout();
      attachment.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      attachment.stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      pass.attachments.push_back(attachment);
    }
    pass.user_data = nullptr;
    passes.push_back(pass);
    stage_copy.push_back(stage);
  }
  // The recorder needs the stage, and the pass carries an opaque handle.
  for (std::size_t i = 0; i < passes.size(); ++i) {
    passes[i].user_data = &stage_copy[i];
  }

  std::vector<GraphNode> nodes;
  nodes.reserve(passes.size());
  for (const GraphPass& pass : passes) {
    nodes.push_back(GraphNode::from_render(pass));
  }

  const CompiledGraph compiled = compile_graph(nodes);

  execute_graph(command_buffer, nodes, compiled,
                +[](VkCommandBuffer cb, const GraphPass& pass, void* user) {
                  const auto* stage =
                      static_cast<const ComposeStage*>(user);
                  if (stage == nullptr || stage->pipeline == nullptr) return;
                  FullscreenPass draw{};
                  draw.pipeline = stage->pipeline->pipeline();
                  draw.pipeline_layout = stage->pipeline->pipeline_layout();
                  draw.render_pass = pass.render_pass;
                  draw.framebuffer = pass.framebuffer;
                  draw.width = pass.width;
                  draw.height = pass.height;
                  draw.clear_values = pass.clear_values;
                  draw.clear_value_count = pass.clear_value_count;
                  draw.push_data = stage->push;
                  draw.push_size = stage->push_size;
                  draw.push_stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT;
                  (void)VulkanRenderer::record_fullscreen_draw(cb, draw,
                                                        stage->set);
                },
                nullptr);
#else
  (void)command_buffer; (void)target_pass; (void)target_framebuffer;
  (void)width; (void)height;
#endif
}

void VulkanRenderer::destroy_compose_resources() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  compose_ready_ = false;
  compose_width_ = 0U;
  compose_height_ = 0U;
  compose_set_ = VK_NULL_HANDLE;
  compose_layout_ = VK_NULL_HANDLE;
  // Explicit, with the recorded device. VulkanPipeline's destructor is a
  // deliberate no-op (cleanup(nullptr) ignored by cleanup()), because a
  // pipeline that outlives its VulkanContext would otherwise call
  // vkDestroy* on a dead device. Anything holding one must therefore release
  // it explicitly, which is what this is for.
  if (compose_pipeline_ != nullptr) {
    compose_pipeline_->cleanup();
    compose_pipeline_.reset();
  }
  if (bloom_down_pipeline_ != nullptr) {
    bloom_down_pipeline_->cleanup();
    bloom_down_pipeline_.reset();
  }
  if (bloom_up_pipeline_ != nullptr) {
    bloom_up_pipeline_->cleanup();
    bloom_up_pipeline_.reset();
  }
  // Targets before the allocator that owns their memory.
  bloom_target_.cleanup();
  hdr_target_.cleanup();
  if (black_texture_.view != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, black_texture_.view, nullptr);
    black_texture_.view = VK_NULL_HANDLE;
  }
  if (black_texture_.image != VK_NULL_HANDLE) {
    vkDestroyImage(device_, black_texture_.image, nullptr);
    black_texture_.image = VK_NULL_HANDLE;
  }
  if (black_texture_.memory != VK_NULL_HANDLE) {
    vkFreeMemory(device_, black_texture_.memory, nullptr);
    black_texture_.memory = VK_NULL_HANDLE;
  }
  if (hdr_sampler_ != VK_NULL_HANDLE) {
    vkDestroySampler(device_, hdr_sampler_, nullptr);
    hdr_sampler_ = VK_NULL_HANDLE;
    linear_sampler_ = VK_NULL_HANDLE;
  }
  compose_descriptor_manager_.reset();
  compose_allocator_.reset();
#endif
}

::warploom::core::Result<void> VulkanRenderer::record_pbr_frame(
    VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
    const PbrFrameTargets& targets) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || targets.render_pass == VK_NULL_HANDLE ||
      targets.framebuffer == VK_NULL_HANDLE || targets.width == 0U ||
      targets.height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  const bool shadow_active =
      scene.shadow_pipeline != VK_NULL_HANDLE &&
      targets.shadow_render_pass != VK_NULL_HANDLE &&
      targets.shadow_framebuffer != VK_NULL_HANDLE &&
      targets.shadow_image != VK_NULL_HANDLE &&
      targets.shadow_width > 0U && targets.shadow_height > 0U;

  // Optional shadow pre-pass node. Declaring the depth attachment lets the
  // graph compiler own the image's state: the UNDEFINED -> DEPTH_ATTACHMENT
  // barrier runs before the pass, VkRenderPass lands it in DEPTH_READ, and
  // the main pass's sampled_images declaration produces the write -> read
  // execution barrier with no manual authoring.
  GraphPass shadow_pass{};
  PbrFrameRecordCtx shadow_ctx{this, &scene, true};
  if (shadow_active) {
    VkClearValue shadow_clear{};
    shadow_clear.depthStencil = {1.0f, 0U};
    shadow_pass.name = "shadow_pre_pass";
    shadow_pass.render_pass = targets.shadow_render_pass;
    shadow_pass.framebuffer = targets.shadow_framebuffer;
    shadow_pass.width = targets.shadow_width;
    shadow_pass.height = targets.shadow_height;
    shadow_pass.clear_values = &shadow_clear;
    shadow_pass.clear_value_count = 1U;
    shadow_pass.attachments = {depth_attachment(
        targets.shadow_image, VK_NULL_HANDLE, targets.shadow_format,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)};
    shadow_pass.user_data = &shadow_ctx;
  }

  // Main lit pass node. The target's images have no in-graph producer, so
  // no attachment declarations are needed: VkRenderPass handles the layout
  // (initial UNDEFINED, final per its attachment descriptions) and the
  // shadow map arrives via sampled_images.
  GraphPass main_pass{};
  main_pass.name = "pbr_main";
  main_pass.render_pass = targets.render_pass;
  main_pass.framebuffer = targets.framebuffer;
  main_pass.width = targets.width;
  main_pass.height = targets.height;
  main_pass.clear_values = targets.clear_values;
  main_pass.clear_value_count = targets.clear_value_count;
  PbrFrameRecordCtx main_ctx{this, &scene, false};
  main_pass.user_data = &main_ctx;
  if (shadow_active) {
    main_pass.sampled_images = {GraphSampledImage{
        targets.shadow_image,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT}};
  }

  std::vector<GraphNode> nodes;
  nodes.reserve(shadow_active ? 2U : 1U);
  if (shadow_active) nodes.push_back(GraphNode::from_render(shadow_pass));
  nodes.push_back(GraphNode::from_render(main_pass));

  const CompiledGraph compiled = compile_graph(nodes);
  execute_graph(command_buffer, nodes, compiled, &pbr_frame_render_cb,
                nullptr);
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)scene;
  (void)targets;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

#ifdef OMNICPP_HAS_VULKAN
namespace {

//! Record shims for the one-submission GPU-driven frame: user_data carries
//! the borrowed frame description. Raw vkCmd* calls here (not renderer
//! methods) because the shims execute inside execute_graph callbacks where
//! the graph already owns sequencing.
struct GpuDrivenCtx {
  const VulkanRenderer::GpuDrivenFrame* frame;
};

void gpu_driven_compute_cb(VkCommandBuffer cb, const GraphComputePass& pass,
                           void* user_data) {
  (void)pass;
  const auto& f = *static_cast<GpuDrivenCtx*>(user_data)->frame;
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f.cull_pipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          f.cull_pipeline_layout, 0, 1, &f.cull_set, 0,
                          nullptr);
  if (f.cull_push.data != nullptr && f.cull_push.size > 0U) {
    vkCmdPushConstants(cb, f.cull_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, f.cull_push.size,
                       f.cull_push.data);
  }
  const std::uint32_t groups = (f.object_count + 63U) / 64U;
  vkCmdDispatch(cb, groups, 1U, 1U);
}

void gpu_driven_render_cb(VkCommandBuffer cb, const GraphPass& pass,
                          void* user_data) {
  const auto& f = *static_cast<GpuDrivenCtx*>(user_data)->frame;

  VkViewport viewport{};
  viewport.width = static_cast<float>(pass.width);
  viewport.height = static_cast<float>(pass.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cb, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {pass.width, pass.height};
  vkCmdSetScissor(cb, 0, 1, &scissor);

  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f.draw_pipeline);
  // Bind in maximal consecutive runs: vkCmdBindDescriptorSets takes a single
  // (firstSet, count) pair, and the slot map may legitimately skip one.
  for (std::uint32_t i = 0; i < f.draw_set_count;) {
    std::uint32_t j = i;
    while (j + 1U < f.draw_set_count &&
           f.draw_set_slots[j + 1U] == f.draw_set_slots[j] + 1U) {
      ++j;
    }
    const std::uint32_t count = j - i + 1U;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            f.draw_pipeline_layout, f.draw_set_slots[i], count,
                            &f.draw_sets[i], 0, nullptr);
    i = j + 1U;
  }
  if (f.draw_push.data != nullptr && f.draw_push.size > 0U) {
    vkCmdPushConstants(cb, f.draw_pipeline_layout,
                       static_cast<VkShaderStageFlags>(
                           VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT),
                       0, f.draw_push.size, f.draw_push.data);
  }
  vkCmdBindIndexBuffer(cb, f.index_buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexedIndirect(cb, f.indirect_buffer, 0, f.object_count,
                           sizeof(VkDrawIndexedIndirectCommand));
}

}  // namespace
#endif  // OMNICPP_HAS_VULKAN

::warploom::core::Result<void> VulkanRenderer::record_gpu_driven_cull(
    VkCommandBuffer command_buffer, const GpuDrivenFrame& frame) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || frame.cull_pipeline == VK_NULL_HANDLE ||
      frame.cull_pipeline_layout == VK_NULL_HANDLE ||
      frame.cull_set == VK_NULL_HANDLE || frame.object_count == 0U ||
      frame.indirect_buffer == VK_NULL_HANDLE) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  GpuDrivenCtx ctx{&frame};
  GraphComputePass cull{};
  cull.name = "gpu_driven_cull";
  cull.group_count_x = (frame.object_count + 63U) / 64U;
  cull.user_data = &ctx;

  const std::vector<GraphNode> nodes{GraphNode::from_compute(cull)};
  const CompiledGraph compiled = compile_graph(nodes);
  execute_graph(command_buffer, nodes, compiled, nullptr, &gpu_driven_compute_cb);

  // Hand the indirect commands to the eventual draw. execute_graph has no
  // consumer to infer that edge from here, so the barrier is explicit.
  VkBufferMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                          VK_ACCESS_SHADER_READ_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = frame.indirect_buffer;
  barrier.offset = 0U;
  barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                       0U, 0U, nullptr, 1U, &barrier, 0U, nullptr);
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)frame;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::record_gpu_driven_draw(
    VkCommandBuffer command_buffer, std::uint32_t width, std::uint32_t height,
    const GpuDrivenFrame& frame) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || frame.draw_pipeline == VK_NULL_HANDLE ||
      frame.draw_pipeline_layout == VK_NULL_HANDLE ||
      frame.draw_set_count == 0U ||
      frame.draw_set_count > std::size(frame.draw_sets) ||
      frame.index_buffer == VK_NULL_HANDLE || width == 0U || height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  // Called inside the caller's already-begun render pass, so there is no
  // render_pass/framebuffer to validate here; GpuDrivenFrame still carries
  // them for record_pbr_frame_gpu_driven's use.
  GpuDrivenCtx ctx{&frame};
  GraphPass pass{};
  pass.name = "gpu_driven_main";
  pass.width = width;
  pass.height = height;
  pass.user_data = &ctx;
  gpu_driven_render_cb(command_buffer, pass, &ctx);
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)width;
  (void)height;
  (void)frame;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}


::warploom::core::Result<void> VulkanRenderer::record_pbr_frame_gpu_driven(
    VkCommandBuffer command_buffer, const GpuDrivenFrame& frame) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || frame.cull_pipeline == VK_NULL_HANDLE ||
      frame.cull_pipeline_layout == VK_NULL_HANDLE ||
      frame.cull_set == VK_NULL_HANDLE || frame.object_count == 0U ||
      frame.indirect_buffer == VK_NULL_HANDLE ||
      frame.draw_pipeline == VK_NULL_HANDLE ||
      frame.draw_pipeline_layout == VK_NULL_HANDLE ||
      frame.draw_set_count == 0U ||
      frame.draw_set_count > std::size(frame.draw_sets) ||
      frame.index_buffer == VK_NULL_HANDLE ||
      frame.render_pass == VK_NULL_HANDLE ||
      frame.framebuffer == VK_NULL_HANDLE || frame.width == 0U ||
      frame.height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  // Compute node: cull + LOD + command generation (one invocation per
  // object; 64-wide groups matching cull_and_draw_lod.comp).
  GraphComputePass cull{};
  cull.name = "gpu_driven_cull";
  cull.group_count_x = (frame.object_count + 63U) / 64U;
  GpuDrivenCtx ctx{&frame};
  cull.user_data = &ctx;

  // Main node: ONE indirect draw consuming the GPU-written commands. The
  // consumer-declared buffer edge makes execute_graph emit the
  // COMPUTE_SHADER(WRITE) -> DRAW_INDIRECT(READ) barrier before the render
  // pass begins; no barrier is hand-authored anywhere in the frame.
  GraphPass main_pass{};
  main_pass.name = "gpu_driven_main";
  main_pass.render_pass = frame.render_pass;
  main_pass.framebuffer = frame.framebuffer;
  main_pass.width = frame.width;
  main_pass.height = frame.height;
  main_pass.clear_values = frame.clear_values;
  main_pass.clear_value_count = frame.clear_value_count;
  GraphBufferEdge edge{};
  edge.buffer = frame.indirect_buffer;
  edge.producer_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  edge.consumer_stage = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
  edge.consumer_access = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
  main_pass.buffer_edges = {edge};
  main_pass.user_data = &ctx;

  std::vector<GraphNode> nodes;
  nodes.reserve(2U);
  nodes.push_back(GraphNode::from_compute(cull));
  nodes.push_back(GraphNode::from_render(main_pass));

  const CompiledGraph compiled = compile_graph(nodes);
  execute_graph(command_buffer, nodes, compiled, &gpu_driven_render_cb,
                &gpu_driven_compute_cb);
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)frame;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

namespace {

//! Fullscreen-record shim for execute_graph: user_data carries a
//! FullscreenCtx (renderer + pass + set), the callback records the draw.
struct FullscreenCtx {
  const VulkanRenderer* self;
  const VulkanRenderer::FullscreenPass* pass;
  VkDescriptorSet set0;
};

void fullscreen_render_cb(VkCommandBuffer cb, const GraphPass& pass,
                          void* user_data) {
  auto& fx = *static_cast<FullscreenCtx*>(user_data);
  (void)pass;
  (void)fx.self->record_fullscreen_draw(cb, *fx.pass, fx.set0);
}

}  // namespace

VulkanRenderer::HiZDepthSource VulkanRenderer::select_hiz_depth_source(
    bool compose, HiZDepthSource hdr_depth, HiZDepthSource swapchain_depth) {
  const auto usable = [](const HiZDepthSource& d) {
    return d.available() && d.sampleable;
  };
  if (compose) {
    // Sampleability decides whether the reduction may read it at all. When the
    // HDR depth is unusable we report unavailable rather than falling back to
    // the swapchain's, which the scene did not write this frame.
    return usable(hdr_depth) ? hdr_depth : HiZDepthSource{};
  }
  return usable(swapchain_depth) ? swapchain_depth : HiZDepthSource{};
}

::warploom::core::Result<void> VulkanRenderer::record_fullscreen_draw(
    VkCommandBuffer command_buffer, const FullscreenPass& pass,
    VkDescriptorSet set0) {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || pass.pipeline == VK_NULL_HANDLE ||
      pass.pipeline_layout == VK_NULL_HANDLE) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  // Dynamic viewport/scissor: safe inside an active render pass (graph
  // callback path) and idempotent before one (direct path).
  VkViewport viewport{};
  viewport.width = static_cast<float>(pass.width);
  viewport.height = static_cast<float>(pass.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {pass.width, pass.height};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pass.pipeline);
  if (set0 != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pass.pipeline_layout, 0, 1, &set0, 0, nullptr);
  }
  if (pass.push_data != nullptr && pass.push_size > 0U) {
    vkCmdPushConstants(command_buffer, pass.pipeline_layout,
                       pass.push_stage_flags, 0U, pass.push_size,
                       pass.push_data);
  }
  vkCmdDraw(command_buffer, pass.vertex_count, pass.instance_count, 0, 0);
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)pass;
  (void)set0;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::record_fullscreen_pass(
    VkCommandBuffer command_buffer, const FullscreenPass& pass,
    VkDescriptorSet set0) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || pass.render_pass == VK_NULL_HANDLE ||
      pass.framebuffer == VK_NULL_HANDLE || pass.width == 0U ||
      pass.height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  VkRenderPassBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin.renderPass = pass.render_pass;
  begin.framebuffer = pass.framebuffer;
  begin.renderArea.extent = {pass.width, pass.height};
  begin.clearValueCount = pass.clear_value_count;
  begin.pClearValues = pass.clear_values;
  vkCmdBeginRenderPass(command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);

  const auto draw = record_fullscreen_draw(command_buffer, pass, set0);
  vkCmdEndRenderPass(command_buffer);
  return draw;
#else
  (void)command_buffer;
  (void)pass;
  (void)set0;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::record_sky_pre_draw(
    VkCommandBuffer command_buffer, const VulkanPbrScene& scene) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  if (scene.sky_pipeline == VK_NULL_HANDLE) {
    return ::warploom::core::Result<void>::ok();  // Optional pass: no-op.
  }

  struct SkyPush {
    std::array<float, 4> camera_position;  // xyz eye, w tan_half_fov
    std::array<float, 4> forward;          // xyz forward, w aspect
    std::array<float, 4> right;
    std::array<float, 4> up;
  } sky_push{};
  sky_push.camera_position = scene.sky_view.camera_position;
  sky_push.forward = scene.sky_view.forward;
  sky_push.right = scene.sky_view.right;
  sky_push.up = scene.sky_view.up;

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    scene.sky_pipeline);
  if (scene.sky_set != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scene.sky_pipeline_layout, 0, 1, &scene.sky_set,
                            0, nullptr);
  }
  vkCmdPushConstants(command_buffer, scene.sky_pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(sky_push), &sky_push);
  vkCmdDraw(command_buffer, 3, 1, 0, 0);
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)scene;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<std::uint32_t> VulkanRenderer::begin_frame() {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_) return ::warploom::core::Result<std::uint32_t>::error(::warploom::core::RuntimeError::vulkan_not_available);

  auto& frame = frames_[current_frame_];
  frame_begin_ns_ = ::warploom::core::SteadyClock::now_ns();
  if (gpu_timing_enabled_) {
    resolve_gpu_timestamps(current_frame_);
  }
  if (timeline_pacing_) {
    // Timeline pacing: this frame's slot is safe when the timeline has passed
    // the value this slot last signaled (one frame in flight per slot).
    // Throttle only when every slot has been used at least once; otherwise
    // the arithmetic would underflow and wait on an unreachable value.
    const std::uint64_t frame_slots = frames_.size();
    if (frame_counter_ >= frame_slots) {
      const std::uint64_t wait_value = frame_counter_ + 1U - frame_slots;
      VkSemaphoreWaitInfo wait_info{};
      wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &timeline_semaphore_;
      wait_info.pValues = &wait_value;
      if (vkWaitSemaphores(device_, &wait_info, UINT64_MAX) != VK_SUCCESS) {
        return ::warploom::core::Result<std::uint32_t>::error(::warploom::core::RuntimeError::vulkan_not_available);
      }
    }
  } else {
    vkWaitForFences(device_, 1, &frame.in_flight_fence, VK_TRUE, UINT64_MAX);
  }
  frame.frame_in_flight = false;

  std::uint32_t image_index = 0;
  VkResult result = vkAcquireNextImageKHR(device_, swapchain_->swapchain(), UINT64_MAX,
      frame.image_available_semaphore, VK_NULL_HANDLE, &image_index);

  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    return ::warploom::core::Result<std::uint32_t>::error(::warploom::core::RuntimeError::invalid_config);
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    return ::warploom::core::Result<std::uint32_t>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  if (image_index >= images_in_flight_.size()) {
    return ::warploom::core::Result<std::uint32_t>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  if (images_in_flight_[image_index] && images_in_flight_[image_index] != frame.in_flight_fence) {
    vkWaitForFences(device_, 1, &images_in_flight_[image_index], VK_TRUE, UINT64_MAX);
  }

  if (timeline_pacing_) {
    // Per-image reuse: wait until the exact frame that last rendered this
    // image has completed (timeline reached that frame's signal value).
    const std::uint64_t last_frame = image_last_frame_[image_index];
    if (last_frame > 0) {
      VkSemaphoreWaitInfo wait_info{};
      wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &timeline_semaphore_;
      wait_info.pValues = &last_frame;
      if (vkWaitSemaphores(device_, &wait_info, UINT64_MAX) != VK_SUCCESS) {
        return ::warploom::core::Result<std::uint32_t>::error(::warploom::core::RuntimeError::vulkan_not_available);
      }
    }
  } else {
    if (images_in_flight_[image_index] && images_in_flight_[image_index] != frame.in_flight_fence) {
      vkWaitForFences(device_, 1, &images_in_flight_[image_index], VK_TRUE, UINT64_MAX);
    }
  }

  if (!timeline_pacing_) {
    vkResetFences(device_, 1, &frame.in_flight_fence);
  }
  vkResetCommandBuffer(frame.command_buffer, 0);
  if (!timeline_pacing_) {
    images_in_flight_[image_index] = frame.in_flight_fence;
  }
  acquired_image_index_ = image_index;
  frame_acquired_ = true;
  return ::warploom::core::Result<std::uint32_t>::ok(image_index);
#else
  return ::warploom::core::Result<std::uint32_t>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::record_commands(
    std::uint32_t image_index, VkFramebuffer framebuffer,
    std::uint32_t width, std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || image_index >= swapchain_->image_count()) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  auto& frame = frames_[current_frame_];
  VkCommandBuffer cb = frame.command_buffer;

  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &begin_info);

  // GPU timestamps: reset this slot's pair and write the frame-start stamp.
  // The begin_frame wait above guarantees the previous submit on this slot
  // completed, so reset and resolve are both race-free.
  if (gpu_timing_enabled_ && timestamp_pool_ != VK_NULL_HANDLE) {
    const std::uint32_t base = current_frame_ * 2U;
    vkCmdResetQueryPool(cb, timestamp_pool_, base, 2U);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp_pool_,
                        base);
  }

  // Application pre-pass hook: independent earlier passes (shadow-map depth
  // pre-pass, compute) recorded before the main render pass. Owns its own
  // render-pass begin/end; a false return fails the frame.
  if (frame_pre_pass_callback_ != nullptr &&
      !frame_pre_pass_callback_(cb, width, height, frame_pre_pass_user_data_)) {
    (void)vkEndCommandBuffer(cb);
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  // HDR compose: the scene renders into an HDR intermediate owned by the
  // renderer, and the compose chain writes the swapchain image afterwards.
  // The application's scene callback therefore stops owning presentation.
  const bool compose = config_.enable_hdr_compose &&
                       ensure_compose_resources(width, height).is_ok() &&
                       compose_ready_;

  // HDR intermediate pass target when composing, else the swapchain.
  VkRenderPass scene_pass = render_pass_;
  VkFramebuffer scene_framebuffer = framebuffer;
  if (compose) {
    scene_pass = hdr_target_.render_pass();
    scene_framebuffer = hdr_target_.framebuffer();
    VkImageMemoryBarrier to_color{};
    to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // The HDR target's color attachment declares initialLayout UNDEFINED,
    // which is also its layout before the first frame of each resize.
    to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = hdr_target_.image();
    to_color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_color.srcAccessMask = 0;
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0U,
                         0U, nullptr, 0U, nullptr, 1U, &to_color);
  }

  VkRenderPassBeginInfo rp_info{};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp_info.renderPass = scene_pass;
  rp_info.framebuffer = scene_framebuffer;
  rp_info.renderArea.offset = {0, 0};
  rp_info.renderArea.extent = {width, height};

  VkClearValue clear_values[2];
  clear_values[0].color = {{config_.clear_color_r, config_.clear_color_g, config_.clear_color_b, config_.clear_color_a}};
  clear_values[1].depthStencil = {config_.clear_depth, config_.clear_depth_stencil};
  rp_info.clearValueCount = 2;
  rp_info.pClearValues = clear_values;

  vkCmdBeginRenderPass(cb, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.x = 0.0f; viewport.y = 0.0f;
  viewport.width = static_cast<float>(width);
  viewport.height = static_cast<float>(height);
  viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cb, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = {width, height};
  vkCmdSetScissor(cb, 0, 1, &scissor);

  if (scene_record_callback_) {
    // Application-owned scene path: the callback records whatever it owns
    // (PBR scene, GPU-driven frame, post chains) inside this render pass.
    if (!scene_record_callback_(cb, width, height, scene_record_user_data_)) {
      vkCmdEndRenderPass(cb);
      (void)vkEndCommandBuffer(cb);
      if (pending_hiz_frame_) {
        hiz_state_.discard_frame(pending_hiz_token_);
        pending_hiz_frame_ = false;
      }
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
  } else if (pipeline_) {
    // Built-in demo: three-vertex triangle (no scene callback installed).
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdDraw(cb, 3, 1, 0, 0);
  }

  vkCmdEndRenderPass(cb);

  // HDR compose: bloom (optional) then tonemap+FXAA into the swapchain
  // image. Runs after the scene pass and before the end stamp, so the
  // timestamp still covers the whole frame.
  if (compose) {
    // Layout tracking is compile_graph's job: each stage declares what it
    // reads and writes, and the compiler emits the transitions. The scene
    // pass is outside the graph, so record_compose_chain seeds the first
    // sample of each image from that pass's declared final layout.
    record_compose_chain(cb, render_pass_, framebuffer, width, height);
  }

  // End stamp: after the main render pass, before presentation commands.
  if (gpu_timing_enabled_ && timestamp_pool_ != VK_NULL_HANDLE) {
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        timestamp_pool_, current_frame_ * 2U + 1U);
  }

  const HiZDepthSource hiz_depth = select_hiz_depth_source(
      compose,
      {hdr_target_.depth_image(), hdr_target_.depth_view(),
       hdr_target_.depth_is_sampleable()},
      {render_pass_resource_ != nullptr ? render_pass_resource_->depth_image()
                                        : VK_NULL_HANDLE,
       render_pass_resource_ != nullptr ? render_pass_resource_->depth_view()
                                        : VK_NULL_HANDLE,
       render_pass_resource_ != nullptr &&
           render_pass_resource_->depth_is_sampleable()});
  if (hiz_enabled_ && (hiz_direct_enabled_ || hiz_record_callback_) &&
      hiz_depth.available()) {
    pending_hiz_token_ = hiz_state_.begin_frame();
    const std::uint32_t destination_index = pending_hiz_token_.write_index;
    const std::uint32_t previous_index = pending_hiz_token_.previous_index;
    HiZFrameRecord record{};
    record.token = pending_hiz_token_;
    record.depth_image = hiz_depth.image;
    record.depth_view = hiz_depth.view;
    record.depth_is_sampleable = hiz_depth.sampleable;
    record.destination_initialized = hiz_pyramid_initialized_[destination_index];
    record.previous_pyramid = pending_hiz_token_.has_previous ? hiz_pyramids_[previous_index].get() : nullptr;
    record.destination_pyramid = hiz_pyramids_[destination_index].get();
    record.render_width = width;
    record.render_height = height;
    record.tile_size = hiz_state_.tile_size();
    record.levels = hiz_state_.levels();
    if (hiz_direct_enabled_ && !record_hiz_reduction(cb, record)) {
      hiz_state_.discard_frame(pending_hiz_token_);
      pending_hiz_frame_ = false;
      vkEndCommandBuffer(cb);
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::vulkan_not_available);
    }
    if (hiz_record_callback_ && !hiz_record_callback_(cb, record, hiz_record_user_data_)) {
      hiz_state_.discard_frame(pending_hiz_token_);
      pending_hiz_frame_ = false;
      vkEndCommandBuffer(cb);
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
    pending_hiz_frame_ = true;
    pending_hiz_destination_index_ = destination_index;
  }

  if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
    if (pending_hiz_frame_) {
      hiz_state_.discard_frame(pending_hiz_token_);
      pending_hiz_frame_ = false;
    }
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::vulkan_not_available);
  }

  return ::warploom::core::Result<void>::ok();
#else
  (void)image_index; (void)framebuffer; (void)width; (void)height;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}


::warploom::core::Result<void> VulkanRenderer::submit_frame() {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || !frame_acquired_ ||
      acquired_image_index_ >= render_finished_semaphores_.size()) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  auto& frame = frames_[current_frame_];
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  VkSemaphore wait_semaphores[] = {frame.image_available_semaphore};
  VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = wait_semaphores;
  submit_info.pWaitDstStageMask = wait_stages;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &frame.command_buffer;
  const VkSemaphore render_finished_semaphore =
      render_finished_semaphores_[acquired_image_index_];
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &render_finished_semaphore;

  // Timeline pacing: signal the monotonic frame counter on submit.
  const std::uint64_t signal_frame = frame_counter_ + 1;

  if (synchronization2_) {
    auto submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(
        device_, "vkQueueSubmit2"));
    if (submit2) {
      VkCommandBufferSubmitInfo command_info{};
      command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
      command_info.commandBuffer = frame.command_buffer;
      VkSemaphoreSubmitInfo wait_info{};
      wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
      wait_info.semaphore = frame.image_available_semaphore;
      wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSemaphoreSubmitInfo signal_info{};
      signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
      signal_info.semaphore = render_finished_semaphores_[acquired_image_index_];
      signal_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo2 submit2_info{};
      submit2_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
      submit2_info.commandBufferInfoCount = 1;
      submit2_info.pCommandBufferInfos = &command_info;
      submit2_info.waitSemaphoreInfoCount = 1;
      submit2_info.pWaitSemaphoreInfos = &wait_info;
      submit2_info.signalSemaphoreInfoCount = 1;
      submit2_info.pSignalSemaphoreInfos = &signal_info;
      if (timeline_pacing_) {
        VkSemaphoreSubmitInfo timeline_signal_info{};
        timeline_signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        timeline_signal_info.semaphore = timeline_semaphore_;
        timeline_signal_info.value = signal_frame;
        timeline_signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        // Contiguous value array; decays to the required pointer type.
        VkSemaphoreSubmitInfo signal_infos[2] = {signal_info, timeline_signal_info};
        submit2_info.pSignalSemaphoreInfos = signal_infos;
        submit2_info.signalSemaphoreInfoCount = 2;
      }
      const VkResult result = submit2(graphics_queue_, 1, &submit2_info,
          timeline_pacing_ ? VK_NULL_HANDLE : frame.in_flight_fence);
      if (result != VK_SUCCESS) {
        if (pending_hiz_frame_) {
          hiz_state_.discard_frame(pending_hiz_token_);
          pending_hiz_frame_ = false;
        }
        return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
      }
      frame.frame_in_flight = true;
      if (pending_hiz_frame_) {
        hiz_pyramid_initialized_[pending_hiz_destination_index_] = true;
        hiz_state_.complete_frame(pending_hiz_token_);
        pending_hiz_frame_ = false;
      }
      if (timeline_pacing_) {
        frame_counter_ = signal_frame;
        image_last_frame_[acquired_image_index_] = signal_frame;
      }
      return ::warploom::core::Result<void>::ok();

    }
  }

  // Timeline pacing on the legacy path: attach signal values via pNext chain.
  VkSemaphore legacy_signal_semaphores[2] = {render_finished_semaphore, timeline_semaphore_};
  std::uint64_t legacy_signal_values[2] = {0, signal_frame};
  VkTimelineSemaphoreSubmitInfo timeline_submit_info{};
  if (timeline_pacing_) {
    timeline_submit_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_submit_info.signalSemaphoreValueCount = 2;
    timeline_submit_info.pSignalSemaphoreValues = legacy_signal_values;
    submit_info.pNext = &timeline_submit_info;
    submit_info.signalSemaphoreCount = 2;
    submit_info.pSignalSemaphores = legacy_signal_semaphores;
  }

  const VkResult result = vkQueueSubmit(graphics_queue_, 1, &submit_info,
      timeline_pacing_ ? VK_NULL_HANDLE : frame.in_flight_fence);
  if (result != VK_SUCCESS) {
    if (pending_hiz_frame_) {
      hiz_state_.discard_frame(pending_hiz_token_);
      pending_hiz_frame_ = false;
    }
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  frame.frame_in_flight = true;
  if (pending_hiz_frame_) {
    hiz_pyramid_initialized_[pending_hiz_destination_index_] = true;
    hiz_state_.complete_frame(pending_hiz_token_);
    pending_hiz_frame_ = false;
  }
  if (timeline_pacing_) {
    frame_counter_ = signal_frame;
    image_last_frame_[acquired_image_index_] = signal_frame;
  }
  return ::warploom::core::Result<void>::ok();
#else
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::present_frame() {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || !frame_acquired_) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  const VkSemaphore render_finished_semaphore =
      render_finished_semaphores_[acquired_image_index_];
  VkPresentInfoKHR present_info{};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &render_finished_semaphore;
  VkSwapchainKHR swapchains[] = {swapchain_->swapchain()};
  const std::uint32_t image_index = acquired_image_index_;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = swapchains;
  present_info.pImageIndices = &image_index;

  const VkResult result = vkQueuePresentKHR(present_queue_, &present_info);
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
    frame_acquired_ = false;
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  frame_acquired_ = false;
  if (frame_latency_enabled_ && frame_begin_ns_ >= 0) {
    const auto now_ns = ::warploom::core::SteadyClock::now_ns();
    if (now_ns >= frame_begin_ns_) {
      frame_latency_.record(static_cast<std::uint64_t>(now_ns - frame_begin_ns_));
      frame_latency_stats_ = frame_latency_.percentiles();
    }
    frame_begin_ns_ = kNoTimestamp;
  }
  current_frame_ = (current_frame_ + 1) % static_cast<std::uint32_t>(frames_.size());
  ++frame_count_;
  return ::warploom::core::Result<void>::ok();
#else
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::end_frame() {
  auto submit_result = submit_frame();
  if (!submit_result.is_ok()) return submit_result;
  return present_frame();
}

const ::warploom::core::LatencyStats& VulkanRenderer::frame_latency_stats() {
  return frame_latency_stats_;
}

HiZGraphPlan VulkanRenderer::make_hiz_graph_plan(
    const HiZFrameRecord& record) const {
  HiZGraphPlan plan;
  if (record.destination_pyramid == nullptr || record.levels == 0U) {
    return plan;
  }

#ifdef OMNICPP_HAS_VULKAN
  constexpr std::uint32_t kShaderRead = VK_ACCESS_SHADER_READ_BIT;
  constexpr std::uint32_t kShaderWrite = VK_ACCESS_SHADER_WRITE_BIT;
  constexpr std::uint32_t kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  constexpr std::uint32_t kDepthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
  constexpr std::uint32_t kDepthWrite = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  constexpr std::uint32_t kLateFragment = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
#else
  constexpr std::uint32_t kShaderRead = 0x20U;
  constexpr std::uint32_t kShaderWrite = 0x40U;
  constexpr std::uint32_t kCompute = 0x20U;
  constexpr std::uint32_t kDepthAspect = 0x2U;
  constexpr std::uint32_t kDepthWrite = 0x200U;
  constexpr std::uint32_t kLateFragment = 0x2000U;
#endif

  const std::uint32_t levels = std::min(record.levels,
                                        record.destination_pyramid->levels());
  plan.passes.reserve(levels + (record.token.has_previous ? 1U : 0U));
  for (std::uint32_t level = 0; level < levels; ++level) {
    GraphComputePass pass{};
    pass.name = "hiz_reduce";
    const std::uint32_t mip_width =
        std::max(1U, record.destination_pyramid->width() >> level);
    const std::uint32_t mip_height =
        std::max(1U, record.destination_pyramid->height() >> level);
    pass.group_count_x = (mip_width + 7U) / 8U;
    pass.group_count_y = (mip_height + 7U) / 8U;
    pass.image_uses.push_back({record.destination_pyramid->image(), level, 1U, 0U,
                               VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               kShaderWrite, kCompute,
                               record.destination_initialized
                                   ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED});
    if (level == 0U && record.depth_image != VK_NULL_HANDLE) {
      pass.image_uses.push_back({record.depth_image, 0U, 1U, kDepthAspect,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 kShaderRead, kCompute,
                                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                 kDepthWrite, kLateFragment});
    } else if (level > 0U) {
      pass.image_uses.push_back({record.destination_pyramid->image(), level - 1U, 1U, 0U,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 kShaderRead, kCompute,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 kShaderRead, kCompute});
    }
    plan.passes.push_back(std::move(pass));
  }

  if (record.token.has_previous && record.previous_pyramid != nullptr) {
    GraphComputePass cull{};
    cull.name = "hiz_cull";
    cull.group_count_x = 1U;
    cull.group_count_y = 1U;
    cull.image_uses.push_back({record.previous_pyramid->image(), 0U,
                               record.previous_pyramid->levels(), 0U,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               kShaderRead, kCompute,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               kShaderRead, kCompute});
    plan.passes.push_back(std::move(cull));
  }
  return plan;
}

void VulkanRenderer::complete_hiz_frame(const HiZFrameToken& token) noexcept {
  hiz_state_.complete_frame(token);
}

void VulkanRenderer::discard_hiz_frame(const HiZFrameToken& token) noexcept {
  hiz_state_.discard_frame(token);
}

const VulkanHiZPyramid* VulkanRenderer::hiz_pyramid(std::uint32_t index) const noexcept {
  return index < 2U ? hiz_pyramids_[index].get() : nullptr;
}

VulkanHiZPyramid* VulkanRenderer::hiz_pyramid(std::uint32_t index) noexcept {
  return index < 2U ? hiz_pyramids_[index].get() : nullptr;
}

void VulkanRenderer::cleanup_hiz_pipeline_resources() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  // Descriptor sets/layouts must be released before the image views they
  // reference. The device is idle whenever this helper is called.
  if (hiz_reduction_pipeline_) {
    hiz_reduction_pipeline_->cleanup(device_);
    hiz_reduction_pipeline_.reset();
  }
  if (hiz_descriptor_manager_) {
    hiz_descriptor_manager_->cleanup();
    hiz_descriptor_manager_.reset();
  }
#endif
  hiz_reduction_layout_ = VK_NULL_HANDLE;
  hiz_reduction_sets_[0].clear();
  hiz_reduction_sets_[1].clear();
  hiz_pyramid_initialized_[0] = false;
  hiz_pyramid_initialized_[1] = false;
  hiz_direct_enabled_ = false;
}

void VulkanRenderer::record_hiz_graph_pass(
    VkCommandBuffer command_buffer, const GraphComputePass& pass, void* user_data) {
  auto* renderer = static_cast<VulkanRenderer*>(user_data);
  if (renderer != nullptr) renderer->record_hiz_dispatch(command_buffer, pass);
}

void VulkanRenderer::record_hiz_dispatch(
    VkCommandBuffer command_buffer, const GraphComputePass& pass) {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || active_hiz_record_ == nullptr ||
      !hiz_reduction_pipeline_) return;
  const auto& record = *active_hiz_record_;
  if (pass.image_uses.empty()) return;
  const std::uint32_t level = pass.image_uses.front().base_mip;
  if (level >= record.levels || level >= hiz_reduction_sets_[record.token.write_index].size()) {
    return;
  }
  const std::uint32_t mip_width =
      std::max(1U, record.destination_pyramid->width() >> level);
  const std::uint32_t mip_height =
      std::max(1U, record.destination_pyramid->height() >> level);
  const VkDescriptorSet set = hiz_reduction_sets_[record.token.write_index][level];
  const std::uint32_t push[4] = {
      record.render_width, record.render_height, record.tile_size, level};
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    hiz_reduction_pipeline_->pipeline());
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          hiz_reduction_pipeline_->pipeline_layout(), 0, 1,
                          &set, 0, nullptr);
  vkCmdPushConstants(command_buffer, hiz_reduction_pipeline_->pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
  vkCmdDispatch(command_buffer, (mip_width + 7U) / 8U,
                (mip_height + 7U) / 8U, 1U);
  ++hiz_dispatch_count_;
#else
  (void)command_buffer;
  (void)pass;
#endif
}
#ifdef OMNICPP_HAS_VULKAN
bool VulkanRenderer::record_hiz_reduction(
    VkCommandBuffer command_buffer, const HiZFrameRecord& record) {
  if (!command_buffer || !hiz_direct_enabled_ || !hiz_reduction_pipeline_ ||
      !hiz_reduction_layout_ || !record.destination_pyramid ||
      !render_pass_resource_ || !render_pass_resource_->depth_is_sampleable()) {
    return false;
  }
  const auto destination_index = record.token.write_index;
  if (destination_index >= 2U ||
      hiz_reduction_sets_[destination_index].size() < record.levels) {
    return false;
  }

  auto plan = make_hiz_graph_plan(record);
  if (plan.passes.size() < record.levels) return false;
  std::vector<GraphNode> nodes;
  nodes.reserve(record.levels);
  for (std::uint32_t level = 0; level < record.levels; ++level) {
    plan.passes[level].user_data = this;
    nodes.push_back(GraphNode::from_compute(plan.passes[level]));
  }
  const auto compiled = compile_graph(nodes);
  hiz_dispatch_count_ = 0U;
  active_hiz_record_ = &record;
  execute_graph(command_buffer, nodes, compiled, nullptr,
                &VulkanRenderer::record_hiz_graph_pass);
  active_hiz_record_ = nullptr;
  return hiz_dispatch_count_ == record.levels;
}
#endif

::warploom::core::Result<void> VulkanRenderer::recreate_hiz_resources(
    std::uint32_t render_width, std::uint32_t render_height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || !physical_device_ || render_width == 0U || render_height == 0U ||
      config_.hiz_tile_size == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  vkDeviceWaitIdle(device_);
  hiz_enabled_ = false;
  pending_hiz_frame_ = false;
  cleanup_hiz_pipeline_resources();
  for (auto& pyramid : hiz_pyramids_) {
    if (pyramid) {
      pyramid->cleanup(device_);
      pyramid.reset();
    }
  }
  if (hiz_allocator_) {
    hiz_allocator_->cleanup();
    hiz_allocator_.reset();
  }

  auto state_result = hiz_state_.configure(render_width, render_height,
                                           config_.hiz_tile_size,
                                           config_.hiz_levels);
  if (!state_result.is_ok()) return state_result;

  const std::uint32_t width = hiz_state_.pyramid_width();
  const std::uint32_t height = hiz_state_.pyramid_height();
  const std::uint32_t levels = hiz_state_.levels();

  auto allocator = std::make_unique<VulkanMemoryAllocator>();
  auto allocator_result = allocator->initialize(device_, physical_device_);
  if (!allocator_result.is_ok()) return allocator_result;

  auto first = std::make_unique<VulkanHiZPyramid>();
  auto first_result = first->create(device_, physical_device_, width, height,
                                    levels, allocator.get());
  if (!first_result.is_ok()) return first_result;

  auto second = std::make_unique<VulkanHiZPyramid>();
  auto second_result = second->create(device_, physical_device_, width, height,
                                      levels, allocator.get());
  if (!second_result.is_ok()) return second_result;

  hiz_allocator_ = std::move(allocator);
  hiz_pyramids_[0] = std::move(first);
  hiz_pyramids_[1] = std::move(second);

  if (!config_.hiz_reduction_shader_path.empty()) {
    hiz_descriptor_manager_ = std::make_unique<VulkanDescriptorManager>();
    auto descriptor_result = hiz_descriptor_manager_->initialize(device_);
    if (!descriptor_result.is_ok()) {
      cleanup_hiz_pipeline_resources();
      for (auto& pyramid : hiz_pyramids_) pyramid.reset();
      if (hiz_allocator_) hiz_allocator_.reset();
      return descriptor_result;
    }
    const std::vector<ReflectedBinding> bindings = {
        {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_COMPUTE_BIT},
        {0U, 1U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         VK_SHADER_STAGE_COMPUTE_BIT},
        {0U, 2U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_COMPUTE_BIT}};
    auto layout_result = hiz_descriptor_manager_->create_layout(bindings, 2U * levels);
    if (!layout_result.is_ok()) {
      cleanup_hiz_pipeline_resources();
      return ::warploom::core::Result<void>::error(layout_result.error());
    }
    hiz_reduction_layout_ = layout_result.value();

    hiz_reduction_pipeline_ = std::make_unique<VulkanPipeline>();
    auto shader_result = hiz_reduction_pipeline_->load_shader_stage_file(
        device_, config_.hiz_reduction_shader_path, "compute");
    if (!shader_result.is_ok()) {
      cleanup_hiz_pipeline_resources();
      return shader_result;
    }
    const VkPushConstantRange push_range{
        VK_SHADER_STAGE_COMPUTE_BIT, 0U, 4U * sizeof(std::uint32_t)};
    auto pipeline_layout_result = hiz_reduction_pipeline_->create_pipeline_layout(
        device_, &hiz_reduction_layout_, 1U, &push_range);
    if (!pipeline_layout_result.is_ok()) {
      cleanup_hiz_pipeline_resources();
      return pipeline_layout_result;
    }
    auto pipeline_result = hiz_reduction_pipeline_->create_compute_pipeline(
        device_, hiz_reduction_pipeline_->pipeline_layout());
    if (!pipeline_result.is_ok()) {
      cleanup_hiz_pipeline_resources();
      return pipeline_result;
    }

    for (std::uint32_t pyramid_index = 0; pyramid_index < 2U; ++pyramid_index) {
      auto& sets = hiz_reduction_sets_[pyramid_index];
      sets.reserve(levels);
      for (std::uint32_t level = 0; level < levels; ++level) {
        auto set_result = hiz_descriptor_manager_->allocate_set(hiz_reduction_layout_);
        if (!set_result.is_ok()) {
          cleanup_hiz_pipeline_resources();
          return ::warploom::core::Result<void>::error(set_result.error());
        }
        const VkImageView preceding = level == 0U
            ? render_pass_resource_->depth_view()
            : hiz_pyramids_[pyramid_index]->mip_view(level - 1U);
        if (!hiz_descriptor_manager_->write_image(
                set_result.value(), 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                hiz_pyramids_[pyramid_index]->sampler(),
                render_pass_resource_->depth_view(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).is_ok() ||
            !hiz_descriptor_manager_->write_image(
                set_result.value(), 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_NULL_HANDLE, hiz_pyramids_[pyramid_index]->mip_view(level),
                VK_IMAGE_LAYOUT_GENERAL).is_ok() ||
            !hiz_descriptor_manager_->write_image(
                set_result.value(), 2U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                hiz_pyramids_[pyramid_index]->sampler(), preceding,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).is_ok()) {
          cleanup_hiz_pipeline_resources();
          for (auto& pyramid : hiz_pyramids_) pyramid.reset();
          if (hiz_allocator_) hiz_allocator_.reset();
          return ::warploom::core::Result<void>::error(
              ::warploom::core::RuntimeError::vulkan_not_available);
        }
        sets.push_back(set_result.value());
      }
    }
    hiz_direct_enabled_ = true;
  }
  hiz_enabled_ = true;
  hiz_state_.invalidate(HiZInvalidation::resize);
  return ::warploom::core::Result<void>::ok();
#else
  (void)render_width;
  (void)render_height;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderer::resync_for_swapchain(
    const VulkanSwapchain& swapchain, const VulkanRenderPass& render_pass) {
  render_pass_resource_ = &render_pass;
  return resync_for_swapchain(swapchain, render_pass.render_pass());
}

::warploom::core::Result<void> VulkanRenderer::resync_for_swapchain(
    const VulkanSwapchain& swapchain, VkRenderPass render_pass) {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || !device_ || !swapchain.is_valid() || swapchain.image_count() == 0 ||
      !render_pass) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  if (frame_acquired_) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }

  vkDeviceWaitIdle(device_);
  swapchain_ = &swapchain;
  render_pass_ = render_pass;
  if (render_pass_resource_ != nullptr &&
      render_pass_resource_->render_pass() != render_pass) {
    render_pass_resource_ = nullptr;
  }
  hiz_state_.invalidate(HiZInvalidation::resize);
  if (hiz_enabled_) {
    auto hiz_result = recreate_hiz_resources(swapchain.extent_width(),
                                              swapchain.extent_height());
    if (!hiz_result.is_ok()) return hiz_result;
  }

  // Retire and rebuild per-image resources for the new image set.
  for (auto semaphore : render_finished_semaphores_) {
    if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
  }
  render_finished_semaphores_.assign(swapchain.image_count(), VK_NULL_HANDLE);
  images_in_flight_.assign(swapchain.image_count(), VK_NULL_HANDLE);
  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (auto& semaphore : render_finished_semaphores_) {
    if (vkCreateSemaphore(device_, &sem_info, nullptr, &semaphore) != VK_SUCCESS) {
      cleanup(device_);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
  }
  if (timeline_pacing_) {
    // Device is idle: no timeline work is pending, so per-image history resets.
    image_last_frame_.assign(swapchain.image_count(), 0);
  }
  current_frame_ = 0;
  acquired_image_index_ = 0;
  return ::warploom::core::Result<void>::ok();
#else
  (void)swapchain;
  (void)render_pass;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanRenderer::wait_idle() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device_) vkDeviceWaitIdle(device_);
#endif
}

void VulkanRenderer::cleanup(VkDevice device) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  VkDevice dev = device ? device : device_;
  if (dev) {
    vkDeviceWaitIdle(dev);
    cleanup_hiz_pipeline_resources();
    destroy_compose_resources();
    for (auto& frame : frames_) frame.cleanup(dev);
    for (auto semaphore : render_finished_semaphores_) {
      if (semaphore) vkDestroySemaphore(dev, semaphore, nullptr);
    }
    if (timeline_semaphore_) vkDestroySemaphore(dev, timeline_semaphore_, nullptr);
    if (timestamp_pool_ != VK_NULL_HANDLE) {
      vkDestroyQueryPool(dev, timestamp_pool_, nullptr);
      timestamp_pool_ = VK_NULL_HANDLE;
    }
    if (command_pool_) vkDestroyCommandPool(dev, command_pool_, nullptr);
  }
  frames_.clear();
  render_finished_semaphores_.clear();
  timeline_semaphore_ = nullptr;
  timeline_pacing_ = false;
  frame_counter_ = 0;
  image_last_frame_.clear();
  command_pool_ = nullptr;
  physical_device_ = nullptr;
  for (auto& pyramid : hiz_pyramids_) pyramid.reset();
  if (hiz_allocator_) hiz_allocator_.reset();
  hiz_enabled_ = false;
  hiz_state_.reset();
  device_ = nullptr;
  graphics_queue_ = nullptr;
  present_queue_ = nullptr;
  render_pass_ = nullptr;
  render_pass_resource_ = nullptr;
  pending_hiz_frame_ = false;
  pipeline_ = nullptr;
  pipeline_layout_ = nullptr;
  swapchain_ = nullptr;
  current_frame_ = 0;
  frame_count_ = 0;
  images_in_flight_.clear();
  timestamp_valid_.clear();
  gpu_timing_ = GpuTiming{};
  gpu_timing_enabled_ = false;
  initialized_ = false;
  acquired_image_index_ = 0;
  frame_acquired_ = false;
#else
  (void)device;
#endif
}

void VulkanRenderer::resolve_gpu_timestamps(std::uint32_t slot) noexcept {
#if !defined(OMNICPP_HAS_VULKAN)
  (void)slot;  // no query pools without a device: GPU timing stays disabled
#else
  if (!gpu_timing_enabled_ || !gpu_timing_.available ||
      timestamp_pool_ == VK_NULL_HANDLE || timestamp_period_ns_ <= 0.0f) {
    return;
  }
  const std::uint32_t base = slot * 2U;
  if (slot >= timestamp_valid_.size() || !timestamp_valid_[slot]) {
    timestamp_valid_[slot] = true;  // armed: written at record time this frame
    return;
  }
  std::uint64_t stamps[2] = {0U, 0U};
  const VkResult result = vkGetQueryPoolResults(
      device_, timestamp_pool_, base, 2U, sizeof(stamps), stamps,
      sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
  if (result != VK_SUCCESS) {
    return;  // VK_NOT_READY or device loss: keep the previous value
  }
  if (stamps[1] >= stamps[0]) {
    gpu_timing_.last_total_ticks = stamps[1] - stamps[0];
    gpu_timing_.last_total_ns =
        static_cast<double>(gpu_timing_.last_total_ticks) *
        static_cast<double>(timestamp_period_ns_);
    ++gpu_timing_.queries_resolved;
  }
#endif
}

::warploom::core::Result<VkCommandPool> VulkanRenderer::create_command_pool(
    VkDevice device, std::uint32_t queue_family_index) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) return ::warploom::core::Result<VkCommandPool>::error(::warploom::core::RuntimeError::vulkan_not_available);

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_index;

  VkCommandPool pool = nullptr;
  VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
  if (result != VK_SUCCESS || !pool) return ::warploom::core::Result<VkCommandPool>::error(::warploom::core::RuntimeError::vulkan_not_available);
  return ::warploom::core::Result<VkCommandPool>::ok(pool);
#else
  (void)device; (void)queue_family_index;
  return ::warploom::core::Result<VkCommandPool>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<VkCommandBuffer> VulkanRenderer::allocate_command_buffer(
    VkDevice device, VkCommandPool pool) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !pool) return ::warploom::core::Result<VkCommandBuffer>::error(::warploom::core::RuntimeError::vulkan_not_available);

  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;

  VkCommandBuffer cb = nullptr;
  VkResult result = vkAllocateCommandBuffers(device, &alloc_info, &cb);
  if (result != VK_SUCCESS || !cb) return ::warploom::core::Result<VkCommandBuffer>::error(::warploom::core::RuntimeError::vulkan_not_available);
  return ::warploom::core::Result<VkCommandBuffer>::ok(cb);
#else
  (void)device; (void)pool;
  return ::warploom::core::Result<VkCommandBuffer>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

} // namespace warploom::render
