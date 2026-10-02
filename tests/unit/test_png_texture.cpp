//! @file test_png_texture.cpp
//! @brief End-to-end image pipeline test on the GPU: a PNG built in memory is
//!        decoded by the engine's PNG decoder, uploaded through the
//!        VulkanFrameUploadArena image-copy path, sampled by a UV-mapped
//!        textured quad, and read back. Every 8x8 source texel is verified at
//!        its 1:1 output location (nearest sampling), proving decode ->
//!        staging -> image layout transitions -> view/sampler -> fragment
//!        sampling end to end under Khronos validation.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/asset/png_decoder.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_frame_upload.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "vulkan_test_readback.hpp"

namespace {

// ============================================================================
// Minimal stored-block PNG builder for the fixture (CPU side)
// ============================================================================
#if defined(WARPLOOM_HAS_VULKAN)

std::uint32_t fixture_crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0xedb88320U : crc >> 1;
    }
  }
  return crc ^ 0xffffffffU;
}

void fixture_chunk(std::vector<std::uint8_t>& out, const char* type,
                   const std::vector<std::uint8_t>& data) {
  const std::uint32_t length = static_cast<std::uint32_t>(data.size());
  out.push_back(static_cast<std::uint8_t>(length >> 24));
  out.push_back(static_cast<std::uint8_t>(length >> 16));
  out.push_back(static_cast<std::uint8_t>(length >> 8));
  out.push_back(static_cast<std::uint8_t>(length));
  const std::size_t type_offset = out.size();
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(type[i]));
  out.insert(out.end(), data.begin(), data.end());
  const std::uint32_t crc =
      fixture_crc32(out.data() + type_offset, out.size() - type_offset);
  out.push_back(static_cast<std::uint8_t>(crc >> 24));
  out.push_back(static_cast<std::uint8_t>(crc >> 16));
  out.push_back(static_cast<std::uint8_t>(crc >> 8));
  out.push_back(static_cast<std::uint8_t>(crc));
}

//! Build a colour-type-6 PNG whose rows are filter-0 scanlines stored in
//! uncompressed DEFLATE blocks.
std::vector<std::uint8_t> build_rgba8_png(std::uint32_t width,
                                          std::uint32_t height,
                                          const std::vector<std::uint8_t>& rgba) {
  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<std::uint8_t> ihdr_data = {
      static_cast<std::uint8_t>(width >> 24), static_cast<std::uint8_t>(width >> 16),
      static_cast<std::uint8_t>(width >> 8), static_cast<std::uint8_t>(width),
      static_cast<std::uint8_t>(height >> 24), static_cast<std::uint8_t>(height >> 16),
      static_cast<std::uint8_t>(height >> 8), static_cast<std::uint8_t>(height),
      8,   // bit depth
      6,   // colour type: truecolour + alpha
      0,   // compression
      0,   // filter
      0};  // interlace
  fixture_chunk(png, "IHDR", ihdr_data);
  // Filter-0 rows, stored DEFLATE blocks.
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(height) * (1U + width * 4U));
  for (std::uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y * width * 4U),
               rgba.begin() + static_cast<std::ptrdiff_t>((y + 1U) * width * 4U));
  }
  std::vector<std::uint8_t> idat = {0x78, 0x01};
  std::size_t offset = 0;
  do {
    const std::size_t remaining = raw.size() - offset;
    const std::size_t block = remaining < 65535U ? remaining : 65535U;
    const bool last = block == remaining;
    idat.push_back(last ? 0x01U : 0x00U);
    idat.push_back(static_cast<std::uint8_t>(block & 0xff));
    idat.push_back(static_cast<std::uint8_t>((block >> 8) & 0xff));
    const std::uint16_t complement = static_cast<std::uint16_t>(~block);
    idat.push_back(static_cast<std::uint8_t>(complement & 0xff));
    idat.push_back(static_cast<std::uint8_t>((complement >> 8) & 0xff));
    idat.insert(idat.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                raw.begin() + static_cast<std::ptrdiff_t>(offset + block));
    offset += block;
  } while (offset < raw.size());
  fixture_chunk(png, "IDAT", idat);
  fixture_chunk(png, "IEND", {});
  return png;
}

//! Copy an image (expected layout TRANSFER_SRC_OPTIMAL, as produced by the
//! offscreen render pass) to a host-visible buffer and return its raw bytes.
std::vector<std::uint8_t> read_image_bytes(
    VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
    std::uint32_t queue_family, VkImage image, std::uint32_t width,
    std::uint32_t height) {
  std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height * 4U);
  const VkDeviceSize byte_size = result.size();

  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = byte_size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
    return {};
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, buffer, &requirements);
  std::uint32_t memory_type = 0;
  if (!omnicpp_test::find_host_memory_type(
          physical_device, requirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          memory_type)) {
    vkDestroyBuffer(device, buffer, nullptr);
    return {};
  }
  VkMemoryAllocateInfo allocation{};
  allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
      vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
    vkDestroyBuffer(device, buffer, nullptr);
    return {};
  }

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = queue_family;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS) {
    VkCommandBufferAllocateInfo command_allocation{};
    command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_allocation.commandPool = pool;
    command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocation.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &command_allocation, &command_buffer);
  }
  if (command_buffer != VK_NULL_HANDLE) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_buffer, &begin) == VK_SUCCESS) {
      // Same-layout barrier: make the render pass's color writes visible to
      // the transfer read (memory dependency across COLOR_ATTACHMENT_OUTPUT
      // -> TRANSFER on the same queue).
      VkImageMemoryBarrier ready{};
      ready.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      ready.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      ready.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      ready.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      ready.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.image = image;
      ready.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      ready.subresourceRange.levelCount = 1;
      ready.subresourceRange.layerCount = 1;
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &ready);
      VkBufferImageCopy copy{};
      copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent = {width, height, 1};
      vkCmdCopyImageToBuffer(command_buffer, image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                             &copy);
      vkEndCommandBuffer(command_buffer);
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      VkFence fence = VK_NULL_HANDLE;
      if (vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        if (vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS) {
          vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        }
        vkDestroyFence(device, fence, nullptr);
      }
    }
  }
  if (memory != VK_NULL_HANDLE) {
    void* mapped = nullptr;
    if (vkMapMemory(device, memory, 0, byte_size, 0, &mapped) == VK_SUCCESS) {
      std::memcpy(result.data(), mapped, byte_size);
      vkUnmapMemory(device, memory);
    }
  }
  if (command_buffer != VK_NULL_HANDLE) {
    vkDestroyCommandPool(device, pool, nullptr);
  }
  vkFreeMemory(device, memory, nullptr);
  vkDestroyBuffer(device, buffer, nullptr);
  return result;
}
#endif  // WARPLOOM_HAS_VULKAN

}  // namespace

TEST(VulkanHardware, PngDecodedTextureUploadAndSample) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  constexpr std::uint32_t kTextureSize = 8;
  constexpr std::uint32_t kTargetSize = 64;

  // --- Source RGBA pattern: distinct per-texel colour + alpha so every texel
  //     is uniquely identifiable after the round trip. ---
  std::vector<std::uint8_t> source_rgba(
      static_cast<std::size_t>(kTextureSize) * kTextureSize * 4U);
  for (std::uint32_t y = 0; y < kTextureSize; ++y) {
    for (std::uint32_t x = 0; x < kTextureSize; ++x) {
      std::uint8_t* texel = source_rgba.data() +
                            (static_cast<std::size_t>(y) * kTextureSize + x) * 4U;
      texel[0] = static_cast<std::uint8_t>((x * 61U + y * 17U) % 256U);
      texel[1] = static_cast<std::uint8_t>((x * 3U + y * 113U) % 256U);
      texel[2] = static_cast<std::uint8_t>((x * 7U + y * 5U + 77U) % 256U);
      texel[3] = static_cast<std::uint8_t>(110U + x * 5U + y * 7U);  // < 256
    }
  }
  const std::vector<std::uint8_t> png = build_rgba8_png(
      kTextureSize, kTextureSize, source_rgba);

  // --- CPU decode: the PNG decoder must reproduce the source grid. ---
  omnicpp::asset::DecodedImage decoded;
  {
    std::string error;
    auto decode_result =
        omnicpp::asset::decode_png(png.data(), png.size(), &error);
    ASSERT_TRUE(decode_result.is_ok()) << "PNG decode failed: " << error;
    decoded = std::move(decode_result).value();
  }
  ASSERT_EQ(decoded.width, kTextureSize);
  ASSERT_EQ(decoded.height, kTextureSize);
  EXPECT_EQ(decoded.rgba, source_rgba);

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppPngTextureTest", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  // --- GPU texture: image + device-local memory + view + sampler. ---
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {kTextureSize, kTextureSize, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage texture_image = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateImage(context.device(), &image_info, nullptr, &texture_image),
            VK_SUCCESS);
  auto image_memory = allocator.bind_image(texture_image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ASSERT_TRUE(image_memory.is_ok());
  omnicpp::render::Allocation texture_allocation = image_memory.value();

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = texture_image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView texture_view = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateImageView(context.device(), &view_info, nullptr, &texture_view),
            VK_SUCCESS);

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler texture_sampler = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateSampler(context.device(), &sampler_info, nullptr, &texture_sampler),
            VK_SUCCESS);

  // --- Upload through the frame arena's image-copy path. ---
  omnicpp::render::VulkanFrameUploadArena arena;
  ASSERT_TRUE(arena.initialize(
      context.device(), context.physical_device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      /*frame_count=*/2, /*bytes_per_frame=*/1U << 20U).is_ok());
  ASSERT_TRUE(arena.begin_frame(0).is_ok());
  auto span_result = arena.acquire(source_rgba.size());
  ASSERT_TRUE(span_result.is_ok());
  std::memcpy(span_result.value().host_data, source_rgba.data(), source_rgba.size());
  arena.record_copy_image_rgba8(span_result.value(), texture_image, kTextureSize,
                                kTextureSize);
  ASSERT_TRUE(arena.submit(context.graphics_queue()).is_ok());
  arena.wait_idle();

  // --- Bind the texture into a descriptor set (non-bindless, count 1). ---
  std::ifstream frag_file(std::string(WARPLOOM_TEST_SHADER_DIR) +
                              "/textured_quad.frag.spv",
                          std::ios::binary);
  ASSERT_TRUE(frag_file.good());
  const std::vector<std::uint8_t> frag_spirv(
      (std::istreambuf_iterator<char>(frag_file)), std::istreambuf_iterator<char>());
  const auto bindings = omnicpp::render::reflect_spirv_resources(
      frag_spirv.data(), frag_spirv.size());
  ASSERT_EQ(bindings.size(), 1U);
  EXPECT_EQ(bindings[0].binding, 0U);
  EXPECT_EQ(bindings[0].type, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  auto layout_result = manager.create_layout(bindings, 1, /*bindless=*/false);
  ASSERT_TRUE(layout_result.is_ok());
  auto set_result = manager.allocate_set(layout_result.value());
  ASSERT_TRUE(set_result.is_ok());
  ASSERT_TRUE(manager.write_image(set_result.value(), 0,
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 texture_sampler, texture_view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok());

  // --- Offscreen target + textured-quad pipeline. ---
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kTargetSize, kTargetSize,
                            &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanPipeline pipeline;
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/textured_quad.vert.spv",
                                              "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(context.device(),
                                              shader_dir + "/textured_quad.frag.spv",
                                              "fragment").is_ok());
  const VkDescriptorSetLayout set_layout = layout_result.value();
  ASSERT_TRUE(pipeline.create_pipeline_layout(context.device(), &set_layout, 1).is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      pipeline.pipeline_layout(), false, false, false).is_ok());

  // --- Record + submit one frame. ---
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
  render_begin.renderArea.extent = {kTargetSize, kTargetSize};
  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  render_begin.clearValueCount = 1;
  render_begin.pClearValues = &clear;
  vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{};
  viewport.width = static_cast<float>(kTargetSize);
  viewport.height = static_cast<float>(kTargetSize);
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {kTargetSize, kTargetSize};
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

  // --- Read back the rendered quad and verify every texel 1:1. ---
  const std::vector<std::uint8_t> bytes = read_image_bytes(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), kTargetSize, kTargetSize);
  ASSERT_EQ(bytes.size(), static_cast<std::size_t>(kTargetSize) * kTargetSize * 4U);

  std::size_t verified = 0;
  for (std::uint32_t cy = 0; cy < kTextureSize; ++cy) {
    for (std::uint32_t cx = 0; cx < kTextureSize; ++cx) {
      // Sample the center of each 8x8 output cell. With nearest filtering and
      // the UV convention above, it must reproduce source texel (cx, cy).
      const std::uint32_t px = cx * kTextureSize + kTextureSize / 2U;
      const std::uint32_t py = cy * kTextureSize + kTextureSize / 2U;
      const std::size_t offset =
          (static_cast<std::size_t>(py) * kTargetSize + px) * 4U;
      const std::uint8_t* texel =
          source_rgba.data() + (static_cast<std::size_t>(cy) * kTextureSize + cx) * 4U;
      // Target is B8G8R8A8: buffer bytes are B,G,R,A.
      EXPECT_EQ(bytes[offset + 0U], texel[2]) << "cell " << cx << "," << cy << " B";
      EXPECT_EQ(bytes[offset + 1U], texel[1]) << "cell " << cx << "," << cy << " G";
      EXPECT_EQ(bytes[offset + 2U], texel[0]) << "cell " << cx << "," << cy << " R";
      EXPECT_EQ(bytes[offset + 3U], texel[3]) << "cell " << cx << "," << cy << " A";
      ++verified;
    }
  }
  EXPECT_EQ(verified, static_cast<std::size_t>(kTextureSize) * kTextureSize);
  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), command_pool, nullptr);
  pipeline.cleanup(context.device());
  target.cleanup(context.device());
  arena.wait_idle();
  vkDestroySampler(context.device(), texture_sampler, nullptr);
  vkDestroyImageView(context.device(), texture_view, nullptr);
  allocator.destroy_allocation(texture_allocation);
  vkDestroyImage(context.device(), texture_image, nullptr);
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
