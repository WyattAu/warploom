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

#include <array>
#include <cstdint>
#include <vector>

#include <warploom/core/deterministic_runtime.hpp>
#include <warploom/render/vulkan_offscreen.hpp>
#include <warploom/render/vulkan_render_graph.hpp>
#include <warploom/render/vulkan_types.hpp>

namespace warploom::render {

class VulkanMemoryAllocator;
class VulkanPipeline;
class VulkanDescriptorManager;

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

//! Record callback for a compose stage inside an execute_graph render node.
//! `user` is the stage (a ComposeStage*); shared by VulkanComposeChain::record
//! and by callers that embed compose stages in a larger graph.
void record_compose_stage(VkCommandBuffer command_buffer,
                          const GraphPass& pass, void* user);

struct ComposeChainConfig {
  //! Initial bloom state. The chain PRE-ALLOCATES every bloom resource
  //! regardless of this value (docs/research/rendering-and-graphs.md, R7), so
  //! this only decides what the first recorded frame does; afterwards
  //! set_bloom_enabled() is a re-record, never a rebuild.
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

  //! Build the compose stages' graph passes WITHOUT recording them, so a
  //! caller can embed them in a larger graph (T0: the scene pass becomes a
  //! graph node in the same compile, and the compiler owns the scene ->
  //! compose transition instead of a hand-written barrier + an initial_layout
  //! seed). `stages` receives the stage descriptions and `passes` one
  //! GraphPass per stage with user_data pointing at its stage; both must
  //! outlive the execute_graph call. `clear_storage` supplies the two clear
  //! values every pass points at (one caller-owned array, not per-pass
  //! temporaries). Performs the per-frame external-source descriptor write,
  //! exactly as record() does.
  void build_graph_passes(VkRenderPass target_pass,
                          VkFramebuffer target_framebuffer, std::uint32_t width,
                          std::uint32_t height, std::vector<GraphPass>& passes,
                          std::vector<ComposeStage>& stages,
                          VkClearValue* clear_storage);

  void destroy() noexcept;

  //! Live exposure for the tonemap push; read at record time, so this takes
  //! effect on the next recorded frame without rebuilding anything.
  void set_exposure(float exposure) noexcept { config_.exposure = exposure; }
  [[nodiscard]] float exposure() const noexcept { return config_.exposure; }

  //! Live bloom state (E2/R4). Read at record time: it only decides whether
  //! the down/up stages are recorded, never whether a resource exists. Every
  //! bloom resource is created by ensure() up front, so a toggle cannot
  //! invalidate a pipeline, a render pass, or a descriptor layout -- the
  //! failure mode that made the first bloom-toggle attempt render black.
  //! Refused (returns false, leaving the state unchanged) when the bloom
  //! shaders were absent at initialize(), so a chain built without them
  //! cannot be asked for a feature it has no pipeline for.
  bool set_bloom_enabled(bool enabled) noexcept {
    if (enabled && !bloom_available_) return false;
    bloom_enabled_ = enabled;
    return true;
  }
  [[nodiscard]] bool bloom_enabled() const noexcept { return bloom_enabled_; }
  //! True when the bloom pipelines were built, i.e. a toggle to ON is legal.
  [[nodiscard]] bool bloom_available() const noexcept { return bloom_available_; }

  //! Tonemap from an EXTERNAL HDR source instead of this chain's own
  //! intermediate (R1b: the path tracer's accumulation image). The source is
  //! a storage image in GENERAL layout produced by an earlier pass in the same
  //! frame; the chain declares that layout as the first-use producer state
  //! and transitions it to SHADER_READ_ONLY itself. Passing VK_NULL_HANDLE
  //! restores the normal path (the chain's own intermediate).
  //!
  //! Like the bloom toggle this is a SELECTION between a pre-written descriptor
  //! set and the chain's own, not a rebuild: switching costs no pipeline, no
  //! render pass and no allocation. Note the source must outlive the chain
  //! (the path tracer owns it); a resize goes through ensure(), which re-writes
  //! every set from the CURRENT source.
  //!
  //! The descriptor write happens HERE, at set time, not at ensure() time --
  //! ensure() has usually already run by the time an application learns what
  //! its external source is, and a set written with the fallback view keeps
  //! tonemapping the chain's own (unused) HDR image forever. That failure is
  //! invisible: the frame is stable, non-black, and completely unaffected by
  //! the source it was pointed at.
  void set_external_source(VkImage image, VkImageView view) noexcept;
  [[nodiscard]] bool using_external_source() const noexcept {
    return external_image_ != VK_NULL_HANDLE;
  }
  //! 1 / accumulated sample count, multiplied into the tonemap. 1.0f for the
  //! raster path, and applied as its own multiply so the raster output is
  //! bit-identical to before this existed.
  void set_inverse_samples(float inverse) noexcept {
    inverse_samples_ = inverse;
  }
  [[nodiscard]] float inverse_samples() const noexcept { return inverse_samples_; }


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
  [[nodiscard]] VkImageView scene_image_view() const noexcept {
    return hdr_target_->image_view();
  }
  [[nodiscard]] VkFormat scene_image_format() const noexcept {
    return hdr_target_->format();
  }
  [[nodiscard]] VkImageLayout scene_color_final_layout() const noexcept {
    return hdr_target_->color_final_layout();
  }
  [[nodiscard]] VkFormat scene_depth_format() const noexcept {
    return hdr_target_->depth_format();
  }
  [[nodiscard]] VkImageLayout scene_depth_final_layout() const noexcept {
    return hdr_target_->depth_final_layout();
  }

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
  //! Second tonemap set, identical except binding 1 points at the 1x1 black
  //! texture instead of the bloom target. The bloom toggle picks between two
  //! PRE-WRITTEN sets rather than rewriting a descriptor at runtime: the two
  //! images are both already in SHADER_READ_ONLY_OPTIMAL for the whole frame,
  //! so neither transition needs a barrier and neither write can race a frame
  //! still in flight. That is what makes the toggle a pure re-record.
  VkDescriptorSet compose_set_nobloom_{VK_NULL_HANDLE};
  //! Tonemap set bound to the external source (see set_external_source), with
  //! the black bloom slot: 1/n divide by the source's own accumulation, so
  //! bloom must not be layered on it.
  VkDescriptorSet compose_set_external_{VK_NULL_HANDLE};
  //! External HDR source image + view (the path tracer's accumulation image).
  VkImage external_image_{VK_NULL_HANDLE};
  VkImageView external_view_{VK_NULL_HANDLE};
  float inverse_samples_{1.0F};
  //! One-sampler layout/set for the bloom bright-pass. It reads the HDR scene,
  //! which in the two-binding compose layout is binding 0 -- where binding 1
  //! (the bloom input the tonemap samples) would instead be. So it needs its
  //! own one-binding set, pointed at the scene.
  VkDescriptorSetLayout bloom_stage_layout_{VK_NULL_HANDLE};
  VkDescriptorSet bloom_down_set_{VK_NULL_HANDLE};
  VkFormat hdr_format_{VK_FORMAT_UNDEFINED};
  std::uint32_t compose_width_{0};
  std::uint32_t compose_height_{0};
  std::uint32_t compose_generation_{0};
  bool compose_ready_{false};
  bool compose_failed_{false};
  //! Live toggle state, distinct from config_.enable_bloom (the value
  //! initialize() started from). The two can only diverge via
  //! set_bloom_enabled(), and ensure() must NOT copy the config value back in
  //! -- that is the same live-control trap exposure hit.
  bool bloom_enabled_{false};
  //! True once the bloom pipelines exist. Set in ensure() regardless of
  //! config_.enable_bloom, so a chain built with bloom off can still be
  //! toggled on at runtime; false only when the bloom shaders were missing.
  bool bloom_available_{false};
  //! Tonemap push constants for the frame being built. A MEMBER (not a
  //! build_graph_passes local) because stages point at it and execute_graph
  //! reads it during execute: a local would die at build return. Overwritten
  //! on every build, and execute_graph is synchronous within record(), so no
  //! frame-in-flight can observe a torn value.
  struct ComposePush {
    float exposure{1.0F};
    float inverse_samples{1.0F};
    float pad[2]{0.0F, 0.0F};
  } compose_push_{};
};

}  // namespace warploom::render
