#pragma once

/**
 * @file vulkan_renderer.hpp
 * @brief Vulkan renderer: command buffers, frame synchronization, draw loop.
 */

#include "engine/core/deterministic_runtime.hpp"
#include "engine/core/latency_telemetry.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_swapchain.hpp"
#include "engine/render/vulkan_render_pass.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_hiz_frame_state.hpp"
#include "engine/render/vulkan_hiz_pyramid.hpp"
#include "engine/render/vulkan_render_graph.hpp"
#include "engine/render/vulkan_scene.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omnicpp::render {

//! Per-frame GPU timing telemetry in nanoseconds.
struct GpuTiming {
  std::uint64_t render_pass_ns{0};
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
};

class VulkanRenderer final {
public:
  VulkanRenderer() = default;
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;
  VulkanRenderer(VulkanRenderer&&) = delete;
  VulkanRenderer& operator=(VulkanRenderer&&) = delete;

  [[nodiscard]] omnicpp::core::Result<void> initialize(
      VulkanContext& context,
      const VulkanSwapchain& swapchain,
      const VulkanRenderPass& render_pass,
      const RendererConfig& config = {});

  [[nodiscard]] omnicpp::core::Result<std::uint32_t> begin_frame();
  [[nodiscard]] omnicpp::core::Result<void> record_commands(
      std::uint32_t image_index,
      VkFramebuffer framebuffer,
      std::uint32_t width, std::uint32_t height);
  //! Submit the acquired frame without presenting it. Keeps the image acquired.
  [[nodiscard]] omnicpp::core::Result<void> submit_frame();
  //! Present the frame previously submitted by submit_frame().
  [[nodiscard]] omnicpp::core::Result<void> present_frame();
  //! Submit and present the acquired frame.
  [[nodiscard]] omnicpp::core::Result<void> end_frame();
  //! Record an immutable indexed scene inside an active render pass. The
  //! scene pipeline uses the 128-byte view_projection + model push ABI;
  //! descriptor sets are supplied by the scene's mesh resource.
  [[nodiscard]] omnicpp::core::Result<void> record_scene(
      VkCommandBuffer command_buffer, const VulkanScene& scene,
      std::uint32_t width, std::uint32_t height) const;

  //! Record an immutable PBR (metallic-roughness) scene inside an active
  //! render pass. The PBR pipeline uses the 160-byte push ABI
  //! (view-projection + model + camera position + material index); sets are
  //! bound as: 0 = per-mesh vertex storage (each object's descriptor set),
  //! 1 = bindless sampler array (scene.texture_set, once), 2 = material SSBO
  //! (scene.material_set, once). Objects whose material_index is invalid are
  //! skipped.
  [[nodiscard]] omnicpp::core::Result<void> record_pbr_scene(
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
  [[nodiscard]] omnicpp::core::Result<void> record_shadow_pre_pass(
      VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
      std::uint32_t width, std::uint32_t height) const;

  //! Graph-driven whole-frame recording: compiles a [shadow pre-pass -> main
  //! lit pass] node sequence (empty when no shadow pipeline is set), lets
  //! compile_graph compute the shadow map's DEPTH_ATTACHMENT -> DEPTH_READ
  //! layout transition and write->read barrier, and records both passes via
  //! execute_graph in one command buffer. All other scene features (sky
  //! pre-draw, GPU LOD resolution, IBL/shadow/skin descriptor sets) behave
  //! exactly as in record_pbr_scene.
  [[nodiscard]] omnicpp::core::Result<void> record_pbr_frame(
      VkCommandBuffer command_buffer, const VulkanPbrScene& scene,
      const PbrFrameTargets& targets) const;

  //! One full-screen triangle sampling up to 4 source images (post-process:
  //! tonemap/FXAA/bloom combine, sky resolve, SSAO blur...). The graph node
  //! declares the sources as sampled_images so compile_graph computes the
  //! producer->sample layout transitions. Callback-based so tests can also
  //! use it inside execute_graph nodes; the direct-call form below records
  //! its own render pass begin/end.
  struct FullscreenPass {
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    std::uint32_t width{0};
    std::uint32_t height{0};
    //! Clear values for the target (color+depth as declared by the render
    //! pass; may be null when every attachment load-ops LOAD).
    const VkClearValue* clear_values{nullptr};
    std::uint32_t clear_value_count{0};
    //! Images the shader samples; layouts must match the descriptor writes.
    struct Sampled {
      VkImage image{VK_NULL_HANDLE};
      VkImageLayout layout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      std::uint32_t aspect{1};  //!< VkImageAspectFlags; 1 = COLOR_BIT.
    };
    std::array<Sampled, 4> samples{};
    std::uint32_t sample_count{0};
    //! Optional draw parameters (default: 3-vertex triangle, 1 instance).
    std::uint32_t vertex_count{3};
    std::uint32_t instance_count{1};
  };

  //! Record one FullscreenPass inside an ACTIVE render pass (no begin/end,
  //! viewport/scissor to width x height, bind pipeline + set 0 = the caller's
  //! single set, draw). Callback-compatible with graph record hooks.
  [[nodiscard]] omnicpp::core::Result<void> record_fullscreen_draw(
      VkCommandBuffer command_buffer, const FullscreenPass& pass,
      VkDescriptorSet set0) const;

  //! Graph node wrapper: records render-pass begin + record_fullscreen_draw
  //! + end. Compatible with execute_graph's record_render hook via the
  //! user_data pointer pattern.
  [[nodiscard]] omnicpp::core::Result<void> record_fullscreen_pass(
      VkCommandBuffer command_buffer, const FullscreenPass& pass,
      VkDescriptorSet set0) const;

  //! Build a GraphPass for a FullscreenPass: extents/clears forwarded,
  //! sampled_images filled from pass.samples so compile_graph computes the
  //! producer -> sample transitions. Attachments stay empty (fullscreen
  //! targets have no in-graph producer; VkRenderPass owns their layout).
  //! The FullscreenPass must outlive the recording call (clear pointer).
  [[nodiscard]] inline GraphPass fullscreen_graph_pass(
      const FullscreenPass& pass) {
    GraphPass out{};
    out.name = "fullscreen";
    out.render_pass = pass.render_pass;
    out.framebuffer = pass.framebuffer;
    out.width = pass.width;
    out.height = pass.height;
    out.clear_values = pass.clear_values;
    out.clear_value_count = pass.clear_value_count;
    for (std::uint32_t i = 0;
         i < pass.sample_count && i < pass.samples.size(); ++i) {
      GraphSampledImage s{};
      s.image = pass.samples[i].image;
      s.used_layout = pass.samples[i].layout;
      s.aspect = pass.samples[i].aspect;
      out.sampled_images.push_back(s);
    }
    return out;
  }

  //! Rebind to a recreated swapchain and rebuilt pass/framebuffer resources.
  [[nodiscard]] omnicpp::core::Result<void> resync_for_swapchain(
      const VulkanSwapchain& swapchain, VkRenderPass render_pass);
  [[nodiscard]] omnicpp::core::Result<void> resync_for_swapchain(
      const VulkanSwapchain& swapchain, const VulkanRenderPass& render_pass);

  void wait_idle() noexcept;
  void cleanup(VkDevice device) noexcept;

  [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
  [[nodiscard]] std::uint32_t current_frame() const noexcept { return current_frame_; }
  [[nodiscard]] std::uint64_t frame_count() const noexcept { return frame_count_; }
  [[nodiscard]] const RendererConfig& config() const noexcept { return config_; }

  //! Bind the graphics pipeline used by record_commands().
  //! The pipeline must outlive any command buffer recorded with it.
  void set_pipeline(VkPipeline pipeline) noexcept { pipeline_ = pipeline; }

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
  [[nodiscard]] const omnicpp::core::LatencyStats& frame_latency_stats();
  [[nodiscard]] const omnicpp::core::LatencyTracker<>& frame_latency_tracker() const noexcept {
    return frame_latency_;
  }

  //! True when renderer-owned persistent H-Z resources are available.
  [[nodiscard]] bool hiz_enabled() const noexcept { return hiz_enabled_; }
  //! True when the renderer also owns the reduction compute pipeline.
  [[nodiscard]] bool hiz_direct_enabled() const noexcept { return hiz_direct_enabled_; }
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
  [[nodiscard]] VulkanHiZPyramid* hiz_pyramid(std::uint32_t index) noexcept;

  [[nodiscard]] omnicpp::core::Result<void> recreate_hiz_resources(
      std::uint32_t render_width, std::uint32_t render_height);

  [[nodiscard]] static omnicpp::core::Result<VkCommandPool> create_command_pool(
      VkDevice device, std::uint32_t queue_family_index);
  [[nodiscard]] static omnicpp::core::Result<VkCommandBuffer> allocate_command_buffer(
      VkDevice device, VkCommandPool pool);

private:
  void cleanup_hiz_pipeline_resources() noexcept;
#ifdef OMNICPP_HAS_VULKAN
  [[nodiscard]] bool record_hiz_reduction(VkCommandBuffer command_buffer,
                                           const HiZFrameRecord& record);
#endif

  static void record_hiz_graph_pass(VkCommandBuffer command_buffer,
                                    const GraphComputePass& pass, void* user_data);
  void record_hiz_dispatch(VkCommandBuffer command_buffer,
                           const GraphComputePass& pass);

  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkQueue graphics_queue_{VK_NULL_HANDLE};
  VkQueue present_queue_{VK_NULL_HANDLE};
  VkCommandPool command_pool_{VK_NULL_HANDLE};
  VkRenderPass render_pass_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  const VulkanSwapchain* swapchain_{nullptr};
  const VulkanRenderPass* render_pass_resource_{nullptr};
  std::vector<FrameResources> frames_;
  // Persistent renderer-owned H-Z pair. Declaration order is intentional:
  // pyramids are destroyed before the allocator on teardown.
  VulkanHiZFrameState hiz_state_{};
  std::unique_ptr<VulkanMemoryAllocator> hiz_allocator_;
  std::unique_ptr<VulkanHiZPyramid> hiz_pyramids_[2];
  std::unique_ptr<VulkanDescriptorManager> hiz_descriptor_manager_;
  std::unique_ptr<VulkanPipeline> hiz_reduction_pipeline_;
  VkDescriptorSetLayout hiz_reduction_layout_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> hiz_reduction_sets_[2];
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
  // CPU frame-time telemetry.
  static constexpr std::int64_t kNoTimestamp = -1;
  std::int64_t frame_begin_ns_{kNoTimestamp};
  bool frame_latency_enabled_{true};
  omnicpp::core::LatencyTracker<> frame_latency_{};
  omnicpp::core::LatencyStats frame_latency_stats_{};
};

} // namespace omnicpp::render
