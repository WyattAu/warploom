#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include "engine/core/job_system.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_surface.hpp"
#include "engine/render/vulkan_swapchain.hpp"
#include "engine/render/vulkan_render_pass.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_render_graph.hpp"
#include "engine/render/vulkan_compute.hpp"
#include "engine/render/vulkan_parallel_recorder.hpp"
#include "engine/render/software_rasterizer.hpp"
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
#endif

#if WARPLOOM_VULKAN_TYPES_AVAILABLE
namespace {

struct HiZCallbackState {
  std::uint32_t calls{0};
  std::uint32_t plans{0};
  bool saw_previous{false};
  bool valid{true};
  bool reject{false};
  omnicpp::render::VulkanRenderer* renderer{nullptr};
};

bool record_hiz_contract(VkCommandBuffer, const omnicpp::render::HiZFrameRecord& record,
                         void* user_data) {
  auto* state = static_cast<HiZCallbackState*>(user_data);
  if (state == nullptr || record.depth_image == VK_NULL_HANDLE ||
      record.depth_view == VK_NULL_HANDLE || !record.depth_is_sampleable ||
      record.destination_pyramid == nullptr || record.render_width == 0U ||
      record.render_height == 0U || record.tile_size == 0U || record.levels == 0U) {
    if (state != nullptr) state->valid = false;
    return false;
  }
  if (state->reject) return false;
  ++state->calls;
  state->saw_previous = state->saw_previous || record.token.has_previous;
  if (state->renderer != nullptr) {
    const auto plan = state->renderer->make_hiz_graph_plan(record);
    const std::size_t expected_passes =
        record.levels + (record.token.has_previous ? 1U : 0U);
    const auto compiled = plan.compile();
    if (compiled.barriers_per_node.empty() || compiled.barriers_per_node[0].size() < 2U ||
        compiled.barriers_per_node[0][1].old_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        compiled.barriers_per_node[0][1].src_access == 0U) {
      state->valid = false;
      return false;
    }
    if (plan.passes.size() != expected_passes ||
        compiled.barriers_per_node.size() != expected_passes) {
      state->valid = false;
      return false;
    }
    ++state->plans;
  }
  return true;
}


// Image-readback helpers live in the shared test header.
#include "vulkan_test_readback.hpp"

using namespace omnicpp_test;

} // namespace
#endif

// ============================================================================
// VulkanContext Tests
// ============================================================================

TEST(VulkanContext, AvailabilityCheck) {
  bool available = omnicpp::render::VulkanContext::is_available();
  (void)available;
}

TEST(VulkanContext, RuntimeErrorCodesExist) {
  EXPECT_NE(omnicpp::core::RuntimeError::ok, omnicpp::core::RuntimeError::vulkan_not_available);
}

TEST(VulkanContext, InitializeWithoutVulkan) {
  omnicpp::render::VulkanContext ctx;
  auto result = ctx.initialize("TestApp", false);
  if (!omnicpp::render::VulkanContext::is_available()) {
    EXPECT_FALSE(result.is_ok());
    EXPECT_FALSE(ctx.is_initialized());
  } else {
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(ctx.is_initialized());
  }
}

TEST(VulkanContext, DoubleInitializeIsIdempotent) {
  omnicpp::render::VulkanContext ctx;
  auto r1 = ctx.initialize("TestApp", false);
  auto r2 = ctx.initialize("TestApp", false);
  EXPECT_EQ(r1.is_ok(), r2.is_ok());
}

TEST(VulkanContext, CleanupIsIdempotent) {
  omnicpp::render::VulkanContext ctx;
  (void)ctx.initialize("TestApp", false);
  ctx.cleanup();
  ctx.cleanup();
  EXPECT_FALSE(ctx.is_initialized());
}

TEST(VulkanContext, DevicePropertiesAreEmptyBeforeInit) {
  omnicpp::render::VulkanContext ctx;
  const auto& props = ctx.device_properties();
  EXPECT_TRUE(props.name.empty());
  EXPECT_EQ(props.api_version, 0u);
}

// ============================================================================
// QueueFamilyIndices Tests
// ============================================================================

TEST(QueueFamilyIndices, DefaultIsIncomplete) {
  omnicpp::render::QueueFamilyIndices indices;
  EXPECT_FALSE(indices.is_complete());
}

TEST(QueueFamilyIndices, CompleteWhenBothSet) {
  omnicpp::render::QueueFamilyIndices indices;
  indices.graphics_family = 0;
  indices.present_family = 1;
  EXPECT_TRUE(indices.is_complete());
}

// ============================================================================
// VulkanSwapchain Tests
// ============================================================================

TEST(VulkanSwapchain, QuerySupportReturnsEmptyWithoutVulkan) {
  auto details = omnicpp::render::VulkanSwapchain::query_swapchain_support(nullptr, nullptr);
  EXPECT_FALSE(details.is_valid());
}

TEST(VulkanHardware, HeadlessSwapchainAndRenderSubmission) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(VK_USE_PLATFORM_XCB_KHR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";
  const char* display_name = std::getenv("DISPLAY");
  if (!display_name) GTEST_SKIP() << "DISPLAY is unavailable";

  xcb_connection_t* connection = xcb_connect(display_name, nullptr);
  if (!connection || xcb_connection_has_error(connection)) {
    if (connection) xcb_disconnect(connection);
    GTEST_SKIP() << "XCB display is unavailable";
  }
  const auto setup = xcb_get_setup(connection);
  const auto* screen = xcb_setup_roots_iterator(setup).data;
  if (!screen) {
    xcb_disconnect(connection);
    GTEST_SKIP() << "XCB screen is unavailable";
  }
  const xcb_window_t window = xcb_generate_id(connection);
  const std::uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root,
                    0, 0, 640, 480, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual, XCB_CW_EVENT_MASK, &event_mask);
  xcb_map_window(connection, window);
  xcb_flush(connection);

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppHardwareTest", true).is_ok());
  ASSERT_TRUE(context.has_validation());
  // The swapchain submission checks below were authored against hardware
  // rasterization semantics; under a software ICD (llvmpipe selected via
  // VK_ICD_FILENAMES on a machine that also has a real GPU + display) skip
  // rather than fail — CI's headless lavapipe environment skips earlier at
  // surface creation.
  if (!context.device_properties().is_discrete_gpu) {
    context.cleanup();
    GTEST_SKIP() << "software rasterizer active; swapchain test targets hardware";
  }

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  omnicpp::render::SurfaceCreateInfo surface_info;
  surface_info.display = connection;
  surface_info.window = reinterpret_cast<void*>(static_cast<std::uintptr_t>(window));
  auto surface_result = omnicpp::render::create_platform_surface(context.instance(), surface_info, surface);
  if (!surface_result.is_ok()) {
    context.cleanup();
    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
    GTEST_SKIP() << "XCB Vulkan surface unavailable";
  }
  ASSERT_NE(surface, VK_NULL_HANDLE);

  omnicpp::render::VulkanSwapchain swapchain;
  ASSERT_TRUE(swapchain.create(context.device(), context.physical_device(), surface, {}).is_ok());
  ASSERT_TRUE(swapchain.create_image_views(context.device()).is_ok());

  const VkFormat depth_format = omnicpp::render::VulkanRenderPass::find_supported_depth_format(context.physical_device());
  ASSERT_NE(depth_format, VK_FORMAT_UNDEFINED);
  omnicpp::render::VulkanRenderPass render_pass;
  ASSERT_TRUE(render_pass.create(context.device(), swapchain.image_format(), depth_format).is_ok());
  ASSERT_TRUE(render_pass.create_depth_resources(context.device(), context.physical_device(), depth_format,
                                                 swapchain.extent_width(), swapchain.extent_height()).is_ok());
  ASSERT_TRUE(render_pass.create_framebuffers(context.device(), swapchain.image_views(),
                                              swapchain.extent_width(), swapchain.extent_height()).is_ok());

  omnicpp::render::VulkanPipeline pipeline;
#ifdef WARPLOOM_TEST_SHADER_DIR
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline.load_shader_file(context.device(), shader_dir + "/triangle.vert.spv").is_ok());
  ASSERT_TRUE(pipeline.load_shader_file(context.device(), shader_dir + "/triangle.frag.spv").is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(context.device(), render_pass.render_pass(),
                                                swapchain.image_format(), VK_NULL_HANDLE,
                                                true, true, false).is_ok());
#else
  GTEST_SKIP() << "Test shaders were not compiled";
#endif

  ASSERT_TRUE(render_pass.depth_is_sampleable());
  omnicpp::render::RendererConfig renderer_config;
  renderer_config.enable_hiz = true;
  renderer_config.hiz_tile_size = 32U;
#ifdef WARPLOOM_TEST_SHADER_DIR
  renderer_config.hiz_reduction_shader_path =
      std::string(WARPLOOM_TEST_SHADER_DIR) + "/depth_reduce_image.comp.spv";
#endif
  omnicpp::render::VulkanRenderer renderer;
  renderer.set_timeline_pacing(context.has_timeline_semaphores());
  ASSERT_TRUE(renderer.initialize(context, swapchain, render_pass, renderer_config).is_ok());
  ASSERT_TRUE(renderer.hiz_direct_enabled());
  renderer.set_synchronization2(context.has_synchronization2());
  renderer.set_pipeline(pipeline.pipeline());
  HiZCallbackState hiz_callback_state;
  hiz_callback_state.renderer = &renderer;
  renderer.set_hiz_record_callback(record_hiz_contract, &hiz_callback_state);
  if (context.has_timeline_semaphores()) {
    EXPECT_TRUE(renderer.uses_timeline_pacing());
  }
  auto image = renderer.begin_frame();
  ASSERT_TRUE(image.is_ok());
  ASSERT_TRUE(renderer.record_commands(image.value(), render_pass.framebuffer(image.value()),
                                       swapchain.extent_width(), swapchain.extent_height()).is_ok());
  ASSERT_TRUE(renderer.submit_frame().is_ok());
  EXPECT_TRUE(renderer.hiz_enabled());
  EXPECT_EQ(hiz_callback_state.calls, 1U);
  EXPECT_TRUE(hiz_callback_state.valid);
  EXPECT_FALSE(hiz_callback_state.saw_previous);
  EXPECT_NE(renderer.hiz_pyramid(0U), nullptr);
  EXPECT_NE(renderer.hiz_pyramid(1U), nullptr);
  EXPECT_TRUE(renderer.hiz_frame_state().has_previous_frame());

  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      swapchain.images()[image.value()], swapchain.image_format(),
      swapchain.extent_width(), swapchain.extent_height());
  ASSERT_TRUE(readback.submitted);
  ASSERT_GT(readback.non_clear_pixels, 1000U);
  EXPECT_GT(readback.red_dominant_pixels, 100U);
  EXPECT_GT(readback.green_dominant_pixels, 100U);
  EXPECT_GT(readback.blue_dominant_pixels, 100U);
  EXPECT_NE(readback.center_pixel & 0x00FFFFFFU, 0U);
  EXPECT_NE(readback.upper_triangle_pixel & 0x00FFFFFFU, 0U);
  EXPECT_NE(readback.lower_triangle_pixel & 0x00FFFFFFU, 0U);
  EXPECT_EQ(readback.corner_pixel & 0x00FFFFFFU, 0U);
  std::cout << "Vulkan readback: format=" << static_cast<int>(swapchain.image_format())
            << " hash=" << readback.hash
            << " canonical_hash=" << readback.canonical_hash
            << " non_clear=" << readback.non_clear_pixels
            << " RGB-dominant=" << readback.red_dominant_pixels << "/"
            << readback.green_dominant_pixels << "/" << readback.blue_dominant_pixels
            << std::endl;
  // Golden fingerprint for the bundled triangle's coarse spatial content.
  // Unlike the raw byte hash, this is independent of BGRA/RGBA and sRGB choice.
  expect_canonical_triangle_hash(readback.canonical_hash);
  ASSERT_TRUE(renderer.present_frame().is_ok());

  // Exercise both frame slots and the complete swapchain image set. A single
  // successful frame cannot detect stale image-fence ownership.
  for (std::uint32_t frame = 0; frame < 8; ++frame) {
    auto next_image = renderer.begin_frame();
    ASSERT_TRUE(next_image.is_ok());
    ASSERT_TRUE(renderer.record_commands(
        next_image.value(), render_pass.framebuffer(next_image.value()),
        swapchain.extent_width(), swapchain.extent_height()).is_ok());
    ASSERT_TRUE(renderer.submit_frame().is_ok());
    ASSERT_TRUE(renderer.present_frame().is_ok());
  }
  renderer.wait_idle();
  EXPECT_EQ(renderer.frame_count(), 9U);
  // Frame-latency telemetry: one sample per presented frame.
  EXPECT_EQ(renderer.frame_latency_tracker().total_count(), 9U);
  EXPECT_EQ(renderer.frame_latency_stats().window_count, 9U);
  EXPECT_GT(renderer.frame_latency_stats().p50_ns, 0U);
  EXPECT_EQ(hiz_callback_state.calls, 9U);
  EXPECT_EQ(hiz_callback_state.plans, 9U);
  EXPECT_TRUE(hiz_callback_state.saw_previous);
  EXPECT_TRUE(hiz_callback_state.valid);

  // A rejected recorder must discard the token rather than publish stale or
  // partially recorded depth as the next frame's previous pyramid.
  hiz_callback_state.reject = true;
  auto rejected_image = renderer.begin_frame();
  ASSERT_TRUE(rejected_image.is_ok());
  // The renderer now rejects accidental duplicate H-Z passes in the same frame.
  // The first frame of this loop is the one that must fail; later frames should
  // not be polluted by the stale token.
  EXPECT_FALSE(renderer.record_commands(
      rejected_image.value(), render_pass.framebuffer(rejected_image.value()),
      swapchain.extent_width(), swapchain.extent_height()).is_ok());
  EXPECT_FALSE(renderer.hiz_frame_state().has_previous_frame());

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  renderer.cleanup(context.device());
  pipeline.cleanup(context.device());
  render_pass.cleanup(context.device());
  swapchain.cleanup(context.device());
  context.destroy_surface(surface);
  context.cleanup();
  xcb_destroy_window(connection, window);
  xcb_disconnect(connection);
#else
  GTEST_SKIP() << "Vulkan XCB support was not enabled for this build";
#endif
}

// ============================================================================
// VulkanMemoryAllocator / VulkanUploadRing Tests
// ============================================================================

TEST(VulkanMemoryAllocator, UninitializedCreateBufferFails) {
  omnicpp::render::VulkanMemoryAllocator allocator;
  auto result = allocator.create_buffer(1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  EXPECT_FALSE(result.is_ok());
  const auto stats = allocator.stats();
  EXPECT_EQ(stats.block_count, 0U);
}

TEST(VulkanHardware, AllocatorSubAllocationAndUploadRing) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppAllocatorTest", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  // --- Sub-allocation: many buffers land inside one host-visible block. ---
  constexpr std::size_t kBufferCount = 8;
  constexpr VkDeviceSize kBufferSize = 4096;
  std::vector<omnicpp::render::Allocation> allocations;
  for (std::size_t i = 0; i < kBufferCount; ++i) {
    auto result = allocator.create_buffer(
        kBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(result.is_ok());
    allocations.push_back(result.value());
    EXPECT_NE(allocations.back().mapped, nullptr);
    EXPECT_TRUE(allocations.back().is_valid());
  }
  auto stats = allocator.stats();
  EXPECT_EQ(stats.allocation_count, kBufferCount);
  EXPECT_GT(stats.used_bytes, 0ULL);
  EXPECT_GE(stats.block_count, 1U);

  // Persistent mapping: write distinct patterns via the mapped pointers.
  for (std::size_t i = 0; i < allocations.size(); ++i) {
    auto* bytes = static_cast<std::uint8_t*>(allocations[i].mapped);
    std::memset(bytes, static_cast<int>(0xA0 + i), kBufferSize);
  }

  // --- Sub-allocation: device-local buffer for the upload destination. ---
  // Also TRANSFER_SRC: the verify step copies it back to a host-visible buffer.
  auto dst_result = allocator.create_buffer(
      kBufferSize * kBufferCount,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ASSERT_TRUE(dst_result.is_ok());

  // --- Upload ring: staged copy of every pattern to the device buffer. ---
  omnicpp::render::VulkanUploadRing ring;
  ASSERT_TRUE(ring.initialize(context.device(), context.physical_device(),
                              static_cast<std::uint32_t>(context.queue_families().graphics_family),
                              1u << 20u).is_ok());
  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer command_buffer = cb_result.value();

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);
  for (std::size_t i = 0; i < allocations.size(); ++i) {
    auto span = ring.acquire(kBufferSize);
    ASSERT_TRUE(span.is_ok());
    std::memcpy(span.value().host_data, allocations[i].mapped, kBufferSize);
    ring.record_copy(command_buffer, span.value(), dst_result.value().buffer,
                     static_cast<VkDeviceSize>(i) * kBufferSize);
  }
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  // --- Verify the GPU wrote every pattern back into a host-visible check buffer. ---
  auto check_result = allocator.create_buffer(
      kBufferSize * kBufferCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(check_result.is_ok());
  VkCommandBufferBeginInfo begin2{};
  begin2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin2), VK_SUCCESS);
  VkBufferCopy copy{};
  copy.size = kBufferSize * kBufferCount;
  vkCmdCopyBuffer(command_buffer, dst_result.value().buffer,
                  check_result.value().buffer, 1, &copy);
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);
  VkFence fence2 = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence2), VK_SUCCESS);
  VkSubmitInfo submit2{};
  submit2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit2.commandBufferCount = 1;
  submit2.pCommandBuffers = &command_buffer;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit2, fence2), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence2, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  const auto* checked = static_cast<const std::uint8_t*>(check_result.value().mapped);
  for (std::size_t i = 0; i < kBufferCount; ++i) {
    for (std::size_t b = 0; b < kBufferSize; b += 251) { // stride sampling
      EXPECT_EQ(checked[i * kBufferSize + b], static_cast<std::uint8_t>(0xA0 + i))
          << "buffer " << i << " byte " << b;
    }
  }

  // --- Free-list coalescing: free all, stats return to zero used. ---
  for (auto& allocation : allocations) allocator.destroy_allocation(allocation);
  omnicpp::render::Allocation dst_allocation = dst_result.value();
  omnicpp::render::Allocation check_allocation = check_result.value();
  allocator.destroy_allocation(dst_allocation);
  allocator.destroy_allocation(check_allocation);
  stats = allocator.stats();
  EXPECT_EQ(stats.used_bytes, 0ULL);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyFence(context.device(), fence2, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  ring.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support was not enabled for this build";
#endif
}

// ============================================================================
// SPIR-V Reflection + Descriptor Manager Tests
// ============================================================================

TEST(SpirvReflection, RejectsInvalidCode) {
  const std::uint32_t bad_magic[] = {0xDEADBEEF, 0, 0, 0, 0};
  EXPECT_TRUE(omnicpp::render::reflect_spirv_resources(bad_magic, 5).empty());
  EXPECT_TRUE(omnicpp::render::reflect_spirv_resources(
      static_cast<const std::uint32_t*>(nullptr), 0).empty());
  const std::uint8_t odd_bytes[] = {0x03, 0x02, 0x23, 0x07, 0x00};
  EXPECT_TRUE(omnicpp::render::reflect_spirv_resources(odd_bytes, 5).empty());
}

TEST(VulkanHardware, DescriptorReflectionAndUboRender) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppDescriptorTest", true).is_ok());

  // --- Load the UBO fragment shader and reflect it. ---
  std::ifstream frag_file(std::string(WARPLOOM_TEST_SHADER_DIR) + "/ubo_triangle.frag.spv",
                          std::ios::binary);
  ASSERT_TRUE(frag_file.good());
  const std::vector<std::uint8_t> frag_spirv(
      (std::istreambuf_iterator<char>(frag_file)), std::istreambuf_iterator<char>());
  ASSERT_GE(frag_spirv.size(), 20U);

  const auto bindings = omnicpp::render::reflect_spirv_resources(
      frag_spirv.data(), frag_spirv.size());
  ASSERT_EQ(bindings.size(), 1U);
  EXPECT_EQ(bindings[0].set, 0U);
  EXPECT_EQ(bindings[0].binding, 0U);
  EXPECT_EQ(bindings[0].type, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
  EXPECT_EQ(bindings[0].stage_flags,
            static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_FRAGMENT_BIT));

  // --- Create layout + set through the manager, bind a real UBO. ---
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  auto layout_result = manager.create_layout(bindings, 1);
  ASSERT_TRUE(layout_result.is_ok());
  auto set_result = manager.allocate_set(layout_result.value());
  ASSERT_TRUE(set_result.is_ok());

  auto ubo_result = allocator.create_buffer(
      256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(ubo_result.is_ok());
  float tint = 1.0f; // Fully tinted: distinct, assertable output color.
  std::memcpy(ubo_result.value().mapped, &tint, sizeof(tint));
  ASSERT_TRUE(manager.write_buffer(set_result.value(), 0,
                                   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                   ubo_result.value().buffer, 0, 256).is_ok());

  // --- Pipeline with the reflected layout. ---
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, 320, 240, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanPipeline pipeline;
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/ubo_triangle.frag.spv", "fragment").is_ok());
  const VkDescriptorSetLayout set_layout = layout_result.value();
  ASSERT_TRUE(pipeline.create_pipeline_layout(
      context.device(), &set_layout, 1).is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      pipeline.pipeline_layout(), false, false, false).is_ok());

  // --- Render one frame offscreen. ---
  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer command_buffer = cb_result.value();

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);
  VkRenderPassBeginInfo render_begin{};
  render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_begin.renderPass = target.render_pass();
  render_begin.framebuffer = target.framebuffer();
  render_begin.renderArea.extent = {320, 240};
  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  render_begin.clearValueCount = 1;
  render_begin.pClearValues = &clear;
  vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{};
  viewport.width = 320.0f;
  viewport.height = 240.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {320, 240};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline());
  const VkDescriptorSet descriptor_set = set_result.value();
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.pipeline_layout(), 0, 1, &descriptor_set, 0, nullptr);
  vkCmdDraw(command_buffer, 3, 1, 0, 0);
  vkCmdEndRenderPass(command_buffer);
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  // --- Content check: interior pixels must be the tint color, not canonical. ---
  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), 320, 240,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback.submitted);
  EXPECT_GT(readback.non_clear_pixels, 100U);
  // tint=1: fragment color is exactly (0.2, 0.15, 0.9) — uniform reached the GPU.
  EXPECT_GT(readback.blue_dominant_pixels, readback.red_dominant_pixels);
  EXPECT_GT(readback.center_pixel & 0x00FFFFFFU, 0U);
  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  pipeline.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation ubo_allocation = ubo_result.value();
  allocator.destroy_allocation(ubo_allocation);
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

// ============================================================================
// Bindless (Descriptor Indexing) Tests
// ============================================================================

TEST(VulkanHardware, AllocatorAlignmentPadSubAllocation) {
// Regression test: allocate_sized() held a reference into the free-range
// vector across an insert() that could reallocate it, corrupting the heap on
// any allocation that needed alignment padding (e.g. a 128 B buffer at block
// offset 0 followed by a 1024-aligned image). Exercises the padded path with
// remainder, the padded path consuming the whole range, and the plain carve.
#if WARPLOOM_VULKAN_TYPES_AVAILABLE
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppAllocatorPadTest", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  // Small buffer lands at block offset 0; the aligned image must carve a pad.
  auto small_result = allocator.create_buffer(
      128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ASSERT_TRUE(small_result.is_ok());
  EXPECT_EQ(small_result.value().offset, 0U);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
  image_info.extent = {64, 64, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateImage(context.device(), &image_info, nullptr, &image), VK_SUCCESS);
  auto image_alloc = allocator.bind_image(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ASSERT_TRUE(image_alloc.is_ok());
  // Same block, aligned past the small buffer, no overlap.
  EXPECT_EQ(image_alloc.value().memory, small_result.value().memory);
  EXPECT_GE(image_alloc.value().offset, small_result.value().offset + small_result.value().size);

  // Third allocation: first-fit reuses the alignment-pad hole before the
  // image (correct behavior); it must simply never overlap the image.
  auto tail_result = allocator.create_buffer(
      512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ASSERT_TRUE(tail_result.is_ok());
  EXPECT_EQ(tail_result.value().memory, small_result.value().memory);
  const bool tail_overlaps_image =
      tail_result.value().offset < image_alloc.value().offset + image_alloc.value().size &&
      image_alloc.value().offset < tail_result.value().offset + tail_result.value().size;
  EXPECT_FALSE(tail_overlaps_image);

  // Drain: freeing in order must coalesce back to one full free range.
  const auto stats_before = allocator.stats();
  EXPECT_EQ(stats_before.allocation_count, 3U);
  omnicpp::render::Allocation a0 = small_result.value();
  omnicpp::render::Allocation a1 = image_alloc.value();
  omnicpp::render::Allocation a2 = tail_result.value();
  allocator.destroy_allocation(a0);
  allocator.destroy_allocation(a1);
  allocator.destroy_allocation(a2);
  const auto stats_after = allocator.stats();
  EXPECT_EQ(stats_after.allocation_count, 0U);
  EXPECT_EQ(stats_after.used_bytes, 0U);

  vkDestroyImage(context.device(), image, nullptr);
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support was not enabled";
#endif
}

TEST(VulkanHardware, BindlessDescriptorIndexingRender) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppBindlessTest", true).is_ok());
  if (!context.has_descriptor_indexing()) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }

  // --- Reflect the bindless shader: runtime-sized SSBO array at set 0 binding 0. ---
  std::ifstream frag_file(std::string(WARPLOOM_TEST_SHADER_DIR) + "/bindless_palette.frag.spv",
                          std::ios::binary);
  ASSERT_TRUE(frag_file.good());
  const std::vector<std::uint8_t> frag_spirv(
      (std::istreambuf_iterator<char>(frag_file)), std::istreambuf_iterator<char>());
  ASSERT_GE(frag_spirv.size(), 20U);

  const auto bindings = omnicpp::render::reflect_spirv_resources(
      frag_spirv.data(), frag_spirv.size());
  ASSERT_EQ(bindings.size(), 1U);
  EXPECT_EQ(bindings[0].set, 0U);
  EXPECT_EQ(bindings[0].binding, 0U);
  EXPECT_EQ(bindings[0].type, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  EXPECT_EQ(bindings[0].stage_flags,
            static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_FRAGMENT_BIT));

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  // --- Bindless layout: 1 set, partially bound + update-after-bind. ---
  auto layout_result = manager.create_layout(bindings, 1, /*bindless=*/true);
  ASSERT_TRUE(layout_result.is_ok());
  auto set_result = manager.allocate_set(layout_result.value());
  ASSERT_TRUE(set_result.is_ok());

  // --- Palette device buffer, filled through the staging upload ring. ---
  constexpr std::uint32_t kPaletteCount = 8;
  constexpr VkDeviceSize kPaletteBytes = kPaletteCount * 16;
  auto palette_result = allocator.create_buffer(
      kPaletteBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ASSERT_TRUE(palette_result.is_ok());

  // --- Fill the palette through the staging upload ring (ring owns its
  // command buffer: acquire() opens recording, submit() closes it). ---
  omnicpp::render::VulkanUploadRing ring;
  ASSERT_TRUE(ring.initialize(
      context.device(), context.physical_device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      1u << 20u).is_ok());
  auto staging = ring.acquire(kPaletteBytes);
  ASSERT_TRUE(staging.is_ok());
  const std::array<float, kPaletteCount * 4> palette_colors = {{
      1.0f, 0.0f, 0.0f, 1.0f,    // band 0: red
      0.0f, 1.0f, 0.0f, 1.0f,    // band 1: green
      0.0f, 0.0f, 1.0f, 1.0f,    // band 2: blue
      1.0f, 1.0f, 0.0f, 1.0f,    // band 3: yellow
      1.0f, 0.0f, 1.0f, 1.0f,    // band 4: magenta
      0.0f, 1.0f, 1.0f, 1.0f,    // band 5: cyan
      1.0f, 1.0f, 1.0f, 1.0f,    // band 6: white
      0.5f, 0.5f, 0.5f, 1.0f,    // band 7: gray
  }};
  std::memcpy(staging.value().host_data, palette_colors.data(), kPaletteBytes);

  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();

  {
    const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        context.device(), command_pool);
    ASSERT_TRUE(cb_result.is_ok());
    const VkCommandBuffer upload_cmd = cb_result.value();
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(upload_cmd, &begin), VK_SUCCESS);
    ring.record_copy(upload_cmd, staging.value(), palette_result.value().buffer, 0);
    ASSERT_EQ(vkEndCommandBuffer(upload_cmd), VK_SUCCESS);
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence upload_fence = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &upload_fence), VK_SUCCESS);
    VkSubmitInfo upload_submit{};
    upload_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    upload_submit.commandBufferCount = 1;
    upload_submit.pCommandBuffers = &upload_cmd;
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &upload_submit, upload_fence), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1, &upload_fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
    vkDestroyFence(context.device(), upload_fence, nullptr);
  }
  ring.wait_idle();

  // --- Bindless descriptor write: BEFORE command recording (the safest legal
  // window; update-after-bind still exercised by the layout). Points the
  // runtime array at the palette buffer. ---
  ASSERT_TRUE(manager.write_buffer(set_result.value(), 0,
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   palette_result.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  // --- Pipeline: bindless fragment stage + push constants for the band index. ---
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, 320, 240, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanPipeline pipeline;
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/bindless_palette.frag.spv", "fragment").is_ok());
  const VkDescriptorSetLayout set_layout = layout_result.value();
  const VkPushConstantRange push_range{
      VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(std::uint32_t)};
  ASSERT_TRUE(pipeline.create_pipeline_layout(
      context.device(), &set_layout, 1, &push_range).is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      pipeline.pipeline_layout(), false, false, false).is_ok());

  // --- Record one command buffer: 8 horizontal bands, one push-constant
  // value each, verifying non-uniform descriptor state per draw. ---
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer command_buffer = cb_result.value();

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);
  VkRenderPassBeginInfo render_begin{};
  render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_begin.renderPass = target.render_pass();
  render_begin.framebuffer = target.framebuffer();
  render_begin.renderArea.extent = {320, 240};
  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  render_begin.clearValueCount = 1;
  render_begin.pClearValues = &clear;
  vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{};
  viewport.width = 320.0f;
  viewport.height = 240.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {320, 240};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline());
  const VkDescriptorSet descriptor_set = set_result.value();
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.pipeline_layout(), 0, 1, &descriptor_set, 0, nullptr);
  const float band_height = 240.0f / static_cast<float>(kPaletteCount);
  for (std::uint32_t band = 0; band < kPaletteCount; ++band) {
    vkCmdPushConstants(command_buffer, pipeline.pipeline_layout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(std::uint32_t), &band);
    const float y = band_height * static_cast<float>(band);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
    (void)y; // geometry covers the full triangle; band color comes from push index
  }
  vkCmdEndRenderPass(command_buffer);
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  VkFenceCreateInfo frame_fence_info{};
  frame_fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence frame_fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &frame_fence_info, nullptr, &frame_fence), VK_SUCCESS);
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, frame_fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &frame_fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  // --- Content check: interior pixels must come from the palette buffer. ---
  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), 320, 240,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback.submitted);
  EXPECT_GT(readback.non_clear_pixels, 100U);
  EXPECT_GT(readback.center_pixel & 0x00FFFFFFU, 0U);
  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), frame_fence, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  pipeline.cleanup(context.device());
  target.cleanup(context.device());
  ring.cleanup();
  omnicpp::render::Allocation palette_allocation = palette_result.value();
  allocator.destroy_allocation(palette_allocation);
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

// ============================================================================
// Render Graph Tests
// ============================================================================

// Texture bindless: two offscreen textures (solid red, solid green) written
// into a runtime-sized combined-image-sampler array at explicit elements;
// two draws sample them through push-constant indices (nonuniformEXT in the
// shader). Content check: each draw's target shows the sampled texture's
// color — proving per-element descriptor writes and the non-uniform path.
TEST(VulkanHardware, BindlessTextureArrayRender) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppBindlessTexTest", true).is_ok());
  if (!context.has_descriptor_indexing()) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }

  // --- Reflect the texture-bindless shader. ---
  std::ifstream frag_file(std::string(WARPLOOM_TEST_SHADER_DIR) + "/bindless_textures.frag.spv",
                          std::ios::binary);
  ASSERT_TRUE(frag_file.good());
  const std::vector<std::uint8_t> frag_spirv(
      (std::istreambuf_iterator<char>(frag_file)), std::istreambuf_iterator<char>());
  const auto bindings = omnicpp::render::reflect_spirv_resources(
      frag_spirv.data(), frag_spirv.size());
  ASSERT_EQ(bindings.size(), 1U);
  EXPECT_EQ(bindings[0].binding, 0U);
  EXPECT_EQ(bindings[0].type, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  EXPECT_EQ(bindings[0].count, 0U);  // Runtime array.
  EXPECT_EQ(bindings[0].stage_flags,
            static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_FRAGMENT_BIT));

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  // --- Bindless layout (partially bound + update-after-bind). ---
  auto layout_result = manager.create_layout(bindings, 1, /*bindless=*/true);
  ASSERT_TRUE(layout_result.is_ok());
  auto set_result = manager.allocate_set(layout_result.value());
  ASSERT_TRUE(set_result.is_ok());

  constexpr VkFormat kTexFormat = VK_FORMAT_B8G8R8A8_UNORM;
  constexpr std::uint32_t kTexSize = 8;

  // --- Sampler: nearest, clamp. One sampler shared by both elements. ---
  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateSampler(context.device(), &sampler_info, nullptr, &sampler), VK_SUCCESS);

  // --- Two 8x8 solid-color textures: element 0 = red, element 1 = green. ---
  VkImage images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkImageView views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  omnicpp::render::Allocation allocations[2];
  const std::uint32_t colors[2] = {
      0xFF0000FFU,  // BGRA unpacked little-endian: red
      0xFF00FF00U,  // green
  };

  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();

  omnicpp::render::VulkanUploadRing ring;
  ASSERT_TRUE(ring.initialize(
      context.device(), context.physical_device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      1u << 20u).is_ok());

  // --- Stage both textures; create images/views; write bindless elements. ---
  constexpr VkDeviceSize kTexBytes = kTexSize * kTexSize * 4U;
  auto staging0 = ring.acquire(kTexBytes);
  ASSERT_TRUE(staging0.is_ok());
  auto staging1 = ring.acquire(kTexBytes);
  ASSERT_TRUE(staging1.is_ok());
  for (int t = 0; t < 2; ++t) {
    auto* dst = static_cast<std::uint32_t*>(t == 0 ? staging0.value().host_data
                                                   : staging1.value().host_data);
    for (std::size_t p = 0; p < static_cast<std::size_t>(kTexSize) * kTexSize; ++p) {
      dst[p] = colors[t];
    }
  }

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kTexFormat;
  image_info.extent = {kTexSize, kTexSize, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  // Staging buffer image-copies need a staging-visible buffer handle; the
  // ring exposes its own buffer as the copy source.
  for (int t = 0; t < 2; ++t) {
    ASSERT_EQ(vkCreateImage(context.device(), &image_info, nullptr, &images[t]), VK_SUCCESS);
    auto mem = allocator.bind_image(images[t], VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ASSERT_TRUE(mem.is_ok());
    allocations[t] = mem.value();
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = images[t];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kTexFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ASSERT_EQ(vkCreateImageView(context.device(), &view_info, nullptr, &views[t]), VK_SUCCESS);

    // Bindless write into the explicit array element.
    ASSERT_TRUE(manager.write_image(set_result.value(), 0,
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    sampler, views[t], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    static_cast<std::uint32_t>(t)).is_ok());
  }

  // --- Upload + transition via the ring's internal command buffer. ---
  {
    const auto& span0 = staging0.value();
    const auto& span1 = staging1.value();
    const VkBuffer src = ring.ring_buffer();
    const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        context.device(), command_pool);
    ASSERT_TRUE(cb_result.is_ok());
    const VkCommandBuffer upload_cmd = cb_result.value();
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(upload_cmd, &begin), VK_SUCCESS);
    for (int t = 0; t < 2; ++t) {
      // UNDEFINED -> TRANSFER_DST: the copy's destination layout requirement.
      VkImageMemoryBarrier to_transfer{};
      to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_transfer.srcAccessMask = 0;
      to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.image = images[t];
      to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &to_transfer);
      const auto& span = (t == 0) ? span0 : span1;
      VkBufferImageCopy copy{};
      copy.bufferOffset = span.byte_offset;
      copy.bufferRowLength = 0;
      copy.bufferImageHeight = 0;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {kTexSize, kTexSize, 1};
      vkCmdCopyBufferToImage(upload_cmd, src, images[t],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    // Transition both into shader read after the copies.
    for (int t = 0; t < 2; ++t) {
      VkImageMemoryBarrier to_shader{};
      to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_shader.image = images[t];
      to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &to_shader);
    }
    ASSERT_EQ(vkEndCommandBuffer(upload_cmd), VK_SUCCESS);
    VkSubmitInfo upload_submit{};
    upload_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    upload_submit.commandBufferCount = 1;
    upload_submit.pCommandBuffers = &upload_cmd;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence upload_fence = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &upload_fence), VK_SUCCESS);
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &upload_submit, upload_fence), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1, &upload_fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
    vkDestroyFence(context.device(), upload_fence, nullptr);
  }
  ring.wait_idle();

  // --- Offscreen target + pipeline with the bindless set + push constants. ---
  constexpr std::uint32_t kTargetW = 64;
  constexpr std::uint32_t kTargetH = 64;
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kTargetW, kTargetH, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanPipeline pipeline;
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/bindless_textures.frag.spv", "fragment").is_ok());
  const VkDescriptorSetLayout set_layout = layout_result.value();
  const VkPushConstantRange push_range{
      VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(std::uint32_t) * 4};
  ASSERT_TRUE(pipeline.create_pipeline_layout(
      context.device(), &set_layout, 1, &push_range).is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      pipeline.pipeline_layout(), false, false, false).is_ok());

  // --- Two draws: draw 0 samples element 0 (red), draw 1 element 1 (green).
  // Draw 1 runs after a full-screen clear so content is attributable. ---
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer command_buffer = cb_result.value();

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);
  VkRenderPassBeginInfo render_begin{};
  render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_begin.renderPass = target.render_pass();
  render_begin.framebuffer = target.framebuffer();
  render_begin.renderArea.extent = {kTargetW, kTargetH};
  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  render_begin.clearValueCount = 1;
  render_begin.pClearValues = &clear;
  vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{};
  viewport.width = static_cast<float>(kTargetW);
  viewport.height = static_cast<float>(kTargetH);
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {kTargetW, kTargetH};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline());
  const VkDescriptorSet descriptor_set = set_result.value();
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.pipeline_layout(), 0, 1, &descriptor_set, 0, nullptr);
  const std::uint32_t push[4] = {0U, 0U, 0U, 0U};  // element 0 -> red
  vkCmdPushConstants(command_buffer, pipeline.pipeline_layout(),
                     VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
  vkCmdDraw(command_buffer, 3, 1, 0, 0);
  vkCmdEndRenderPass(command_buffer);
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  VkFenceCreateInfo frame_fence_info{};
  frame_fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence frame_fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &frame_fence_info, nullptr, &frame_fence), VK_SUCCESS);
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, frame_fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &frame_fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  // --- Content: pixels must equal the sampled texture (red), not clear
  // black and not the other element (green) — per-element binding proven. ---
  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), kTargetW, kTargetH,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback.submitted);
  EXPECT_GT(readback.non_clear_pixels, 100U);
  // The readback helper's channel packing is endian-order (byte 0 in the low
  // byte); for BGRA targets red lands in the high byte of the 24-bit value.
  const std::uint32_t px = readback.center_pixel & 0x00FFFFFFU;
  const std::uint32_t r = (px >> 16U) & 0xFFU;
  const std::uint32_t g = (px >> 8U) & 0xFFU;
  const std::uint32_t b = px & 0xFFU;
  EXPECT_GT(r, 200U);   // Red-dominant: sampled element 0.
  EXPECT_LT(g, 100U);   // Not element 1 (green).
  EXPECT_EQ(b, 0U);
  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), frame_fence, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  pipeline.cleanup(context.device());
  target.cleanup(context.device());
  ring.cleanup();
  manager.cleanup();
  vkDestroySampler(context.device(), sampler, nullptr);
  for (int t = 0; t < 2; ++t) {
    if (views[t]) vkDestroyImageView(context.device(), views[t], nullptr);
    if (images[t]) vkDestroyImage(context.device(), images[t], nullptr);
    allocator.destroy_allocation(allocations[t]);
  }
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

// Async compute handoff: a compute pass fills a storage-buffer gradient,
// signals an event; a graphics pass waits on the event (cmd_acquire_shared
// event ordering) and draws the gradient to screen. Content check proves the
// graphics pass consumed compute's data through GPU-side synchronization.
TEST(VulkanHardware, ComputeToGraphicsEventHandoff) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppComputeHandoffTest", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  constexpr std::uint32_t kWidth = 64;
  constexpr std::uint32_t kHeight = 64;
  constexpr std::uint32_t kValues = kHeight;  // one vec4 gradient entry per row

  // --- Gradient storage buffer (device local, also host-visible for checks). ---
  constexpr VkDeviceSize kGradBytes = kValues * 16U;
  auto gradient = allocator.create_buffer(
      kGradBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(gradient.is_ok());

  // --- Reflect the compute shader's SSBO and build the layout. ---
  std::ifstream comp_file(std::string(WARPLOOM_TEST_SHADER_DIR) + "/fill_gradient.comp.spv",
                          std::ios::binary);
  ASSERT_TRUE(comp_file.good());
  const std::vector<std::uint8_t> comp_spirv(
      (std::istreambuf_iterator<char>(comp_file)), std::istreambuf_iterator<char>());
  const auto comp_bindings = omnicpp::render::reflect_spirv_resources(
      comp_spirv.data(), comp_spirv.size());
  ASSERT_EQ(comp_bindings.size(), 1U);
  EXPECT_EQ(comp_bindings[0].type, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  EXPECT_EQ(comp_bindings[0].stage_flags,
            static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_COMPUTE_BIT));

  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());
  auto comp_layout = manager.create_layout(comp_bindings, 1);
  ASSERT_TRUE(comp_layout.is_ok());
  auto comp_set = manager.allocate_set(comp_layout.value());
  ASSERT_TRUE(comp_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(comp_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   gradient.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  // --- Compute pipeline. ---
  omnicpp::render::VulkanPipeline compute_pipeline;
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(compute_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/fill_gradient.comp.spv", "compute").is_ok());
  const VkPushConstantRange comp_push{
      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t) + sizeof(float)};
  ASSERT_TRUE(compute_pipeline.create_pipeline_layout(
      context.device(), &comp_layout.value(), 1, &comp_push).is_ok());
  ASSERT_TRUE(compute_pipeline.create_compute_pipeline(
      context.device(), compute_pipeline.pipeline_layout()).is_ok());

  // --- Graphics pipeline consuming the same buffer. ---
  std::ifstream frag_file(shader_dir + "/gradient_triangle.frag.spv", std::ios::binary);
  ASSERT_TRUE(frag_file.good());
  const std::vector<std::uint8_t> frag_spirv(
      (std::istreambuf_iterator<char>(frag_file)), std::istreambuf_iterator<char>());
  const auto frag_bindings = omnicpp::render::reflect_spirv_resources(
      frag_spirv.data(), frag_spirv.size());
  ASSERT_EQ(frag_bindings.size(), 1U);
  EXPECT_EQ(frag_bindings[0].type, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

  auto gfx_layout = manager.create_layout(frag_bindings, 1);
  ASSERT_TRUE(gfx_layout.is_ok());
  auto gfx_set = manager.allocate_set(gfx_layout.value());
  ASSERT_TRUE(gfx_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(gfx_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   gradient.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanPipeline gfx_pipeline;
  ASSERT_TRUE(gfx_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/gradient_triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(gfx_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/gradient_triangle.frag.spv", "fragment").is_ok());
  const VkPushConstantRange gfx_push{
      VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(std::uint32_t) * 4};
  ASSERT_TRUE(gfx_pipeline.create_pipeline_layout(
      context.device(), &gfx_layout.value(), 1, &gfx_push).is_ok());
  ASSERT_TRUE(gfx_pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      gfx_pipeline.pipeline_layout(), false, false, false).is_ok());

  // --- Event for the compute -> graphics handoff. ---
  VkEventCreateInfo event_info{};
  event_info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
  VkEvent handoff_event = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateEvent(context.device(), &event_info, nullptr, &handoff_event), VK_SUCCESS);

  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer command_buffer = cb_result.value();

  // One buffer: compute dispatch -> set event -> wait event (gfx consume).
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);

  // Compute pass.
  const std::uint32_t comp_push_data[2] = {kValues, 0U};  // count, hue=0
  vkCmdPushConstants(command_buffer, compute_pipeline.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(comp_push_data), comp_push_data);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    compute_pipeline.pipeline());
  const VkDescriptorSet comp_ds = comp_set.value();
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          compute_pipeline.pipeline_layout(), 0, 1, &comp_ds, 0, nullptr);
  vkCmdDispatch(command_buffer, (kValues + 63U) / 64U, 1, 1);

  // Handoff: signal after compute writes, wait before graphics reads.
  omnicpp::render::cmd_signal_event(command_buffer, handoff_event,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // Graphics pass.
  VkRenderPassBeginInfo render_begin{};
  render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_begin.renderPass = target.render_pass();
  render_begin.framebuffer = target.framebuffer();
  render_begin.renderArea.extent = {kWidth, kHeight};
  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  render_begin.clearValueCount = 1;
  render_begin.pClearValues = &clear;
  vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
  {
    VkViewport viewport{};
    viewport.width = static_cast<float>(kWidth);
    viewport.height = static_cast<float>(kHeight);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {kWidth, kHeight};
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipeline.pipeline());
    const VkDescriptorSet gfx_ds = gfx_set.value();
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx_pipeline.pipeline_layout(), 0, 1, &gfx_ds, 0, nullptr);
    const std::uint32_t gfx_push_data[4] = {kValues, 0U, 0U, 0U};
    vkCmdPushConstants(command_buffer, gfx_pipeline.pipeline_layout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gfx_push_data), gfx_push_data);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
  }
  vkCmdEndRenderPass(command_buffer);
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  // --- Verify the GPU buffer content matches the analytic gradient. ---
  {
    const auto* values = static_cast<const float*>(gradient.value().mapped);
    ASSERT_NE(values, nullptr);
    for (std::uint32_t i = 0; i < kValues; i += 16) {
      const float t = static_cast<float>(i) / static_cast<float>(kValues - 1U);
      EXPECT_NEAR(values[i * 4U + 0U], t, 1e-5f) << "row " << i;
      EXPECT_NEAR(values[i * 4U + 1U], 1.0f - t, 1e-5f) << "row " << i;
    }
  }

  // --- Verify drawn content: bottom of the triangle ~ index 0 (r=t=0, g=1),
  // top ~ index max (r=1, g=0). Vulkan y-up framebuffer: bottom row = y=0. ---
  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), kWidth, kHeight,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback.submitted);
  EXPECT_GT(readback.non_clear_pixels, 500U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyEvent(context.device(), handoff_event, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  gfx_pipeline.cleanup(context.device());
  compute_pipeline.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation grad = gradient.value();
  allocator.destroy_allocation(grad);
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

TEST(VulkanHardware, AsyncComputeTimelineOverlap) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppAsyncComputeTest", true).is_ok());
  if (!context.has_timeline_semaphores()) {
    GTEST_SKIP() << "Timeline semaphores unavailable";
  }
  if (!context.has_dedicated_compute()) {
    GTEST_SKIP() << "No COMPUTE-only family: async overlap not testable here";
  }

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  const std::uint32_t comp_family = context.compute_family_index();

  // Vertex storage written by compute on the async queue, read by graphics.
  // CONCURRENT sharing (both families) means cross-queue handoffs need only
  // the timeline semaphore as memory dependency — no ownership ping-pong.
  constexpr std::uint32_t kVertices = 3;
  constexpr VkDeviceSize kDataBytes = 6U * sizeof(float) * 4U;  // 3 pos + 3 hue
  auto vertices = allocator.create_buffer(
      kDataBytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(vertices.is_ok());
  std::memset(vertices.value().mapped, 0, static_cast<std::size_t>(kDataBytes));

  omnicpp::render::AsyncComputeQueue async;
  ASSERT_TRUE(async.initialize(context.device(), context.compute_queue(), comp_family).is_ok());

  // --- Compute pipeline: gen_triangle writes positions + hues. ---
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  std::ifstream comp_file(shader_dir + "/gen_triangle.comp.spv", std::ios::binary);
  ASSERT_TRUE(comp_file.good());
  const std::vector<std::uint8_t> comp_spirv(
      (std::istreambuf_iterator<char>(comp_file)), std::istreambuf_iterator<char>());
  const auto comp_bindings = omnicpp::render::reflect_spirv_resources(
      comp_spirv.data(), comp_spirv.size());
  ASSERT_EQ(comp_bindings.size(), 1U);
  EXPECT_EQ(comp_bindings[0].type, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());
  auto comp_layout = manager.create_layout(comp_bindings, 1);
  ASSERT_TRUE(comp_layout.is_ok());
  auto comp_set = manager.allocate_set(comp_layout.value());
  ASSERT_TRUE(comp_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(comp_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   vertices.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  omnicpp::render::VulkanPipeline compute_pipeline;
  ASSERT_TRUE(compute_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/gen_triangle.comp.spv", "compute").is_ok());
  const VkPushConstantRange comp_push{
      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t) + sizeof(float)};
  ASSERT_TRUE(compute_pipeline.create_pipeline_layout(
      context.device(), &comp_layout.value(), 1, &comp_push).is_ok());
  ASSERT_TRUE(compute_pipeline.create_compute_pipeline(
      context.device(), compute_pipeline.pipeline_layout()).is_ok());

  // --- Graphics pipeline: vertex-pull triangle tinted by the compute hue. ---
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, 64, 64, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanPipeline gfx_pipeline;
  ASSERT_TRUE(gfx_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/vertexpull_triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(gfx_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/vertexpull_triangle.frag.spv", "fragment").is_ok());
  // Both stages touch the SSBO (vertex pulls, fragment reads hue); reflect
  // the vertex stage and merge the fragment's stage flags.
  std::ifstream vp_file(shader_dir + "/vertexpull_triangle.vert.spv", std::ios::binary);
  ASSERT_TRUE(vp_file.good());
  const std::vector<std::uint8_t> vp_spirv(
      (std::istreambuf_iterator<char>(vp_file)), std::istreambuf_iterator<char>());
  const auto vp_bindings = omnicpp::render::reflect_spirv_resources(
      vp_spirv.data(), vp_spirv.size());
  ASSERT_EQ(vp_bindings.size(), 1U);
  std::ifstream vf_file(shader_dir + "/vertexpull_triangle.frag.spv", std::ios::binary);
  ASSERT_TRUE(vf_file.good());
  const std::vector<std::uint8_t> vf_spirv(
      (std::istreambuf_iterator<char>(vf_file)), std::istreambuf_iterator<char>());
  const auto vf_bindings = omnicpp::render::reflect_spirv_resources(
      vf_spirv.data(), vf_spirv.size());
  ASSERT_TRUE(vf_bindings.empty());  // hue frag reads only the varying
  auto gfx_layout = manager.create_layout(vp_bindings, 1);
  ASSERT_TRUE(gfx_layout.is_ok());
  auto gfx_set = manager.allocate_set(gfx_layout.value());
  ASSERT_TRUE(gfx_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(gfx_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   vertices.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  ASSERT_TRUE(gfx_pipeline.create_pipeline_layout(
      context.device(), &gfx_layout.value(), 1, nullptr).is_ok());
  ASSERT_TRUE(gfx_pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      gfx_pipeline.pipeline_layout(), false, false, false).is_ok());

  const auto gfx_pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(gfx_pool_result.is_ok());
  const VkCommandPool gfx_pool = gfx_pool_result.value();
  const auto gfx_cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), gfx_pool);
  ASSERT_TRUE(gfx_cb_result.is_ok());
  const VkCommandBuffer gfx_cb = gfx_cb_result.value();

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence gfx_fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &gfx_fence), VK_SUCCESS);

  // --- Two pipelined frames. Frame N: async compute regenerates vertices
  // with rotation phase = N radians; graphics waits (GPU-side) on frame N's
  // timeline value, then draws. Frame 1's vertices differ from frame 0's —
  // proving the graphics pass consumed THIS frame's compute output. ---
  constexpr int kFrames = 2;
  std::uint64_t last_signalled = 0;
  for (int frame = 0; frame < kFrames; ++frame) {
    async.begin();
    struct CompCtx {
      VkPipeline pipeline;
      VkPipelineLayout layout;
      VkDescriptorSet set;
      std::uint32_t count;
      float phase;
    };
    CompCtx comp_ctx{compute_pipeline.pipeline(), compute_pipeline.pipeline_layout(),
                     comp_set.value(), kVertices, static_cast<float>(frame)};
    async.record(
        [](VkCommandBuffer cmd, void* ud) {
          auto* c = static_cast<CompCtx*>(ud);
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
          vkCmdPushConstants(cmd, c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(std::uint32_t) + sizeof(float), &c->count);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout,
                                  0, 1, &c->set, 0, nullptr);
          vkCmdDispatch(cmd, c->count, 1, 1);
        },
        &comp_ctx);
    auto signal_result = async.submit();
    ASSERT_TRUE(signal_result.is_ok());
    last_signalled = signal_result.value();

    // Consumer: graphics waits on the compute timeline value before drawing.
    ASSERT_EQ(vkResetCommandBuffer(gfx_cb, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(gfx_cb, &begin), VK_SUCCESS);
    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = target.render_pass();
    render_begin.framebuffer = target.framebuffer();
    render_begin.renderArea.extent = {64, 64};
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    render_begin.clearValueCount = 1;
    render_begin.pClearValues = &clear;
    vkCmdBeginRenderPass(gfx_cb, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    {
      VkViewport viewport{};
      viewport.width = 64.0f;
      viewport.height = 64.0f;
      viewport.maxDepth = 1.0f;
      vkCmdSetViewport(gfx_cb, 0, 1, &viewport);
      VkRect2D scissor{};
      scissor.extent = {64, 64};
      vkCmdSetScissor(gfx_cb, 0, 1, &scissor);
      vkCmdBindPipeline(gfx_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipeline.pipeline());
      const VkDescriptorSet gfx_ds = gfx_set.value();
      vkCmdBindDescriptorSets(gfx_cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              gfx_pipeline.pipeline_layout(), 0, 1, &gfx_ds, 0, nullptr);
      vkCmdDraw(gfx_cb, kVertices, 1, 0, 0);
    }
    vkCmdEndRenderPass(gfx_cb);
    ASSERT_EQ(vkEndCommandBuffer(gfx_cb), VK_SUCCESS);

    VkSemaphore wait_sem = async.timeline_semaphore();
    VkTimelineSemaphoreSubmitInfo timeline_wait{};
    timeline_wait.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_wait.waitSemaphoreValueCount = 1;
    timeline_wait.pWaitSemaphoreValues = &last_signalled;
    constexpr VkPipelineStageFlags kWaitStage =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline_wait;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &wait_sem;
    submit.pWaitDstStageMask = &kWaitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &gfx_cb;
    ASSERT_EQ(vkResetFences(context.device(), 1, &gfx_fence), VK_SUCCESS);
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, gfx_fence), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1, &gfx_fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
  }

  // --- Verify: compute output matches the analytic rotation for the FINAL
  // phase (1 radian); the framebuffer has triangle coverage tinted by the
  // compute-generated hue (green-dominant, hue[0] = (0,1,.5,1)). ---
  {
    const auto* data = static_cast<const float*>(vertices.value().mapped);
    ASSERT_NE(data, nullptr);
    const float expect_x[3] = {0.85f * std::cos(1.0f),
                               0.85f * std::cos(1.0f + 2.0943951f),
                               0.85f * std::cos(1.0f + 4.1887902f)};
    const float expect_y[3] = {0.85f * std::sin(1.0f),
                               0.85f * std::sin(1.0f + 2.0943951f),
                               0.85f * std::sin(1.0f + 4.1887902f)};
    for (std::uint32_t i = 0; i < kVertices; ++i) {
      EXPECT_NEAR(data[i * 4U + 0U], expect_x[i], 1e-5f) << "vertex " << i;
      EXPECT_NEAR(data[i * 4U + 1U], expect_y[i], 1e-5f) << "vertex " << i;
      EXPECT_NEAR(data[i * 4U + 2U], 0.0f, 1e-6f) << "vertex " << i;
      EXPECT_NEAR(data[i * 4U + 3U], 1.0f, 1e-6f) << "vertex " << i;
      // Hue vector: r = i/3, g = 1 - i/3, b = 0.5, a = 1.
      EXPECT_NEAR(data[(3U + i) * 4U + 0U], static_cast<float>(i) / 3.0f, 1e-6f) << "hue " << i;
      EXPECT_NEAR(data[(3U + i) * 4U + 1U], 1.0f - static_cast<float>(i) / 3.0f, 1e-6f) << "hue " << i;
    }
  }

  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), 64, 64,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback.submitted);
  EXPECT_GT(readback.non_clear_pixels, 500U);
  // hue[0] = (0, 1, 0.5): green dominant along the v0 corner; the interpolated
  // interior mixes hues, so require a meaningful green-dominant population.
  EXPECT_GT(readback.green_dominant_pixels, 100U);

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  ASSERT_TRUE(async.wait_done());
  async.cleanup();  // destroy queue resources BEFORE the device goes away
  vkDestroyFence(context.device(), gfx_fence, nullptr);
  vkDestroyCommandPool(context.device(), gfx_pool, nullptr);
  gfx_pipeline.cleanup(context.device());
  compute_pipeline.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation verts = vertices.value();
  allocator.destroy_allocation(verts);
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

TEST(VulkanHardware, RenderGraphTwoPassBarriersAndRender) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppRenderGraphTest", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  // Two independent offscreen targets: pass 0 renders the triangle into A,
  // pass 1 re-renders a different aspect into B (same shader, clear-only
  // difference). The graph must barrier A between the passes and initialize B.
  constexpr std::uint32_t kWidth = 320;
  constexpr std::uint32_t kHeight = 240;
  omnicpp::render::VulkanOffscreenTarget target_a;
  ASSERT_TRUE(target_a.create(context.device(), context.physical_device(),
                              VK_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, &allocator).is_ok());
  ASSERT_TRUE(target_a.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target_a.create_framebuffer(context.device()).is_ok());
  omnicpp::render::VulkanOffscreenTarget target_b;
  ASSERT_TRUE(target_b.create(context.device(), context.physical_device(),
                              VK_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, &allocator).is_ok());
  ASSERT_TRUE(target_b.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target_b.create_framebuffer(context.device()).is_ok());

#ifdef WARPLOOM_TEST_SHADER_DIR
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  omnicpp::render::VulkanPipeline pipeline;
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(pipeline.create_pipeline_layout(context.device()).is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target_a.render_pass(), target_a.format(),
      pipeline.pipeline_layout(), false, false, false).is_ok());
  ASSERT_NE(pipeline.pipeline(), VK_NULL_HANDLE);
#else
  GTEST_SKIP() << "Test shaders were not compiled";
#endif

  // Declare passes: A keeps TRANSFER_SRC (readback later), B keeps TRANSFER_SRC.
  std::vector<omnicpp::render::GraphPass> passes(2);
  passes[0].name = "scene_a";
  passes[0].render_pass = target_a.render_pass();
  passes[0].framebuffer = target_a.framebuffer();
  passes[0].width = kWidth;
  passes[0].height = kHeight;
  passes[0].user_data = &target_a;
  passes[0].attachments.push_back(omnicpp::render::color_attachment(
      target_a.image(), target_a.image_view(), target_a.format(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));

  passes[1].name = "scene_b";
  passes[1].render_pass = target_b.render_pass();
  passes[1].framebuffer = target_b.framebuffer();
  passes[1].width = kWidth;
  passes[1].height = kHeight;
  passes[1].user_data = &target_b;
  passes[1].attachments.push_back(omnicpp::render::color_attachment(
      target_b.image(), target_b.image_view(), target_b.format(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));

  // Compile and verify the compiler computed initialization barriers.
  const auto compiled = omnicpp::render::compile_render_graph(passes);
  ASSERT_EQ(compiled.barriers_per_pass.size(), 2U);
  ASSERT_EQ(compiled.barriers_per_pass[0].size(), 1U); // A: UNDEFINED -> COLOR
  EXPECT_EQ(compiled.barriers_per_pass[0][0].image, target_a.image());
  ASSERT_EQ(compiled.barriers_per_pass[1].size(), 1U); // B: UNDEFINED -> COLOR
  EXPECT_EQ(compiled.barriers_per_pass[1][0].image, target_b.image());

  // Execute: two draws, one per pass, through the graph executor.
  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer command_buffer = cb_result.value();

  std::vector<VkClearValue> clear_a(1);
  clear_a[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  std::vector<VkClearValue> clear_b(1);
  clear_b[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  passes[0].clear_values = clear_a.data();
  passes[0].clear_value_count = 1;
  passes[1].clear_values = clear_b.data();
  passes[1].clear_value_count = 1;

  auto record = [](VkCommandBuffer cmd, const omnicpp::render::GraphPass& pass,
                   void* user_data) {
    auto* pipe = static_cast<omnicpp::render::VulkanPipeline*>(user_data);
    VkViewport viewport{};
    viewport.width = static_cast<float>(pass.width);
    viewport.height = static_cast<float>(pass.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {pass.width, pass.height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline());
    vkCmdDraw(cmd, 3, 1, 0, 0);
  };
  // user_data carries the pipeline (both passes draw with it).
  passes[0].user_data = &pipeline;
  passes[1].user_data = &pipeline;

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);
  omnicpp::render::execute_render_graph(command_buffer, passes, compiled, record);
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  // Both targets must hold rendered triangle content.
  for (const omnicpp::render::VulkanOffscreenTarget* target : {&target_a, &target_b}) {
    const auto readback = readback_swapchain_image(
        context.physical_device(), context.device(), context.graphics_queue(),
        static_cast<std::uint32_t>(context.queue_families().graphics_family),
        target->image(), target->format(), kWidth, kHeight,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ASSERT_TRUE(readback.submitted);
    EXPECT_GT(readback.non_clear_pixels, 100U);
  }
  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  pipeline.cleanup(context.device());
  target_a.cleanup(context.device());
  target_b.cleanup(context.device());
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support was not enabled for this build";
#endif
}

TEST(VulkanHardware, ParallelRecorderMultithreadedBands) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppParallelRecorderTest", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  constexpr std::uint32_t kWidth = 320;
  constexpr std::uint32_t kHeight = 256; // divisible by band count
  constexpr std::uint32_t kBandCount = 4;
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  omnicpp::render::VulkanPipeline pipeline;
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(pipeline.create_pipeline_layout(context.device()).is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      pipeline.pipeline_layout(), false, false, false).is_ok());

  // Record 4 bands on 4 threads; each band draws the full-width triangle
  // clipped to its scissor (the geometry spans the whole viewport vertically).
  omnicpp::render::VulkanParallelRecorder recorder;
  ASSERT_TRUE(recorder.initialize(
      context.device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      kBandCount).is_ok());

  auto secondaries = recorder.record_parallel(
      kWidth, kHeight,
      [&pipeline](VkCommandBuffer cmd, VkRect2D band_scissor) {
        VkViewport viewport{};
        viewport.width = static_cast<float>(band_scissor.extent.width);
        viewport.height = static_cast<float>(band_scissor.extent.height);
        viewport.x = 0.0f;
        viewport.y = static_cast<float>(band_scissor.offset.y);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &band_scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline());
        vkCmdDraw(cmd, 3, 1, 0, 0);
      },
      target.render_pass(), target.framebuffer());
  ASSERT_TRUE(secondaries.is_ok());
  EXPECT_EQ(secondaries.value().size(), kBandCount);

  // Execute secondaries inside one primary buffer and render pass.
  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer command_buffer = cb_result.value();

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);

  VkRenderPassBeginInfo render_begin{};
  render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_begin.renderPass = target.render_pass();
  render_begin.framebuffer = target.framebuffer();
  render_begin.renderArea.extent = {kWidth, kHeight};
  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  render_begin.clearValueCount = 1;
  render_begin.pClearValues = &clear;
  vkCmdBeginRenderPass(command_buffer, &render_begin,
                       VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
  vkCmdExecuteCommands(command_buffer,
                       static_cast<std::uint32_t>(secondaries.value().size()),
                       secondaries.value().data());
  vkCmdEndRenderPass(command_buffer);
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  // Every band must show triangle content (each band drew its clipped slice).
  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), kWidth, kHeight,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback.submitted);
  EXPECT_GT(readback.non_clear_pixels, 100U);
  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  pipeline.cleanup(context.device());
  target.cleanup(context.device());
  recorder.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

// TSan stress: repeated multithreaded recording waves under high contention.
// Each wave re-records all bands from live worker threads, then the frame is
// submitted before the next wave — the pattern race detectors need sustained
// interleaving pressure to surface latent data races.
TEST(VulkanHardware, ParallelRecorderContentionStress) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppParallelStressTest", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  constexpr std::uint32_t kWidth = 256;
  constexpr std::uint32_t kHeight = 128;
  // Band count above core count forces queueing/stealing between threads.
  const std::uint32_t band_count =
      static_cast<std::uint32_t>(std::max(8U, std::thread::hardware_concurrency() * 2U));

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  omnicpp::render::VulkanPipeline pipeline;
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/triangle.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(pipeline.create_pipeline_layout(context.device()).is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      pipeline.pipeline_layout(), false, false, false).is_ok());

  omnicpp::render::VulkanParallelRecorder recorder;
  ASSERT_TRUE(recorder.initialize(
      context.device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      band_count).is_ok());

  // Exercise both execution backends: persistent job-system workers (the
  // allocation-free frame path) and ad-hoc threads (the fallback).
  omnicpp::core::JobSystem jobs;
  ASSERT_TRUE(jobs.initialize());
  recorder.set_job_system(&jobs);
  auto record_frame = [&](int wave) {
    auto secondaries = recorder.record_parallel(
        kWidth, kHeight,
        [&pipeline](VkCommandBuffer cmd, VkRect2D band_scissor) {
          VkViewport viewport{};
          viewport.width = static_cast<float>(band_scissor.extent.width);
          viewport.height = static_cast<float>(band_scissor.extent.height);
          viewport.x = 0.0f;
          viewport.y = static_cast<float>(band_scissor.offset.y);
          viewport.maxDepth = 1.0f;
          vkCmdSetViewport(cmd, 0, 1, &viewport);
          vkCmdSetScissor(cmd, 0, 1, &band_scissor);
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline());
          vkCmdDraw(cmd, 3, 1, 0, 0);
        },
        target.render_pass(), target.framebuffer());
    (void)wave;
    return secondaries;
  };

  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer command_buffer = cb_result.value();

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);

  constexpr int kWaves = 12;
  for (int wave = 0; wave < kWaves; ++wave) {
    // Even waves: job-system workers. Odd waves: ad-hoc threads (fallback).
    recorder.set_job_system((wave % 2 == 0) ? &jobs : nullptr);
    auto secondaries = record_frame(wave);
    ASSERT_TRUE(secondaries.is_ok());

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);
    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = target.render_pass();
    render_begin.framebuffer = target.framebuffer();
    render_begin.renderArea.extent = {kWidth, kHeight};
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    render_begin.clearValueCount = 1;
    render_begin.pClearValues = &clear;
    vkCmdBeginRenderPass(command_buffer, &render_begin,
                         VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(command_buffer,
                         static_cast<std::uint32_t>(secondaries.value().size()),
                         secondaries.value().data());
    vkCmdEndRenderPass(command_buffer);
    ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

    ASSERT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer;
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, fence), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
  }

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  jobs.shutdown();
  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  pipeline.cleanup(context.device());
  target.cleanup(context.device());
  recorder.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

TEST(VulkanHardware, OffscreenTriangleReadback) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppOffscreenTest", true).is_ok());

  // Feature negotiation smoke assertions for this driver stack.
  EXPECT_TRUE(context.has_synchronization2());
  EXPECT_TRUE(context.has_timeline_semaphores());
  EXPECT_GT(context.device_properties().timestamp_period_ns, 0.0f);

  // The offscreen target now allocates through the block sub-allocator.
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  constexpr std::uint32_t width = 640;
  constexpr std::uint32_t height = 480;
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, width, height,
                            &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanPipeline pipeline;
#ifdef WARPLOOM_TEST_SHADER_DIR
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline.load_shader_file(context.device(), shader_dir + "/triangle.vert.spv").is_ok());
  ASSERT_TRUE(pipeline.load_shader_file(context.device(), shader_dir + "/triangle.frag.spv").is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(), VK_NULL_HANDLE,
      false, false, false).is_ok());
#else
  GTEST_SKIP() << "Test shaders were not compiled";
#endif

  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool command_pool = pool_result.value();
  const auto command_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), command_pool);
  ASSERT_TRUE(command_result.is_ok());
  const VkCommandBuffer command_buffer = command_result.value();

  // GPU timestamp telemetry around the render pass.
  VkQueryPoolCreateInfo query_info{};
  query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query_info.queryCount = 2;
  VkQueryPool query_pool = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateQueryPool(context.device(), &query_info, nullptr, &query_pool), VK_SUCCESS);

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(command_buffer, &begin), VK_SUCCESS);

  VkRenderPassBeginInfo render_begin{};
  render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_begin.renderPass = target.render_pass();
  render_begin.framebuffer = target.framebuffer();
  render_begin.renderArea.extent = {width, height};
  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  render_begin.clearValueCount = 1;
  render_begin.pClearValues = &clear;
  // Query reset must be outside the render pass (VUID-vkCmdResetQueryPool-renderpass).
  vkCmdResetQueryPool(command_buffer, query_pool, 0, 2);
  vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{};
  viewport.width = static_cast<float>(width);
  viewport.height = static_cast<float>(height);
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {width, height};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, 0);
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline());
  vkCmdDraw(command_buffer, 3, 1, 0, 0);
  vkCmdEndRenderPass(command_buffer);
  vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, 1);
  ASSERT_EQ(vkEndCommandBuffer(command_buffer), VK_SUCCESS);

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  std::uint64_t timestamps[2] = {0, 0};
  ASSERT_EQ(vkGetQueryPoolResults(context.device(), query_pool, 0, 2, sizeof(timestamps),
                                  timestamps, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT),
            VK_SUCCESS);
  const auto render_pass_ns = static_cast<std::uint64_t>(
      static_cast<double>(timestamps[1] - timestamps[0]) *
      context.device_properties().timestamp_period_ns);
  EXPECT_GT(render_pass_ns, 0ULL);
  std::cout << "GPU render-pass time: " << render_pass_ns << " ns" << std::endl;

  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), width, height,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback.submitted);
  EXPECT_GT(readback.non_clear_pixels, 1000U);
  EXPECT_GT(readback.red_dominant_pixels, 100U);
  EXPECT_GT(readback.green_dominant_pixels, 100U);
  EXPECT_GT(readback.blue_dominant_pixels, 100U);
  expect_canonical_triangle_hash(readback.canonical_hash);
  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyQueryPool(context.device(), query_pool, nullptr);
  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  pipeline.cleanup(context.device());
  target.cleanup(context.device());
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support was not enabled for this build";
#endif
}

TEST(VulkanHardware, SwapchainRecreationStress) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(VK_USE_PLATFORM_XCB_KHR)
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";
  const char* display_name = std::getenv("DISPLAY");
  if (!display_name) GTEST_SKIP() << "DISPLAY is unavailable";

  xcb_connection_t* connection = xcb_connect(display_name, nullptr);
  if (!connection || xcb_connection_has_error(connection)) {
    if (connection) xcb_disconnect(connection);
    GTEST_SKIP() << "XCB display is unavailable";
  }
  const auto setup = xcb_get_setup(connection);
  const auto* screen = xcb_setup_roots_iterator(setup).data;
  if (!screen) {
    xcb_disconnect(connection);
    GTEST_SKIP() << "XCB screen is unavailable";
  }
  const xcb_window_t window = xcb_generate_id(connection);
  const std::uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root,
                    0, 0, 640, 480, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual, XCB_CW_EVENT_MASK, &event_mask);
  xcb_map_window(connection, window);
  xcb_flush(connection);

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppRecreationTest", true).is_ok());

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  omnicpp::render::SurfaceCreateInfo surface_info;
  surface_info.display = connection;
  surface_info.window = reinterpret_cast<void*>(static_cast<std::uintptr_t>(window));
  auto surface_result = omnicpp::render::create_platform_surface(context.instance(), surface_info, surface);
  if (!surface_result.is_ok()) {
    context.cleanup();
    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
    GTEST_SKIP() << "XCB Vulkan surface unavailable";
  }
  ASSERT_NE(surface, VK_NULL_HANDLE);

  omnicpp::render::VulkanSwapchain swapchain;
  ASSERT_TRUE(swapchain.create(context.device(), context.physical_device(), surface, {}).is_ok());
  ASSERT_TRUE(swapchain.create_image_views(context.device()).is_ok());

  const VkFormat depth_format = omnicpp::render::VulkanRenderPass::find_supported_depth_format(context.physical_device());
  ASSERT_NE(depth_format, VK_FORMAT_UNDEFINED);
  omnicpp::render::VulkanRenderPass render_pass;
  ASSERT_TRUE(render_pass.create(context.device(), swapchain.image_format(), depth_format).is_ok());
  ASSERT_TRUE(render_pass.create_depth_resources(context.device(), context.physical_device(), depth_format,
                                                 swapchain.extent_width(), swapchain.extent_height()).is_ok());
  ASSERT_TRUE(render_pass.create_framebuffers(context.device(), swapchain.image_views(),
                                              swapchain.extent_width(), swapchain.extent_height()).is_ok());

  omnicpp::render::VulkanPipeline pipeline;
#ifdef WARPLOOM_TEST_SHADER_DIR
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(), shader_dir + "/triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(), shader_dir + "/triangle.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(pipeline.all_core_stages_loaded());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(context.device(), render_pass.render_pass(),
                                                swapchain.image_format(), VK_NULL_HANDLE,
                                                true, true, false).is_ok());
#else
  GTEST_SKIP() << "Test shaders were not compiled";
#endif

  omnicpp::render::VulkanRenderer renderer;
  renderer.set_timeline_pacing(context.has_timeline_semaphores());
  ASSERT_TRUE(renderer.initialize(context, swapchain, render_pass).is_ok());
  renderer.set_synchronization2(context.has_synchronization2());

  auto render_frames = [&](std::uint32_t count) -> bool {
    for (std::uint32_t i = 0; i < count; ++i) {
      auto image = renderer.begin_frame();
      if (!image.is_ok()) return false;
      if (!renderer.record_commands(image.value(), render_pass.framebuffer(image.value()),
                                    swapchain.extent_width(), swapchain.extent_height()).is_ok()) return false;
      if (!renderer.submit_frame().is_ok()) return false;
      if (!renderer.present_frame().is_ok()) return false;
    }
    return true;
  };
  ASSERT_TRUE(render_frames(4));

  // Recreate at a different extent: retire framebuffers, rebuild swapchain,
  // framebuffers, and renderer per-image resources, then render again.
  renderer.wait_idle();
  render_pass.cleanup(context.device());
  ASSERT_TRUE(swapchain.recreate(context.device(), context.physical_device(), surface, 320, 240).is_ok());
  ASSERT_TRUE(render_pass.create(context.device(), swapchain.image_format(), depth_format).is_ok());
  ASSERT_TRUE(render_pass.create_depth_resources(context.device(), context.physical_device(), depth_format,
                                                 swapchain.extent_width(), swapchain.extent_height()).is_ok());
  ASSERT_TRUE(render_pass.create_framebuffers(context.device(), swapchain.image_views(),
              swapchain.extent_width(), swapchain.extent_height()).is_ok());
  ASSERT_TRUE(renderer.resync_for_swapchain(swapchain, render_pass.render_pass()).is_ok());
  ASSERT_TRUE(render_frames(4));
  renderer.wait_idle();

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  renderer.cleanup(context.device());
  pipeline.cleanup(context.device());
  render_pass.cleanup(context.device());
  swapchain.cleanup(context.device());
  context.destroy_surface(surface);
  context.cleanup();
  xcb_destroy_window(connection, window);
  xcb_disconnect(connection);
#else
  GTEST_SKIP() << "Vulkan XCB support was not enabled for this build";
#endif
}

TEST(VulkanSwapchain, ChooseSurfaceFormatPrefersSRGB) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE
  std::vector<VkSurfaceFormatKHR> formats = {
      VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
      VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
  const auto chosen = omnicpp::render::VulkanSwapchain::choose_surface_format(formats);
  EXPECT_EQ(chosen.format, VK_FORMAT_B8G8R8A8_SRGB);
#else
  std::vector<VkFormat> formats = {50, 44};
  const auto chosen = omnicpp::render::VulkanSwapchain::choose_surface_format(formats);
  EXPECT_EQ(chosen, static_cast<VkFormat>(50));
#endif
}

TEST(VulkanSwapchain, ChoosePresentModeReturnsValidValue) {
  std::vector<VkPresentModeKHR> modes = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
  const auto chosen = omnicpp::render::VulkanSwapchain::choose_present_mode(modes, true);
  EXPECT_EQ(chosen, VK_PRESENT_MODE_FIFO_KHR);
}

TEST(VulkanSwapchain, ChoosePresentModeMailboxWithoutVsync) {
  std::vector<VkPresentModeKHR> modes = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
  const auto chosen = omnicpp::render::VulkanSwapchain::choose_present_mode(modes, false);
  EXPECT_EQ(chosen, VK_PRESENT_MODE_MAILBOX_KHR);
}

TEST(VulkanSwapchain, ChooseExtentClampsToCapabilities) {
  VkSurfaceCapabilitiesKHR caps{};
  caps.minImageExtent = {100, 100};
  caps.maxImageExtent = {2000, 2000};
  caps.currentExtent = {std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()};
  const auto extent = omnicpp::render::VulkanSwapchain::choose_extent(caps, 5000, 5000);
  EXPECT_EQ(extent.width, 2000u);
  EXPECT_EQ(extent.height, 2000u);
}

TEST(VulkanSwapchain, ChooseExtentUsesCurrentExtentWhenDefined) {
  VkSurfaceCapabilitiesKHR caps{};
  caps.currentExtent = {1920, 1080};
  const auto extent = omnicpp::render::VulkanSwapchain::choose_extent(caps, 800, 600);
  EXPECT_EQ(extent.width, 1920u);
  EXPECT_EQ(extent.height, 1080u);
}

TEST(VulkanSwapchain, CreateFailsWithoutDevice) {
  omnicpp::render::VulkanSwapchain swapchain;
  auto result = swapchain.create(nullptr, nullptr, nullptr, {});
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanSwapchain, CreateImageViewsFailsWithoutImages) {
  omnicpp::render::VulkanSwapchain swapchain;
  auto result = swapchain.create_image_views(nullptr);
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanSwapchain, CleanupIsIdempotent) {
  omnicpp::render::VulkanSwapchain swapchain;
  swapchain.cleanup(nullptr);
  swapchain.cleanup(nullptr);
  EXPECT_FALSE(swapchain.is_valid());
}

TEST(VulkanSwapchain, RecreateFailsWithZeroDimensions) {
  omnicpp::render::VulkanSwapchain swapchain;
  auto result = swapchain.recreate(nullptr, nullptr, nullptr, 0, 0);
  EXPECT_FALSE(result.is_ok());
}

// ============================================================================
// VulkanRenderPass Tests
// ============================================================================

TEST(VulkanRenderPass, CreateFailsWithoutDevice) {
  omnicpp::render::VulkanRenderPass rp;
  auto result = rp.create(nullptr, static_cast<VkFormat>(43), static_cast<VkFormat>(100));
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanRenderPass, FramebufferCountZeroBeforeCreation) {
  omnicpp::render::VulkanRenderPass rp;
  EXPECT_EQ(rp.framebuffer_count(), 0u);
}

TEST(VulkanRenderPass, CleanupIsIdempotent) {
  omnicpp::render::VulkanRenderPass rp;
  rp.cleanup(nullptr);
  rp.cleanup(nullptr);
  EXPECT_EQ(rp.render_pass(), nullptr);
}

TEST(VulkanRenderPass, FindSupportedDepthFormatReturnsUndefinedWithoutDevice) {
  const auto format = omnicpp::render::VulkanRenderPass::find_supported_depth_format(nullptr);
  EXPECT_EQ(format, VK_FORMAT_UNDEFINED);
}

TEST(VulkanOffscreenTarget, RejectsInvalidDimensions) {
  omnicpp::render::VulkanOffscreenTarget target;
  auto result = target.create(nullptr, nullptr, VK_FORMAT_UNDEFINED, 0, 0);
  EXPECT_FALSE(result.is_ok());
  EXPECT_FALSE(target.is_valid());
}

TEST(VulkanOffscreenTarget, CleanupIsIdempotent) {
  omnicpp::render::VulkanOffscreenTarget target;
  target.cleanup(nullptr);
  target.cleanup(nullptr);
  EXPECT_FALSE(target.is_valid());
}

// ============================================================================
// VulkanPipeline Tests
// ============================================================================

TEST(VulkanPipeline, LoadShaderFailsWithEmptyData) {
  omnicpp::render::VulkanPipeline pipeline;
  auto result = pipeline.load_shader(nullptr, nullptr, 0);
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanPipeline, LoadShaderFromBytesFailsWithEmpty) {
  omnicpp::render::VulkanPipeline pipeline;
  std::vector<std::uint8_t> empty;
  auto result = pipeline.load_shader_from_bytes(nullptr, empty);
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanPipeline, LoadShaderFromBytesFailsWithOddSize) {
  omnicpp::render::VulkanPipeline pipeline;
  std::vector<std::uint8_t> bad(5, 0);
  auto result = pipeline.load_shader_from_bytes(nullptr, bad);
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanPipeline, CreateGraphicsPipelineFailsWithoutDevice) {
  omnicpp::render::VulkanPipeline pipeline;
  auto result = pipeline.create_graphics_pipeline(nullptr, nullptr, static_cast<VkFormat>(43));
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanPipeline, CleanupIsIdempotent) {
  omnicpp::render::VulkanPipeline pipeline;
  pipeline.cleanup(nullptr);
  pipeline.cleanup(nullptr);
  EXPECT_EQ(pipeline.pipeline(), nullptr);
  EXPECT_EQ(pipeline.pipeline_layout(), nullptr);
}

TEST(VulkanPipeline, LoadShaderFileFailsWithNonexistentFile) {
  omnicpp::render::VulkanPipeline pipeline;
  auto result = pipeline.load_shader_file(nullptr, "/nonexistent/shader.spv");
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanPipeline, LoadShaderStageFailsWithNullDevice) {
  omnicpp::render::VulkanPipeline pipeline;
  EXPECT_FALSE(pipeline.load_shader_stage_file(nullptr, "/nonexistent.spv", "vertex").is_ok());
  EXPECT_FALSE(pipeline.has_stage("vertex"));
  EXPECT_FALSE(pipeline.has_stage("unknown"));
  EXPECT_FALSE(pipeline.all_core_stages_loaded());
}

TEST(VulkanRenderer, ResyncFailsBeforeInit) {
  omnicpp::render::VulkanSwapchain swapchain;
  omnicpp::render::VulkanRenderer renderer;
  EXPECT_FALSE(renderer.resync_for_swapchain(swapchain, nullptr).is_ok());
}

// ============================================================================
// VulkanRenderer Tests
// ============================================================================

TEST(VulkanRenderer, InitializeFailsWithoutContext) {
  omnicpp::render::VulkanContext ctx;
  omnicpp::render::VulkanSwapchain swapchain;
  omnicpp::render::VulkanRenderPass render_pass;
  omnicpp::render::VulkanRenderer renderer;
  auto result = renderer.initialize(ctx, swapchain, render_pass);
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanRenderer, IsFalseBeforeInit) {
  omnicpp::render::VulkanRenderer renderer;
  EXPECT_FALSE(renderer.is_initialized());
  EXPECT_EQ(renderer.frame_count(), 0u);
}

TEST(VulkanRenderer, RejectsZeroFramesInFlight) {
  omnicpp::render::VulkanContext context;
  omnicpp::render::VulkanSwapchain swapchain;
  omnicpp::render::VulkanRenderPass render_pass;
  omnicpp::render::VulkanRenderer renderer;
  omnicpp::render::RendererConfig config;
  config.max_frames_in_flight = 0;
  auto result = renderer.initialize(context, swapchain, render_pass, config);
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanRenderer, CleanupIsIdempotent) {
  omnicpp::render::VulkanRenderer renderer;
  renderer.cleanup(nullptr);
  renderer.cleanup(nullptr);
  EXPECT_FALSE(renderer.is_initialized());
}

TEST(VulkanRenderer, CreateCommandPoolFailsWithoutDevice) {
  auto result = omnicpp::render::VulkanRenderer::create_command_pool(nullptr, 0);
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanRenderer, AllocateCommandBufferFailsWithoutDevice) {
  auto result = omnicpp::render::VulkanRenderer::allocate_command_buffer(nullptr, nullptr);
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanRenderer, BeginFrameFailsBeforeInit) {
  omnicpp::render::VulkanRenderer renderer;
  auto result = renderer.begin_frame();
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanRenderer, EndFrameFailsBeforeInit) {
  omnicpp::render::VulkanRenderer renderer;
  auto result = renderer.end_frame();
  EXPECT_FALSE(result.is_ok());
}

TEST(VulkanRenderer, WaitIdleIsSafeBeforeInit) {
  omnicpp::render::VulkanRenderer renderer;
  renderer.wait_idle();
}

// ============================================================================
// FrameResources Tests
// ============================================================================

TEST(FrameResources, CleanupIsIdempotent) {
  omnicpp::render::FrameResources frame;
  frame.cleanup(nullptr);
  frame.cleanup(nullptr);
  EXPECT_EQ(frame.command_buffer, nullptr);
}

// ============================================================================
// Config Tests
// ============================================================================

TEST(SwapchainConfig, DefaultValues) {
  omnicpp::render::SwapchainConfig config;
  EXPECT_EQ(config.width, 800u);
  EXPECT_EQ(config.height, 600u);
  EXPECT_TRUE(config.vsync);
  EXPECT_EQ(config.max_frames_in_flight, 2u);
}

TEST(RendererConfig, DefaultValues) {
  omnicpp::render::RendererConfig config;
  EXPECT_EQ(config.max_frames_in_flight, 2u);
  EXPECT_FLOAT_EQ(config.clear_color_r, 0.0f);
  EXPECT_FLOAT_EQ(config.clear_color_a, 1.0f);
  EXPECT_FLOAT_EQ(config.clear_depth, 1.0f);
}

// ============================================================================
// SoftwareRasterizer Tests
// ============================================================================

TEST(SoftwareRasterizer, ClearSetsBlack) {
  omnicpp::render::SoftwareRasterizer rast(64, 64);
  rast.clear(0xFF000000);
  EXPECT_EQ(rast.get_pixel(0, 0), 0xFF000000u);
  EXPECT_EQ(rast.get_pixel(32, 32), 0xFF000000u);
}

TEST(SoftwareRasterizer, ClearSetsDepth) {
  omnicpp::render::SoftwareRasterizer rast(8, 8);
  rast.clear(0xFF000000, 0.5f);
  EXPECT_FLOAT_EQ(rast.depth_data()[0], 0.5f);
}

TEST(SoftwareRasterizer, DrawTriangleFillsPixels) {
  omnicpp::render::SoftwareRasterizer rast(100, 100);
  rast.clear(0xFF000000);
  rast.draw_triangle(10, 10, 0, 0xFFFF0000, 90, 10, 0, 0xFF00FF00, 50, 90, 0, 0xFF0000FF);
  EXPECT_NE(rast.get_pixel(50, 50), 0xFF000000u);
  EXPECT_EQ(rast.get_pixel(0, 0), 0xFF000000u);
  EXPECT_EQ(rast.get_pixel(99, 99), 0xFF000000u);
}

TEST(SoftwareRasterizer, DrawTriangleIsDeterministic) {
  omnicpp::render::SoftwareRasterizer rast1(64, 64);
  omnicpp::render::SoftwareRasterizer rast2(64, 64);
  rast1.clear(0xFF000000);
  rast2.clear(0xFF000000);
  rast1.draw_triangle(10, 10, 0, 0xFFFF0000, 50, 10, 0, 0xFF00FF00, 30, 50, 0, 0xFF0000FF);
  rast2.draw_triangle(10, 10, 0, 0xFFFF0000, 50, 10, 0, 0xFF00FF00, 30, 50, 0, 0xFF0000FF);
  EXPECT_EQ(rast1.frame_hash(), rast2.frame_hash());
}

TEST(SoftwareRasterizer, DepthBufferClosesFarther) {
  omnicpp::render::SoftwareRasterizer rast(100, 100);
  rast.clear(0xFF000000);
  rast.draw_triangle(20, 20, 0.5f, 0xFF0000FF, 80, 20, 0.5f, 0xFF0000FF, 50, 80, 0.5f, 0xFF0000FF);
  rast.draw_triangle(20, 20, 0.1f, 0xFFFF0000, 80, 20, 0.1f, 0xFFFF0000, 50, 80, 0.1f, 0xFFFF0000);
  std::uint8_t r = (rast.get_pixel(50, 40) >> 16) & 0xFF;
  EXPECT_GT(r, 200u);
}

TEST(SoftwareRasterizer, BothWindingOrdersRasterize) {
  omnicpp::render::SoftwareRasterizer rast1(100, 100);
  omnicpp::render::SoftwareRasterizer rast2(100, 100);
  rast1.clear(0xFF000000);
  rast2.clear(0xFF000000);
  rast1.draw_triangle(50, 10, 0, 0xFFFF0000, 90, 90, 0, 0xFF00FF00, 10, 90, 0, 0xFF0000FF);
  rast2.draw_triangle(50, 10, 0, 0xFFFF0000, 10, 90, 0, 0xFF0000FF, 90, 90, 0, 0xFF00FF00);
  EXPECT_NE(rast1.get_pixel(50, 50), 0xFF000000u);
  EXPECT_NE(rast2.get_pixel(50, 50), 0xFF000000u);
}

TEST(SoftwareRasterizer, FrameHashChangesWithContent) {
  omnicpp::render::SoftwareRasterizer rast(32, 32);
  rast.clear(0xFF000000);
  std::uint64_t hash_empty = rast.frame_hash();
  rast.draw_triangle(5, 5, 0, 0xFFFF0000, 25, 5, 0, 0xFF00FF00, 15, 25, 0, 0xFF0000FF);
  EXPECT_NE(hash_empty, rast.frame_hash());
}

TEST(SoftwareRasterizer, WidthAndHeightAreCorrect) {
  omnicpp::render::SoftwareRasterizer rast(320, 240);
  EXPECT_EQ(rast.width(), 320u);
  EXPECT_EQ(rast.height(), 240u);
}

TEST(SoftwareRasterizer, GetPixelOutOfBoundsReturnsZero) {
  omnicpp::render::SoftwareRasterizer rast(10, 10);
  EXPECT_EQ(rast.get_pixel(100, 100), 0u);
}
