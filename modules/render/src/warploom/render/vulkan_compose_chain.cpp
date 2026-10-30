//! @file vulkan_compose_chain.cpp
//! @brief HDR compose chain (bloom + tonemap/FXAA) as an ownable object.
//!
//! Bodies moved verbatim from VulkanRenderer (B3b, docs/roadmap.md); the only
//! edits are the class name, member access through the unique_ptr targets, and
//! the command-pool helpers, which are now free functions in
//! vulkan_command.hpp rather than renderer statics.

#include <warploom/render/vulkan_compose_chain.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
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
  // Exposure is a LIVE control (E2/R4: set_exposure writes it between
  // frames), but record_commands calls ensure() every frame and this copy
  // would silently reset it to the start-up value before each record -- which
  // is exactly what the two-exposure probe caught: setter applied, frames
  // unchanged, both records logged 1.0. Bloom's live toggle is the same class
  // of control and gets the same treatment: config_.enable_bloom seeds
  // bloom_enabled_ ONCE (below, guarded by "first ensure"), and the live
  // value survives every later ensure().
  const float live_exposure = config_.exposure;
  const bool live_bloom = bloom_enabled_;
  const bool first_ensure = !allocator_;
  config_ = config;
  // Only a LATER ensure preserves the live values. Preserving them on the
  // first ensure too -- which is what this did, because the bloom guard below
  // covered bloom and nothing covered exposure -- threw away the caller's
  // configured exposure and kept the default instead. It was invisible until a
  // 20x exposure change (0.05 -> 1.0) produced a BYTE-IDENTICAL frame: the
  // chain was never reading the exposure it had been handed.
  if (!first_ensure) {
    config_.exposure = live_exposure;
    config_.enable_bloom = live_bloom;
  }
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
         VK_SHADER_STAGE_FRAGMENT_BIT}}, 3U);
    if (!layout.is_ok()) return refuse("compose descriptor layout failed");
    compose_layout_ = layout.value();
    auto set = descriptor_manager_->allocate_set(compose_layout_);
    if (!set.is_ok()) return refuse("compose descriptor set failed");
    compose_set_ = set.value();
    // Two tonemap sets, not one: the bloom toggle binds whichever is already
    // written, so switching costs nothing at runtime. Capacity 2 matches.
    auto nobloom_set = descriptor_manager_->allocate_set(compose_layout_);
    if (!nobloom_set.is_ok()) return refuse("compose (no-bloom) set failed");
    compose_set_nobloom_ = nobloom_set.value();
    // Third tonemap set: same shape, binding 0 later rewritten to the external
    // source (see set_external_source). All three are written before the first
    // frame so a mode switch binds rather than writes.
    auto external_set = descriptor_manager_->allocate_set(compose_layout_);
    if (!external_set.is_ok()) return refuse("compose (external) set failed");
    compose_set_external_ = external_set.value();

    // The bloom stage set is allocated UNCONDITIONALLY. The original code
    // allocated it only when bloom was on, "because otherwise they are dead
    // weight" -- which is precisely the lazy-create anti-pattern that made the
    // runtime toggle a rebuild. One extra combined-image-sampler descriptor is
    // not a memory concern; a toggle that has to build pipelines at runtime is.
    auto stage_layout = descriptor_manager_->create_layout({
        {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT}}, 1U);
    if (!stage_layout.is_ok()) return refuse("bloom stage layout failed");
    bloom_stage_layout_ = stage_layout.value();
    auto down_set = descriptor_manager_->allocate_set(bloom_stage_layout_);
    if (!down_set.is_ok()) return refuse("bloom downsample set failed");
    bloom_down_set_ = down_set.value();
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
    return true;
  };

  // Bloom pipelines are built too, so the toggle stays a re-record rather than
  // a rebuild. Failure here is NOT fatal: a compose_shader_dir without the
  // bloom shaders still yields a working tonemap, and set_bloom_enabled(true)
  // is refused rather than silently ignored. That distinction matters -- the
  // original code returned false from a single combined lambda, so a missing
  // bloom shader took the whole HDR compose chain down with it.
  const auto build_bloom_pipelines = [&]() -> bool {
    VkDescriptorSetLayout stage_layouts[1] = {bloom_stage_layout_};
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
    return bloom_down_pipeline_
        ->create_pipeline_layout(device_, stage_layouts, 1U, nullptr)
        .is_ok();
  };

  // ---- size-dependent resources ---------------------------------------
  if (compose_ready_ && compose_width_ == width && compose_height_ == height) {
    return ::warploom::core::Result<void>::ok();
  }

  // Pipelines bake in the render pass and format, so they are rebuilt with
  // the targets rather than reused across a resize.
  const std::uint32_t bw = std::max(1U, width / static_cast<std::uint32_t>(config_.bloom_downscale));
  const std::uint32_t bh = std::max(1U, height / static_cast<std::uint32_t>(config_.bloom_downscale));

  // Direct members in the renderer became unique_ptrs here; construct before
  // first use (and again on every resize below).
  if (hdr_target_ == nullptr) hdr_target_ = std::make_unique<VulkanOffscreenTarget>();
  if (bloom_target_ == nullptr) bloom_target_ = std::make_unique<VulkanOffscreenTarget>();
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
  compose_ready_ = false;
  compose_width_ = width;
  compose_height_ = height;

  // R6b: the depth usage probe create_depth() used to do inline. The store
  // needs the usage BEFORE creating the image (adopt separates creation
  // from the render-pass shell), so the probe moves up here. Same rule:
  // D32_SFLOAT with SAMPLED only when the format supports it.
  bool depth_sampleable = false;
  {
    constexpr VkImageUsageFlags kBaseDepthUsage =
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageFormatProperties fmt_props{};
    if (vkGetPhysicalDeviceImageFormatProperties(
            physical_device_, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, kBaseDepthUsage, 0U, &fmt_props) !=
        VK_SUCCESS) {
      return refuse("HDR depth format unsupported");
    }
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_,
                                       VK_FORMAT_D32_SFLOAT,
                                       &format_properties);
    depth_sampleable = (format_properties.optimalTilingFeatures &
                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U;
    if (depth_sampleable) {
      VkImageFormatProperties sampled_props{};
      if (vkGetPhysicalDeviceImageFormatProperties(
              physical_device_, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D,
              VK_IMAGE_TILING_OPTIMAL,
              kBaseDepthUsage | VK_IMAGE_USAGE_SAMPLED_BIT, 0U,
              &sampled_props) != VK_SUCCESS) {
        depth_sampleable = false;
      }
    }
  }

  // Bloom shader/layout attempt, hoisted ahead of the plan: the skeleton
  // includes the bloom_down stage exactly when it could record, so the
  // store only creates a bloom image the chain can actually use. Failure
  // here is NOT fatal (same as before): the chain runs tonemap-only.
  const bool bloom_shaders_ok = build_bloom_pipelines();
  if (!bloom_shaders_ok) {
    WARPLOOM_WARN("render",
                  "bloom shaders unavailable; continuing tonemap-only "
                  "(no bloom image planned)\n");
  }

  // R6b: derive the superset plan, materialize it in the store, and adopt
  // the images into the render-pass shells. The hand-written target image
  // list is gone: no create()/bind_image() call names an HDR/bloom/depth
  // image anymore. SAMPLED on the color images: the compose chain samples
  // them, and so does anything the application reads back through capture.
  std::vector<TransientImageDesc> store_descs;
  if (!derive_compose_plan(width, height, bw, bh, hdr_format_,
                           depth_sampleable, bloom_shaders_ok, store_descs)) {
    return refuse("transient plan derivation failed");
  }
  if (!transient_store_
           .allocate(device_, physical_device_, allocator_.get(),
                     planned_resources_, planned_plan_, store_descs)
           .is_ok()) {
    return refuse("transient store allocation failed");
  }
  if (!hdr_target_
           ->adopt_color(device_,
                         transient_store_.image(
                             transient_kind_name(kKindHdrColor)),
                         transient_store_.view(transient_kind_name(kKindHdrColor)),
                         hdr_format_, width, height)
           .is_ok() ||
      !hdr_target_
           ->adopt_depth(device_,
                         transient_store_.image(
                             transient_kind_name(kKindHdrDepth)),
                         transient_store_.view(transient_kind_name(kKindHdrDepth)),
                         VK_FORMAT_D32_SFLOAT, depth_sampleable)
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

  // Bloom shell over the store's image, built when the shaders exist.
  // Quarter-resolution RGBA16F: cheap enough to always exist when usable
  // (docs/research/rendering-and-graphs.md, R7 -- the idTech 6 lesson that a
  // disabled feature's resources are still there, and only its COST is
  // skipped). Without shaders nothing is planned, the shell stays empty,
  // and the chain runs with bloom permanently off.
  bloom_available_ = false;
  if (!bloom_shaders_ok) {
    bloom_target_->cleanup();
  } else if (!bloom_target_
                    ->adopt_color(device_,
                                 transient_store_.image(
                                     transient_kind_name(kKindBloomColor)),
                                 transient_store_.view(
                                     transient_kind_name(kKindBloomColor)),
                                 hdr_format_, bw, bh)
                    .is_ok() ||
               !bloom_target_->create_render_pass(
                    device_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                    .is_ok() ||
               !bloom_target_->create_framebuffer(device_).is_ok()) {
    // Not fatal. Bloom is an enhancement; losing it must not take the HDR
    // tonemap with it.
    WARPLOOM_WARN("render",
                  "bloom target creation failed; bloom unavailable this "
                  "session (tonemap unaffected)\n");
    bloom_target_->cleanup();
  } else if (!bloom_down_pipeline_
                    ->create_graphics_pipeline(
                        device_, bloom_target_->render_pass(), hdr_format_,
                        bloom_down_pipeline_->pipeline_layout(), false, false,
                        false)
                    .is_ok()) {
    WARPLOOM_WARN("render",
                  "bloom pipeline creation failed; bloom unavailable this "
                  "session (tonemap unaffected)\n");
    bloom_target_->cleanup();
  } else {
    bloom_available_ = true;
  }
  bloom_enabled_ = bloom_enabled_ && bloom_available_;

  // Bind the two sampler slots of BOTH tonemap sets: HDR input at 0, and at 1
  // either the bloom target or the 1x1 black texture. Writing both up front is
  // what lets the toggle be a bind-time choice instead of a descriptor write.
  if (!descriptor_manager_
           ->write_image(compose_set_, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         hdr_sampler_, hdr_target_->image_view(),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok() ||
      !descriptor_manager_
           ->write_image(compose_set_nobloom_, 0U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, hdr_sampler_,
                         hdr_target_->image_view(),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    return refuse("HDR descriptor write failed");
  }
  // With no bloom target, the bloom-bound set must also point at black: it is
  // never selected (bloom_enabled_ is false and set_bloom_enabled(true) is
  // refused), but an unwritten binding 1 would be a dangling descriptor.
  const VkImageView bloom_slot_view =
      bloom_available_ ? bloom_target_->image_view() : black_texture_.view;
  if (!descriptor_manager_
           ->write_image(compose_set_, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         linear_sampler_, bloom_slot_view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    return refuse("bloom descriptor write failed");
  }
  if (!descriptor_manager_
           ->write_image(compose_set_nobloom_, 1U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         linear_sampler_, black_texture_.view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    return refuse("no-bloom descriptor write failed");
  }
  // External-source set: until set_external_source() says otherwise it mirrors
  // the plain path (own intermediate, black bloom slot), so selecting it
  // without a source is harmless rather than a dangling view.
  {
    const bool have_external = external_image_ != VK_NULL_HANDLE &&
                               external_view_ != VK_NULL_HANDLE;
    if (!descriptor_manager_
             ->write_image(compose_set_external_, 0U,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           hdr_sampler_,
                           have_external ? external_view_
                                         : hdr_target_->image_view(),
                           have_external
                               ? VK_IMAGE_LAYOUT_GENERAL
                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           0U)
             .is_ok() ||
        !descriptor_manager_
             ->write_image(compose_set_external_, 1U,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           linear_sampler_, black_texture_.view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
             .is_ok()) {
      return refuse("external-source descriptor write failed");
    }
  }
  // The bright-pass reads the HDR scene. Only when bloom exists: with no target
  // there is no pipeline to bind this to, and writing a null view fails.
  if (bloom_available_ &&
      !descriptor_manager_
           ->write_image(bloom_down_set_, 0U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, hdr_sampler_,
                         hdr_target_->image_view(),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
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

void record_compose_stage(VkCommandBuffer command_buffer,
                          const GraphPass& pass, void* user) {
#ifdef OMNICPP_HAS_VULKAN
  const auto* stage = static_cast<const ComposeStage*>(user);
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
  (void)record_fullscreen_draw(command_buffer, draw, stage->set);
#else
  (void)command_buffer;
  (void)pass;
  (void)user;
#endif
}

void VulkanComposeChain::set_external_source(VkImage image,
                                             VkImageView view) noexcept {
  external_image_ = image;
  external_view_ = view;
  // Written HERE rather than only in ensure(): ensure() has normally already
  // run by the time an application knows its source, and a set still holding
  // the fallback view keeps tonemapping the chain's own unused HDR image -- a
  // failure that looks fine on screen (stable, non-black) while ignoring the
  // source entirely.
  if (!compose_ready_ || descriptor_manager_ == nullptr ||
      compose_set_external_ == VK_NULL_HANDLE) {
    return;
  }
  const bool have = image != VK_NULL_HANDLE && view != VK_NULL_HANDLE;
  (void)descriptor_manager_->write_image(
      compose_set_external_, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      hdr_sampler_, have ? view : hdr_target_->image_view(),
      have ? VK_IMAGE_LAYOUT_GENERAL
           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      0U);
}

void VulkanComposeChain::build_graph_passes(
    VkRenderPass target_pass, VkFramebuffer target_framebuffer,
    std::uint32_t width, std::uint32_t height, std::vector<GraphPass>& passes,
    std::vector<ComposeStage>& stages, VkClearValue* clear_storage) {
#ifdef OMNICPP_HAS_VULKAN
  passes.clear();
  stages.clear();
  if (!compose_ready_ || hdr_sampler_ == VK_NULL_HANDLE ||
      clear_storage == nullptr) {
    return;
  }

  // Where the scene pass left the HDR image. It is recorded outside this graph
  // when record() is used alone, so each stage's first sample of it declares
  // this as the producer state. When a caller embeds these passes AFTER a
  // scene node in one compile, the compiler already tracks the HDR image from
  // that node's attachment and these seeds are simply ignored.
  const VkImageLayout hdr_from_scene = hdr_target_->color_final_layout();

  clear_storage[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clear_storage[1].depthStencil = {1.0f, 0U};

  compose_push_.exposure = config_.exposure;
  compose_push_.inverse_samples = inverse_samples_;
  compose_push_.pad[0] = 0.0F;
  compose_push_.pad[1] = 0.0F;


  // --- Declare the stages. ---
  //
  // The live toggle decides ONLY what records here. Every pipeline, target and
  // descriptor set involved was created by ensure() before the first frame, so
  // a toggle cannot invalidate anything the application baked against -- which
  // is exactly what the reverted first attempt got wrong.
  const bool bloom_on =
      bloom_enabled_ && bloom_available_ && bloom_down_pipeline_ != nullptr &&
      bloom_target_->image() != VK_NULL_HANDLE;
  // One bloom stage, not two: the half-resolution bright-pass, whose result the
  // tonemap samples directly (its binding 1) and adds AFTER the curve.
  //
  // There used to be a second "upsample" stage writing into hdr_target_. It was
  // dead weight AND wrong twice over:
  //   - it rendered into hdr_target_'s render pass, whose color attachment is
  //     LOAD_OP_CLEAR, so it ERASED the scene it was supposed to be adding to;
  //   - the pipeline has blending disabled (VulkanPipeline hardcodes
  //     blendEnable = VK_FALSE), so even without the clear it would have
  //     replaced rather than added.
  // With both, bloom-on produced a black frame. Nothing caught it because
  // every prior check was "does it run without VUIDs" (the viewport matrix) or
  // "is the toggle accepted" -- never "are the pixels right". Bloom is now
  // verified by the same probe that verifies exposure, in
  // VulkanHardware.HeadlessComposeFrameGoldenHash.
  //
  // The shader stays in assets/shaders (bloom_upsample.frag) and
  // test_postprocessing.cpp still exercises it directly: it is a legitimate
  // standalone pass, just not part of this chain.
  if (bloom_on) {
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
  }

  // The tonemap always runs: it samples the HDR scene and, when bloom is on,
  // the bloom buffer; it writes the presentation target.
  //
  // Which of the TWO pre-written descriptor sets it binds is the whole runtime
  // cost of the toggle. The off-set's slot 1 is the 1x1 black texture, so the
  // shader is unchanged between configurations and the set is chosen, not
  // rewritten.
  ComposeStage tonemap{};
  tonemap.pipeline = compose_pipeline_.get();
  tonemap.target = nullptr;  // presentation target
  tonemap.pass = target_pass;
  tonemap.framebuffer = target_framebuffer;
  tonemap.width = width;
  tonemap.height = height;
  tonemap.push = &compose_push_;
  tonemap.push_size = sizeof(compose_push_);
  // Three pre-written sets, selected by what this frame actually contains:
  // external source (path tracing) > bloom > plain raster.
  const bool external = external_image_ != VK_NULL_HANDLE;
  tonemap.set = external ? compose_set_external_
                         : (bloom_on ? compose_set_ : compose_set_nobloom_);
  tonemap.samples[0].image = external ? external_image_ : hdr_target_->image();
  tonemap.samples[0].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // With no bloom stage, the scene pass is this stage's only producer. With
  // bloom, the upsample rewrote hdr and the compiler already tracked it.
  if (external) {
    // The source is a storage image that stays in GENERAL for its whole life
    // (see set_external_source), so there is no producer transition to compute:
    // same layout in and out. Transitioning it to SHADER_READ_ONLY here would
    // make the layout depend on WHICH consumer ran last, and the capture chain
    // -- a second instance recording after the live frame -- would then declare
    // a layout the image is not in. The write it received this frame is ordered
    // by the tracer's own barrier and the submission's dependency, not by
    // layout.
    tonemap.samples[0].used_layout = VK_IMAGE_LAYOUT_GENERAL;
    tonemap.samples[0].initial_layout = VK_IMAGE_LAYOUT_GENERAL;
    // The SOURCE SCOPE of the graph's barrier. It was 0U/0U, and a barrier
    // with an empty source scope orders nothing: the write->read dependency
    // the tonemap needs was never established, so the sample returned whatever
    // the image happened to contain.
    //
    // The symptom was a path-traced view that FROZE at a plausible value
    // while the accumulation underneath it advanced -- the tracer's linear mean
    // moved 75% of its pixels between n=300 and n=1500, and the composed image
    // changed 0.02% of its pixels, by one LSB. Measured on both the live view
    // (two window grabs 25 s apart: identical mean and standard deviation) and
    // the capture (byte-identical across nine consecutive captures). No
    // validation diagnostic, because a zero-scope barrier is legal; it simply
    // does nothing.
    //
    // The old comment here claimed the write "is ordered by the tracer's own
    // barrier and the submission's dependency". The tracer's barrier declared
    // SHADER_WRITE -> TRANSFER_READ, which does not cover a SHADER read at
    // all, and the graph's barrier -- the one that would have covered it --
    // had no source scope. So nothing ordered it.
    tonemap.samples[0].initial_access = VK_ACCESS_SHADER_WRITE_BIT;
    tonemap.samples[0].initial_stage =
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
  } else {
    tonemap.samples[0].initial_layout = hdr_from_scene;
    tonemap.samples[0].initial_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    tonemap.samples[0].initial_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  }
  tonemap.sample_count = 1U;
  if (!external && bloom_on) {
    tonemap.samples[1].image = bloom_target_->image();
    tonemap.samples[1].used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tonemap.samples[1].initial_layout = bloom_target_->color_final_layout();
    tonemap.samples[1].initial_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    tonemap.samples[1].initial_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    tonemap.sample_count = 2U;
  }
  stages.push_back(tonemap);

  // --- Build graph passes. Each stage writes its target as a real attachment,
  // so the compiler tracks write-after-write and write-after-read edges too.
  // The pass carries the stage as its opaque record handle; `stages` (the
  // out-param) owns them, so user_data stays valid through the caller's
  // execute_graph call.
  passes.reserve(stages.size());
  for (std::size_t i = 0; i < stages.size(); ++i) {
    const ComposeStage& stage = stages[i];
    GraphPass pass{};
    pass.name = "compose";
    pass.render_pass = stage.pass;
    pass.framebuffer = stage.framebuffer;
    pass.width = stage.width;
    pass.height = stage.height;
    pass.clear_values = clear_storage;
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
    pass.user_data = &stages[i];
    passes.push_back(pass);
  }

  // The external-source descriptor is written here, every frame, rather than
  // only in set_external_source() and ensure().
  //
  // WHY: the binding is the thing that must be correct at DRAW time, and
  // there are two other writers -- ensure() (which writes the fallback view
  // when no source is set) and the bloom/no-bloom set selection. A write made
  // once, at configuration time, can be undone by a later ensure() on a
  // resized chain, and the symptom is not an error: the compose still runs,
  // still produces a plausible image, and the source is simply ignored. The
  // viewport's capture was measured doing exactly that -- with
  // using_external_source() true and the captured image being the chain's own
  // (raster) intermediate rather than the path-traced accumulation.
  //
  // One vkUpdateDescriptorSets per frame is not free, but it is a handful of
  // nanoseconds next to a frame, and the alternative is a binding that is
  // correct only if nothing else touches it.
  if (external_image_ != VK_NULL_HANDLE && external_view_ != VK_NULL_HANDLE &&
      compose_set_external_ != VK_NULL_HANDLE && descriptor_manager_ != nullptr) {
    (void)descriptor_manager_->write_image(
        compose_set_external_, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        hdr_sampler_, external_view_, VK_IMAGE_LAYOUT_GENERAL, 0U);
  }

  // NOTE: no nodes/compile/execute here. The caller owns the graph: record()
  // below wraps this in a compose-only graph, while the renderer prepends a
  // scene node and compiles once (T0 seam) so the scene -> compose transition
  // is compiler-owned instead of hand-seeded.
#else
  (void)target_pass; (void)target_framebuffer;
  (void)width; (void)height;
  (void)passes; (void)stages; (void)clear_storage;
#endif
}

void VulkanComposeChain::record(VkCommandBuffer command_buffer,
                                VkRenderPass target_pass,
                                VkFramebuffer target_framebuffer,
                                std::uint32_t width, std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  std::vector<GraphPass> passes;
  std::vector<ComposeStage> stages;
  VkClearValue clear[2]{};
  build_graph_passes(target_pass, target_framebuffer, width, height, passes,
                     stages, clear);
  if (passes.empty()) return;

  std::vector<GraphNode> nodes;
  nodes.reserve(passes.size());
  for (const GraphPass& pass : passes) {
    nodes.push_back(GraphNode::from_render(pass));
  }

  const CompiledGraph compiled = compile_graph(nodes);
  execute_graph(command_buffer, nodes, compiled, &record_compose_stage,
                nullptr);
#else
  (void)command_buffer; (void)target_pass; (void)target_framebuffer;
  (void)width; (void)height;
#endif
}

VkImage VulkanComposeChain::skeleton_image_of(int kind) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  return reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(kind));
#else
  return reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(kind));
#endif
}

int VulkanComposeChain::skeleton_kind_of(VkImage image) noexcept {
  const auto id = static_cast<std::uintptr_t>(
      reinterpret_cast<std::uintptr_t>(image));
  if (id >= static_cast<std::uintptr_t>(kKindHdrColor) &&
      id <= static_cast<std::uintptr_t>(kKindExternal)) {
    return static_cast<int>(id);
  }
  return 0;
}

const char* VulkanComposeChain::transient_kind_name(int kind) noexcept {
  switch (kind) {
    case kKindHdrColor:
      return "hdr_color";
    case kKindHdrDepth:
      return "hdr_depth";
    case kKindBloomColor:
      return "bloom_color";
    case kKindExternal:
      return "external";
    default:
      return "unknown";
  }
}

void VulkanComposeChain::build_plan_skeleton_nodes(
    bool include_bloom, std::vector<GraphPass>& passes) {
  passes.clear();
  GraphPass scene{};
  scene.name = "scene";
  RenderPassAttachment hdr_att{};
  hdr_att.image = skeleton_image_of(kKindHdrColor);
  RenderPassAttachment depth_att{};
  depth_att.image = skeleton_image_of(kKindHdrDepth);
  scene.attachments = {hdr_att, depth_att};
  passes.push_back(scene);
  if (include_bloom) {
    GraphPass down{};
    down.name = "bloom_down";
    RenderPassAttachment bloom_att{};
    bloom_att.image = skeleton_image_of(kKindBloomColor);
    down.attachments = {bloom_att};
    GraphSampledImage sample{};
    sample.image = skeleton_image_of(kKindHdrColor);
    down.sampled_images = {sample};
    passes.push_back(down);
  }
  GraphPass tonemap{};
  tonemap.name = "tonemap";
  GraphSampledImage sample_hdr{};
  sample_hdr.image = skeleton_image_of(kKindHdrColor);
  tonemap.sampled_images.push_back(sample_hdr);
  if (include_bloom) {
    GraphSampledImage sample_bloom{};
    sample_bloom.image = skeleton_image_of(kKindBloomColor);
    tonemap.sampled_images.push_back(sample_bloom);
  }
  GraphSampledImage sample_external{};
  sample_external.image = skeleton_image_of(kKindExternal);
  tonemap.sampled_images.push_back(sample_external);
  passes.push_back(tonemap);
}

bool VulkanComposeChain::derive_compose_plan(
    std::uint32_t width, std::uint32_t height, std::uint32_t bw,
    std::uint32_t bh, VkFormat hdr_format, bool depth_sampleable,
    bool include_bloom, std::vector<TransientImageDesc>& descs) {
  descs.clear();
  if (width == 0U || height == 0U || bw == 0U || bh == 0U ||
      hdr_format == VK_FORMAT_UNDEFINED) {
    return false;
  }
  std::vector<GraphPass> skeleton;
  build_plan_skeleton_nodes(include_bloom, skeleton);
  // Kind-aware walk: the same visit order derive_frame_resources uses
  // (attachments, then sampled images, per node), so these intervals ARE the
  // skeleton's lifetimes, not restated constants.
  struct Interval {
    bool seen{false};
    std::uint32_t first{0};
    std::uint32_t last{0};
  };
  Interval by_kind[kKindExternal + 1]{};
  int first_touch[kKindExternal + 1]{0, 0, 0, 0, 0};
  int touch_count = 0;
  for (std::uint32_t p = 0; p < skeleton.size(); ++p) {
    const GraphPass& node = skeleton[p];
    for (const RenderPassAttachment& att : node.attachments) {
      const int kind = skeleton_kind_of(att.image);
      if (kind <= 0) return false;
      if (!by_kind[kind].seen) {
        by_kind[kind].seen = true;
        by_kind[kind].first = p;
        first_touch[touch_count++] = kind;
      }
      by_kind[kind].last = p;
    }
    for (const GraphSampledImage& sample : node.sampled_images) {
      const int kind = skeleton_kind_of(sample.image);
      if (kind <= 0) return false;
      if (!by_kind[kind].seen) {
        by_kind[kind].seen = true;
        by_kind[kind].first = p;
        first_touch[touch_count++] = kind;
      }
      by_kind[kind].last = p;
    }
  }
  // Bytes per HDR pixel: the format was picked as SFLOAT-with-fallback, so
  // only these two can arrive here; anything else fails loudly.
  std::uint64_t hdr_bpp = 0U;
  if (hdr_format == VK_FORMAT_R16G16B16A16_SFLOAT) {
    hdr_bpp = 8U;
  } else if (hdr_format == VK_FORMAT_B8G8R8A8_UNORM) {
    hdr_bpp = 4U;
  } else {
    return false;
  }
  planned_resources_.clear();
  planned_kinds_.clear();
  for (int t = 0; t < touch_count; ++t) {
    const int kind = first_touch[t];
    FrameResource r;
    r.name = transient_kind_name(kind);
    r.first_use = by_kind[kind].first;
    r.last_use = by_kind[kind].last;
    // The external source is caller-owned history (read across the frame
    // boundary by the tracer/capture); everything the store creates is
    // frame-local transient.
    r.history = (kind == kKindExternal);
    switch (kind) {
      case kKindHdrColor:
        r.size_bytes = static_cast<std::uint64_t>(width) * height * hdr_bpp;
        break;
      case kKindHdrDepth:
        r.size_bytes = static_cast<std::uint64_t>(width) * height * 4U;
        break;
      case kKindBloomColor:
        r.size_bytes = static_cast<std::uint64_t>(bw) * bh * hdr_bpp;
        break;
      case kKindExternal:
        r.size_bytes = static_cast<std::uint64_t>(width) * height * 16U;
        break;
      default:
        return false;
    }
    r.layout_tag = static_cast<std::uint64_t>(kind);
    planned_resources_.push_back(r);
    planned_kinds_.push_back(kind);
  }
  planned_plan_ = plan_resource_allocations(
      planned_resources_, static_cast<std::uint32_t>(skeleton.size()));
  if (planned_plan_.size() != planned_resources_.size()) return false;

  // One desc per OWNED kind present in the skeleton. The external source is
  // caller-owned: planned (its history blocks aliasing) but never created.
  TransientImageDesc hdr{};
  hdr.name = transient_kind_name(kKindHdrColor);
  hdr.format = hdr_format;
  hdr.width = width;
  hdr.height = height;
  hdr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  hdr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  descs.push_back(hdr);
  TransientImageDesc depth{};
  depth.name = transient_kind_name(kKindHdrDepth);
  depth.format = VK_FORMAT_D32_SFLOAT;
  depth.width = width;
  depth.height = height;
  depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (depth_sampleable) depth.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
  descs.push_back(depth);
  if (include_bloom) {
    TransientImageDesc bloom{};
    bloom.name = transient_kind_name(kKindBloomColor);
    bloom.format = hdr_format;
    bloom.width = bw;
    bloom.height = bh;
    bloom.usage = hdr.usage;
    bloom.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    descs.push_back(bloom);
  }
  return true;
}

int VulkanComposeChain::kind_of_image(VkImage image) const noexcept {
  if (image == VK_NULL_HANDLE) return 0;
  if (image == external_image_) return kKindExternal;
  if (image == transient_store_.image(transient_kind_name(kKindHdrColor))) {
    return kKindHdrColor;
  }
  if (image == transient_store_.image(transient_kind_name(kKindHdrDepth))) {
    return kKindHdrDepth;
  }
  if (image == transient_store_.image(transient_kind_name(kKindBloomColor))) {
    return kKindBloomColor;
  }
  return 0;
}

bool VulkanComposeChain::verify_frame_nodes(
    const std::vector<GraphPass>& passes) const {
  if (!transient_store_.ready()) return false;
  if (planned_resources_.size() != planned_kinds_.size() ||
      planned_resources_.size() != planned_plan_.size()) {
    return false;
  }
  // Kind-aware walk over what was ACTUALLY recorded (same visit order as
  // the skeleton walk). An unknown handle is a shape violation, not a skip:
  // every image in these nodes must be a planned kind or the external
  // source, otherwise the plan does not cover the frame.
  struct Interval {
    bool seen{false};
    std::uint32_t first{0};
    std::uint32_t last{0};
  };
  Interval by_kind[kKindExternal + 1]{};
  for (std::uint32_t p = 0; p < passes.size(); ++p) {
    const GraphPass& node = passes[p];
    for (const RenderPassAttachment& att : node.attachments) {
      const int kind = kind_of_image(att.image);
      if (kind <= 0) return false;
      if (!by_kind[kind].seen) {
        by_kind[kind].seen = true;
        by_kind[kind].first = p;
      }
      by_kind[kind].last = p;
    }
    for (const GraphSampledImage& sample : node.sampled_images) {
      const int kind = kind_of_image(sample.image);
      if (kind <= 0) return false;
      if (!by_kind[kind].seen) {
        by_kind[kind].seen = true;
        by_kind[kind].first = p;
      }
      by_kind[kind].last = p;
    }
  }
  const auto planned_interval = [this](int kind, std::uint32_t* first,
                                       std::uint32_t* last,
                                       bool* history) -> bool {
    for (std::size_t i = 0; i < planned_kinds_.size(); ++i) {
      if (planned_kinds_[i] == kind) {
        *first = planned_resources_[i].first_use;
        *last = planned_resources_[i].last_use;
        *history = planned_resources_[i].history;
        return true;
      }
    }
    return false;
  };
  // Every recorded kind must be planned, and its actual interval a subset
  // of the planned one. A planned-but-unrecorded kind (bloom image with the
  // toggle off, HDR in PT mode) is idle memory, not a violation: skipping
  // it is sound because an untouched image cannot corrupt an alias pair.
  for (int kind = kKindHdrColor; kind <= kKindExternal; ++kind) {
    if (!by_kind[kind].seen) continue;
    std::uint32_t planned_first = 0U, planned_last = 0U;
    bool planned_history = false;
    if (!planned_interval(kind, &planned_first, &planned_last,
                          &planned_history)) {
      return false;
    }
    if (by_kind[kind].first < planned_first ||
        by_kind[kind].last > planned_last) {
      return false;
    }
    // Owned images are never history; only the caller-owned external is.
    if (planned_history && kind != kKindExternal) return false;
  }
  // Every planned alias pair still disjoint, with no history on either side.
  // Vacuous on today's frame (no disjoint same-desc pair exists: HDR spans
  // the tonemap, depth and bloom differ in format/size) -- the subset
  // checks above carry the weight, and this is the tripwire for any future
  // shape that introduces sharing.
  for (const ResourceAllocationPlan& entry : planned_plan_) {
    if (entry.kind != ResourceAllocation::Aliased) continue;
    int owner_kind = 0, sharer_kind = 0;
    for (std::size_t i = 0; i < planned_resources_.size(); ++i) {
      if (planned_resources_[i].name == entry.resource) {
        sharer_kind = planned_kinds_[i];
      }
      if (planned_resources_[i].name == entry.shares_with) {
        owner_kind = planned_kinds_[i];
      }
    }
    if (owner_kind <= 0 || sharer_kind <= 0) return false;
    if (!by_kind[owner_kind].seen || !by_kind[sharer_kind].seen) continue;
    const std::uint32_t a_first = by_kind[sharer_kind].first;
    const std::uint32_t a_last = by_kind[sharer_kind].last;
    const std::uint32_t b_first = by_kind[owner_kind].first;
    const std::uint32_t b_last = by_kind[owner_kind].last;
    if (a_first <= b_last && b_first <= a_last) return false;
  }
  // History is never aliased, in either direction. The planner proves this
  // structurally; the store re-checks the materialized plan because a
  // corrupted plan entry here would be a cross-frame rendering bug.
  for (std::size_t i = 0; i < planned_resources_.size(); ++i) {
    if (!planned_resources_[i].history) continue;
    if (planned_plan_[i].kind == ResourceAllocation::Aliased) return false;
    for (const ResourceAllocationPlan& entry : planned_plan_) {
      if (entry.kind == ResourceAllocation::Aliased &&
          entry.shares_with == planned_resources_[i].name) {
        return false;
      }
    }
  }
  return true;
}

void VulkanComposeChain::destroy() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  compose_ready_ = false;
  compose_width_ = 0U;
  compose_height_ = 0U;
  compose_set_ = VK_NULL_HANDLE;
  compose_set_nobloom_ = VK_NULL_HANDLE;
  compose_set_external_ = VK_NULL_HANDLE;
  external_image_ = VK_NULL_HANDLE;
  external_view_ = VK_NULL_HANDLE;
  inverse_samples_ = 1.0F;
  compose_layout_ = VK_NULL_HANDLE;
  bloom_enabled_ = false;
  bloom_available_ = false;
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
  // Targets before the allocator that owns their memory. Both may be null when
  // ensure() refused before reaching them (a Vulkan-off build never
  // constructs them), which is why the pipelines above are null-checked and
  // these were not -- an unguarded unique_ptr deref on the refuse path.
  if (bloom_target_ != nullptr) bloom_target_->cleanup();
  if (hdr_target_ != nullptr) hdr_target_->cleanup();
  // The store after the shells that adopt from it (adopted images are
  // borrowed, so shell cleanup must run first), and before the allocator
  // whose blocks back its owner allocations.
  transient_store_.destroy();
  planned_resources_.clear();
  planned_kinds_.clear();
  planned_plan_.clear();
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
