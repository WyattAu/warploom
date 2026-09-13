//! @file test_ui_gpu.cpp
//! @brief M4 GPU end-to-end proof: a widget tree is laid out and painted to
//!        a PaintList, then rendered through the VulkanUiRenderer onto an
//!        offscreen target and read back. Pixel assertions:
//!          1. the root panel fill covers the whole target,
//!          2. the sidebar rect draws over the root with its exact color,
//!          3. a glyph quad lights pixels inside the text box only,
//!          4. two runs produce identical frame hashes (determinism).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_ui_renderer.hpp"
#include "engine/ui/widget.hpp"
#include "vulkan_test_readback.hpp"

#if defined(OMNICPP_HAS_VULKAN)
#include <vulkan/vulkan.h>

namespace {

namespace ui = omnicpp::ui;
using omnicpp::render::VulkanContext;
using omnicpp::render::VulkanMemoryAllocator;
using omnicpp::render::VulkanOffscreenTarget;
using omnicpp::render::VulkanUiRenderer;

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;

struct UiGpuHarness {
  VulkanContext ctx;
  VulkanMemoryAllocator alloc;
  VulkanOffscreenTarget target;
  VulkanUiRenderer ui;
  std::uint32_t qf = 0;

  bool init() {
    if (!ctx.initialize("omnicpp_ui_gpu", true).is_ok()) return false;
    if (!alloc.initialize(ctx.device(), ctx.physical_device()).is_ok()) {
      return false;
    }
    qf = static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
    if (!target.create(ctx.device(), ctx.physical_device(),
                       VK_FORMAT_B8G8R8A8_UNORM, kW, kH, &alloc).is_ok() ||
        !target.create_render_pass(ctx.device()).is_ok() ||
        !target.create_framebuffer(ctx.device()).is_ok()) {
      return false;
    }
    std::string sd = OMNICPP_TEST_SHADER_DIR;
    return ui.initialize(ctx.device(), ctx.physical_device(),
                         target.render_pass(), alloc, sd).is_ok();
  }

  omnicpp_test::ReadbackResult render(const ui::PaintList& paint) {
    auto quads = ui.upload_paint_list(paint, static_cast<float>(kW),
                                      static_cast<float>(kH));
    if (!quads.is_ok()) return {};

    VkDevice dev = ctx.device();
    auto pr = omnicpp::render::VulkanRenderer::create_command_pool(dev, qf);
    if (!pr.is_ok()) return {};
    auto cr =
        omnicpp::render::VulkanRenderer::allocate_command_buffer(dev, pr.value());
    if (!cr.is_ok()) {
      vkDestroyCommandPool(dev, pr.value(), nullptr);
      return {};
    }
    VkCommandBuffer cb = cr.value();
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(dev, &fi, nullptr, &fence);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    // One-time atlas layout barrier MUST precede the render pass.
    ui.ensure_layout(cb);
    VkClearValue clear{};
    clear.color = {{0.f, 0.f, 0.f, 1.f}};
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = target.render_pass();
    rpb.framebuffer = target.framebuffer();
    rpb.renderArea.extent = {kW, kH};
    rpb.clearValueCount = 1;
    rpb.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, static_cast<float>(kW), static_cast<float>(kH), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {kW, kH}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    ui.record(cb, kW, kH, quads.value());
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    vkResetFences(dev, 1, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, pr.value(), nullptr);

    // Offscreen target leaves the image in TRANSFER_SRC for readback.
    return omnicpp_test::readback_swapchain_image(
        ctx.physical_device(), dev, ctx.graphics_queue(), qf, target.image(),
        target.format(), kW, kH, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*store_pixels=*/true);
  }

  void cleanup() {
    VkDevice dev = ctx.device();
    ui.cleanup(dev);
    target.cleanup(dev);
    alloc.cleanup();
    ctx.cleanup();
  }
};

//! Demo paint: root panel fill, sidebar rect, one 'A' glyph at (20, 20).
ui::PaintList demo_paint() {
  ui::WidgetTree tree;
  ui::Widget& root = tree.get(tree.root());
  root.kind = ui::WidgetKind::Panel;
  root.color = 0xFF202020;
  ui::Widget sidebar;
  sidebar.kind = ui::WidgetKind::Panel;
  sidebar.color = 0xFF3050C0;
  sidebar.fixed_w = 100.0F;
  sidebar.flex_grow = 1.0F;
  tree.add(std::move(sidebar), tree.root());
  ui::compute_layout(tree, static_cast<float>(kW), static_cast<float>(kH));
  ui::PaintList p;
  ui::paint(tree, p);
  p.texts.push_back(ui::PaintText{20.0F, 20.0F, "A", 0xFFFFFFFFu});
  return p;
}

//! BGRA decode matching the readback helper (B lowest byte... see helper).
std::uint32_t pixel_at(const omnicpp_test::ReadbackResult& r,
                       std::uint32_t x, std::uint32_t y) {
  return r.pixels[static_cast<std::size_t>(y) * kW + x];
}

TEST(UiGpu, PanelSidebarAndGlyphPixels) {
  UiGpuHarness h;
  if (!h.init()) {
    GTEST_SKIP() << "no Vulkan device";
  }
  const auto rb = h.render(demo_paint());
  ASSERT_TRUE(rb.submitted);
  ASSERT_EQ(rb.pixels.size(), static_cast<std::size_t>(kW) * kH);

  // 1. Root panel fill on the right side (past the 100-px sidebar).
  //    Decode: helper writes (r | g<<8 | b<<16 | a<<24).
  const auto decode = [](std::uint32_t p) {
    return std::array<std::uint32_t, 4>{p & 0xFFu, (p >> 8) & 0xFFu,
                                        (p >> 16) & 0xFFu, (p >> 24) & 0xFFu};
  };
  const auto root_px = decode(pixel_at(rb, 200, 120));
  EXPECT_NEAR(root_px[0], 0x20, 2);  // R
  EXPECT_NEAR(root_px[1], 0x20, 2);  // G
  EXPECT_NEAR(root_px[2], 0x20, 2);  // B

  // 2. Sidebar over the root: blue-dominant fill.
  const auto side_px = decode(pixel_at(rb, 50, 120));
  EXPECT_NEAR(side_px[0], 0x30, 2);
  EXPECT_NEAR(side_px[1], 0x50, 2);
  EXPECT_NEAR(side_px[2], 0xC0, 2);
  EXPECT_GT(side_px[2], side_px[0] + 0x60);  // blue-dominant

  // 3. Glyph 'A' at (20, 20): cell is 8x8, glyph bits occupy columns 0-4,
  //    rows 0-6. Apex (col 2, row 0) must be lit (white); the corner of the
  //    cell must stay sidebar fill.
  const auto apex = decode(pixel_at(rb, 22, 20));
  EXPECT_GT(apex[0], 200);
  EXPECT_GT(apex[1], 200);
  EXPECT_GT(apex[2], 200);
  const auto corner = decode(pixel_at(rb, 27, 20));  // past glyph col 4
  EXPECT_NEAR(corner[0], 0x30, 2);                   // sidebar shows through

  h.cleanup();
}

TEST(UiGpu, DeterministicAcrossRuns) {
  UiGpuHarness h;
  if (!h.init()) {
    GTEST_SKIP() << "no Vulkan device";
  }
  const auto a = h.render(demo_paint());
  const auto b = h.render(demo_paint());
  ASSERT_TRUE(a.submitted);
  ASSERT_TRUE(b.submitted);
  EXPECT_EQ(a.hash, b.hash);
  ASSERT_EQ(a.pixels.size(), b.pixels.size());
  EXPECT_EQ(a.pixels, b.pixels);
  h.cleanup();
}

}  // namespace
#endif  // OMNICPP_HAS_VULKAN
