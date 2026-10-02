/**
 * @file vulkan_pipeline.cpp
 * @brief Vulkan graphics pipeline and shader module implementation.
 */

#include "warploom/render/vulkan_pipeline.hpp"
#include <cstring>
#include <fstream>
#include <utility>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

VulkanPipeline::~VulkanPipeline() { cleanup(nullptr); }

::warploom::core::Result<void> VulkanPipeline::load_shader_file(
    VkDevice device, const std::string& path) {
#ifdef OMNICPP_HAS_VULKAN
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  auto file_size = static_cast<std::size_t>(file.tellg());
  if (file_size < 4 || file_size % 4 != 0) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  std::vector<std::uint8_t> buffer(file_size);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(file_size));
  if (!file || file.gcount() != static_cast<std::streamsize>(file_size)) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  return load_shader_from_bytes(device, buffer);
#else
  (void)device; (void)path;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanPipeline::load_shader(
    VkDevice device, const std::uint32_t* code, std::size_t code_size_bytes) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !code || code_size_bytes < 4 || code_size_bytes % 4 != 0) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  VkShaderModuleCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  create_info.codeSize = code_size_bytes;
  create_info.pCode = code;

  // We don't know if this is vertex or fragment — caller uses load_shader_from_bytes
  // which stores in the appropriate slot. This method is used internally.
  if (code[0] != 0x07230203U) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  VkShaderModule module = nullptr;
  VkResult result = vkCreateShaderModule(device, &create_info, nullptr, &module);
  if (result != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  // We store the last loaded module — caller must use create_graphics_pipeline
  // to bind vertex/fragment. For simplicity, we store both here.
  // In practice, callers should load vertex then fragment.
  if (!vertex_shader_) {
    vertex_shader_ = module;
  } else {
    fragment_shader_ = module;
  }

  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)code; (void)code_size_bytes;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanPipeline::load_shader_from_bytes(
    VkDevice device, const std::vector<std::uint8_t>& spirv_bytes) {
  if (spirv_bytes.empty() || spirv_bytes.size() % 4 != 0) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  return load_shader(device,
      reinterpret_cast<const std::uint32_t*>(spirv_bytes.data()),
      spirv_bytes.size());
}

::warploom::core::Result<void> VulkanPipeline::load_shader_stage_file(
    VkDevice device, const std::string& path, const std::string& stage) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  VkShaderStageFlagBits stage_flag;
  if (stage == "vertex") {
    stage_flag = VK_SHADER_STAGE_VERTEX_BIT;
  } else if (stage == "fragment") {
    stage_flag = VK_SHADER_STAGE_FRAGMENT_BIT;
  } else if (stage == "compute") {
    stage_flag = VK_SHADER_STAGE_COMPUTE_BIT;
  } else {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  if (has_stage(stage)) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }

  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  const auto file_size = static_cast<std::size_t>(file.tellg());
  if (file_size < 4 || file_size % 4 != 0) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  std::vector<std::uint8_t> buffer(file_size);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(file_size));
  if (!file || file.gcount() != static_cast<std::streamsize>(file_size)) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  const auto* code = reinterpret_cast<const std::uint32_t*>(buffer.data());
  if (code[0] != 0x07230203U) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  VkShaderModuleCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  create_info.codeSize = file_size;
  create_info.pCode = code;
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &create_info, nullptr, &module) != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  switch (stage_flag) {
    case VK_SHADER_STAGE_VERTEX_BIT:
      vertex_shader_ = module;
      break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
      fragment_shader_ = module;
      break;
    case VK_SHADER_STAGE_COMPUTE_BIT:
      compute_shader_ = module;
      break;
    default:
      vkDestroyShaderModule(device, module, nullptr);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)path; (void)stage;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

bool VulkanPipeline::has_stage(const std::string& stage) const noexcept {
  if (stage == "vertex") return vertex_shader_ != VK_NULL_HANDLE;
  if (stage == "fragment") return fragment_shader_ != VK_NULL_HANDLE;
  if (stage == "compute") return compute_shader_ != VK_NULL_HANDLE;
  return false;
}

::warploom::core::Result<void> VulkanPipeline::create_compute_pipeline(
    VkDevice device, VkPipelineLayout pipeline_layout) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !compute_shader_) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  if (!pipeline_layout) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  if (layout_ != pipeline_layout) {
    if (owns_layout_ && layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device, layout_, nullptr);
      owns_layout_ = false;
    }
    layout_ = pipeline_layout;
  }

  VkPipelineShaderStageCreateInfo stage_info{};
  stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage_info.module = compute_shader_;
  stage_info.pName = "main";

  VkComputePipelineCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  create_info.stage = stage_info;
  create_info.layout = layout_;

  const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create_info, nullptr, &pipeline_);
  if (result != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)pipeline_layout;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanPipeline::create_graphics_pipeline(
    VkDevice device, VkRenderPass render_pass,
    VkFormat vertex_format,
    VkPipelineLayout pipeline_layout,
    bool enable_depth_test,
    bool enable_depth_write,
    bool enable_backface_cull,
    float depth_bias_slope) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !render_pass || !vertex_shader_ || !fragment_shader_) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  // If no layout was provided, create one
  if (!pipeline_layout) {
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 0;
    layout_info.pushConstantRangeCount = 0;

    VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_);
    if (result != VK_SUCCESS) {
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
    owns_layout_ = true;
  } else {
    if (layout_ != pipeline_layout) {
      // Replace a previously owned layout rather than leaking it.
      if (owns_layout_ && layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
      layout_ = pipeline_layout;
      owns_layout_ = false;
    }
    // Same handle: preserve current ownership (common when the caller created
    // the layout through create_pipeline_layout and rebinds it here).
  }

  // Shader stages
  VkPipelineShaderStageCreateInfo vert_stage{};
  vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vert_stage.module = vertex_shader_;
  vert_stage.pName = "main";

  VkPipelineShaderStageCreateInfo frag_stage{};
  frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  frag_stage.module = fragment_shader_;
  frag_stage.pName = "main";

  VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};

  // Vertex input
  // The bundled triangle shaders use gl_VertexIndex, so no vertex buffer is needed.
  (void)vertex_format;

  // Use empty vertex input (positions only via gl_VertexIndex)
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input.vertexBindingDescriptionCount = 0;
  vertex_input.vertexAttributeDescriptionCount = 0;

  // Input assembly
  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly.primitiveRestartEnable = VK_FALSE;

  // Dynamic viewport and scissor; depth bias joins when the pipeline opts in
  // (shadow-map pipelines set per-draw slope-scaled bias via vkCmdSetDepthBias).
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR,
                                     VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dynamic_state{};
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.dynamicStateCount =
      depth_bias_slope != 0.0f ? 3U : 2U;
  dynamic_state.pDynamicStates = dynamic_states;

  // Viewport (placeholder — set dynamically)
  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  // Rasterizer
  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = enable_backface_cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = depth_bias_slope != 0.0f ? VK_TRUE : VK_FALSE;
  rasterizer.depthBiasConstantFactor = 2.0f;
  rasterizer.depthBiasSlopeFactor = depth_bias_slope;
  rasterizer.depthBiasClamp = 0.0f;

  // Multisampling
  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Depth stencil
  VkPipelineDepthStencilStateCreateInfo depth_stencil{};
  depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = enable_depth_test ? VK_TRUE : VK_FALSE;
  depth_stencil.depthWriteEnable = enable_depth_write ? VK_TRUE : VK_FALSE;
  depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
  depth_stencil.depthBoundsTestEnable = VK_FALSE;
  depth_stencil.stencilTestEnable = VK_FALSE;

  // Color blending
  VkPipelineColorBlendAttachmentState color_blend_attachment{};
  color_blend_attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color_blend_attachment.blendEnable = VK_FALSE;

  VkPipelineColorBlendStateCreateInfo color_blending{};
  color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blending.logicOpEnable = VK_FALSE;
  color_blending.attachmentCount = 1;
  color_blending.pAttachments = &color_blend_attachment;

  // Create pipeline
  VkGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &rasterizer;
  pipeline_info.pMultisampleState = &multisampling;
  pipeline_info.pDepthStencilState = &depth_stencil;
  pipeline_info.pColorBlendState = &color_blending;
  pipeline_info.pDynamicState = &dynamic_state;
  pipeline_info.layout = layout_;
  pipeline_info.renderPass = render_pass;
  pipeline_info.subpass = 0;
  pipeline_info.basePipelineHandle = VK_NULL_HANDLE;

  VkResult result = vkCreateGraphicsPipelines(
      device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);
  if (result != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)render_pass; (void)vertex_format; (void)pipeline_layout;
  (void)enable_depth_test; (void)enable_depth_write; (void)enable_backface_cull;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanPipeline::create_pipeline_layout(
    VkDevice device, const void* push_constant_range) {
  return create_pipeline_layout(device, nullptr, 0, push_constant_range);
}

::warploom::core::Result<void> VulkanPipeline::create_pipeline_layout(
    VkDevice device, const VkDescriptorSetLayout* set_layouts,
    std::uint32_t set_layout_count, const void* push_constant_range) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  if (layout_ && owns_layout_) {
    vkDestroyPipelineLayout(device, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
  }

  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = set_layout_count;
  layout_info.pSetLayouts = set_layouts;
  layout_info.pushConstantRangeCount = push_constant_range ? 1 : 0;
  layout_info.pPushConstantRanges =
      static_cast<const VkPushConstantRange*>(push_constant_range);

  VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_);
  if (result != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  owns_layout_ = true;

  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)set_layouts; (void)set_layout_count; (void)push_constant_range;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanPipeline::cleanup(VkDevice device) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device) {
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (owns_layout_ && layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    if (vertex_shader_) vkDestroyShaderModule(device, vertex_shader_, nullptr);
    if (fragment_shader_) vkDestroyShaderModule(device, fragment_shader_, nullptr);
    if (compute_shader_) vkDestroyShaderModule(device, compute_shader_, nullptr);
  }
#else
  (void)device;
#endif
  pipeline_ = nullptr;
  layout_ = nullptr;
  vertex_shader_ = nullptr;
  fragment_shader_ = nullptr;
  owns_layout_ = false;
}

} // namespace warploom::render
