//! @file vulkan_compose_chain.cpp
//! @brief HDR compose chain (bloom + tonemap/FXAA) as an ownable object.
//!
//! Bodies moved verbatim from VulkanRenderer (B3b, docs/roadmap.md); the only
//! edits are the class name, member access through the unique_ptr targets, and
//! the command-pool helpers, which are now free functions in
//! vulkan_command.hpp rather than renderer statics.

#include <warploom/render/vulkan_compose_chain.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <warploom/core/diagnostics.hpp>

#include "warploom/render/vulkan_command.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_render_graph.hpp"
#include "warploom/render/vulkan_fullscreen.hpp"

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

::warploom::core::Result<void> VulkanComposeChain::ensure(
    [[maybe_unused]] VkDevice device,
    [[maybe_unused]] VkPhysicalDevice physical_device,
    [[maybe_unused]] VkQueue graphics_queue,
    [[maybe_unused]] const ComposeChainConfig& config,
    [[maybe_unused]] VkRenderPass present_pass,
    [[maybe_unused]] VkFormat present_format,
    [[maybe_unused]] std::uint32_t width,
    [[maybe_unused]] std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  // Note: no enable_hdr_compose check. The renderer only constructs a chain
  // when compose is enabled, so "not enabled" is "no chain exists".
  // The original bodies read these straight off the renderer; as a standalone
  // object they arrive as parameters and must be captured before anything
  // below uses them. Missing this was a segfault on the first run: every
  // vkCreate* saw a null device.
  device_ = device;
  physical_device_ = physical_device;
  graphics_queue_ = graphics_queue;
  config_ = config;
  present_pass_ = present_pass;
  present_format_ = present_format;
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
    WARPLOOM_WARN("render",
                  "HDR compose disabled (%s). The scene renders straight to "
                  "the swapchain with clipped highlights.\n",
                  why);
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
  hdr_format_ = hdr_format;

  // One-time setup: allocator, descriptors, samplers, pipelines, black
  // texture. Size-dependent resources are (re)created below.
  if (!allocator_) {
    allocator_ = std::make_unique<VulkanMemoryAllocator>();
    if (!allocator_->initialize(device_, physical_device_).is_ok()) {
      return refuse("allocator init failed");
    }
  }
  if (!descriptor_manager_) {
    descriptor_manager_ = std::make_unique<VulkanDescriptorManager>();
    if (!descriptor_manager_->initialize(device_).is_ok()) {
      return refuse("descriptor manager init failed");
    }
    // ReflectedBinding is {set, binding, count, type, stage}: both samplers
    // live at set 0, bindings 0 (HDR input) and 1 (additive bloom input).
    auto layout = descriptor_manager_->create_layout({
        {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT},
        {0, 1, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT}}, 1U);
    if (!layout.is_ok()) return refuse("compose descriptor layout failed");
    compose_layout_ = layout.value();
    auto set = descriptor_manager_->allocate_set(compose_layout_);
    if (!set.is_ok()) return refuse("compose descriptor set failed");
    compose_set_ = set.value();

    // Only when bloom is on: these sets are otherwise dead weight, and
    // allocating them unconditionally exhausted the pool and silently
    // disabled compose altogether.
    if (config_.enable_bloom) {
      // Capacity 2: one set per bloom stage, sharing this single-binding
      // layout. create_layout sizes the pool from sets_to_reserve, so
      // allocating two sets from a layout reserved for one fails.
      auto stage_layout = descriptor_manager_->create_layout({
          {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           VK_SHADER_STAGE_FRAGMENT_BIT}}, 2U);
      if (!stage_layout.is_ok()) return refuse("bloom stage layout failed");
      bloom_stage_layout_ = stage_layout.value();
      auto down_set =
          descriptor_manager_->allocate_set(bloom_stage_layout_);
      if (!down_set.is_ok()) return refuse("bloom downsample set failed");
      bloom_down_set_ = down_set.value();
      auto up_set =
          descriptor_manager_->allocate_set(bloom_stage_layout_);
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
    if (auto pool = warploom::render::create_command_pool(
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
      if (auto cb = warploom::render::allocate_command_buffer(device_, pool.value());
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
  const std::uint32_t bw = std::max(1U, width / static_cast<std::uint32_t>(config_.bloom_downscale));
  const std::uint32_t bh = std::max(1U, height / static_cast<std::uint32_t>(config_.bloom_downscale));

  hdr_target_->cleanup();
  bloom_target_->cleanup();
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
  if (!hdr_target_->create(device_, physical_device_, hdr_format_, width,
                          height, allocator_.get(),
                          VK_IMAGE_USAGE_SAMPLED_BIT)
           .is_ok() ||
      !hdr_target_->create_depth(device_, physical_device_, VK_FORMAT_D32_SFLOAT)
           .is_ok() ||
      // COLOR_ATTACHMENT_OPTIMAL: this target is sampled immediately, and
      // letting the compose chain own the transition to SHADER_READ_ONLY keeps
      // one explicit barrier per image.
      !hdr_target_->create_render_pass(
           device_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
           .is_ok() ||
      !hdr_target_->create_framebuffer(device_).is_ok()) {
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
    if (!bloom_target_->create(device_, physical_device_, hdr_format_, bw, bh,
                               allocator_.get(), VK_IMAGE_USAGE_SAMPLED_BIT)
             .is_ok() ||
        !bloom_target_->create_render_pass(
             device_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
             .is_ok() ||
        !bloom_target_->create_framebuffer(device_).is_ok()) {
      return refuse("bloom target creation failed");
    }
    if (!bloom_down_pipeline_
             ->create_graphics_pipeline(device_, bloom_target_->render_pass(),
                                       hdr_format_,
                                       bloom_down_layout, false, false, false)
             .is_ok() ||
        !bloom_up_pipeline_
             ->create_graphics_pipeline(device_, hdr_target_->render_pass(),
                                       hdr_format_,
                                       bloom_up_layout, false, false, false)
             .is_ok()) {
      return refuse("bloom pipeline creation failed");
    }
  }

  // Bind the two sampler slots: HDR input and the bloom input (the black
  // texture when bloom is off).
  if (!descriptor_manager_
           ->write_image(compose_set_, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         hdr_sampler_, hdr_target_->image_view(),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    return refuse("HDR descriptor write failed");
  }
  const VkImageView bloom_view =
      config_.enable_bloom ? bloom_target_->image_view() : black_texture_.view;
  if (!descriptor_manager_
           ->write_image(compose_set_, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         linear_sampler_, bloom_view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    return refuse("bloom descriptor write failed");
  }
  // Each bloom stage reads its own input at binding 0. Only when bloom is on:
  // the sets do not exist otherwise, and writing to a null set fails.
  if (config_.enable_bloom &&
      (!descriptor_manager_
           ->write_image(bloom_down_set_, 0U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, hdr_sampler_,
                         hdr_target_->image_view(),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok() ||
       !descriptor_manager_
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

void VulkanComposeChain::record(VkCommandBuffer command_buffer,
                                          VkRenderPass target_pass,
                                          VkFramebuffer target_framebuffer,
                                          std::uint32_t width,
                                          std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!compose_ready_ || hdr_sampler_ == VK_NULL_HANDLE) return;

  // Where the scene pass left the HDR image. It is recorded outside this graph,
  // so each stage's first sample of it declares this as the producer state.
  const VkImageLayout hdr_from_scene = hdr_target_->color_final_layout();

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
    down.target = bloom_target_.get();
    down.pass = bloom_target_->render_pass();
    down.framebuffer = bloom_target_->framebuffer();
    down.width = bloom_target_->width();
    down.height = bloom_target_->height();
    down.set = bloom_down_set_;
    down.samples[0].image = hdr_target_->image();
    down.samples[0].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    down.samples[0].initial_layout = hdr_from_scene;
    down.samples[0].initial_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    down.samples[0].initial_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    down.sample_count = 1;
    stages.push_back(down);

    ComposeStage up{};
    up.pipeline = bloom_up_pipeline_.get();
    up.target = hdr_target_.get();
    up.pass = hdr_target_->render_pass();
    up.framebuffer = hdr_target_->framebuffer();
    up.width = width;
    up.height = height;
    up.set = bloom_up_set_;
    up.samples[0].image = bloom_target_->image();
    up.samples[0].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    up.samples[0].initial_layout = bloom_target_->color_final_layout();
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
  tonemap.target = nullptr;  // presentation target
  tonemap.pass = target_pass;
  tonemap.framebuffer = target_framebuffer;
  tonemap.width = width;
  tonemap.height = height;
  tonemap.push = &push;
  tonemap.push_size = sizeof(push);
  tonemap.set = compose_set_;
  tonemap.samples[0].image = hdr_target_->image();
  tonemap.samples[0].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // With no bloom stage, the scene pass is this stage's only producer. With
  // bloom, the upsample rewrote hdr and the compiler already tracked it.
  tonemap.samples[0].initial_layout = hdr_from_scene;
  tonemap.samples[0].initial_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  tonemap.samples[0].initial_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  tonemap.samples[1].image = bloom_target_->image();
  tonemap.samples[1].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (bloom_target_->image() != VK_NULL_HANDLE) {
    tonemap.samples[1].initial_layout = bloom_target_->color_final_layout();
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
                  (void)record_fullscreen_draw(cb, draw, stage->set);
                },
                nullptr);
#else
  (void)command_buffer; (void)target_pass; (void)target_framebuffer;
  (void)width; (void)height;
#endif
}

void VulkanComposeChain::destroy() noexcept {
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
  bloom_target_->cleanup();
  hdr_target_->cleanup();
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
  descriptor_manager_.reset();
  allocator_.reset();
#endif
}

VulkanComposeChain::~VulkanComposeChain() { destroy(); }

}  // namespace warploom::render
