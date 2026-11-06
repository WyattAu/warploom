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
#include <warploom/render/vulkan_transient_store.hpp>
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
  //! Presentation write (Blade stage 2: the dynamic tonemap). When set,
  //! the stage writes the swapchain image directly and `pass`/`framebuffer`
  //! are unused. Exactly one of `target`, `present_view` is set per stage.
  VkImage present_image{VK_NULL_HANDLE};
  VkImageView present_view{VK_NULL_HANDLE};
  VkRenderPass pass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  //! Blade stage 1: when true the stage records with dynamic rendering
  //! (GraphDynamicPass) instead of its baked pass/framebuffer. Decided at
  //! ensure() time from ComposeChainConfig::enable_dynamic_rendering.
  bool dynamic{false};
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

//! Record callback for a DYNAMIC compose stage (Blade stage 1): builds the
//! draw from the stage (pipeline, layout, set, push) and the node's
//! extents, and records it with record_fullscreen_dynamic_pass. The node's
//! view/layout come from the compiled pass, not the stage.
void record_compose_dynamic_stage(VkCommandBuffer command_buffer,
                                  const GraphDynamicPass& pass, void* user);

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
  //! Record fullscreen stages with dynamic rendering (Blade stage 1).
  //! Decided at ensure() time (pipelines bake the framing); the chain
  //! falls back to baked passes when the device lacks dynamicRendering.
  bool enable_dynamic_rendering{false};
  //! HDR intermediate format; VK_FORMAT_UNDEFINED picks R16G16B16A16_SFLOAT
  //! when the device supports rendering to it and sampling from it.
  VkFormat hdr_format{VK_FORMAT_UNDEFINED};
  //! Cross-queue sharing (loop28 step 1) for the HDR depth image: set when
  //! a dedicated COMPUTE-only family exists, so the H-Z compute leg may
  //! later sample it. The HDR color and bloom images stay EXCLUSIVE -- they
  //! never leave the graphics queue.
  ImageSharing cross_queue_sharing{};
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
  //! `target_framebuffer`. Requires a successful ensure(). `present_image`
  //! / `present_view` carry the swapchain image for the dynamic tonemap
  //! (Blade stage 2); null selects the baked tonemap regardless of the
  //! negotiated framing.
  void record(VkCommandBuffer command_buffer, VkRenderPass target_pass,
              VkFramebuffer target_framebuffer, VkImage present_image,
              VkImageView present_view, std::uint32_t width,
              std::uint32_t height);

  //! Build the compose stages' graph passes WITHOUT recording them, so a
  //! caller can embed them in a larger graph (T0: the scene pass becomes a
  //! graph node in the same compile, and the compiler owns the scene ->
  //! compose transition instead of a hand-written barrier + an initial_layout
  //! seed). `stages` receives the stage descriptions, `passes` one GraphPass
  //! per baked stage and `dyn_passes` one GraphDynamicPass per dynamic stage
  //! (Blade stage 1: the bloom downsample when enable_dynamic_rendering),
  //! each with user_data pointing at its stage; all three must outlive the
  //! execute_graph call. `clear_storage` supplies the two clear values every
  //! baked pass points at (one caller-owned array, not per-pass
  //! temporaries). Performs the per-frame external-source descriptor write,
  //! exactly as record() does.
  void build_graph_passes(VkRenderPass target_pass,
                          VkFramebuffer target_framebuffer,
                          VkImage present_image, VkImageView present_view,
                          std::uint32_t width, std::uint32_t height,
                          std::vector<GraphPass>& passes,
                          std::vector<GraphDynamicPass>& dyn_passes,
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
  //! True when the PT bloom pipeline was built: PT + bloom actually layers.
  //! When false, PT frames compose without bloom even with the toggle on
  //! (never a refused frame). Test seam: distinguishes "layered nothing
  //! because the shader was missing" from "layered nothing but should have".
  [[nodiscard]] bool pt_bloom_available() const noexcept {
    return pt_bloom_available_;
  }
  //! Whether dynamic rendering negotiated on (requested AND advertised).
  //! Blade stage 3: the renderer also consults this for the scene node --
  //! the per-stage flag below stays the bloom-specific seam it was.
  [[nodiscard]] bool dynamic_rendering_negotiated() const noexcept {
    return dynamic_enabled_;
  }
  //! Whether the bloom stage currently records dynamically. Test seam for
  //! Blade stage 1 (no device needed to assert the negotiated outcome).
  [[nodiscard]] bool dynamic_bloom() const noexcept { return dynamic_enabled_; }

  //! R6b per-frame gate: re-derive kind lifetimes from the scene + compose
  //! nodes actually recorded and check the stored plan still covers them:
  //! every recorded image's actual interval is a subset of the planned one
  //! for its SHAPE (bloom on/off move the tonemap's absolute index, so one
  //! absolute map false-positives on the legal toggle-off shrinkage -- PT
  //! with bloom off recorded the external at [1,1] against a planned [2,2]
  //! and failed every frame with no validation diagnostic), and every
  //! planned alias pair is still disjoint with no history on either side.
  //! Returns false (fail the frame, never render wrong) when the recorded
  //! shape outgrew the plan -- e.g. a future pass extends an interval into
  //! its alias partner. The renderer calls this on the unified graph before
  //! executing it.
  [[nodiscard]] bool verify_frame_nodes(
      const std::vector<GraphPass>& passes) const;
  //! Node-list overload (Blade stage 1): the unified frame is a mixed
  //! render + dynamic node list, so the gate walks nodes rather than
  //! passes. Identical rule (subset of the shape's planned intervals,
  //! alias pairs still disjoint); a dynamic node's single target folds in
  //! as its attachment and its sampled_images as its samples.
  [[nodiscard]] bool verify_frame_nodes(
      const std::vector<GraphNode>& nodes) const;

  //! Image kinds for the plan-derived store. The kind IS the alias-compat-
  //! ibility tag: equal kinds have identical creation parameters BY
  //! CONSTRUCTION (one desc per kind), so the planner's size+tag equality
  //! implies identical memory requirements for the store's bind check.
  //! Public because the plan skeleton below is a test seam: unit tests run
  //! derive_frame_resources over it with placeholder callbacks.
  enum TransientKind : int {
    kKindHdrColor = 1,
    kKindHdrDepth = 2,
    kKindBloomColor = 3,
    kKindExternal = 4,
    //! The presentation image (Blade stage 2: the dynamic tonemap writes
    //! it as a graph attachment). Caller-owned history like the external
    //! source -- planned (so the gate covers it) but never created.
    kKindPresent = 5,
  };

  //! The node shapes derive_compose_plan walks, with placeholder handles
  //! (skeleton_image_of(kind)). Mirrors build_graph_passes' stage structure:
  //! scene writes HDR color + depth; bloom_down (when recordable) writes
  //! bloom sampling HDR AND the external source (PT + bloom layering: the
  //! bloom stage reads whichever source the frame actually has, so the
  //! skeleton is the SUPERSET of both); tonemap samples HDR + bloom +
  //! external. The tonemap
  //! sampling all three is the SUPERSET of the raster shape (HDR + bloom)
  //! and the PT shape (external) -- but only SHAPE-relatively, not in
  //! absolute indices: without the bloom stage the tonemap sits at index 1,
  //! not 2, so verify keeps one interval map per bloom shape and compares
  //! each recorded frame against the shape it actually has.
  static void build_plan_skeleton_nodes(bool include_bloom,
                                        std::vector<GraphPass>& passes);
  //! Per-kind first/last pass indices over one skeleton shape.
  struct KindInterval {
    bool seen{false};
    std::uint32_t first{0};
    std::uint32_t last{0};
  };
  //! Walk one skeleton shape into per-kind intervals (+ first-touch order).
  //! The single source for planned intervals: derive uses it for both bloom
  //! shapes and verify reads the map matching the recorded frame.
  static void skeleton_intervals(bool include_bloom, KindInterval* out,
                                 int* first_touch, int* touch_count);
  [[nodiscard]] static int skeleton_kind_of(VkImage image) noexcept;
  [[nodiscard]] static VkImage skeleton_image_of(int kind) noexcept;
  [[nodiscard]] static const char* transient_kind_name(int kind) noexcept;

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
  //!
  //! src_access/src_stage describe the producer that wrote the source this
  //! frame, for the graph's source-scope barrier. Default is the tracer's
  //! ray-trace write; a caller that filters the accumulation first (the T2
  //! denoiser's compute dispatch) passes COMPUTE/SHADER_WRITE instead, so
  //! the same-submit tonemap AND the cross-submit capture read stay
  //! ordered. A wrong scope here is silent (a zero-scope barrier is legal
  //! and orders nothing), so the scope travels with the image, not as a
  //! separate call that can drift from it.
  void set_external_source(VkImage image, VkImageView view,
                           VkAccessFlags src_access = VK_ACCESS_SHADER_WRITE_BIT,
                           VkPipelineStageFlags src_stage =
                               VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR) noexcept;
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
  [[maybe_unused]] VkDevice device_{VK_NULL_HANDLE};
  [[maybe_unused]] VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  [[maybe_unused]] VkQueue graphics_queue_{VK_NULL_HANDLE};
  ComposeChainConfig config_{};
  [[maybe_unused]] VkRenderPass present_pass_{VK_NULL_HANDLE};
  VkFormat present_format_{VK_FORMAT_UNDEFINED};

  std::unique_ptr<VulkanMemoryAllocator> allocator_;
  std::unique_ptr<VulkanDescriptorManager> descriptor_manager_;
  std::unique_ptr<VulkanPipeline> compose_pipeline_;
  std::unique_ptr<VulkanPipeline> bloom_down_pipeline_;
  //! PT-variant bloom pipeline (bloom_downsample_pt.frag: Karis over the
  //! accumulation sum x inverse_samples). Built in the same framing as the
  //! raster bloom pipeline (baked or dynamic per dynamic_enabled_), so the
  //! PT + bloom stage records whichever way the chain negotiated.
  std::unique_ptr<VulkanPipeline> bloom_down_pt_pipeline_;
  //! Dynamic tonemap pipeline (Blade stage 2): same shaders as the baked
  //! tonemap, created without a render pass against the presentation
  //! format. Selected by the negotiated framing whenever the caller
  //! supplied the present image/view.
  std::unique_ptr<VulkanPipeline> compose_dynamic_pipeline_;
  std::unique_ptr<VulkanOffscreenTarget> hdr_target_;
  std::unique_ptr<VulkanOffscreenTarget> bloom_target_;
  [[maybe_unused]] VkDescriptorSetLayout compose_layout_{VK_NULL_HANDLE};
  VkSampler hdr_sampler_{VK_NULL_HANDLE};
  [[maybe_unused]] VkSampler linear_sampler_{VK_NULL_HANDLE};
  //! 1x1 black texture bound to the bloom slot when bloom is disabled, so
  //! tonemap_fxaa.frag keeps a single shader for both configurations.
  struct BlackTexture {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
  } black_texture_{};
  [[maybe_unused]] VkDescriptorSet compose_set_{VK_NULL_HANDLE};
  //! Second tonemap set, identical except binding 1 points at the 1x1 black
  //! texture instead of the bloom target. The bloom toggle picks between two
  //! PRE-WRITTEN sets rather than rewriting a descriptor at runtime: the two
  //! images are both already in SHADER_READ_ONLY_OPTIMAL for the whole frame,
  //! so neither transition needs a barrier and neither write can race a frame
  //! still in flight. That is what makes the toggle a pure re-record.
  [[maybe_unused]] VkDescriptorSet compose_set_nobloom_{VK_NULL_HANDLE};
  //! Tonemap set bound to the external source (see set_external_source), with
  //! the black bloom slot (PT without bloom). When PT + bloom layer, the
  //! chain binds compose_set_external_bloom_ instead: same source at
  //! binding 0, the real bloom target at binding 1.
  VkDescriptorSet compose_set_external_{VK_NULL_HANDLE};
  //! Tonemap set for PT + bloom: binding 0 is the external accumulation in
  //! GENERAL, binding 1 the bloom target. Pre-written (see set_external_-
  //  source), so layering bloom on a path-traced frame is a bind-time
  //! selection like the raster toggle, never a descriptor write.
  VkDescriptorSet compose_set_external_bloom_{VK_NULL_HANDLE};
  //! External HDR source image + view (the path tracer's accumulation image).
  VkImage external_image_{VK_NULL_HANDLE};
  VkImageView external_view_{VK_NULL_HANDLE};
  //! Producer scope for the external source's barrier (see set_external_-
  //! source): the ray-trace write by default, the denoise compute dispatch
  //! when the viewport points the source at the filtered mean.
  VkAccessFlags external_src_access_{VK_ACCESS_SHADER_WRITE_BIT};
  VkPipelineStageFlags external_src_stage_{VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR};
  //! Presentation image + view of the frame being built (Blade stage 2).
  //! Cached from build_graph_passes' parameters because kind_of_image()
  //! maps handles to kinds and the swapchain image is caller-owned: the
  //! member lives exactly as long as the built passes (one synchronous
  //! build/verify/execute), the same lifetime rule as compose_push_.
  VkImage present_image_{VK_NULL_HANDLE};
  [[maybe_unused]] VkImageView present_view_{VK_NULL_HANDLE};
  float inverse_samples_{1.0F};
  //! One-sampler layout/set for the bloom bright-pass. It reads the HDR scene,
  //! which in the two-binding compose layout is binding 0 -- where binding 1
  //! (the bloom input the tonemap samples) would instead be. So it needs its
  //! own one-binding set, pointed at the scene.
  [[maybe_unused]] VkDescriptorSetLayout bloom_stage_layout_{VK_NULL_HANDLE};
  [[maybe_unused]] VkDescriptorSet bloom_down_set_{VK_NULL_HANDLE};
  //! PT-variant bloom set: same one-binding layout, pointed at the external
  //! accumulation in GENERAL (PT + bloom layering). Written by
  //! set_external_source() and ensure(), exactly like the tonemap's
  //! external sets, so entering PT never writes a descriptor per frame.
  VkDescriptorSet bloom_down_set_external_{VK_NULL_HANDLE};
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
  //! True once the PT bloom pipeline exists (bloom_downsample_pt.frag
  //! present). Independent of bloom_available_: a chain with raster bloom
  //! but no PT shader layers nothing on path-traced frames, and PT + bloom
  //! falls back to PT-only rather than refusing the frame.
  bool pt_bloom_available_{false};
  //! True when the bloom stage records with dynamic rendering (Blade
  //! stage 1): requested by ComposeChainConfig::enable_dynamic_rendering
  //! AND advertised by the device. Decided in ensure(); stages built by
  //! build_graph_passes read it. Falls back to baked passes when false.
  bool dynamic_enabled_{false};
  //! R6b: the plan-derived image store backing the HDR color, HDR depth and
  //! bloom targets. ensure() derives the superset skeleton (bloom + external
  //! shapes included), plans it, and materializes it here; the
  //! VulkanOffscreenTargets adopt the store's images and own only their
  //! render passes + framebuffers. The hand-written target image list is
  //! gone: no create()/bind_image() calls name an HDR/bloom/depth image.
  VulkanTransientStore transient_store_;
  //! Derive the superset plan into planned_* and fill `descs` with one
  //! image description per owned kind. Pure (no device): the intervals come
  //! from walking the skeleton nodes, never from constants.
  bool derive_compose_plan(std::uint32_t width, std::uint32_t height,
                            std::uint32_t bw, std::uint32_t bh,
                            VkFormat hdr_format, bool depth_sampleable,
                            bool include_bloom,
                            std::vector<TransientImageDesc>& descs,
                            const ImageSharing& sharing);
  //! Kind of a live image handle (store-owned or the external source).
  //! 0 when unknown -- verify treats that as a shape violation, not a skip.
  [[nodiscard]] int kind_of_image(VkImage image) const noexcept;
  //! Skeleton derivation output, kept for verify_frame_nodes(): the resource
  //! intervals the store was materialized from.
  std::vector<FrameResource> planned_resources_;
  //! Kind per planned resource (parallel to planned_resources_).
  std::vector<int> planned_kinds_;
  //! Planned allocation over the skeleton (parallel to planned_resources_).
  std::vector<ResourceAllocationPlan> planned_plan_;
  //! Reference intervals per bloom shape ([0] without, [1] with the bloom
  //! stage), each walked from its own skeleton. verify selects by whether
  //! the recorded frame contains a bloom write.
  KindInterval planned_intervals_[2][kKindPresent + 1]{};
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
