#pragma once

/**
 * @file vulkan_renderer.hpp
 * @brief Vulkan renderer: command buffers, frame synchronization, draw loop.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/core/latency_telemetry.hpp"
#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_swapchain.hpp"
#include "warploom/render/vulkan_render_pass.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_compose_chain.hpp"
#include "warploom/render/vulkan_hiz_frame_state.hpp"
#include "warploom/render/vulkan_hiz_pyramid.hpp"
#include "warploom/render/vulkan_render_graph.hpp"
#include "warploom/render/vulkan_scene.hpp"
#include "warploom/render/vulkan_compute.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace warploom::render {

//! Per-frame GPU timing telemetry in nanoseconds.
//! Where a frame's GPU time goes. Two timestamps answer "is the frame
//! over budget"; six answer "which pass is over budget", which is the question
//! you actually have when a frame is slow.
enum class GpuSegment : std::uint8_t {
  PrePass = 0,   //!< Frame start -> after the pre-pass hooks (shadow depth).
  Scene = 1,     //!< -> after the main scene render pass.
  Compose = 2,   //!< -> after the HDR compose chain (bloom, tonemap, FXAA).
  HZ = 3,        //!< -> after the H-Z reduction, when enabled.
  Present = 4,   //!< -> after present-path bookkeeping.
  Count = 5,
};

[[nodiscard]] const char* to_string(GpuSegment segment) noexcept;

struct GpuTiming {
  //! False on devices without graphics-stage timestamps or when disabled.
  bool available{false};
  //! Device timestamp period (nanoseconds per tick).
  float timestamp_period_ns{0.0f};
  //! Last resolved frame duration for the slot being reused, in device
  //! ticks and nanoseconds: TOP_OF_PIPE at command-buffer start to
  //! BOTTOM_OF_PIPE at the end of the frame.
  std::uint64_t last_total_ticks{0};
  double last_total_ns{0.0};
  //! Per-segment nanoseconds for the last resolved frame. Indexed by
  //! GpuSegment. A segment that did not run this frame reads 0 -- which is
  //! itself informative, since "compose took 0ms" and "compose never ran"
  //! are different facts and only the second is a bug.
  double segment_ns[static_cast<std::size_t>(GpuSegment::Count)]{};
  //! Frame whose segments are in segment_ns. Lets a consumer tell a stale
  //! reading from the current frame.
  std::uint64_t resolved_frame{0};
  //! Number of successfully resolved frames.
  std::uint64_t queries_resolved{0};

  //! One line summarising the frame, for the diagnostics channel. Kept here so
  //! the format lives next to the numbers it describes.
  [[nodiscard]] std::string summary() const;
};

//! Resources and immutable frame token passed to the H-Z recording callback.
//! The callback records reduction/culling commands; ownership stays with the
//! renderer and the callback must not retain these pointers after returning.
struct HiZFrameRecord {
  HiZFrameToken token{};
  VkImage depth_image{VK_NULL_HANDLE};
  VkImageView depth_view{VK_NULL_HANDLE};
  bool depth_is_sampleable{false};
  bool destination_initialized{false};
  const VulkanHiZPyramid* previous_pyramid{nullptr};
  VulkanHiZPyramid* destination_pyramid{nullptr};
  std::uint32_t render_width{0};
  std::uint32_t render_height{0};
  std::uint32_t tile_size{0};
  std::uint32_t levels{0};
};

//! Return false when recording cannot complete; the renderer then discards
//! the token and does not publish the destination pyramid as previous-frame data.
using HiZRecordCallback = bool (*)(VkCommandBuffer, const HiZFrameRecord&, void*);

//! Owned graph metadata for the H-Z portion of a frame. Pass objects remain
//! stable until compile() returns; no Vulkan commands are recorded here.
struct HiZGraphPlan {
  std::vector<GraphComputePass> passes;
  [[nodiscard]] CompiledGraph compile() const {
    std::vector<GraphNode> nodes;
    nodes.reserve(passes.size());
    for (const auto& pass : passes) nodes.push_back(GraphNode::from_compute(pass));
    return compile_graph(nodes);
  }
};

struct FrameResources {
  VkCommandBuffer command_buffer{VK_NULL_HANDLE};
  VkFence in_flight_fence{VK_NULL_HANDLE};
  VkSemaphore image_available_semaphore{VK_NULL_HANDLE};
  VkSemaphore render_finished_semaphore{VK_NULL_HANDLE};
  bool frame_in_flight{false};
  void cleanup(VkDevice device) noexcept;
};

struct RendererConfig {
  std::uint32_t max_frames_in_flight{2};
  float clear_color_r{0.0f};
  float clear_color_g{0.0f};
  float clear_color_b{0.0f};
  float clear_color_a{1.0f};
  float clear_depth{1.0f};
  std::uint32_t clear_depth_stencil{0};
  //! Create persistent previous-frame H-Z resources for this renderer.
  bool enable_hiz{false};
  //! Base render pixels per H-Z L0 texel.
  std::uint32_t hiz_tile_size{32};
  //! Zero selects the complete legal mip chain.
  std::uint32_t hiz_levels{0};
  //! Optional compiled reduction shader. When set, the renderer owns and
  //! records the depth-to-H-Z reduction; the callback remains available for
  //! application-specific culling and scene-buffer work.
  std::string hiz_reduction_shader_path{};
  //! Loop28 step 3: record the H-Z reduce + pack on the dedicated compute
  //! queue reading the persisted snapshot (step 2), instead of inline on
  //! the graphics queue reading live depth. Requires a COMPUTE-only family
  //! (else silently stays on graphics); the snapshot copy still runs on
  //! graphics. Off by default: enable explicitly per run.
  bool hiz_compute_reduce{false};
  //! Loop28 step 4: pipeline the compute leg a frame behind graphics
  //! instead of CPU-waiting it. The graphics queue stops reducing inline:
  //! it copies live depth to the snapshot, packs the previous compute
  //! pyramid for the cull, submits, and the compute queue reduces the
  //! just-written snapshot concurrently with the NEXT frame. A timeline
  //! handoff (not a CPU wait) orders the three cross-queue edges.
  //! Requires hiz_compute_reduce + a dedicated family; silent graphics
  //! fallback otherwise. Off by default: enable explicitly per run.
  bool hiz_compute_pipelined{false};
  //! Record GPU timestamp queries around each frame (2 per frame slot) and
  //! resolve the previous frame's duration on slot reuse. Zero overhead when
  //! disabled; `gpu_timing().available` reports whether the device supports
  //! graphics-stage timestamps.
  bool enable_gpu_timing{false};

  // --- HDR compose -------------------------------------------------------
  //! Render the scene into an HDR intermediate, then tonemap + FXAA it into
  //! the swapchain image. Without this the scene is written straight to an
  //! 8-bit target: values above 1.0 clip hard, so the only response to a
  //! bright key is to dim the whole frame.
  //!
  //! When enabled, `scene_record_callback` runs inside a render pass on the
  //! HDR target rather than the swapchain, and the compose chain runs
  //! afterwards. The application's callback therefore stops owning the
  //! final presentation, which is the point: the engine decides the pass
  //! sequence and can own the render graph.
  //!
  //! Requires `compose_shader_dir` to hold tonemap_fxaa.frag (and, when
  //! `enable_bloom` is set, bloom_downsample.frag and bloom_upsample.frag).
  bool enable_hdr_compose{false};
  //! Directory holding the compiled compose shaders. Empty disables compose
  //! even when enable_hdr_compose is true, and is reported at configure time.
  std::string compose_shader_dir{};
  //! HDR intermediate format. R16G16B16A16_SFLOAT when the device supports
  //! it, else B8G8R8A8_UNORM, else compose is refused with an error.
  VkFormat hdr_format{VK_FORMAT_R16G16B16A16_SFLOAT};
  //! Multiplies scene radiance before the tonemap curve.
  float exposure{1.0f};
  //! Two-stage bloom: one half-resolution downsample, then one tent upsample
  //! added back by the tonemap shader. Ignored when enable_hdr_compose is off.
  //!
  //! This is the INITIAL state only. Every bloom resource is allocated
  //! regardless of this value, so set_bloom_enabled() switches it at runtime
  //! without rebuilding anything (docs/research/rendering-and-graphs.md, R7).
  bool enable_bloom{false};
  //! Half-resolution bloom buffer edge, in pixels.
  std::uint32_t bloom_downscale{2};
  //! Record the compose fullscreen stages with dynamic rendering
  //! (vkCmdBeginRendering, no render pass objects) instead of baked
  //! render passes. Requires a 1.3 device with dynamicRendering enabled
  //! (see VulkanContext::has_dynamic_rendering); when unsupported the
  //! chain falls back to baked passes. Same pixels either way — the
  //! golden hashes pin both framings.
  bool enable_dynamic_rendering{false};
};

class VulkanRenderer final {
public:
  VulkanRenderer() = default;
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;
  VulkanRenderer(VulkanRenderer&&) = delete;
  VulkanRenderer& operator=(VulkanRenderer&&) = delete;

  [[nodiscard]] ::warploom::core::Result<void> initialize(
      VulkanContext& context,
      const VulkanSwapchain& swapchain,
      const VulkanRenderPass& render_pass,
      const RendererConfig& config = {});

  //! Initialize with NO swapchain, for recording into an offscreen framebuffer.
  //!
  //! Why this exists: a Vulkan swapchain can only be created from a surface, and
  //! a surface needs a window system. That made begin_frame/submit_frame/
  //! present_frame -- the renderer's whole frame loop -- unexercisable in CI,
  //! because every test using it was behind VK_USE_PLATFORM_XCB_KHR, which is
  //! never defined for the test target. The result was a frame loop with zero
  //! executing tests and a "coverage" number that counted nothing.
  //!
  //! In headless mode begin_frame skips vkAcquireNextImageKHR (image index is
  //! always 0), submit_frame waits on no semaphore (nothing signals
  //! image_available without an acquire), and present_frame skips the queue
  //! present. Everything else -- compose graph, H-Z, GPU timestamps, per-frame
  //! fences, diagnostics -- behaves identically, which is the point: the test
  //! covers the real path rather than a parallel one.
  //! `width`/`height` replace the swapchain extent and size the H-Z pyramid and
  //! the compose chain, exactly as the swapchain's extent would. `present_format`
  //! is the format of the target `record_commands` will write when compose is
  //! enabled -- the tonemap pipeline bakes it in, and a headless frame has no
  //! swapchain to read it from, so the caller states it. `present_image` /
  //! `present_view` are the headless stand-in for the swapchain image (Blade
  //! stage 2: the dynamic tonemap writes it as a graph attachment). The
  //! caller owns them -- typically the offscreen target behind the
  //! framebuffer passed to record_commands -- and they must outlive the
  //! renderer, exactly like the render pass's own images.
  [[nodiscard]] ::warploom::core::Result<void> initialize_headless(
      VulkanContext& context,
      const VulkanRenderPass& render_pass,
      std::uint32_t width,
      std::uint32_t height,
      const RendererConfig& config = {},
      VkFormat present_format = VK_FORMAT_B8G8R8A8_UNORM,
      VkImage present_image = VK_NULL_HANDLE,
      VkImageView present_view = VK_NULL_HANDLE);

 private:
  //! Shared setup for initialize() and initialize_headless(); `swapchain` may
  //! be null for the headless path. Private so the swapchain-optional contract
  //! stays internal rather than becoming a second public way to start.
  [[nodiscard]] ::warploom::core::Result<void> initialize_common(
      VulkanContext& context,
      const VulkanRenderPass& render_pass,
      const RendererConfig& config,
      const VulkanSwapchain* swapchain,
      std::uint32_t target_width,
      std::uint32_t target_height,
      VkFormat headless_present_format);

 public:

  //! True when initialized without a swapchain. Present is a no-op and the
  //! frame always uses image index 0.
  [[nodiscard]] bool is_headless() const noexcept { return headless_; }

  //! Live exposure (E2/R4). Forwarded to the compose chain; a no-op when
  //! compose is off (the forward path has no tonemap to scale). Returns the
  //! value in effect either way.
  float set_exposure(float exposure) noexcept {
    if (compose_ != nullptr) compose_->set_exposure(exposure);
    return exposure;
  }
  //! Exposure the compose chain will apply on the next recorded frame; -1
  //! when compose is off (there is no tonemap to ask).
  [[nodiscard]] float compose_exposure() const noexcept {
    return compose_ != nullptr ? compose_->exposure() : -1.0f;
  }

  //! Live bloom toggle (E2/R4). Forwarded to the compose chain; a no-op when
  //! compose is off. Returns the state in effect: false means the request was
  //! refused because the chain was built without the bloom shaders, which is
  //! reported rather than silently ignored.
  bool set_bloom_enabled(bool enabled) noexcept {
    return compose_ != nullptr && compose_->set_bloom_enabled(enabled);
  }
  [[nodiscard]] bool compose_bloom_enabled() const noexcept {
    return compose_ != nullptr && compose_->bloom_enabled();
  }
  //! True when the chain has bloom pipelines, i.e. toggling ON is possible.
  [[nodiscard]] bool compose_bloom_available() const noexcept {
    return compose_ != nullptr && compose_->bloom_available();
  }

  //! R1b: tonemap from an external HDR source instead of this renderer's HDR
  //! intermediate -- the path tracer's accumulation image. The caller owns the
  //! image and must keep it alive; pass VK_NULL_HANDLE to go back to the
  //! normal path. No-op when compose is off.
  void set_external_tonemap_source(VkImage image, VkImageView view) noexcept {
    if (compose_ != nullptr) compose_->set_external_source(image, view);
  }
  //! 1 / accumulated sample count, multiplied into the tonemap. 1.0f is the
  //! raster default and is an exact no-op there.
  void set_compose_inverse_samples(float inverse) noexcept {
    if (compose_ != nullptr) compose_->set_inverse_samples(inverse);
  }
  [[nodiscard]] bool compose_uses_external_source() const noexcept {
    return compose_ != nullptr && compose_->using_external_source();
  }

  // --- HDR compose introspection ---------------------------------------
  // An application whose pipelines are built against a render pass must
  // build them against the HDR intermediate when compose is on. These
  // accessors expose it, and `compose_generation()` bumps whenever the
  // intermediate is (re)created, so the application knows to rebuild.
  // Before the first frame both are null/undefined and generation is 0.
  [[nodiscard]] bool hdr_compose_active() const noexcept {
    return config_.enable_hdr_compose && compose_ != nullptr &&
           compose_->ready();
  }
  [[nodiscard]] VkRenderPass hdr_render_pass() const noexcept {
        return compose_ != nullptr && compose_->ready()
                 ? compose_->scene_pass()
                 : VK_NULL_HANDLE;
  }
  [[nodiscard]] VkFormat hdr_format() const noexcept {
    return compose_ != nullptr ? compose_->hdr_format_present() : VK_FORMAT_UNDEFINED;
  }
  //! HDR depth format for a dynamic-created scene pipeline (Blade stage
  //! 3): the format the dynamic scene node's pDepthAttachment carries.
  //! UNDEFINED before the first frame, like hdr_format().
  [[nodiscard]] VkFormat hdr_depth_format() const noexcept {
    return compose_ != nullptr ? compose_->scene_depth_format()
                                : VK_FORMAT_UNDEFINED;
  }
  [[nodiscard]] std::uint32_t compose_generation() const noexcept {
    return compose_ != nullptr ? compose_->generation() : 0U;
  }

  [[nodiscard]] ::warploom::core::Result<std::uint32_t> begin_frame();
  [[nodiscard]] ::warploom::core::Result<void> record_commands(
      std::uint32_t image_index,
      VkFramebuffer framebuffer,
      std::uint32_t width, std::uint32_t height);
  //! Submit the acquired frame without presenting it. Keeps the image acquired.
  [[nodiscard]] ::warploom::core::Result<void> submit_frame();
  //! Present the frame previously submitted by submit_frame().
  [[nodiscard]] ::warploom::core::Result<void> present_frame();
  //! Submit and present the acquired frame.
  [[nodiscard]] ::warploom::core::Result<void> end_frame();
  //! Record an immutable indexed scene inside an active render pass. The
  //! scene pipeline uses the 128-byte view_projection + model push ABI;
  //! descriptor sets are supplied by the scene's mesh resource.
  [[nodiscard]] ::warploom::core::Result<void> record_scene(
      VkCommandBuffer command_buffer, const VulkanScene& scene,
      std::uint32_t width, std::uint32_t height) const;

  //! Record an immutable PBR (metallic-roughness) scene inside an active
  //! render pass. The PBR pipeline uses the 160-byte push ABI
  //! (view-projection + model + camera position + material index); sets are
  //! bound as: 0 = per-mesh vertex storage (each object's descriptor set),
  //! 1 = bindless sampler array (scene.texture_set, once), 2 = material SSBO
  //! (scene.material_set, once). Objects whose material_index is invalid are
  //! skipped.
  [[nodiscard]] ::warploom::core::Result<void> record_pbr_scene(
      VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
      std::uint32_t width, std::uint32_t height) const;

  //! Target resources the graph-driven PBR frame records into. The renderer
  //! never owns these; they must outlive the recording call.
  struct PbrFrameTargets {
    //! Shadow pre-pass (depth-only). Used when scene.shadow_pipeline is set.
    VkRenderPass shadow_render_pass{VK_NULL_HANDLE};
    VkFramebuffer shadow_framebuffer{VK_NULL_HANDLE};
    VkImage shadow_image{VK_NULL_HANDLE};
    VkFormat shadow_format{VK_FORMAT_UNDEFINED};
    std::uint32_t shadow_width{0};
    std::uint32_t shadow_height{0};
    //! Main lit pass.
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    std::uint32_t width{0};
    std::uint32_t height{0};
    //! Clear values for the main pass (may be null when the render pass has
    //! no clear-load attachments, e.g. color-only to an already-cleared
    //! target; most callers pass colour + depth).
    const VkClearValue* clear_values{nullptr};
    std::uint32_t clear_value_count{0};
  };

  //! Record the shadow-map depth pre-pass inside an active render pass.
  //! Same draw list semantics as record_pbr_scene (skips non-drawable meshes
  //! and invalid materials); LOD selection does not apply here — the shadow
  //! of an object is drawn from its full-detail mesh.
  [[nodiscard]] ::warploom::core::Result<void> record_shadow_pre_pass(
      VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
      std::uint32_t width, std::uint32_t height) const;

  //! Record the analytic sky pre-draw inside an active render pass (full-
  //! screen triangle, depth test on / writes off, LEQUAL vs cleared 1.0).
  //! Callers recording their own scene can compose it; record_pbr_scene
  //! invokes this when scene.sky_pipeline is set. No-op Ok when unset.
  [[nodiscard]] ::warploom::core::Result<void> record_sky_pre_draw(
      VkCommandBuffer command_buffer, const VulkanPbrScene& scene) const;

  //! Graph-driven whole-frame recording: compiles a [shadow pre-pass -> main
  //! lit pass] node sequence (empty when no shadow pipeline is set), lets
  //! compile_graph compute the shadow map's DEPTH_ATTACHMENT -> DEPTH_READ
  //! layout transition and write->read barrier, and records both passes via
  //! execute_graph in one command buffer. All other scene features (sky
  //! pre-draw, GPU LOD resolution, IBL/shadow/skin descriptor sets) behave
  //! exactly as in record_pbr_scene.
  [[nodiscard]] ::warploom::core::Result<void> record_pbr_frame(
      VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
      const PbrFrameTargets& targets) const;

  //! Borrowed resources for one renderer-recorded GPU-driven frame. The
  //! renderer records [cull compute -> indirect main draw] as graph nodes in
  //! ONE command buffer; compile_graph emits the compute -> draw-indirect
  //! buffer barrier from the command buffer edge, so the CPU never waits on
  //! or reads GPU cull results inside the frame. All handles are borrowed:
  //! they must be valid for the duration of the recording call.
  struct GpuDrivenPush {
    const void* data{nullptr};
    std::uint32_t size{0};
  };
  struct GpuDrivenFrame {
    //! Compute cull/LOD pass (writes VkDrawIndexedIndirectCommands + a
    //! visible counter into command_buffer_handle, per the proven
    //! cull_and_draw_lod.comp contract).
    VkPipeline cull_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout cull_pipeline_layout{VK_NULL_HANDLE};
    VkDescriptorSet cull_set{VK_NULL_HANDLE};
    std::uint32_t object_count{0};
    GpuDrivenPush cull_push{};
    //! Buffer holding the GPU-written draw commands (INDIRECT_USAGE).
    VkBuffer indirect_buffer{VK_NULL_HANDLE};
    //! Main lit pass drawing vkCmdDrawIndexedIndirect over the commands.
    //! Partitioned draws (R3: static commands through the static pipeline,
    //! skinned commands through the skinned pipeline) share one command
    //! buffer: draw_first_command selects the first command, and
    //! draw_command_count == 0 means object_count - draw_first_command.
    //! Defaults (0, 0) reproduce the historical whole-buffer draw.
    std::uint32_t draw_first_command{0U};
    std::uint32_t draw_command_count{0U};
    VkPipeline draw_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout draw_pipeline_layout{VK_NULL_HANDLE};
    //! Six entries: slots 0..5 cover per-object sets, bindless samplers,
    //! the material SSBO, the bone SSBO (the skinned partition's palette;
    //! the static partition leaves slot 3 unbound), the shadow map and
    //! the IBL resources.
    VkDescriptorSet draw_sets[6]{};
    std::uint32_t draw_set_count{0};
    //! Descriptor-set slot each entry of draw_sets binds to. The pipeline
    //! layouts in this engine reserve slot 3 for the bone SSBO, which only
    //! the skinned partition binds, so a static-partition frame is not
    //! contiguous: it binds 0,1,2 then 4,5. vkCmdBindDescriptorSets
    //! requires consecutive firstSet/count, so the recorder groups
    //! consecutive runs rather than assuming one range.
    std::uint32_t draw_set_slots[6]{0U, 1U, 2U, 3U, 4U, 5U};
    GpuDrivenPush draw_push{};
    VkBuffer index_buffer{VK_NULL_HANDLE};
    //! Target.
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    std::uint32_t width{0};
    std::uint32_t height{0};
    const VkClearValue* clear_values{nullptr};
    std::uint32_t clear_value_count{0};
  };

  //! Record the one-submission GPU-driven frame. Validation fails with
  //! invalid_config when required handles are missing. The compute node's
  //! dispatch is ceil(object_count / 64) groups (matches the shared shader).
  [[nodiscard]] ::warploom::core::Result<void> record_pbr_frame_gpu_driven(
      VkCommandBuffer command_buffer, const GpuDrivenFrame& frame) const;

  //! Record ONLY the compute cull/LOD pass: bind the cull pipeline and
  //! descriptor set, push the cull constants, dispatch ceil(object_count/64)
  //! groups, and emit the COMPUTE(WRITE) -> DRAW_INDIRECT(READ) buffer
  //! barrier the indirect draw needs.
  //!
  //! record_pbr_frame_gpu_driven runs the cull and the draw inside a render
  //! graph, which is the right shape when the caller wants the whole frame.
  //! An application that renders a shadow pass between the two -- so the
  //! shadow map is filled before anything is lit -- needs the halves
  //! separately, and previously had to hand-roll both.
  [[nodiscard]] ::warploom::core::Result<void> record_gpu_driven_cull(
      VkCommandBuffer command_buffer, const GpuDrivenFrame& frame) const;

  //! Record ONLY the main lit pass of the GPU-driven path, inside an
  //! ALREADY-BEGUN render pass: sets viewport/scissor, binds the draw
  //! pipeline, its descriptor sets and push constants, binds the shared
  //! index buffer, and issues ONE vkCmdDrawIndexedIndirect over the GPU-written
  //! commands. The counterpart of record_pbr_scene for a driven draw list.
  [[nodiscard]] ::warploom::core::Result<void> record_gpu_driven_draw(
      VkCommandBuffer command_buffer, std::uint32_t width,
      std::uint32_t height, const GpuDrivenFrame& frame) const;

  //! The chain's config is a projection of the renderer's; one conversion so
  //! both call sites cannot drift.
  [[nodiscard]] ComposeChainConfig compose_config() const noexcept {
    ComposeChainConfig cc{};
    cc.enable_bloom = config_.enable_bloom;
    cc.bloom_downscale = config_.bloom_downscale;
    cc.compose_shader_dir = config_.compose_shader_dir;
    cc.exposure = config_.exposure;
    cc.hdr_format = config_.hdr_format;
    cc.enable_dynamic_rendering = config_.enable_dynamic_rendering;
    cc.cross_queue_sharing = cross_queue_sharing_;
    return cc;
  }

  //! HDR compose chain: owned, never shared (B3b). A second instance is what
  //! frame capture will create to compose into its own target.
  std::unique_ptr<VulkanComposeChain> compose_;
  //! Presentation target identity, captured at initialize() and handed to the
  //! chain: the tonemap pipeline bakes in the target's render pass + format.
  VkRenderPass present_pass_{VK_NULL_HANDLE};
  VkFormat present_format_{VK_FORMAT_UNDEFINED};
  //! Headless presentation image/view (Blade stage 2): the dynamic tonemap
  //! writes it as a graph attachment. Set by initialize_headless; the real
  //! path resolves the swapchain image/view per frame instead, so these
  //! stay null there. Borrowed -- must outlive the renderer.
  VkImage headless_present_image_{VK_NULL_HANDLE};
  VkImageView headless_present_view_{VK_NULL_HANDLE};

  //! Full-screen passes moved to vulkan_fullscreen.hpp (B3b step 1): the
  //! compose chain and the graph callbacks both need them, and the chain must
  //! not reach back into renderer statics. `record_fullscreen_pass` and
  //! `fullscreen_graph_pass` are free functions there too.
    //! Rebind to a recreated swapchain and rebuilt pass/framebuffer resources.
  [[nodiscard]] ::warploom::core::Result<void> resync_for_swapchain(
      const VulkanSwapchain& swapchain, VkRenderPass render_pass);
  [[nodiscard]] ::warploom::core::Result<void> resync_for_swapchain(
      const VulkanSwapchain& swapchain, const VulkanRenderPass& render_pass);

  void wait_idle() noexcept;
  void cleanup(VkDevice device) noexcept;

  [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
  [[nodiscard]] std::uint32_t current_frame() const noexcept { return current_frame_; }
  [[nodiscard]] std::uint64_t frame_count() const noexcept { return frame_count_; }
  [[nodiscard]] const RendererConfig& config() const noexcept { return config_; }

  //! Bind the graphics pipeline used by record_commands().
  //! The pipeline must outlive any command buffer recorded with it.
  //! Declares the pipeline baked (render-pass created): the unified scene
  //! node records baked. Last write wins against set_dynamic_scene_pipeline.
  void set_pipeline(VkPipeline pipeline) noexcept {
    pipeline_ = pipeline;
    scene_pipeline_dynamic_ = false;
  }

  //! Bind the scene pipeline AND declare it dynamic-rendering created
  //! (via VulkanPipeline::create_graphics_pipeline_dynamic with the scene
  //! depth format). Blade stage 3: when dynamic rendering negotiated on AND
  //! the scene pipeline was bound here, the unified scene node records via
  //! vkCmdBeginRendering instead of the scene render pass. The framing
  //! mismatch (baked pipeline in a dynamic node, or vice versa) is a VUID
  //! and shades undefined, so the renderer never infers this -- the caller
  //! that created the pipeline states which it is. Last write wins.
  void set_dynamic_scene_pipeline(VkPipeline pipeline) noexcept {
    pipeline_ = pipeline;
    scene_pipeline_dynamic_ = true;
  }

  //! Application-owned scene recorder invoked by record_commands() INSIDE
  //! the render pass (after viewport/scissor), replacing the built-in
  //! three-vertex demo draw. This is the windowed-app frame hook: install it
  //! with a callback that calls record_pbr_scene / record_pbr_frame_gpu_
  //! driven / record_fullscreen_draw etc. When null, record_commands keeps
  //! the built-in demo draw. The callback returns false to fail the frame.
  using SceneRecordCallback = bool (*)(VkCommandBuffer command_buffer,
                                       std::uint32_t width,
                                       std::uint32_t height, void* user_data);
  void set_scene_record_callback(SceneRecordCallback callback,
                                 void* user_data = nullptr) noexcept {
    scene_record_callback_ = callback;
    scene_record_user_data_ = user_data;
  }

  //! Optional pre-pass hook: invoked with the raw frame command buffer
  //! BEFORE the main render pass begins. This is where an application
  //! records independent earlier passes (e.g. the shadow-map depth pre-pass
  //! through a graph, or compute) that the main pass then consumes. The
  //! hook owns its own render-pass begin/end; returning false fails the
  //! frame.
  using FramePrePassCallback = bool (*)(VkCommandBuffer command_buffer,
                                        std::uint32_t width,
                                        std::uint32_t height, void* user_data);
  //! Install (or replace) the PRIMARY pre-pass. Chained passes added with
  //! add_frame_pre_pass_callback are NOT cleared: two owners of one hook is an
  //! API gap (R1b), and "last writer wins" was how the path-tracing round was
  //! silently displaced by the shadow pass -- every signal said PT was running
  //! while its callback never fired again. Call clear_frame_pre_pass_chain()
  //! to drop the extras deliberately.
  void set_frame_pre_pass_callback(FramePrePassCallback callback,
                                   void* user_data = nullptr) noexcept {
    frame_pre_pass_callback_ = callback;
    frame_pre_pass_user_data_ = user_data;
  }
  //! Drop every chained pass added with add_frame_pre_pass_callback.
  void clear_frame_pre_pass_chain() noexcept { extra_pre_passes_.clear(); }

  //! Register an ADDITIONAL pre-pass pass, chained after the existing one in
  //! registration order. `set_frame_pre_pass_callback` replaces; this composes.
  //!
  //! Why this exists: R1b's path-tracing round and the shadow-map depth pass
  //! both need the pre-pass, and a single slot meant whichever was installed
  //! last silently won. In the viewport that was invisible: the PT stack built,
  //! the mode switch worked, no validation error, and the captured frames never
  //! converged -- because its callback was overwritten 20 lines later by the
  //! shadow pass and never ran again. Two owners of one hook is an API gap, not
  //! an application bug.
  //! Returns the registration index for remove_frame_pre_pass_callback, or
  //! SIZE_MAX if the slot list is full.
  [[nodiscard]] std::size_t add_frame_pre_pass_callback(
      FramePrePassCallback callback, void* user_data = nullptr) noexcept;
  //! Remove a pass added by add_frame_pre_pass_callback. `set_...` clears them
  //! all (it means "the application wants one pass and owns the slot"), so call
  //! this first if a chain is in use.
  void remove_frame_pre_pass_callback(std::size_t index) noexcept;

  //! Install the application-owned H-Z recorder used after the depth pass.
  void set_hiz_record_callback(HiZRecordCallback callback, void* user_data = nullptr) noexcept {
    hiz_record_callback_ = callback;
    hiz_record_user_data_ = user_data;
  }
  //! Build the graph contract corresponding to one renderer-owned H-Z frame.
  [[nodiscard]] HiZGraphPlan make_hiz_graph_plan(const HiZFrameRecord& record) const;

  //! Enable Synchronization 2 submission path (requires negotiated device).
  void set_synchronization2(bool enabled) noexcept { synchronization2_ = enabled; }
  [[nodiscard]] bool uses_synchronization2() const noexcept { return synchronization2_; }
  //! Enable timeline-semaphore frame pacing (requires negotiated Vulkan 1.2 feature).
  //! Replaces binary fence waits with a monotonic frame-counter timeline:
  //! CPU throttling and per-image reuse waits use exact signaled frame values.
  void set_timeline_pacing(bool enabled) noexcept { timeline_pacing_requested_ = enabled; }
  [[nodiscard]] bool uses_timeline_pacing() const noexcept { return timeline_pacing_; }
  //! Monotonic count of successfully submitted frames (timeline signal value).
  [[nodiscard]] std::uint64_t frame_counter() const noexcept { return frame_counter_; }
  //! Query function pointers from the device each initialize(); nullptr on 1.2 devices.
  [[nodiscard]] const GpuTiming& gpu_timing() const noexcept { return gpu_timing_; }

  //! CPU frame-time telemetry: one sample per successfully presented frame,
  //! measured from begin_frame() to present completion. Windowed percentiles
  //! (p50/p90/p99/p99.9/max) over the most recent samples.
  void record_frame_latency(bool enabled) noexcept { frame_latency_enabled_ = enabled; }
  [[nodiscard]] const ::warploom::core::LatencyStats& frame_latency_stats();
  [[nodiscard]] const ::warploom::core::LatencyTracker<>& frame_latency_tracker() const noexcept {
    return frame_latency_;
  }

  //! True when renderer-owned persistent H-Z resources are available.
  [[nodiscard]] bool hiz_enabled() const noexcept { return hiz_enabled_; }
  //! True when the renderer also owns the reduction compute pipeline.
  [[nodiscard]] bool hiz_direct_enabled() const noexcept { return hiz_direct_enabled_; }

  //! Which depth buffer an H-Z reduction must read this frame. Under HDR
  //! compose the scene renders into the HDR intermediate's own depth, so the
  //! swapchain's depth is a stale attachment describing an earlier frame.
  //! Reducing it would publish a pyramid that does not match what was drawn.
  struct HiZDepthSource {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    bool sampleable{false};
    [[nodiscard]] bool available() const noexcept { return image != VK_NULL_HANDLE; }
  };
  [[nodiscard]] static HiZDepthSource select_hiz_depth_source(
      bool compose, HiZDepthSource hdr_depth, HiZDepthSource swapchain_depth);
  //! State contract for current/previous H-Z selection and invalidation.
  [[nodiscard]] const VulkanHiZFrameState& hiz_frame_state() const noexcept {
    return hiz_state_;
  }
  [[nodiscard]] VulkanHiZFrameState& hiz_frame_state() noexcept { return hiz_state_; }
  //! Begin/complete/discard are the only supported publication sequence for H-Z.
  [[nodiscard]] HiZFrameToken begin_hiz_frame() const noexcept { return hiz_state_.begin_frame(); }
  void complete_hiz_frame(const HiZFrameToken& token) noexcept;
  void discard_hiz_frame(const HiZFrameToken& token) noexcept;
  void invalidate_hiz(HiZInvalidation reason) noexcept { hiz_state_.invalidate(reason); }
  [[nodiscard]] const VulkanHiZPyramid* hiz_pyramid(std::uint32_t index) const noexcept;
  //! Persisted depth snapshot for ping-pong slot `index` (loop28 step 2),
  //! or null when snapshots are unsupported/disabled. Rests in
  //! SHADER_READ_ONLY_OPTIMAL after any completed H-Z frame.
  //! True when the compute-queue reduce path is armed (config requested AND
  //! a dedicated COMPUTE-only family exists AND H-Z direct mode is on).
  [[nodiscard]] bool hiz_compute_reduce_enabled() const noexcept {
    return hiz_compute_enabled_;
  }
  //! True when the fire-and-forget pipelined leg is armed (explicitly
  //! requested on top of an armed compute leg: per-slot gfx_done
  //! semaphores created). Submit then kicks the compute reduce with a
  //! timeline handoff instead of the CPU blocking.
  [[nodiscard]] bool hiz_pipelined_enabled() const noexcept {
    return hiz_pipelined_enabled_;
  }
  //! True when the frame loop pipelines the compute H-Z leg (step 4).
  [[nodiscard]] bool hiz_compute_pipelined_enabled() const noexcept {
    return hiz_pipelined_enabled_;
  }
  //! Timeline value the compute queue last signalled (0 before any kick).
  //! Lets a test prove the pipelined leg was kicked asynchronously instead
  //! of CPU-waited: N frames without wait_idle must still advance this.
  [[nodiscard]] std::uint64_t hiz_compute_last_signal() const noexcept {
    return hiz_compute_queue_.is_initialized()
               ? hiz_compute_queue_.last_timeline_value()
               : 0U;
  }
  //! Reduce the persisted snapshot for `slot` on the compute queue
  //! (loop28 step 3, synchronous proof shape): records the same per-level
  //! dispatches the graphics path would run -- reading the snapshot view
  //! instead of live depth -- plus the pack copy, submits on the dedicated
  //! compute queue, and CPU-waits for completion. The destination pyramid
  //! and packed buffer then hold THIS snapshot's reduction, bit-identical
  //! to the graphics-queue result. Returns the signalled timeline value.
  //! Fails cleanly (never partial) when compute is unavailable, the slot
  //! has no snapshot, or submission fails.
  [[nodiscard]] ::warploom::core::Result<std::uint64_t> compute_reduce_hiz_snapshot(
      std::uint32_t slot);
  [[nodiscard]] VkImage hiz_depth_snapshot_image(std::uint32_t index) const noexcept {
    return index < 2U ? hiz_depth_snapshot_[index] : VK_NULL_HANDLE;
  }
  [[nodiscard]] VkImageView hiz_depth_snapshot_view(std::uint32_t index) const noexcept {
    return index < 2U ? hiz_depth_snapshot_view_[index] : VK_NULL_HANDLE;
  }
  [[nodiscard]] bool hiz_depth_snapshots_valid() const noexcept {
    return hiz_snapshot_valid_;
  }
  //! Live depth image the last H-Z frame was built from (null before any).
  [[nodiscard]] VkImage hiz_last_depth_image() const noexcept {
    return hiz_last_depth_image_;
  }
  //! Snapshot slot written by the last H-Z frame (0/1, 2 = none yet).
  [[nodiscard]] std::uint32_t hiz_last_snapshot_slot() const noexcept {
    return hiz_last_snapshot_slot_;
  }
  struct PackedPyramid;  //!< Defined with the H-Z members below.
  struct PackedPyramid {
    Allocation buffer{};  //!< valid() once created; device-local, no mapping
    std::vector<std::uint32_t> level_offsets;  //!< words, per level
    std::vector<std::pair<std::uint32_t, std::uint32_t>> level_dims;
    std::uint64_t built_generation{0};  //!< hiz_state_ generation copied last
  };

  //! Packed pyramid for the occlusion cull (B5b). Valid after a frame whose
  //! H-Z build completed; built_generation distinguishes fresh from stale.
  //! Out-of-line accessor: the type is incomplete here.
  [[nodiscard]] const PackedPyramid& hiz_packed_pyramid() const noexcept;
  [[nodiscard]] VulkanHiZPyramid* hiz_pyramid(std::uint32_t index) noexcept;

  [[nodiscard]] ::warploom::core::Result<void> recreate_hiz_resources(
      std::uint32_t render_width, std::uint32_t render_height);

  [[nodiscard]] static ::warploom::core::Result<VkCommandPool> create_command_pool(
      VkDevice device, std::uint32_t queue_family_index);
  [[nodiscard]] static ::warploom::core::Result<VkCommandBuffer> allocate_command_buffer(
      VkDevice device, VkCommandPool pool);

private:
  void cleanup_hiz_pipeline_resources() noexcept;
  //! Release the loop28-step-2 persisted depth snapshots (device must be
  //! idle; called before the binding allocator is torn down).
  void destroy_hiz_depth_snapshots() noexcept;
  //! Re-point a slot's compute sets at its snapshot view (loop28 step 3).
  //! Called after a snapshot is (re)created; no-op when the view is
  //! unchanged (steady state performs zero updates). Returns false when a
  //! descriptor write fails (caller marks snapshots invalid, never fails
  //! the frame).
  [[nodiscard]] bool repoint_hiz_compute_sets(std::uint32_t slot) noexcept;
  //! AsyncComputeQueue::record callback: reduce + pack for one snapshot
  //! slot onto the compute command buffer.
  static void record_hiz_compute_work(VkCommandBuffer command_buffer, void* user_data);
#ifdef OMNICPP_HAS_VULKAN
  //! Ensure the snapshot slot holds a depth image matching width/height/
  //! format (loop28 step 2, lazy: the compose HDR depth may not exist when
  //! recreate_hiz_resources runs). Creates on first use and recreates on
  //! extent/format drift. Returns false on Vulkan failure (caller skips the
  //! copy and leaves snapshots invalid, never failing the frame).
  [[nodiscard]] bool ensure_hiz_depth_snapshot(std::uint32_t slot,
                                               std::uint32_t width,
                                               std::uint32_t height,
                                               VkFormat format) noexcept;
  //! Copy the frame's live depth into the snapshot slot. The depth's
  //! incoming layout depends on the caller: the inline path copies AFTER
  //! the reduction (depth in SHADER_READ_ONLY_OPTIMAL, returned there),
  //! the pipelined snapshot-only path copies the depth it just rendered
  //! (still in DEPTH_STENCIL_ATTACHMENT_OPTIMAL, returned there — a pure
  //! borrow; the compute leg never touches the live depth). Best-effort:
  //! failures leave snapshots invalid but never fail the reduction.
  void copy_hiz_depth_to_snapshot(VkCommandBuffer command_buffer,
                                   const HiZFrameRecord& record,
                                   VkFormat snapshot_format,
                                   bool depth_from_attachment) noexcept;
#endif
#ifdef OMNICPP_HAS_VULKAN
  [[nodiscard]] bool record_hiz_reduction(VkCommandBuffer command_buffer,
                                           const HiZFrameRecord& record);
  //! Loop28 step 4, record half: the snapshot copy without the inline
  //! reduce + pack (the compute leg produces the pyramid a frame behind).
  //! Best-effort like the copy it wraps: never fails the frame.
  void record_hiz_snapshot_only(VkCommandBuffer command_buffer,
                                 const HiZFrameRecord& record) noexcept;
  //! Loop28 step 4, pre-scene half: pack the compute-produced pyramid for
  //! the cull before the scene reads it. The slot is this frame's write
  //! index — the compute leg kicked two frames ago wrote exactly it, and
  //! the submit wait about to be recorded orders that write before this
  //! frame executes. Skipped until the slot's first kick (signal == 0),
  //! so startup/recreate frames keep whatever the pack held, same as the
  //! one undefined-pack frame the inline path has at startup.
  void pack_hiz_pyramid_for_cull(VkCommandBuffer command_buffer) noexcept;
  //! Loop28 step 4, submit half: reduce the just-written snapshot for
  //! `slot` on the compute queue WITHOUT CPU-waiting (the timeline
  //! handoff carries the ordering). Records the signalled value so the
  //! next reuse of the slot waits exactly this kick.
  void kick_hiz_compute_reduce(std::uint32_t slot) noexcept;
  //! Snapshot format owning `record.depth_image` (compose HDR depth vs
  //! swapchain depth), or UNDEFINED when the owner is unknown. Shared by
  //! the inline reduce path and the pipelined snapshot-only path.
  [[nodiscard]] VkFormat
  hiz_snapshot_format_for_record(const HiZFrameRecord& record) noexcept;
  //! Compute-queue record for one snapshot slot (loop28 steps 3-4): the
  //! same geometry the graphics path would reduce, reading the snapshot.
  //! Returns false when the slot has no snapshot yet (caller skips).
  [[nodiscard]] bool
  make_hiz_compute_record(std::uint32_t slot, HiZFrameRecord& record);
#endif

  static void record_hiz_graph_pass(VkCommandBuffer command_buffer,
                                    const GraphComputePass& pass, void* user_data);
  //! Shared pack body (graphics inline, compute-queue, pipelined pre-scene
  //! copy): per-mip SHADER_READ_ONLY -> TRANSFER_SRC -> copy -> back, level
  //! 0 first at monotonic word offsets. One body so the three record paths
  //! cannot drift (the fill_blas_geometry lesson). The caller owns
  //! freshness (built_generation): a pack is only as new as its pyramid.
  static void pack_hiz_pyramid_levels(VkCommandBuffer command_buffer,
                                      const VulkanHiZPyramid* pyramid,
                                      std::uint32_t levels,
                                      PackedPyramid& pack) noexcept;
  void record_hiz_dispatch(VkCommandBuffer command_buffer,
                           const GraphComputePass& pass);
  //! One node of the unified scene+compose frame graph (T0): either the
  //! scene contents or one compose stage. Defined in vulkan_renderer.cpp.
  struct UnifiedFrameNode;
  static void record_unified_frame_node(VkCommandBuffer command_buffer,
                                        const GraphPass& pass,
                                        void* user_data);
  //! Dynamic twin of record_unified_frame_node (Blade stage 1): routes a
  //! dynamic compose node (bloom) to record_compose_dynamic_stage. The
  //! scene never records dynamically, so a scene-marked dispatch here is a
  //! wiring error and fails the frame rather than recording nothing.
  static void record_unified_frame_dynamic_node(
      VkCommandBuffer command_buffer, const GraphDynamicPass& pass,
      void* user_data);
#ifdef OMNICPP_HAS_VULKAN
  //! Scene contents for the unified graph's scene node: viewport/scissor
  //! plus the application callback (or the built-in demo draw). Returns
  //! false when the application callback fails the frame.
  [[nodiscard]] bool record_scene_node_contents(VkCommandBuffer command_buffer,
                                                std::uint32_t width,
                                                std::uint32_t height) const;
#endif

  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkQueue graphics_queue_{VK_NULL_HANDLE};
  VkQueue present_queue_{VK_NULL_HANDLE};
  //! H-Z cross-queue sharing (loop28 step 1), captured from the context at
  //! initialize: concurrent over {graphics, compute} with a dedicated
  //! COMPUTE-only family, exclusive otherwise. Flows into the pyramid
  //! images (recreate_hiz_resources) and the compose HDR depth (compose
  //! chain config). Set once in initialize_common; both initialize paths
  //! funnel there.
  ImageSharing cross_queue_sharing_{};
  VkCommandPool command_pool_{VK_NULL_HANDLE};
  VkRenderPass render_pass_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  //! True when pipeline_ was bound via set_dynamic_scene_pipeline (Blade
  //! stage 3): the unified scene node may record dynamically. False by
  //! default, so every existing caller (baked pipelines) is untouched.
  bool scene_pipeline_dynamic_{false};
  SceneRecordCallback scene_record_callback_{nullptr};
  void* scene_record_user_data_{nullptr};
  FramePrePassCallback frame_pre_pass_callback_{nullptr};
  void* frame_pre_pass_user_data_{nullptr};
  //! Additional chained pre-passes (see add_frame_pre_pass_callback). The
  //! primary runs first, then these in registration order.
  struct ExtraPrePass {
    FramePrePassCallback callback{nullptr};
    void* user_data{nullptr};
  };
  std::vector<ExtraPrePass> extra_pre_passes_{};
  static constexpr std::size_t kMaxExtraPrePasses = 4U;
  const VulkanSwapchain* swapchain_{nullptr};
  //! No swapchain: recording goes to an offscreen framebuffer (see
  //! initialize_headless). Nothing else in the renderer changes.
  bool headless_{false};
  const VulkanRenderPass* render_pass_resource_{nullptr};
  std::vector<FrameResources> frames_;

  // Persistent renderer-owned H-Z pair. Declaration order is intentional:
  // pyramids are destroyed before the allocator on teardown.
  VulkanHiZFrameState hiz_state_{};
  std::unique_ptr<VulkanMemoryAllocator> hiz_allocator_;
  std::unique_ptr<VulkanHiZPyramid> hiz_pyramids_[2];

  // --- Packed pyramid (B5b) ---------------------------------------------
  //! The occlude cull shader (cull_and_draw_lod_occlude.comp) reads the
  //! pyramid as packed float-bit words from a BUFFER, not an image. After each
  //! H-Z build the mips are copied here (image -> buffer, per level, at
  //! monotonically increasing word offsets) so the NEXT frame's cull can bind
  //! the whole pyramid as one storage buffer with 1-frame latency. Word
  //! layout: level 0 tiles first (row-major, 0u = tile never written), then
  //! each halved level. Level dims derive from hiz_tile_size, matching the
  //! shader's walk.
  PackedPyramid hiz_packed_;
  std::unique_ptr<VulkanDescriptorManager> hiz_descriptor_manager_;
  std::unique_ptr<VulkanPipeline> hiz_reduction_pipeline_;
  VkDescriptorSetLayout hiz_reduction_layout_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> hiz_reduction_sets_[2];
  //! Depth view currently bound in each index's reduction sets (binding 0
  //! every level, binding 2 at level 0). The sets are written once at H-Z
  //! setup, but the frame's depth source varies -- the compose chain's scene
  //! depth under HDR compose, the swapchain depth otherwise -- and either is
  //! rebuilt on resize, so the binding is refreshed when it drifts.
  VkImageView hiz_bound_depth_view_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
  // --- Compute-queue reduce (loop28 step 3) ----------------------------
  //! Snapshot-reading twin of hiz_reduction_sets_: binding 0 every level
  //! and binding 2 at level 0 point at the persisted snapshot view for the
  //! slot instead of the live depth view. Allocated with the graphics sets
  //! (same layout, 2x levels sets) and re-pointed when a snapshot is
  //! (re)created. Only consumed by the compute-queue reduce; the graphics
  //! path never touches these.
  std::vector<VkDescriptorSet> hiz_compute_sets_[2];
  //! View currently bound in each slot's compute sets (snapshot view).
  VkImageView hiz_compute_bound_view_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
  //! Set array the dispatch callback binds (graphics sets on the graphics
  //! record path, compute sets on the compute record path). Null outside a
  //! reduction record.
  const std::vector<VkDescriptorSet>* active_hiz_sets_{nullptr};
  //! Renderer-owned compute submission context (dedicated COMPUTE-only
  //! family). Initialized when hiz_compute_reduce is requested AND the
  //! device provides a dedicated family; otherwise the reduce stays on
  //! graphics regardless of the flag.
  AsyncComputeQueue hiz_compute_queue_;
  VkQueue hiz_compute_raw_queue_{VK_NULL_HANDLE};
  std::uint32_t hiz_compute_family_{0U};
  bool hiz_compute_available_{false};
  //! True when the frame loop (or tests) should reduce on compute. Requires
  //! hiz_compute_available_ AND the config flag; kept as a member so the
  //! record path branches once instead of re-deriving it per frame.
  bool hiz_compute_enabled_{false};
  //! Loop28 step 4: true when the frame loop pipelines the compute leg
  //! (config hiz_compute_pipelined AND hiz_compute_enabled_). The record
  //! path then skips the inline reduce (snapshot copy stays) and submit
  //! kicks the compute reduce + hands off over the timeline semaphore.
  bool hiz_pipelined_enabled_{false};
  //! Timeline value the last kicked compute reduce signalled, per
  //! ping-pong slot (0 = never kicked: nothing to wait for). The graphics
  //! submit for a frame reusing `slot` waits this value, which orders the
  //! snapshot overwrite, the pyramid rewrite, AND the cull's pack read
  //! behind the compute leg that produced them — one wait, three edges.
  std::uint64_t hiz_compute_signal_[2]{0U, 0U};
  //! Binary gfx->compute handoff, per slot: the graphics submit signals
  //! done[slot], the kicked compute reduce waits it before sampling the
  //! snapshot the graphics frame just wrote. Submission order across
  //! queues is not execution order, so this edge must be explicit.
  VkSemaphore hiz_gfx_done_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
  // --- Persisted per-frame H-Z depth (loop28 step 2) ---------------------
  //! The live depth attachment is overwritten by the next frame's scene
  //! render, so a compute-queue reduce (loop28 step 3) cannot read it behind
  //! a same-frame wait without serializing the queues. Each frame's depth is
  //! therefore copied here on the graphics queue right after the
  //! reduction+pack; the snapshot rests in SHADER_READ_ONLY_OPTIMAL and is
  //! indexed by the same ping-pong token as the destination pyramid. Step 2
  //! writes but does not read the snapshots (the reduction still samples the
  //! live depth: no behaviour change); step 3 moves the reduce onto the
  //! compute queue reading the snapshot.
  VkImage hiz_depth_snapshot_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkImageView hiz_depth_snapshot_view_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
  Allocation hiz_depth_snapshot_alloc_[2]{};
  bool hiz_snapshot_initialized_[2]{false, false};
  bool hiz_snapshot_valid_{false};
  std::uint32_t hiz_snapshot_width_[2]{0U, 0U};
  std::uint32_t hiz_snapshot_height_[2]{0U, 0U};
  VkFormat hiz_snapshot_format_[2]{VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
  //! Depth source the last H-Z frame copied from (diagnostic/test
  //! introspection: lets a test read back the live depth the snapshot must
  //! match without reaching into the compose chain).
  VkImage hiz_last_depth_image_{VK_NULL_HANDLE};
  VkImageView hiz_last_depth_view_{VK_NULL_HANDLE};
  std::uint32_t hiz_last_snapshot_slot_{2U};
  bool hiz_pyramid_initialized_[2]{false, false};
  bool hiz_direct_enabled_{false};
  bool hiz_enabled_{false};
  std::uint32_t pending_hiz_destination_index_{0};
  HiZFrameToken pending_hiz_token_{};
  bool pending_hiz_frame_{false};
  const HiZFrameRecord* active_hiz_record_{nullptr};
  std::uint32_t hiz_dispatch_count_{0};
  HiZRecordCallback hiz_record_callback_{nullptr};
  void* hiz_record_user_data_{nullptr};
  // A present operation may retain its signal semaphore after the frame slot
  // advances, so render-finished semaphores are owned by swapchain image.
  std::vector<VkSemaphore> render_finished_semaphores_;
  std::uint32_t current_frame_{0};
  std::uint64_t frame_count_{0};
  bool initialized_{false};
  RendererConfig config_;
  std::uint32_t acquired_image_index_{0};
  bool frame_acquired_{false};
  std::vector<VkFence> images_in_flight_;
  bool synchronization2_{false};
  bool timeline_pacing_requested_{false};
  bool timeline_pacing_{false};
  VkSemaphore timeline_semaphore_{VK_NULL_HANDLE};
  std::uint64_t frame_counter_{0};
  // Timeline mode: frame value whose submit last rendered each swapchain image.
  std::vector<std::uint64_t> image_last_frame_;
  GpuTiming gpu_timing_{};
  //! GPU timestamp queries (config_.enable_gpu_timing): 2 per frame slot,
  //! reset+written at record time, resolved on slot reuse (the begin_frame
  //! wait guarantees the previous submit on this slot completed).
  VkQueryPool timestamp_pool_{VK_NULL_HANDLE};
  std::vector<bool> timestamp_valid_;
  float timestamp_period_ns_{0.0f};
  bool gpu_timing_enabled_{false};
  //! Resolve the previous frame's timestamps for `slot` into gpu_timing_.
  void resolve_gpu_timestamps(std::uint32_t slot) noexcept;
  // CPU frame-time telemetry.
  static constexpr std::int64_t kNoTimestamp = -1;
  std::int64_t frame_begin_ns_{kNoTimestamp};
  bool frame_latency_enabled_{true};
  ::warploom::core::LatencyTracker<> frame_latency_{};
  ::warploom::core::LatencyStats frame_latency_stats_{};
};

} // namespace warploom::render

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef WARPLOOM_COMPAT_RENDER_NS
#define WARPLOOM_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // WARPLOOM_COMPAT_RENDER_NS
