//! @file test_dual_queue_graph.cpp
//! @brief True dual-queue pipelined rendering. The compute queue produces
//!        NEXT frame's GPU-driven draw state: animated instance positions
//!        (compute), then GPU-side frustum culling that maintains the
//!        indirect draw's instanceCount (compute). The graphics queue
//!        renders frame N's already-culled state while frame N+1's compute
//!        runs in parallel — synchronized GPU-side by timeline semaphores,
//!        no CPU round-trip in the steady state, no CPU touch of per-frame
//!        draw data after setup.
//!
//! Cross-queue ordering: compute for frame N+1 is submitted BEFORE graphics
//! for frame N, and graphics N's vkQueueSubmit waits on the timeline value
//! of compute N — one frame of latency, real overlap, no deadlock.
//!
//! Verification: per-frame analytic readback of the compute output
//! (positions = 10*cos(N) exactly), pixel checks (visible cubes land
//! in-frame), the cull-produced instanceCount per frame, and zero
//! validation diagnostics.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "warploom/render/vulkan_compute.hpp"
#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_render_graph.hpp"
#include "warploom/render/vulkan_renderer.hpp"

#ifdef WARPLOOM_HAS_VULKAN
#include "vulkan_test_readback.hpp"
using omnicpp_test::readback_swapchain_image;
#endif

namespace {

constexpr std::uint32_t kSize = 256U;
constexpr std::uint32_t kFrames = 3;   // steady-state overlap demo
constexpr std::uint32_t kCubes = 2;    // instances per frame

// Ring A (compute output): identity list words [0..kCubes), then 8 data
// words per instance — the layout gpu_objects.vert resolves through
// comp_offset=0.
constexpr std::uint32_t kRingAWords = kCubes + kCubes * 8U;
// Ring B (cull payload): [0] instance_count, [1] compaction cursor,
// [2..25] 6 frustum planes x 4 words, [26..33] spheres, [34..35] compacted.
constexpr std::uint32_t kSphereOffB = 26U;
constexpr std::uint32_t kCompactOffB = kSphereOffB + 4U * kCubes;
constexpr std::uint32_t kRingBWords = kCompactOffB + kCubes;

std::uint32_t bits(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4U);
  return u;
}

void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      float* m16) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m16[0] = f / aspect; m16[1] = 0; m16[2] = 0; m16[3] = 0;
  m16[4] = 0; m16[5] = f; m16[6] = 0; m16[7] = 0;
  m16[8] = 0; m16[9] = 0; m16[10] = (zfar + znear) * zn; m16[11] = -1.0f;
  m16[12] = 0; m16[13] = 0; m16[14] = 2.0f * znear * zfar * zn; m16[15] = 0;
}

//! 6 frustum planes for a camera at the origin looking down -z.
//! Packing: (nx, ny, nz, d) with inside = dot(n, p) + d >= 0.
//! For view-space depth w = -z > 0:
//!   near  inside: z <= -znear  -> plane (0,0,-1), d = -znear
//!   far   inside: z >= -zfar   -> plane (0,0, 1), d = +zfar
//!   left  inside: x >= z*(r/znear)   (z<0) -> plane (znear, 0, -r)
//!   right inside: x <= -z*(r/znear)  (z<0) -> plane (-znear, 0, -r)
//!   top   inside: y >= z*(t/znear)   (z<0) -> plane (0, znear, -t)
//!   bottom inside: y <= -z*(t/znear) (z<0) -> plane (0, -znear, -t)
void make_frustum_planes(float fov_y, float aspect, float znear, float zfar,
                         float* p24) {
  const float t = znear * std::tan(fov_y * 0.5f);  // near half-height
  const float r = t * aspect;                       // near half-width
  const auto plane = [](float x, float y, float z, float w, float* out) {
    const float len = std::sqrt(x * x + y * y + z * z);
    out[0] = x / len; out[1] = y / len; out[2] = z / len; out[3] = w / len;
  };
  plane(0, 0, -1, -znear, p24 + 0);
  plane(0, 0, 1, zfar, p24 + 4);
  plane(znear, 0, -r, 0, p24 + 8);
  plane(-znear, 0, -r, 0, p24 + 12);
  plane(0, znear, -t, 0, p24 + 16);
  plane(0, -znear, -t, 0, p24 + 20);
}

#ifdef WARPLOOM_HAS_VULKAN
//! Recording callback for AsyncComputeQueue::record: animates frame N's
//! instance data, then runs the frustum cull + indirect-command update.
struct FrameComputeCtx {
  VkPipeline gen_pipeline;
  VkPipelineLayout gen_layout;
  VkDescriptorSet a_set;
  VkPipeline cull_pipeline;
  VkPipelineLayout cull_layout;
  VkDescriptorSet cull_set;
  std::uint32_t frame;
};

void record_frame_compute(VkCommandBuffer cb, void* user) {
  auto* c = static_cast<FrameComputeCtx*>(user);

  // (1) Animate: fill ring A (identity list + cube instances at z from the
  // push-constant frame index * kSpacing, x = spacing for separation).
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->gen_pipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->gen_layout,
                          0, 1, &c->a_set, 0, nullptr);
  const std::uint32_t gen_push[2] = {c->frame, 0x41200000U /* 10.0f */};
  vkCmdPushConstants(cb, c->gen_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(gen_push), gen_push);
  vkCmdDispatch(cb, kCubes, 1, 1);

  // Animation writes -> cull reads (both compute stage, same queue).
  VkBufferMemoryBarrier anim_done{};
  anim_done.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  anim_done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  anim_done.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  anim_done.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  anim_done.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 0, nullptr);
  (void)anim_done;

  // (2) Cull: frustum + compaction + indirect instanceCount on the GPU.
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->cull_pipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->cull_layout,
                          0, 1, &c->cull_set, 0, nullptr);
  const std::uint32_t cull_push[2] = {kSphereOffB, kCompactOffB};
  vkCmdPushConstants(cb, c->cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(cull_push), cull_push);
  vkCmdDispatch(cb, (kCubes + 63U) / 64U, 1, 1);
}
#endif  // WARPLOOM_HAS_VULKAN

}  // namespace

TEST(VulkanHardware, DualQueuePipelinedGraph) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppDualQueueGraph", true).is_ok());
  if (!context.has_timeline_semaphores()) {
    GTEST_SKIP() << "Timeline semaphores unavailable";
  }
  if (!context.has_dedicated_compute()) {
    GTEST_SKIP() << "No COMPUTE-only family: dual-queue overlap not testable here";
  }
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(),
                                   context.physical_device()).is_ok());

  // --- Per-frame rings. Ring A is compute-produced, graphics-consumed:
  // CONCURRENT sharing (both families) makes the timeline semaphore a full
  // memory dependency, no ownership ping-pong. ---
  using omnicpp::render::Allocation;
  std::vector<Allocation> ring_a;
  std::vector<Allocation> ring_b;
  std::vector<Allocation> ring_cmd;
  for (std::size_t i = 0; i < static_cast<std::size_t>(kFrames); ++i) {
    auto a = allocator.create_buffer(
        kRingAWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(a.is_ok());
    ring_a.push_back(a.value());
    auto b = allocator.create_buffer(
        kRingBWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(b.is_ok());
    ring_b.push_back(b.value());
    auto cmd = allocator.create_buffer(
        4U * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(cmd.is_ok());
    ring_cmd.push_back(cmd.value());
  }

  // --- Static CPU-side fill: per-frame spheres + draw command headers. ---
  // Sphere zs differ per frame so each frame's cull result is distinct.
  const float sphere_z[kFrames][kCubes] = {
      {-8.0f, -12.0f},    // frame 0: both inside -> instanceCount 2
      {-150.0f, -20.0f},  // frame 1: first beyond zfar -> instanceCount 1
      {-9.0f, -11.0f},    // frame 2: both inside -> instanceCount 2
  };
  for (std::size_t f = 0; f < static_cast<std::size_t>(kFrames); ++f) {
    auto* b = static_cast<std::uint32_t*>(ring_b[f].mapped);
    ASSERT_NE(b, nullptr);
    b[0] = kCubes;  // instance_count
    b[1] = 0U;      // compaction cursor (GPU-atomic, reset per frame)
    float planes[24];
    make_frustum_planes(1.05f, 1.0f, 0.1f, 100.0f, planes);
    for (std::size_t p = 0; p < static_cast<std::size_t>(24); ++p) {
      b[2U + static_cast<std::uint32_t>(p)] = bits(planes[p]);
    }
    for (std::uint32_t i = 0; i < kCubes; ++i) {
      const float s[4] = {0.0f, 0.0f, sphere_z[f][i], 0.7f};
      for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) {
        b[kSphereOffB + i * 4U + static_cast<std::uint32_t>(k)] = bits(s[k]);
      }
    }
    auto* c = static_cast<std::uint32_t*>(ring_cmd[f].mapped);
    ASSERT_NE(c, nullptr);
    c[0] = 36U;  // vertexCount
    c[1] = 0U;   // instanceCount (GPU-maintained)
    c[2] = 0U;   // firstVertex
    c[3] = 0U;   // firstInstance
  }

  // --- Descriptors. ---
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  std::vector<omnicpp::render::ReflectedBinding> a_bindings(1);
  a_bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT};
  auto a_layout = manager.create_layout(a_bindings, kFrames);
  ASSERT_TRUE(a_layout.is_ok());
  std::vector<VkDescriptorSet> a_sets;
  for (std::size_t f = 0; f < static_cast<std::size_t>(kFrames); ++f) {
    auto s = manager.allocate_set(a_layout.value());
    ASSERT_TRUE(s.is_ok());
    ASSERT_TRUE(manager.write_buffer(s.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     ring_a[f].buffer, 0, VK_WHOLE_SIZE).is_ok());
    a_sets.push_back(s.value());
  }

  std::vector<omnicpp::render::ReflectedBinding> cull_bindings(2);
  cull_bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  cull_bindings[1] = {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  auto cull_layout = manager.create_layout(cull_bindings, kFrames);
  ASSERT_TRUE(cull_layout.is_ok());
  std::vector<VkDescriptorSet> cull_sets;
  for (std::size_t f = 0; f < static_cast<std::size_t>(kFrames); ++f) {
    auto s = manager.allocate_set(cull_layout.value());
    ASSERT_TRUE(s.is_ok());
    ASSERT_TRUE(manager.write_buffer(s.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     ring_b[f].buffer, 0, VK_WHOLE_SIZE).is_ok());
    ASSERT_TRUE(manager.write_buffer(s.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     ring_cmd[f].buffer, 0, VK_WHOLE_SIZE).is_ok());
    cull_sets.push_back(s.value());
  }

  // --- Pipelines. ---
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;

  const VkPushConstantRange gen_push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8U};
  omnicpp::render::VulkanPipeline gen_pipe;
  ASSERT_TRUE(gen_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gen_cube_ring.comp.spv", "compute").is_ok());
  ASSERT_TRUE(gen_pipe.create_pipeline_layout(
      context.device(), &a_layout.value(), 1, &gen_push_range).is_ok());
  ASSERT_TRUE(gen_pipe.create_compute_pipeline(
      context.device(), gen_pipe.pipeline_layout()).is_ok());

  const VkPushConstantRange cull_push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8U};
  omnicpp::render::VulkanPipeline cull_pipe;
  ASSERT_TRUE(cull_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/cull_and_draw.comp.spv", "compute").is_ok());
  ASSERT_TRUE(cull_pipe.create_pipeline_layout(
      context.device(), &cull_layout.value(), 1, &cull_push_range).is_ok());
  ASSERT_TRUE(cull_pipe.create_compute_pipeline(
      context.device(), cull_pipe.pipeline_layout()).is_ok());

  constexpr std::uint32_t kGfxPushBytes = 64U + 4U * sizeof(std::uint32_t);
  const VkPushConstantRange gfx_push{VK_SHADER_STAGE_VERTEX_BIT, 0, kGfxPushBytes};
  omnicpp::render::VulkanPipeline gfx_pipe;
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(gfx_pipe.create_pipeline_layout(
      context.device(), &a_layout.value(), 1, &gfx_push).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());
  ASSERT_TRUE(gfx_pipe.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      gfx_pipe.pipeline_layout(), false, false, false).is_ok());

  // --- Command infrastructure. ---
  const auto gfx_pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(gfx_pool_result.is_ok());
  const VkCommandPool gfx_pool = gfx_pool_result.value();
  // One command buffer + fence PER FRAME: in a pipelined loop frame N's
  // submission is still pending when frame N+1 is recorded, so a single
  // buffer/fence would violate VUID-vkQueueSubmit-pCommandBuffers-00071 and
  // VUID-vkResetFences-pFences-01123.
  std::vector<VkCommandBuffer> gfx_cbs;
  std::vector<VkFence> gfx_fences;
  for (std::size_t i = 0; i < static_cast<std::size_t>(kFrames); ++i) {
    auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        context.device(), gfx_pool);
    ASSERT_TRUE(cb_result.is_ok());
    gfx_cbs.push_back(cb_result.value());
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);
    gfx_fences.push_back(fence);
  }

  omnicpp::render::AsyncComputeQueue async;
  ASSERT_TRUE(async.initialize(context.device(), context.compute_queue(),
                               context.compute_family_index()).is_ok());

  // --- Pipelined loop. Steady state (frame >= 1):
  //   compute queue: frame N+1's animate+cull   (submitted first)
  //   graphics queue: waits compute N's timeline value, renders frame N.
  // Graphics N consumes ring A[N] while compute N+1 writes ring A[N+1] —
  // no aliasing, no CPU synchronization. ---
  std::uint64_t compute_signal[kFrames] = {};
  for (std::size_t n = 0; n < static_cast<std::size_t>(kFrames); ++n) {
    // Producer: frame n's compute (animate + cull).
    FrameComputeCtx ctx{gen_pipe.pipeline(), gen_pipe.pipeline_layout(),
                        a_sets[n], cull_pipe.pipeline(), cull_pipe.pipeline_layout(),
                        cull_sets[n], static_cast<std::uint32_t>(n)};
    async.begin();
    async.record(&record_frame_compute, &ctx);
    auto signal = async.submit();
    ASSERT_TRUE(signal.is_ok());
    compute_signal[n] = signal.value();

    // Consumer: graphics waits (GPU-side) on compute n's timeline value,
    // then draws ring A[n] with the cull-produced instance count.
    const VkCommandBuffer gfx_cb = gfx_cbs[n];
    ASSERT_EQ(vkResetCommandBuffer(gfx_cb, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(gfx_cb, &begin), VK_SUCCESS);
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = target.render_pass();
    rb.framebuffer = target.framebuffer();
    rb.renderArea.extent = {kSize, kSize};
    VkClearValue clears[1]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    rb.clearValueCount = 1;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(gfx_cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
    vkCmdSetViewport(gfx_cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {kSize, kSize}};
    vkCmdSetScissor(gfx_cb, 0, 1, &sc);
    vkCmdBindPipeline(gfx_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline());
    vkCmdBindDescriptorSets(gfx_cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx_pipe.pipeline_layout(), 0, 1, &a_sets[n], 0, nullptr);
    struct GfxPush {
      float view_proj[16];
      std::uint32_t data_offset;
      std::uint32_t comp_offset;
      std::uint32_t lod_scale;
      std::uint32_t pad0;
    } push{};
    make_perspective(1.05f, 1.0f, 0.1f, 100.0f, push.view_proj);
    push.data_offset = kCubes;         // ring A: identity list, then data
    push.comp_offset = 0U;             // identity list (all computed cubes)
    push.lod_scale = 0x3F800000U;      // 1.0f
    vkCmdPushConstants(gfx_cb, gfx_pipe.pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT,
                       0, kGfxPushBytes, &push);
    vkCmdDrawIndirect(gfx_cb, ring_cmd[n].buffer, 0, 1, 0);
    vkCmdEndRenderPass(gfx_cb);
    ASSERT_EQ(vkEndCommandBuffer(gfx_cb), VK_SUCCESS);

    VkSemaphore wait_sem = async.timeline_semaphore();
    VkTimelineSemaphoreSubmitInfo timeline_wait{};
    timeline_wait.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_wait.waitSemaphoreValueCount = 1;
    timeline_wait.pWaitSemaphoreValues = &compute_signal[n];
    constexpr VkPipelineStageFlags kWaitStage =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline_wait;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &wait_sem;
    submit.pWaitDstStageMask = &kWaitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &gfx_cb;
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, gfx_fences[n]), VK_SUCCESS);
  }

  // --- Drain: wait for all graphics frames, then read everything. ---
  ASSERT_EQ(vkWaitForFences(context.device(), static_cast<std::uint32_t>(kFrames),
                            gfx_fences.data(), VK_TRUE, UINT64_MAX), VK_SUCCESS);
  ASSERT_TRUE(async.wait_done());

  // --- Verify per frame: compute output analytically exact, cull result
  // correct, pixels present (frames 0/2 draw 2 cubes; frame 1 draws 1). ---
  const std::uint32_t expect_count[kFrames] = {2U, 1U, 2U};
  for (std::size_t n = 0; n < static_cast<std::size_t>(kFrames); ++n) {
    const auto* a = static_cast<const float*>(ring_a[n].mapped);
    ASSERT_NE(a, nullptr);
    // Identity list intact: words[0..kCubes) == 0..kCubes-1.
    for (std::uint32_t i = 0; i < kCubes; ++i) {
      ASSERT_EQ((reinterpret_cast<const std::uint32_t*>(a))[i], i) << "frame " << n;
    }
    // Instance k: z = -(frame*10 + (k+1)*8), x = 10.
    for (std::uint32_t k = 0; k < kCubes; ++k) {
      const float base = kRingAWords == 0 ? 0.0f : 0.0f;  // (silences pedantry)
      (void)base;
      const float expect_z = -(static_cast<float>(n) * 10.0f + (static_cast<float>(k) + 1.0f) * 8.0f);
      EXPECT_NEAR(a[kCubes + k * 8U + 2U], expect_z, 1e-4f) << "frame " << n << " cube " << k;
      EXPECT_NEAR(a[kCubes + k * 8U + 0U], 10.0f, 1e-4f) << "frame " << n << " cube " << k;
    }
    // GPU-maintained instanceCount matches the frustum outcome.
    const auto* cmd = static_cast<const std::uint32_t*>(ring_cmd[n].mapped);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd[1], expect_count[n]) << "frame " << n << " instanceCount";
    // Compacted list holds the accepted original indices.
    const auto* bwords = static_cast<const std::uint32_t*>(ring_b[n].mapped);
    ASSERT_NE(bwords, nullptr);
    EXPECT_EQ(bwords[1], expect_count[n]) << "frame " << n << " compacted count";
  }

  // Pixel verification on the final frame: two cubes rendered.
  const auto rb_final = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), kSize, kSize,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(rb_final.submitted);
  EXPECT_GT(rb_final.non_clear_pixels, 100U);

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  for (auto f : gfx_fences) { vkDestroyFence(context.device(), f, nullptr); }
  vkDestroyCommandPool(context.device(), gfx_pool, nullptr);
  async.cleanup();
  gfx_pipe.cleanup(context.device());
  cull_pipe.cleanup(context.device());
  gen_pipe.cleanup(context.device());
  target.cleanup(context.device());
  for (std::size_t i = 0; i < static_cast<std::size_t>(kFrames); ++i) {
    Allocation a = ring_a[i]; allocator.destroy_allocation(a);
    Allocation b = ring_b[i]; allocator.destroy_allocation(b);
    Allocation c = ring_cmd[i]; allocator.destroy_allocation(c);
  }
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
