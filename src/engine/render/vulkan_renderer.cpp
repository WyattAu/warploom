/**
 * @file vulkan_renderer.cpp
 * @brief Vulkan renderer implementation: command buffers, frame sync, draw loop.
 */

#include "engine/render/vulkan_renderer.hpp"
#include "engine/core/clock.hpp"
#include <algorithm>
#include <cstring>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace omnicpp::render {

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

omnicpp::core::Result<void> VulkanRenderer::initialize(
    VulkanContext& context,
    const VulkanSwapchain& swapchain,
    const VulkanRenderPass& render_pass,
    const RendererConfig& config) {
#ifdef OMNICPP_HAS_VULKAN
  if (!context.is_initialized() || !context.device()) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  device_ = context.device();
  physical_device_ = context.physical_device();
  graphics_queue_ = context.graphics_queue();
  present_queue_ = context.present_queue();
  render_pass_ = render_pass.render_pass();
  render_pass_resource_ = &render_pass;
  swapchain_ = &swapchain;
  config_ = config;

  auto pool_result = create_command_pool(
      device_, static_cast<std::uint32_t>(context.queue_families().graphics_family));
  if (!pool_result.is_ok()) {
    return omnicpp::core::Result<void>::error(pool_result.error());
  }
  command_pool_ = pool_result.value();

  if (config.max_frames_in_flight == 0 || swapchain.image_count() == 0) {
    cleanup(device_);
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }

  frames_.resize(config.max_frames_in_flight);
  images_in_flight_.assign(swapchain.image_count(), VK_NULL_HANDLE);
  render_finished_semaphores_.assign(swapchain.image_count(), VK_NULL_HANDLE);
  // Timeline pacing decision up front: it changes which resources are created.
  timeline_pacing_ = timeline_pacing_requested_ && context.has_timeline_semaphores();
  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    auto cb_result = allocate_command_buffer(device_, command_pool_);
    if (!cb_result.is_ok()) {
      cleanup(device_);
      return omnicpp::core::Result<void>::error(cb_result.error());
    }
    frames_[i].command_buffer = cb_result.value();

    if (!timeline_pacing_) {
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      if (vkCreateFence(device_, &fence_info, nullptr, &frames_[i].in_flight_fence) != VK_SUCCESS) {
        cleanup(device_);
        return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
      }
    }
    // In timeline mode the per-frame fence is omitted entirely: the timeline
    // semaphore paces CPU-GPU, and a never-reset fence would be submitted in
    // the SIGNALED state (VUID-vkQueueSubmit-fence-00063).

    if (vkCreateSemaphore(device_, &sem_info, nullptr, &frames_[i].image_available_semaphore) != VK_SUCCESS) {
      cleanup(device_);
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
    }
  }
  for (auto& semaphore : render_finished_semaphores_) {
    const VkResult result = vkCreateSemaphore(device_, &sem_info, nullptr, &semaphore);
    if (result != VK_SUCCESS) {
      cleanup(device_);
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
    }
    image_last_frame_.assign(swapchain.image_count(), 0);
  }

  if (config_.enable_hiz &&
      (!render_pass_resource_ || !render_pass_resource_->depth_image() ||
       !render_pass_resource_->depth_is_sampleable())) {
    cleanup(device_);
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
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
  return omnicpp::core::Result<void>::ok();
#else
  (void)context; (void)swapchain; (void)render_pass; (void)config;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanRenderer::record_scene(
    VkCommandBuffer command_buffer, const VulkanScene& scene,
    std::uint32_t width, std::uint32_t height) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || !scene.pipeline || !scene.pipeline_layout ||
      width == 0U || height == 0U) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
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
  return omnicpp::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)scene;
  (void)width;
  (void)height;
  return omnicpp::core::Result<void>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanRenderer::record_pbr_scene(
    VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
    std::uint32_t width, std::uint32_t height) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || !scene.pipeline || !scene.pipeline_layout ||
      width == 0U || height == 0U) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
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

  // 160-byte PBR ABI: view_projection (64) + model (64) + camera_position
  // (16) + material_index (4) + padding (12), matching the push blocks in
  // pbr_scene.{vert,frag}. Texture indices and shading factors are read by
  // the fragment stage from the set-2 material SSBO.
  struct PushConstants {
    SceneMatrix view_projection;
    SceneMatrix model;
    std::array<float, 4> camera_position;
    std::uint32_t material_index{0xffffffffU};
    std::uint32_t pad[3]{0, 0, 0};
  } push{};
  push.view_projection = scene.camera.view_projection;
  push.camera_position = scene.camera_position;

  constexpr std::uint32_t kInvalidMaterial = 0xffffffffU;
  if (scene.texture_set != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scene.pipeline_layout, 1, 1, &scene.texture_set, 0,
                            nullptr);
  }
  if (scene.material_set != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scene.pipeline_layout, 2, 1, &scene.material_set, 0,
                            nullptr);
  }

  constexpr VkShaderStageFlags kPushStages =
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT);
  for (const ScenePbrObject& object : scene.objects) {
    const SceneMesh* mesh_ptr = object.effective_mesh();
    if (mesh_ptr == nullptr || !mesh_ptr->is_drawable() ||
        object.material_index == kInvalidMaterial) {
      continue;
    }
    const SceneMesh& mesh = *mesh_ptr;
    push.model = object.model;
    push.material_index = object.material_index;
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scene.pipeline_layout, 0, 1,
                            &mesh.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer, scene.pipeline_layout, kPushStages, 0,
                       sizeof(push), &push);
    vkCmdBindIndexBuffer(command_buffer, mesh.index_buffer,
                         mesh.index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(command_buffer, mesh.index_count, 1, 0, 0, 0);
  }
  return omnicpp::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)scene;
  (void)width;
  (void)height;
  return omnicpp::core::Result<void>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<std::uint32_t> VulkanRenderer::begin_frame() {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_) return omnicpp::core::Result<std::uint32_t>::error(omnicpp::core::RuntimeError::vulkan_not_available);

  auto& frame = frames_[current_frame_];
  frame_begin_ns_ = omnicpp::core::SteadyClock::now_ns();
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
        return omnicpp::core::Result<std::uint32_t>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
    return omnicpp::core::Result<std::uint32_t>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    return omnicpp::core::Result<std::uint32_t>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  if (image_index >= images_in_flight_.size()) {
    return omnicpp::core::Result<std::uint32_t>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
        return omnicpp::core::Result<std::uint32_t>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
  return omnicpp::core::Result<std::uint32_t>::ok(image_index);
#else
  return omnicpp::core::Result<std::uint32_t>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanRenderer::record_commands(
    std::uint32_t image_index, VkFramebuffer framebuffer,
    std::uint32_t width, std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || image_index >= swapchain_->image_count()) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  auto& frame = frames_[current_frame_];
  VkCommandBuffer cb = frame.command_buffer;

  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &begin_info);

  VkRenderPassBeginInfo rp_info{};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp_info.renderPass = render_pass_;
  rp_info.framebuffer = framebuffer;
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

  if (pipeline_) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdDraw(cb, 3, 1, 0, 0);
  }

  vkCmdEndRenderPass(cb);

  if (hiz_enabled_ && (hiz_direct_enabled_ || hiz_record_callback_) &&
      render_pass_resource_ && render_pass_resource_->depth_is_sampleable()) {
    pending_hiz_token_ = hiz_state_.begin_frame();
    const std::uint32_t destination_index = pending_hiz_token_.write_index;
    const std::uint32_t previous_index = pending_hiz_token_.previous_index;
    HiZFrameRecord record{};
    record.token = pending_hiz_token_;
    record.depth_image = render_pass_resource_->depth_image();
    record.depth_view = render_pass_resource_->depth_view();
    record.depth_is_sampleable = render_pass_resource_->depth_is_sampleable();
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
      return omnicpp::core::Result<void>::error(
          omnicpp::core::RuntimeError::vulkan_not_available);
    }
    if (hiz_record_callback_ && !hiz_record_callback_(cb, record, hiz_record_user_data_)) {
      hiz_state_.discard_frame(pending_hiz_token_);
      pending_hiz_frame_ = false;
      vkEndCommandBuffer(cb);
      return omnicpp::core::Result<void>::error(
          omnicpp::core::RuntimeError::invalid_config);
    }
    pending_hiz_frame_ = true;
    pending_hiz_destination_index_ = destination_index;
  }

  if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
    if (pending_hiz_frame_) {
      hiz_state_.discard_frame(pending_hiz_token_);
      pending_hiz_frame_ = false;
    }
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }

  return omnicpp::core::Result<void>::ok();
#else
  (void)image_index; (void)framebuffer; (void)width; (void)height;
  return omnicpp::core::Result<void>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}


omnicpp::core::Result<void> VulkanRenderer::submit_frame() {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || !frame_acquired_ ||
      acquired_image_index_ >= render_finished_semaphores_.size()) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
        return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
      return omnicpp::core::Result<void>::ok();

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
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
  return omnicpp::core::Result<void>::ok();
#else
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanRenderer::present_frame() {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || !frame_acquired_) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  frame_acquired_ = false;
  if (frame_latency_enabled_ && frame_begin_ns_ >= 0) {
    const auto now_ns = omnicpp::core::SteadyClock::now_ns();
    if (now_ns >= frame_begin_ns_) {
      frame_latency_.record(static_cast<std::uint64_t>(now_ns - frame_begin_ns_));
      frame_latency_stats_ = frame_latency_.percentiles();
    }
    frame_begin_ns_ = kNoTimestamp;
  }
  current_frame_ = (current_frame_ + 1) % static_cast<std::uint32_t>(frames_.size());
  ++frame_count_;
  return omnicpp::core::Result<void>::ok();
#else
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanRenderer::end_frame() {
  auto submit_result = submit_frame();
  if (!submit_result.is_ok()) return submit_result;
  return present_frame();
}

const omnicpp::core::LatencyStats& VulkanRenderer::frame_latency_stats() {
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

omnicpp::core::Result<void> VulkanRenderer::recreate_hiz_resources(
    std::uint32_t render_width, std::uint32_t render_height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || !physical_device_ || render_width == 0U || render_height == 0U ||
      config_.hiz_tile_size == 0U) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
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
      return omnicpp::core::Result<void>::error(layout_result.error());
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
          return omnicpp::core::Result<void>::error(set_result.error());
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
          return omnicpp::core::Result<void>::error(
              omnicpp::core::RuntimeError::vulkan_not_available);
        }
        sets.push_back(set_result.value());
      }
    }
    hiz_direct_enabled_ = true;
  }
  hiz_enabled_ = true;
  hiz_state_.invalidate(HiZInvalidation::resize);
  return omnicpp::core::Result<void>::ok();
#else
  (void)render_width;
  (void)render_height;
  return omnicpp::core::Result<void>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanRenderer::resync_for_swapchain(
    const VulkanSwapchain& swapchain, const VulkanRenderPass& render_pass) {
  render_pass_resource_ = &render_pass;
  return resync_for_swapchain(swapchain, render_pass.render_pass());
}

omnicpp::core::Result<void> VulkanRenderer::resync_for_swapchain(
    const VulkanSwapchain& swapchain, VkRenderPass render_pass) {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || !device_ || !swapchain.is_valid() || swapchain.image_count() == 0 ||
      !render_pass) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  if (frame_acquired_) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
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
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
    }
  }
  if (timeline_pacing_) {
    // Device is idle: no timeline work is pending, so per-image history resets.
    image_last_frame_.assign(swapchain.image_count(), 0);
  }
  current_frame_ = 0;
  acquired_image_index_ = 0;
  return omnicpp::core::Result<void>::ok();
#else
  (void)swapchain;
  (void)render_pass;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
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
    for (auto& frame : frames_) frame.cleanup(dev);
    for (auto semaphore : render_finished_semaphores_) {
      if (semaphore) vkDestroySemaphore(dev, semaphore, nullptr);
    }
    if (timeline_semaphore_) vkDestroySemaphore(dev, timeline_semaphore_, nullptr);
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
  initialized_ = false;
  acquired_image_index_ = 0;
  frame_acquired_ = false;
#else
  (void)device;
#endif
}

omnicpp::core::Result<VkCommandPool> VulkanRenderer::create_command_pool(
    VkDevice device, std::uint32_t queue_family_index) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) return omnicpp::core::Result<VkCommandPool>::error(omnicpp::core::RuntimeError::vulkan_not_available);

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_index;

  VkCommandPool pool = nullptr;
  VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
  if (result != VK_SUCCESS || !pool) return omnicpp::core::Result<VkCommandPool>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  return omnicpp::core::Result<VkCommandPool>::ok(pool);
#else
  (void)device; (void)queue_family_index;
  return omnicpp::core::Result<VkCommandPool>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<VkCommandBuffer> VulkanRenderer::allocate_command_buffer(
    VkDevice device, VkCommandPool pool) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !pool) return omnicpp::core::Result<VkCommandBuffer>::error(omnicpp::core::RuntimeError::vulkan_not_available);

  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;

  VkCommandBuffer cb = nullptr;
  VkResult result = vkAllocateCommandBuffers(device, &alloc_info, &cb);
  if (result != VK_SUCCESS || !cb) return omnicpp::core::Result<VkCommandBuffer>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  return omnicpp::core::Result<VkCommandBuffer>::ok(cb);
#else
  (void)device; (void)pool;
  return omnicpp::core::Result<VkCommandBuffer>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

} // namespace omnicpp::render
