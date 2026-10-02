//! @file vulkan_ibl_baker.cpp
//! @brief Engine-side IBL bake: analytic sky -> prefiltered/irradiance/LUT.
//! Direct port of the proven test harness sequence (test_pbr_ibl.cpp).

#include "warploom/render/vulkan_ibl_baker.hpp"

#include <algorithm>
#include <cstring>

#include "warploom/render/vulkan_renderer.hpp"

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

namespace {
#ifdef OMNICPP_HAS_VULKAN
constexpr std::uint32_t kSkyWidth = 256U;
constexpr std::uint32_t kSkyHeight = 128U;
constexpr std::uint32_t kPrefilterBase = 64U;
constexpr std::uint32_t kPrefilterMips = 7U;  // 64..1
constexpr std::uint32_t kIrradianceBase = 32U;
constexpr std::uint32_t kBrdfLutSize = 256U;
constexpr VkFormat kIblFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
#endif
}  // namespace

bool VulkanIblBaker::initialize(VkDevice device, VkQueue graphics_queue,
                                std::uint32_t queue_family,
                                VulkanMemoryAllocator& allocator,
                                VulkanDescriptorManager& descriptors,
                                const std::string& shader_dir,
                                std::string& error) {
#ifdef OMNICPP_HAS_VULKAN
  device_ = device;
  allocator_ = &allocator;
  descriptors_ = &descriptors;
  queue_ = graphics_queue;
  queue_family_ = queue_family;

  const VkImageUsageFlags kStorageSampled =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!create_image(sky_image_, sky_memory_, kSkyWidth, kSkyHeight, 1U, 1U,
                    kStorageSampled, 0U) ||
      !create_image(prefiltered_image_, prefiltered_memory_, kPrefilterBase,
                    kPrefilterBase, kPrefilterMips, 6U, kStorageSampled,
                    VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
      !create_image(irradiance_image_, irradiance_memory_, kIrradianceBase,
                    kIrradianceBase, 1U, 6U, kStorageSampled,
                    VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
      !create_image(lut_image_, lut_memory_, kBrdfLutSize, kBrdfLutSize, 1U,
                    1U, kStorageSampled, 0U)) {
    error = "ibl baker: image creation failed";
    return false;
  }

  if (!create_view(sky_view_, sky_image_, VK_IMAGE_VIEW_TYPE_2D, 0U, 1U, 1U)) {
    error = "ibl baker: sky view failed";
    return false;
  }
  for (std::uint32_t m = 0; m < kPrefilterMips; ++m) {
    if (!create_view(prefilter_mip_views_[m], prefiltered_image_,
                     VK_IMAGE_VIEW_TYPE_2D_ARRAY, m, 1U, 6U)) {
      error = "ibl baker: prefilter mip view failed";
      return false;
    }
  }
  if (!create_view(irradiance_mip_view_, irradiance_image_,
                   VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0U, 1U, 6U) ||
      !create_view(prefiltered_cube_view_, prefiltered_image_,
                   VK_IMAGE_VIEW_TYPE_CUBE, 0U, kPrefilterMips, 6U) ||
      !create_view(irradiance_cube_view_, irradiance_image_,
                   VK_IMAGE_VIEW_TYPE_CUBE, 0U, 1U, 6U) ||
      !create_view(lut_view_, lut_image_, VK_IMAGE_VIEW_TYPE_2D, 0U, 1U, 1U)) {
    error = "ibl baker: sampled views failed";
    return false;
  }

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = 0.5f;
  if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_sky_) !=
      VK_SUCCESS) {
    error = "ibl baker: sky sampler failed";
    return false;
  }
  sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = static_cast<float>(kPrefilterMips - 1U);
  if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_cube_) !=
      VK_SUCCESS) {
    error = "ibl baker: cube sampler failed";
    return false;
  }
  sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = 0.5f;
  if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_flat_) !=
      VK_SUCCESS) {
    error = "ibl baker: flat sampler failed";
    return false;
  }

  const std::vector<ReflectedBinding> sky_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
       VK_SHADER_STAGE_COMPUTE_BIT}};
  const std::vector<ReflectedBinding> cube_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_COMPUTE_BIT},
      {0U, 1U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
       VK_SHADER_STAGE_COMPUTE_BIT}};
  auto sky_layout = descriptors_->create_layout(sky_bindings, 1U);
  auto prefilter_layout = descriptors_->create_layout(cube_bindings, kPrefilterMips);
  auto irradiance_layout = descriptors_->create_layout(cube_bindings, 1U);
  auto lut_layout = descriptors_->create_layout(sky_bindings, 1U);
  if (!sky_layout.is_ok() || !prefilter_layout.is_ok() ||
      !irradiance_layout.is_ok() || !lut_layout.is_ok()) {
    error = "ibl baker: bake layouts failed";
    return false;
  }
  bake_layouts_[0] = sky_layout.value();
  bake_layouts_[1] = prefilter_layout.value();
  bake_layouts_[2] = irradiance_layout.value();
  bake_layouts_[3] = lut_layout.value();

  auto sky_set = descriptors_->allocate_set(bake_layouts_[0]);
  auto irradiance_set = descriptors_->allocate_set(bake_layouts_[2]);
  auto lut_set = descriptors_->allocate_set(bake_layouts_[3]);
  if (!sky_set.is_ok() || !irradiance_set.is_ok() || !lut_set.is_ok()) {
    error = "ibl baker: bake sets failed";
    return false;
  }
  bake_sets_[0] = sky_set.value();
  bake_sets_[2] = irradiance_set.value();
  bake_sets_[3] = lut_set.value();

  if (!descriptors_
           ->write_image(bake_sets_[0], 0U,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                         sky_view_, VK_IMAGE_LAYOUT_GENERAL, 0U)
           .is_ok() ||
      !descriptors_
           ->write_image(bake_sets_[2], 0U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         sampler_sky_, sky_view_,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok() ||
      !descriptors_
           ->write_image(bake_sets_[3], 0U,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                         lut_view_, VK_IMAGE_LAYOUT_GENERAL, 0U)
           .is_ok()) {
    error = "ibl baker: bake set writes failed";
    return false;
  }

  for (std::uint32_t m = 0; m < kPrefilterMips; ++m) {
    auto ps = descriptors_->allocate_set(bake_layouts_[1]);
    if (!ps.is_ok()) {
      error = "ibl baker: prefilter set failed";
      return false;
    }
    prefilter_sets_[m] = ps.value();
    if (!descriptors_
             ->write_image(prefilter_sets_[m], 0U,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           sampler_sky_, sky_view_,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
             .is_ok() ||
        !descriptors_
             ->write_image(prefilter_sets_[m], 1U,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                           prefilter_mip_views_[m], VK_IMAGE_LAYOUT_GENERAL,
                           0U)
             .is_ok()) {
      error = "ibl baker: prefilter set writes failed";
      return false;
    }
  }

  if (!sky_pipeline_
           .load_shader_stage_file(device_, shader_dir + "/ibl_sky.comp.spv",
                                   "compute")
           .is_ok() ||
      !prefilter_pipeline_
           .load_shader_stage_file(device_,
                                   shader_dir + "/ibl_prefilter.comp.spv",
                                   "compute")
           .is_ok() ||
      !irradiance_pipeline_
           .load_shader_stage_file(device_,
                                   shader_dir + "/ibl_irradiance.comp.spv",
                                   "compute")
           .is_ok() ||
      !lut_pipeline_
           .load_shader_stage_file(device_,
                                   shader_dir + "/ibl_brdf_lut.comp.spv",
                                   "compute")
           .is_ok()) {
    error = "ibl baker: compute shader load failed (dir: " + shader_dir + ")";
    return false;
  }
  const VkPushConstantRange sky_push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 64U};
  const VkPushConstantRange prefilter_push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 4U};
  if (!sky_pipeline_
           .create_pipeline_layout(device_, &bake_layouts_[0], 1U, &sky_push)
           .is_ok() ||
      !prefilter_pipeline_
           .create_pipeline_layout(device_, &bake_layouts_[1], 1U,
                                   &prefilter_push)
           .is_ok() ||
      !irradiance_pipeline_
           .create_pipeline_layout(device_, &bake_layouts_[2], 1U, nullptr)
           .is_ok() ||
      !lut_pipeline_
           .create_pipeline_layout(device_, &bake_layouts_[3], 1U, nullptr)
           .is_ok()) {
    error = "ibl baker: compute layouts failed";
    return false;
  }
  if (!sky_pipeline_
           .create_compute_pipeline(device_, sky_pipeline_.pipeline_layout())
           .is_ok() ||
      !prefilter_pipeline_
           .create_compute_pipeline(device_,
                                    prefilter_pipeline_.pipeline_layout())
           .is_ok() ||
      !irradiance_pipeline_
           .create_compute_pipeline(device_,
                                    irradiance_pipeline_.pipeline_layout())
           .is_ok() ||
      !lut_pipeline_
           .create_compute_pipeline(device_, lut_pipeline_.pipeline_layout())
           .is_ok()) {
    error = "ibl baker: compute pipelines failed";
    return false;
  }

  // --- IBL descriptor set (bindings 0/1/2: prefiltered, irradiance, LUT). --
  const std::vector<ReflectedBinding> ibl_bindings = {
      {3U, 0U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT},
      {3U, 1U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT},
      {3U, 2U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto ibl_layout_result = descriptors_->create_layout(ibl_bindings, 1U);
  if (!ibl_layout_result.is_ok()) {
    error = "ibl baker: ibl layout failed";
    return false;
  }
  ibl_layout_ = ibl_layout_result.value();
  auto ibl_set_result = descriptors_->allocate_set(ibl_layout_);
  if (!ibl_set_result.is_ok()) {
    error = "ibl baker: ibl set failed";
    return false;
  }
  ibl_set_ = ibl_set_result.value();
  if (!descriptors_
           ->write_image(ibl_set_, 0U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         sampler_cube_, prefiltered_cube_view_,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok() ||
      !descriptors_
           ->write_image(ibl_set_, 1U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         sampler_flat_, irradiance_cube_view_,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok() ||
      !descriptors_
           ->write_image(ibl_set_, 2U,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         sampler_flat_, lut_view_,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    error = "ibl baker: ibl set writes failed";
    return false;
  }

  auto pool_result = VulkanRenderer::create_command_pool(device_, queue_family_);
  if (!pool_result.is_ok()) {
    error = "ibl baker: command pool failed";
    return false;
  }
  pool_ = pool_result.value();
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vkCreateFence(device_, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
    error = "ibl baker: fence failed";
    return false;
  }
  return true;
#else
  (void)device;
  (void)graphics_queue;
  (void)queue_family;
  (void)allocator;
  (void)descriptors;
  (void)shader_dir;
  (void)error;
  return false;
#endif
}

#ifdef OMNICPP_HAS_VULKAN
bool VulkanIblBaker::create_image(VkImage& image, Allocation& memory,
                                  std::uint32_t width, std::uint32_t height,
                                  std::uint32_t mips, std::uint32_t layers,
                                  VkImageUsageFlags usage,
                                  VkImageCreateFlags flags) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = kIblFormat;
  info.extent = {width, height, 1U};
  info.mipLevels = mips;
  info.arrayLayers = layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info.flags = flags;
  if (vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) {
    return false;
  }
  auto result = allocator_->bind_image(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!result.is_ok()) return false;
  memory = result.value();
  return true;
}

bool VulkanIblBaker::create_view(VkImageView& view, VkImage image,
                                 VkImageViewType type, std::uint32_t base_mip,
                                 std::uint32_t mip_count,
                                 std::uint32_t layer_count) {
  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image;
  info.viewType = type;
  info.format = kIblFormat;
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, 0U,
                           layer_count};
  return vkCreateImageView(device_, &info, nullptr, &view) == VK_SUCCESS;
}

void VulkanIblBaker::transition(VkCommandBuffer cb, VkImage image,
                                std::uint32_t mips, std::uint32_t layers,
                                VkImageLayout old_layout,
                                VkImageLayout new_layout,
                                VkPipelineStageFlags src_stage,
                                VkAccessFlags src_access,
                                VkPipelineStageFlags dst_stage,
                                VkAccessFlags dst_access) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, mips, 0U, layers};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0U, 0U, nullptr, 0U, nullptr,
                       1U, &barrier);
}
#endif  // OMNICPP_HAS_VULKAN

bool VulkanIblBaker::bake(const IblBakeParams& params) {
#ifdef OMNICPP_HAS_VULKAN
  if (device_ == VK_NULL_HANDLE || pool_ == VK_NULL_HANDLE) return false;
  auto cb_result = VulkanRenderer::allocate_command_buffer(device_, pool_);
  if (!cb_result.is_ok()) return false;
  const VkCommandBuffer cb = cb_result.value();

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) return false;

  // 1. Equirect sky: UNDEFINED -> GENERAL, dispatch, -> READ_ONLY.
  transition(cb, sky_image_, 1U, 1U, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                    sky_pipeline_.pipeline());
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          sky_pipeline_.pipeline_layout(), 0U, 1U,
                          &bake_sets_[0], 0U, nullptr);
  const float sun_dir[4] = {params.sun_direction[0], params.sun_direction[1],
                            params.sun_direction[2], 0.0f};
  const float sun_color[4] = {params.sun_color[0], params.sun_color[1],
                              params.sun_color[2], 0.0f};
  const float sky_color[4] = {params.sky_color[0], params.sky_color[1],
                              params.sky_color[2], 0.0f};
  const float misc[4] = {params.sun_radius_rad, params.sky_gain, 0.0f, 0.0f};
  vkCmdPushConstants(cb, sky_pipeline_.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, 16U, sun_dir);
  vkCmdPushConstants(cb, sky_pipeline_.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 16U, 16U, sun_color);
  vkCmdPushConstants(cb, sky_pipeline_.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 32U, 16U, sky_color);
  vkCmdPushConstants(cb, sky_pipeline_.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 48U, 16U, misc);
  vkCmdDispatch(cb, kSkyWidth / 8U, kSkyHeight / 8U, 1U);
  transition(cb, sky_image_, 1U, 1U, VK_IMAGE_LAYOUT_GENERAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

  // 2. Prefiltered cube: one dispatch per mip, all six faces.
  transition(cb, prefiltered_image_, kPrefilterMips, 6U,
             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
  for (std::uint32_t m = 0; m < kPrefilterMips; ++m) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      prefilter_pipeline_.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            prefilter_pipeline_.pipeline_layout(), 0U, 1U,
                            &prefilter_sets_[m], 0U, nullptr);
    const float roughness =
        static_cast<float>(m) / static_cast<float>(kPrefilterMips - 1U);
    vkCmdPushConstants(cb, prefilter_pipeline_.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, 4U, &roughness);
    const std::uint32_t dim = kPrefilterBase >> m;
    vkCmdDispatch(cb, std::max(dim / 8U, 1U), std::max(dim / 8U, 1U), 6U);
  }

  // 3. Irradiance cube (single dispatch, all six faces).
  transition(cb, irradiance_image_, 1U, 6U, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
  if (!descriptors_
           ->write_image(bake_sets_[2], 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, irradiance_mip_view_,
                         VK_IMAGE_LAYOUT_GENERAL, 0U)
           .is_ok()) {
    return false;
  }
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                    irradiance_pipeline_.pipeline());
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          irradiance_pipeline_.pipeline_layout(), 0U, 1U,
                          &bake_sets_[2], 0U, nullptr);
  vkCmdDispatch(cb, kIrradianceBase / 8U, kIrradianceBase / 8U, 6U);

  // 4. BRDF LUT (single dispatch).
  transition(cb, lut_image_, 1U, 1U, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lut_pipeline_.pipeline());
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          lut_pipeline_.pipeline_layout(), 0U, 1U,
                          &bake_sets_[3], 0U, nullptr);
  vkCmdDispatch(cb, kBrdfLutSize / 8U, kBrdfLutSize / 8U, 1U);

  // 5. Everything -> SHADER_READ_ONLY for the fragment stage.
  constexpr VkPipelineStageFlags kDstStage =
      static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition(cb, prefiltered_image_, kPrefilterMips, 6U,
             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
             kDstStage, VK_ACCESS_SHADER_READ_BIT);
  transition(cb, irradiance_image_, 1U, 6U, VK_IMAGE_LAYOUT_GENERAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
             kDstStage, VK_ACCESS_SHADER_READ_BIT);
  transition(cb, lut_image_, 1U, 1U, VK_IMAGE_LAYOUT_GENERAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
             kDstStage, VK_ACCESS_SHADER_READ_BIT);

  if (vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
  (void)vkResetFences(device_, 1U, &fence_);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &cb;
  if (vkQueueSubmit(queue_, 1U, &submit, fence_) != VK_SUCCESS) return false;
  return vkWaitForFences(device_, 1U, &fence_, VK_TRUE, UINT64_MAX) ==
         VK_SUCCESS;
#else
  (void)params;
  return false;
#endif
}

void VulkanIblBaker::cleanup(VkDevice device) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device == VK_NULL_HANDLE) return;
  sky_pipeline_.cleanup(device);
  prefilter_pipeline_.cleanup(device);
  irradiance_pipeline_.cleanup(device);
  lut_pipeline_.cleanup(device);
  if (fence_ != VK_NULL_HANDLE) {
    vkDestroyFence(device, fence_, nullptr);
    fence_ = VK_NULL_HANDLE;
  }
  if (pool_ != VK_NULL_HANDLE) {
    vkDestroyCommandPool(device, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
  }
  for (VkSampler* s : {&sampler_sky_, &sampler_cube_, &sampler_flat_}) {
    if (*s != VK_NULL_HANDLE) {
      vkDestroySampler(device, *s, nullptr);
      *s = VK_NULL_HANDLE;
    }
  }
  for (VkImageView* view : {&sky_view_, &prefiltered_cube_view_,
                            &irradiance_mip_view_, &irradiance_cube_view_,
                            &lut_view_}) {
    if (*view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, *view, nullptr);
      *view = VK_NULL_HANDLE;
    }
  }
  for (VkImageView& view : prefilter_mip_views_) {
    if (view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, view, nullptr);
      view = VK_NULL_HANDLE;
    }
  }
  if (sky_memory_.is_valid()) allocator_->destroy_allocation(sky_memory_);
  if (prefiltered_memory_.is_valid()) {
    allocator_->destroy_allocation(prefiltered_memory_);
  }
  if (irradiance_memory_.is_valid()) {
    allocator_->destroy_allocation(irradiance_memory_);
  }
  if (lut_memory_.is_valid()) allocator_->destroy_allocation(lut_memory_);
  for (VkImage* image : {&sky_image_, &prefiltered_image_, &irradiance_image_,
                         &lut_image_}) {
    if (*image != VK_NULL_HANDLE) {
      vkDestroyImage(device, *image, nullptr);
      *image = VK_NULL_HANDLE;
    }
  }
  device_ = VK_NULL_HANDLE;
#else
  (void)device;
#endif
}

}  // namespace warploom::render
