#pragma once

//! @file vulkan_test_readback.hpp
//! @brief Shared image-readback helpers for the Vulkan hardware tests.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

#if defined(WARPLOOM_HAS_VULKAN) || defined(OMNICPP_HAS_VULKAN)
#include <vulkan/vulkan.h>

namespace omnicpp_test {

inline bool find_host_memory_type(VkPhysicalDevice physical_device, std::uint32_t type_bits,
                           VkMemoryPropertyFlags required, std::uint32_t& index) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    if ((type_bits & (1U << i)) != 0U &&
        (properties.memoryTypes[i].propertyFlags & required) == required) {
      index = i;
      return true;
    }
  }
  return false;
}


struct ReadbackResult {
  bool submitted{false};
  //! Decoded pixels (r | g<<8 | b<<16 | a<<24), row-major, top-left origin
  //! semantics matching pixel_at. Populated only when store_pixels=true.
  std::vector<std::uint32_t> pixels;
  std::size_t non_clear_pixels{0};
  std::size_t red_dominant_pixels{0};
  std::size_t green_dominant_pixels{0};
  std::size_t blue_dominant_pixels{0};
  //! Pixels whose decoded RGB channels are all >= 200 (near-white). Used by
  //! the PBR tests to detect a bright specular highlight.
  std::size_t bright_pixels{0};
  //! Largest decoded r+g+b across the image (0..765).
  std::uint32_t peak_luma{0};
  std::uint32_t center_pixel{0};
  std::uint32_t upper_triangle_pixel{0};
  std::uint32_t lower_triangle_pixel{0};
  std::uint32_t corner_pixel{0};
  std::uint64_t hash{0};
  std::uint64_t canonical_hash{0};
};

// Golden fingerprint for the bundled triangle's coarse spatial content. Unlike
// the raw byte hash, this is independent of BGRA/RGBA and sRGB choice — but it
// is still driver-specific (rasterization sampling differs between NVIDIA and
// Mesa lavapipe), so every known-good value is accepted.
inline void expect_canonical_triangle_hash(std::uint64_t canonical_hash) {
  switch (canonical_hash) {
    case 9189736358881991061ULL:  // NVIDIA (RTX 2060)
    case 2348590267748365716ULL:  // Mesa lavapipe (CI)
      SUCCEED();
      break;
    default:
      ADD_FAILURE() << "unexpected canonical_hash=" << canonical_hash
                    << " (add the new driver's known-good value if valid)";
      break;
  }
}

inline ReadbackResult readback_swapchain_image(VkPhysicalDevice physical_device, VkDevice device,
                                        VkQueue queue, std::uint32_t queue_family,
                                        VkImage image, VkFormat image_format,
                                        std::uint32_t width, std::uint32_t height,
                                        VkImageLayout initial_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                        VkImageLayout final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                        bool store_pixels = false) {
  ReadbackResult output;
  // Texel size depends on the format: the 4-byte path handles BGRA8/RGBA8;
  // RGBA16F (HDR bloom/scene targets) packs 8 bytes per pixel and is
  // converted to 8-bit BGRA on readback so callers see the same stats.
  const bool is_rgba16f = image_format == VK_FORMAT_R16G16B16A16_SFLOAT;
  const std::uint32_t bytes_per_texel = is_rgba16f ? 8U : 4U;
  const VkDeviceSize byte_size =
      static_cast<VkDeviceSize>(width) * height * bytes_per_texel;

  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = byte_size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer buffer = VK_NULL_HANDLE;
  if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) return output;

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, buffer, &requirements);
  std::uint32_t memory_type = 0;
  if (!find_host_memory_type(physical_device, requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             memory_type)) {
    vkDestroyBuffer(device, buffer, nullptr);
    return output;
  }

  VkMemoryAllocateInfo allocation{};
  allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS) {
    vkDestroyBuffer(device, buffer, nullptr);
    return output;
  }
  if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
    vkFreeMemory(device, memory, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    return output;
  }

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = queue_family;
  VkCommandPool pool = VK_NULL_HANDLE;
  if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    vkFreeMemory(device, memory, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    return output;
  }

  VkCommandBufferAllocateInfo command_allocation{};
  command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_allocation.commandPool = pool;
  command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_allocation.commandBufferCount = 1;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device, &command_allocation, &command_buffer) != VK_SUCCESS) {
    vkDestroyCommandPool(device, pool, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    return output;
  }

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bool valid = vkBeginCommandBuffer(command_buffer, &begin) == VK_SUCCESS;

  if (valid) {
    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_transfer.oldLayout = initial_layout;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    if (initial_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &to_transfer);
    }

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer, 1, &copy);

    if (final_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      VkImageMemoryBarrier restore_layout = to_transfer;
      restore_layout.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      restore_layout.dstAccessMask = 0;
      restore_layout.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      restore_layout.newLayout = final_layout;
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &restore_layout);
    }
    valid = vkEndCommandBuffer(command_buffer) == VK_SUCCESS;
  }

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  if (valid && vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer;
    valid = vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS &&
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
  }

  if (valid) {
    void* mapped = nullptr;
    valid = vkMapMemory(device, memory, 0, byte_size, 0, &mapped) == VK_SUCCESS;
    if (valid) {
      const auto* bytes = static_cast<const std::uint8_t*>(mapped);
      const bool rgba = image_format == VK_FORMAT_R8G8B8A8_UNORM ||
                        image_format == VK_FORMAT_R8G8B8A8_SRGB;
      // RGBA16F half-float decode (F16 -> 0..255 8-bit, clamped).
      const auto f16_to_u8 = [](std::uint16_t h) -> std::uint32_t {
        const std::uint32_t sign = (h >> 15U) & 1U;
        const std::uint32_t exp = (h >> 10U) & 0x1FU;
        const std::uint32_t frac = h & 0x3FFU;
        if (exp == 0U) return 0U;                       // zero/subnormal -> 0
        if (exp == 31U) return 255U;                    // inf/nan -> clamp
        // value = 1.frac * 2^(exp-15); scale via bit construction.
        const std::uint32_t f32_exp = exp - 15U + 127U;
        const std::uint32_t bits = (f32_exp << 23U) | (frac << 13U);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        const float scaled = f * 255.0F;
        const auto v = static_cast<std::uint32_t>(scaled < 0.0F ? 0.0F
                                                : (scaled > 255.0F ? 255.0F
                                                                   : scaled));
        return sign != 0U ? 0U : v;
      };
      const auto texel_at = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t offset =
            (static_cast<std::size_t>(y) * width + x) * bytes_per_texel;
        if (is_rgba16f) {
          std::uint16_t c[4];
          std::memcpy(c, bytes + offset, 8U);
          return f16_to_u8(c[2]) | (f16_to_u8(c[1]) << 8U) |
                 (f16_to_u8(c[0]) << 16U) | (f16_to_u8(c[3]) << 24U);
        }
        const auto first = bytes[offset];
        const auto second = bytes[offset + 1];
        const auto third = bytes[offset + 2];
        const auto alpha = bytes[offset + 3];
        const auto r = rgba ? first : third;
        const auto g = second;
        const auto b = rgba ? third : first;
        return static_cast<std::uint32_t>(r) |
               (static_cast<std::uint32_t>(g) << 8U) |
               (static_cast<std::uint32_t>(b) << 16U) |
               (static_cast<std::uint32_t>(alpha) << 24U);
      };
      const auto pixel_at = [&](std::uint32_t x, std::uint32_t y) {
        return texel_at(x, y);
      };

      std::uint64_t hash = 1469598103934665603ULL;
      for (std::uint32_t py = 0; py < height; ++py) {
        for (std::uint32_t px2 = 0; px2 < width; ++px2) {
          const std::uint32_t pixel = texel_at(px2, py);
          const auto r = static_cast<std::uint32_t>(pixel & 0xFFU);
          const auto g = static_cast<std::uint32_t>((pixel >> 8U) & 0xFFU);
          const auto b = static_cast<std::uint32_t>((pixel >> 16U) & 0xFFU);
          const auto a = static_cast<std::uint32_t>((pixel >> 24U) & 0xFFU);
          if (r != 0 || g != 0 || b != 0) ++output.non_clear_pixels;
          if (r > g && r > b) ++output.red_dominant_pixels;
          if (g > r && g > b) ++output.green_dominant_pixels;
          if (b > r && b > g) ++output.blue_dominant_pixels;
          if (r >= 200U && g >= 200U && b >= 200U) ++output.bright_pixels;
          const std::uint32_t luma = r + g + b;
          if (luma > output.peak_luma) output.peak_luma = luma;
          hash ^= pixel;
          hash *= 1099511628211ULL;
          (void)a;
        }
      }
      output.center_pixel = pixel_at(width / 2U, height / 2U);
      if (store_pixels) {
        output.pixels.resize(static_cast<std::size_t>(width) * height);
        for (std::uint32_t py = 0; py < height; ++py) {
          for (std::uint32_t px2 = 0; px2 < width; ++px2) {
            output.pixels[static_cast<std::size_t>(py) * width + px2] =
                pixel_at(px2, py);
          }
        }
      }
      // Both samples lie on the triangle's vertical centerline, away from its
      // edges. Vulkan's framebuffer row orientation does not affect this test.
      output.upper_triangle_pixel = pixel_at(width / 2U, height / 4U);
      output.lower_triangle_pixel = pixel_at(width / 2U, (height * 13U) / 16U);
      output.corner_pixel = pixel_at(0, 0);
      output.hash = hash;

      // Canonicalize a coarse spatial classification rather than raw bytes.
      // This remains stable across BGRA/RGBA and UNORM/SRGB swapchain formats.
      constexpr std::uint32_t grid_width = 16;
      constexpr std::uint32_t grid_height = 12;
      std::uint64_t canonical_hash = 1469598103934665603ULL;
      for (std::uint32_t gy = 0; gy < grid_height; ++gy) {
        for (std::uint32_t gx = 0; gx < grid_width; ++gx) {
          const auto pixel = pixel_at(
              ((2U * gx + 1U) * width) / (2U * grid_width),
              ((2U * gy + 1U) * height) / (2U * grid_height));
          const auto r = pixel & 0xFFU;
          const auto g = (pixel >> 8U) & 0xFFU;
          const auto b = (pixel >> 16U) & 0xFFU;
          const std::uint8_t category =
              (r == 0U && g == 0U && b == 0U) ? 0U :
              (r > g && r > b) ? 1U :
              (g > r && g > b) ? 2U :
              (b > r && b > g) ? 3U : 4U;
          canonical_hash ^= category;
          canonical_hash *= 1099511628211ULL;
        }
      }
      output.canonical_hash = canonical_hash;
      output.submitted = true;
      vkUnmapMemory(device, memory);
    }
  }

  if (fence) vkDestroyFence(device, fence, nullptr);
  vkDestroyCommandPool(device, pool, nullptr);
  vkFreeMemory(device, memory, nullptr);
  vkDestroyBuffer(device, buffer, nullptr);
  return output;
}



}  // namespace omnicpp_test
#endif
