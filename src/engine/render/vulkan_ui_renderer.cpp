//! @file vulkan_ui_renderer.cpp
//! @brief Vulkan UI renderer bodies (see the header). SPIR-V modules are
//!        loaded directly from the shader directory; the graphics pipeline
//!        is built explicitly because the shared VulkanPipeline helper
//!        hardcodes an empty vertex-input state (triangle path).

#include "engine/render/vulkan_ui_renderer.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

#include "engine/ui/glyphs.hpp"

namespace omnicpp::render {

#ifdef OMNICPP_HAS_VULKAN

namespace {

//! Unpacks 0xAARRGGBB into vertex-packed 0xAABBGGRR (the shader expands).
std::uint32_t pack_vertex_color(std::uint32_t argb) {
  const std::uint32_t a = (argb >> 24) & 0xFFU;
  const std::uint32_t r = (argb >> 16) & 0xFFU;
  const std::uint32_t g = (argb >> 8) & 0xFFU;
  const std::uint32_t b = argb & 0xFFU;
  return (a << 24) | (b << 16) | (g << 8) | r;
}

//! Solid-white atlas cell centre (the reserved kSolidCell).
constexpr float kSolidU =
    (static_cast<float>(ui::kSolidCell % ui::kGlyphColumns) + 0.5F) *
    static_cast<float>(ui::kGlyphCellW) / static_cast<float>(ui::kAtlasW);
constexpr float kSolidV =
    (static_cast<float>(ui::kSolidCell / ui::kGlyphColumns) + 0.5F) *
    static_cast<float>(ui::kGlyphCellH) / static_cast<float>(ui::kAtlasH);

//! Glyph cell UV rect for ASCII `c` (cell grid positions).
void glyph_uv(char c, float& u0, float& v0, float& u1, float& v1) {
  const auto uc = static_cast<std::uint8_t>(c);
  const std::uint32_t cell = uc - 0x20U;
  const std::uint32_t col = cell % ui::kGlyphColumns;
  const std::uint32_t row = cell / ui::kGlyphColumns;
  u0 = static_cast<float>(col * ui::kGlyphCellW) / static_cast<float>(ui::kAtlasW);
  v0 = static_cast<float>(row * ui::kGlyphCellH) / static_cast<float>(ui::kAtlasH);
  u1 = static_cast<float>((col + 1) * ui::kGlyphCellW) / static_cast<float>(ui::kAtlasW);
  v1 = static_cast<float>((row + 1) * ui::kGlyphCellH) / static_cast<float>(ui::kAtlasH);
}

void push_quad(std::vector<VulkanUiRenderer::UiVertex>& out, float x, float y, float w, float h,
               float u0, float v0, float u1, float v1, std::uint32_t color) {
  const std::uint32_t c = pack_vertex_color(color);
  out.push_back({{x, y}, {u0, v0}, c});
  out.push_back({{x + w, y}, {u1, v0}, c});
  out.push_back({{x + w, y + h}, {u1, v1}, c});
  out.push_back({{x, y}, {u0, v0}, c});
  out.push_back({{x + w, y + h}, {u1, v1}, c});
  out.push_back({{x, y + h}, {u0, v1}, c});
}

[[nodiscard]] std::vector<std::uint8_t> read_file_bytes(
    const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

}  // namespace

VulkanUiRenderer::~VulkanUiRenderer() {
  // Device-scoped resources are released in cleanup(); this destructor only
  // guards against missed cleanup with no device handle available.
}

VkShaderModule VulkanUiRenderer::load_module(VkDevice device,
                                             const std::string& path) {
  const auto bytes = read_file_bytes(path);
  if (bytes.empty() || bytes.size() % 4 != 0) {
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = bytes.size();
  info.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return module;
}

omnicpp::core::Result<void> VulkanUiRenderer::initialize(
    VkDevice device, VkPhysicalDevice physical_device,
    VkRenderPass render_pass, VulkanMemoryAllocator& allocator,
    const std::string& shader_dir) {
  device_ = device;
  allocator_ = &allocator;

  // --- Glyph atlas -------------------------------------------------------
  const auto atlas_pixels = ui::build_atlas(0xFFFFFFFFu);
  atlas_w_ = ui::kAtlasW;
  atlas_h_ = ui::kAtlasH;

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {atlas_w_, atlas_h_, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_LINEAR;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  if (vkCreateImage(device, &image_info, nullptr, &atlas_image_) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device, atlas_image_, &req);
  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = req.size;
  VkPhysicalDeviceMemoryProperties mem_props{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
  bool found = false;
  for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1U << i)) != 0U &&
        (mem_props.memoryTypes[i].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) != 0U) {
      alloc_info.memoryTypeIndex = i;
      found = true;
      break;
    }
  }
  if (!found) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  if (vkAllocateMemory(device, &alloc_info, nullptr, &atlas_memory_) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  if (vkBindImageMemory(device, atlas_image_, atlas_memory_, 0) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  // Host-write the atlas (LINEAR + host-visible: valid for small images on
  // desktop implementations; the engine's swapchain path uses optimal +
  // staging for scene textures).
  void* data = nullptr;
  if (vkMapMemory(device, atlas_memory_, 0, req.size, 0, &data) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkImageSubresource sub{};
  sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  VkSubresourceLayout layout{};
  vkGetImageSubresourceLayout(device, atlas_image_, &sub, &layout);
  const auto* src = atlas_pixels.data();
  for (std::uint32_t row = 0; row < atlas_h_; ++row) {
    std::memcpy(static_cast<std::uint8_t*>(data) + layout.offset +
                    static_cast<VkDeviceSize>(row) * layout.rowPitch,
                src + static_cast<std::size_t>(row) * atlas_w_,
                static_cast<std::size_t>(atlas_w_) * sizeof(std::uint32_t));
  }
  vkUnmapMemory(device, atlas_memory_);

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = atlas_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &view_info, nullptr, &atlas_view_) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  if (vkCreateSampler(device, &sampler_info, nullptr, &sampler_) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }

  // --- Descriptor set -----------------------------------------------------
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info{};
  set_layout_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                  &set_layout_) != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_size.descriptorCount = 1;
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkDescriptorSetAllocateInfo set_alloc{};
  set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_alloc.descriptorPool = pool_;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &set_layout_;
  if (vkAllocateDescriptorSets(device, &set_alloc, &atlas_set_) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkDescriptorImageInfo image_desc{};
  image_desc.sampler = sampler_;
  image_desc.imageView = atlas_view_;
  // Layout transition: the image is never used in a render pass before the
  // descriptor write, so a pipeline barrier on first record() moves it from
  // UNDEFINED to SHADER_READ_ONLY (recorded once via first_record_).
  image_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = atlas_set_;
  write.dstBinding = 0;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.descriptorCount = 1;
  write.pImageInfo = &image_desc;
  vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

  // --- Shaders + pipeline --------------------------------------------------
  vert_module_ = load_module(device, shader_dir + "/ui_quad.vert.spv");
  frag_module_ = load_module(device, shader_dir + "/ui_quad.frag.spv");
  if (vert_module_ == VK_NULL_HANDLE || frag_module_ == VK_NULL_HANDLE) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }

  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push.offset = 0;
  push.size = sizeof(float) * 2;
  VkPipelineLayoutCreateInfo pl_info{};
  pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl_info.setLayoutCount = 1;
  pl_info.pSetLayouts = &set_layout_;
  pl_info.pushConstantRangeCount = 1;
  pl_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device, &pl_info, nullptr, &layout_) !=
      VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }

  VkVertexInputBindingDescription bind{};
  bind.binding = 0;
  bind.stride = sizeof(UiVertex);
  bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 3> attrs{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, pos)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, uv)};
  attrs[2] = {2, 0, VK_FORMAT_R32_UINT, offsetof(UiVertex, color)};
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &bind;
  vertex_input.vertexAttributeDescriptionCount = 3;
  vertex_input.pVertexAttributeDescriptions = attrs.data();

  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                 VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dyn_states;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;  // quad winding is CPU-authored
  raster.lineWidth = 1.0F;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth{};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = VK_FALSE;
  depth.depthWriteEnable = VK_FALSE;

  // Alpha blending: glyph quads carry empty texels (alpha 0) which must
  // not punch black holes over previously drawn UI.
  VkPipelineColorBlendAttachmentState blend_att{};
  blend_att.blendEnable = VK_TRUE;
  blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend_att.colorBlendOp = VK_BLEND_OP_ADD;
  blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
  blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                             VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_att;

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_module_;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_module_;
  stages[1].pName = "main";

  VkGraphicsPipelineCreateInfo pipe{};
  pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipe.stageCount = 2;
  pipe.pStages = stages;
  pipe.pVertexInputState = &vertex_input;
  pipe.pInputAssemblyState = &assembly;
  pipe.pViewportState = &viewport;
  pipe.pRasterizationState = &raster;
  pipe.pMultisampleState = &multisample;
  pipe.pDepthStencilState = &depth;
  pipe.pColorBlendState = &blend;
  pipe.pDynamicState = &dynamic;
  pipe.layout = layout_;
  pipe.renderPass = render_pass;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr,
                                &pipeline_) != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }

  // --- Quad vertex buffer (grow-on-demand, host-visible) -------------------
  const VkDeviceSize initial_bytes =
      64ULL * 6ULL * sizeof(UiVertex);  // 64 quads initially
  auto buffer_result = allocator.create_buffer(
      initial_bytes,
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!buffer_result.is_ok()) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  quad_allocation_ = std::move(buffer_result.value());
  return omnicpp::core::Result<void>::ok();
}

omnicpp::core::Result<std::uint32_t> VulkanUiRenderer::upload_paint_list(
    const omnicpp::ui::PaintList& paint, float viewport_w, float viewport_h) {
  (void)viewport_w;
  (void)viewport_h;
  vertices_.clear();

  // Rects: solid fills pointing at the reserved solid-white cell.
  for (const auto& rect : paint.rects) {
    push_quad(vertices_, rect.x, rect.y, rect.w, rect.h, kSolidU, kSolidV,
              kSolidU, kSolidV, rect.color);
  }
  // Text: one quad per glyph cell, UVs selecting that glyph.
  for (const auto& t : paint.texts) {
    float cx = t.x;
    for (const char ch : t.text) {
      if (!ui::has_glyph(ch)) {
        cx += ui::font_metrics().char_width;
        continue;
      }
      float u0 = 0.0F;
      float v0 = 0.0F;
      float u1 = 0.0F;
      float v1 = 0.0F;
      glyph_uv(ch, u0, v0, u1, v1);
      push_quad(vertices_, cx, t.y, ui::kGlyphCellW, ui::kGlyphCellH, u0, v0,
                u1, v1, t.color);
      cx += ui::font_metrics().char_width;
    }
  }

  const auto bytes =
      static_cast<VkDeviceSize>(vertices_.size()) * sizeof(UiVertex);
  if (bytes == 0) {
    return omnicpp::core::Result<std::uint32_t>::ok(0U);
  }
  if (quad_allocation_.mapped == nullptr) {
    return omnicpp::core::Result<std::uint32_t>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  std::memcpy(quad_allocation_.mapped, vertices_.data(),
              static_cast<std::size_t>(bytes));
  return omnicpp::core::Result<std::uint32_t>::ok(
      static_cast<std::uint32_t>(vertices_.size() / 6U));
}

void VulkanUiRenderer::ensure_layout(VkCommandBuffer cmd) {
  if (cmd == VK_NULL_HANDLE || atlas_layout_ready_ ||
      atlas_image_ == VK_NULL_HANDLE) {
    return;
  }
  // One-time barrier: host-written atlas -> shader-readable. The image is
  // created without TRANSFER usage, so this is a pure layout transition.
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = atlas_image_;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;  // make host writes visible
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                       0, nullptr, 1, &barrier);
  atlas_layout_ready_ = true;
}

void VulkanUiRenderer::record(VkCommandBuffer cmd, std::uint32_t width,
                              std::uint32_t height,
                              std::uint32_t quad_count) {
  if (cmd == VK_NULL_HANDLE || quad_count == 0 ||
      pipeline_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  const float res[2] = {static_cast<float>(width),
                        static_cast<float>(height)};
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                     sizeof(res), res);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                          1, &atlas_set_, 0, nullptr);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &quad_allocation_.buffer, &offset);
  vkCmdDraw(cmd, quad_count * 6U, 1, 0, 0);
}

void VulkanUiRenderer::cleanup(VkDevice device) noexcept {
  if (device == VK_NULL_HANDLE) {
    return;
  }
  if (allocator_ != nullptr && quad_allocation_.buffer != VK_NULL_HANDLE) {
    allocator_->destroy_allocation(quad_allocation_);
  }
  if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device, sampler_, nullptr);
  if (atlas_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device, atlas_view_, nullptr);
  }
  if (atlas_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device, atlas_image_, nullptr);
  }
  if (atlas_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device, atlas_memory_, nullptr);
  }
  if (pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device, pool_, nullptr);
  }
  if (set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
  }
  if (pipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device, pipeline_, nullptr);
  }
  if (layout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device, layout_, nullptr);
  }
  if (vert_module_ != VK_NULL_HANDLE) {
    vkDestroyShaderModule(device, vert_module_, nullptr);
  }
  if (frag_module_ != VK_NULL_HANDLE) {
    vkDestroyShaderModule(device, frag_module_, nullptr);
  }
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
  atlas_view_ = VK_NULL_HANDLE;
  atlas_image_ = VK_NULL_HANDLE;
  atlas_memory_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  vert_module_ = VK_NULL_HANDLE;
  frag_module_ = VK_NULL_HANDLE;
  allocator_ = nullptr;
  device_ = VK_NULL_HANDLE;
}

#endif  // OMNICPP_HAS_VULKAN

}  // namespace omnicpp::render
