#pragma once

//! @file vulkan_compose_chain.hpp
//! @brief The HDR compose chain (bloom + tonemap/FXAA) as an ownable object.
//!
//! Extracted from VulkanRenderer (B3b, docs/roadmap.md). The renderer keeps
//! one instance for presentation; a second instance is what lets frame capture
//! record the composed frame into its own target instead of refusing while
//! compose is on -- the first B3b attempt pointed the capture at the
//! renderer's live intermediates, bloom's upsample overwrote the HDR image the
//! capture was reading, and the attempt was reverted.
//!
//! Owns everything: allocator, descriptor manager, HDR + bloom targets,
//! pipelines, samplers, the black fallback texture. Nothing is shared with the
//! caller, so two instances cannot touch each other's images.

#include <cstdint>
#include <memory>
#include <string>

#include <warploom/core/deterministic_runtime.hpp>
#include <warploom/render/vulkan_offscreen.hpp>
#include <warploom/render/vulkan_types.hpp>

namespace warploom::render {

class VulkanMemoryAllocator;
class VulkanPipeline;
class VulkanDescriptorManager;

struct ComposeChainConfig {
  bool enable_bloom{false};
  std::uint32_t bloom_downscale{2};
  //! Directory holding tonemap_fxaa.frag.spv and, when bloom is enabled,
  //! bloom_downsample.frag.spv / bloom_upsample.frag.spv.
  std::string compose_shader_dir;
  float exposure{1.0f};
  //! HDR intermediate format; VK_FORMAT_UNDEFINED picks R16G16B16A16_SFLOAT
  //! when the device supports rendering to it and sampling from it.
  VkFormat hdr_format{VK_FORMAT_UNDEFINED};
};

class VulkanComposeChain final {
 public:
  VulkanComposeChain() = default;
  ~VulkanComposeChain();
  VulkanComposeChain(const VulkanComposeChain&) = delete;
  VulkanComposeChain& operator=(const VulkanComposeChain&) = delete;
  VulkanComposeChain(VulkanComposeChain&&) = delete;
  VulkanComposeChain& operator=(VulkanComposeChain&&) = delete;

  //! Create (or resize) every resource. `present_format` is the format of the
  //! target the final tonemap writes, which bakes into its VkPipeline. Idempotent
  //! at the same size; disabled gracefully (ready() == false, failed() == true)
  //! when the device cannot support the requested configuration.
  [[nodiscard]] ::warploom::core::Result<void> ensure(
      VkDevice device, VkPhysicalDevice physical_device, VkQueue graphics_queue,
      const ComposeChainConfig& config, VkRenderPass present_pass,
      VkFormat present_format, std::uint32_t width, std::uint32_t height);

  //! Record bloom (when enabled) then tonemap+FXAA into `target_pass` /
  //! `target_framebuffer`. Requires a successful ensure().
  void record(VkCommandBuffer command_buffer, VkRenderPass target_pass,
              VkFramebuffer target_framebuffer, std::uint32_t width,
              std::uint32_t height);

  void destroy() noexcept;

  [[nodiscard]] bool ready() const noexcept { return compose_ready_; }
  //! True when ensure() refused permanently; do not retry every frame.
  [[nodiscard]] bool failed() const noexcept { return compose_failed_; }
  //! Bumped on every (re)creation of the HDR intermediate.
  [[nodiscard]] std::uint32_t generation() const noexcept {
    return compose_generation_;
  }
  //! Format of the HDR intermediate (what the tonemap samples).
  [[nodiscard]] VkFormat hdr_format_present() const noexcept {
    return hdr_format_;
  }
  [[nodiscard]] std::uint32_t width() const noexcept { return compose_width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return compose_height_; }

  //! The scene pass renders into these when compose is ready, instead of the
  //! presentation target.
  [[nodiscard]] VkRenderPass scene_pass() const noexcept {
    return hdr_target_->render_pass();
  }
  [[nodiscard]] VkFramebuffer scene_framebuffer() const noexcept {
    return hdr_target_->framebuffer();
  }
  [[nodiscard]] VkImage scene_image() const noexcept { return hdr_target_->image(); }

  //! The HDR depth attachment: the scene pass writes it and the H-Z reduction
  //! samples it, so under compose the H-Z pyramid must read THIS depth rather
  //! than the presentation target's stale one.
  [[nodiscard]] VkImage scene_depth_image() const noexcept {
    return hdr_target_->depth_image();
  }
  [[nodiscard]] VkImageView scene_depth_view() const noexcept {
    return hdr_target_->depth_view();
  }
  [[nodiscard]] bool scene_depth_sampleable() const noexcept {
    return hdr_target_->depth_is_sampleable();
  }

 private:
  // Declaration order is load-bearing: the pipelines and targets hold buffers
  // allocated from allocator_, so they must be destroyed before it.
  // VulkanOffscreenTarget's destructor releases its own device handles, so the
  // targets come first.
  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkQueue graphics_queue_{VK_NULL_HANDLE};
  ComposeChainConfig config_{};
  VkRenderPass present_pass_{VK_NULL_HANDLE};
  VkFormat present_format_{VK_FORMAT_UNDEFINED};

  std::unique_ptr<VulkanMemoryAllocator> allocator_;
  std::unique_ptr<VulkanDescriptorManager> descriptor_manager_;
  std::unique_ptr<VulkanPipeline> compose_pipeline_;
  std::unique_ptr<VulkanPipeline> bloom_down_pipeline_;
  std::unique_ptr<VulkanPipeline> bloom_up_pipeline_;
  std::unique_ptr<VulkanOffscreenTarget> hdr_target_;
  std::unique_ptr<VulkanOffscreenTarget> bloom_target_;
  VkDescriptorSetLayout compose_layout_{VK_NULL_HANDLE};
  VkSampler hdr_sampler_{VK_NULL_HANDLE};
  VkSampler linear_sampler_{VK_NULL_HANDLE};
  //! 1x1 black texture bound to the bloom slot when bloom is disabled, so
  //! tonemap_fxaa.frag keeps a single shader for both configurations.
  struct BlackTexture {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
  } black_texture_{};
  VkDescriptorSet compose_set_{VK_NULL_HANDLE};
  //! Single-sampler layouts/sets for the two bloom stages. Both bloom shaders
  //! declare their input at set 0 binding 0, which in the two-binding compose
  //! layout is the HDR scene -- so the upsample would sample the very image it
  //! is writing. Each bloom stage therefore gets its own one-binding set
  //! pointed at its actual input.
  VkDescriptorSetLayout bloom_stage_layout_{VK_NULL_HANDLE};
  VkDescriptorSet bloom_down_set_{VK_NULL_HANDLE};
  VkDescriptorSet bloom_up_set_{VK_NULL_HANDLE};
  VkFormat hdr_format_{VK_FORMAT_UNDEFINED};
  std::uint32_t compose_width_{0};
  std::uint32_t compose_height_{0};
  std::uint32_t compose_generation_{0};
  bool compose_ready_{false};
  bool compose_failed_{false};
};

}  // namespace warploom::render
