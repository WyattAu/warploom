//! @file vulkan_path_tracer.cpp
//! @brief Ownable deterministic path tracer (R1a).
//!
//! Bodies mirror the setup that used to live inside
//! tests/unit/test_path_tracing*.cpp, minus the scene literals and the test
//! scaffolding. The point of the extraction is that the accumulation and
//! seeding contract — clear once, add per frame, seed = f(pixel, frame) —
//! now has an engine home an application can drive, not just a fixture.

#include <warploom/render/vulkan_path_tracer.hpp>

#include <cstdio>
#include <cstring>
#include <utility>

#include <warploom/core/diagnostics.hpp>

#include "warploom/render/vulkan_command.hpp"
#include "warploom/render/vulkan_descriptors.hpp"

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

namespace {

#ifdef OMNICPP_HAS_VULKAN

//! Host mirror of pt_real's Params block (camera words, dims incl. the sky
//! mode in dims[3], then the five sky vec4s). ONE definition: three
//! function-local copies used to live at the use sites, and a layout this
//! wide must not exist in triplicate — that is the drift class that bit
//! the BLAS geometry. Prefix offsets are unchanged, so mode-0 tracers read
//! bit-identical params to before.
struct TracerGpuParams {
  float cam_pos_frame[4];
  float cam_basis_r[4];
  float cam_basis_u[4];
  float cam_basis_f[4];
  std::uint32_t dims[4];
  float cam_fov[4];
  float sky_sun[4];
  float sky_rayleigh[4];
  float sky_mie[4];
  float sky_misc[4];
  float sky_origin[4];
};

//! Minimal SPIR-V file loader for the RT stages. VulkanPipeline's stage loader
//! only understands vertex/fragment/compute; the ray stages are out of its
//! vocabulary, and a path tracer must not borrow a graphics pipeline object
//! just to get a file read.
[[nodiscard]] VkShaderModule load_spv_module(VkDevice device, const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return VK_NULL_HANDLE;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return VK_NULL_HANDLE;
  }
  std::vector<char> code(static_cast<std::size_t>(size));
  const auto read = std::fread(code.data(), 1, code.size(), f);
  std::fclose(f);
  if (read != code.size()) return VK_NULL_HANDLE;
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = code.size();
  info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return module;
}

#endif  // OMNICPP_HAS_VULKAN

}  // namespace

::warploom::core::Result<void> VulkanPathTracer::create(
    [[maybe_unused]] VkPhysicalDevice physical_device, [[maybe_unused]] VkDevice device,
    [[maybe_unused]] VkQueue graphics_queue, [[maybe_unused]] std::uint32_t graphics_family,
    [[maybe_unused]] VulkanMemoryAllocator& allocator,
    [[maybe_unused]] const PathTracerConfig& config) {
#ifdef OMNICPP_HAS_VULKAN
  if (failed_) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  const auto refuse = [this](const char* why) -> ::warploom::core::Result<void> {
    failed_ = true;
    WARPLOOM_WARN("render", "path tracer unavailable (%s)\n", why);
    destroy_resources();
    return ::warploom::core::Result<void>::ok();
  };

  if (device == VK_NULL_HANDLE || graphics_queue == VK_NULL_HANDLE ||
      config.width == 0U || config.height == 0U ||
      config.shader_dir.empty() || config.meshes.empty()) {
    return refuse("invalid configuration");
  }

  // Re-entrant: a resize calls create() again on a live tracer. Without this
  // the second call would allocate a second accumulation image and replace the
  // descriptor manager, leaking both -- the same class of bug the compose
  // chain's ensure() had before it took (idempotence, width/height) into
  // account.
  if (ready_) {
    destroy_resources();
  }

  physical_device_ = physical_device;
  device_ = device;
  graphics_queue_ = graphics_queue;
  graphics_family_ = graphics_family;
  allocator_ = &allocator;
  shader_dir_ = config.shader_dir;
  width_ = config.width;
  height_ = config.height;
  max_bounces_ = config.max_bounces;

  // ---- Accumulation + second-moment images (rgba32f storage) ---------------
  // One creation path for both: the moment image is the same extent, format
  // and usage as the accumulation, cleared and added alongside it. A second
  // copy of this block would be a second place for the usage flags to drift
  // (the SAMPLED-bit lesson above was exactly that class of drift).
  // Returns false when refuse() ran (which itself returns ok() by the
  // file's convention: failure is reported via failed(), not the Result).
  const auto make_storage_image = [&](VkImage& image, VkImageView& view,
                                      Allocation& memory) -> bool {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    image_info.extent = {width_, height_, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       // SAMPLED is not optional: the accumulation image is what
                       // the compose chain tonemaps (R1b), and binding a
                       // storage-only view as a combined image sampler is
                       // VUID-VkWriteDescriptorSet-descriptorType-00337. The
                       // tests that only read the accumulation back never
                       // sampled it, so this was invisible until the image
                       // reached a real tonemap. The moment image carries the
                       // same flags so a future moment-sampling tonemap or
                       // denoiser needs no recreate.
                       VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &image_info, nullptr, &image) != VK_SUCCESS) {
      refuse("accumulation image creation failed");
      return false;
    }
    auto mem = allocator.bind_image(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!mem.is_ok()) {
      refuse("accumulation image memory failed");
      return false;
    }
    memory = mem.value();
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view_info, nullptr, &view) != VK_SUCCESS) {
      refuse("accumulation image view failed");
      return false;
    }
    return true;
  };
  if (!make_storage_image(accum_image_, accum_view_, accum_allocation_)) {
    return ::warploom::core::Result<void>::ok();  // refuse() already ran
  }
  if (!make_storage_image(moment_image_, moment_view_, moment_allocation_)) {
    return ::warploom::core::Result<void>::ok();  // refuse() already ran
  }

  // ---- Host-visible params + instance buffers -------------------------------
  auto params_buf = allocator.create_buffer(
      sizeof(TracerGpuParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!params_buf.is_ok()) return refuse("params buffer failed");
  params_allocation_ = params_buf.value();
  auto* gpu_params = static_cast<TracerGpuParams*>(params_allocation_.mapped);
  *gpu_params = TracerGpuParams{};
  // Legacy 90-degree mapping until set_camera says otherwise.
  gpu_params->cam_fov[0] = 1.0F;
  gpu_params->cam_fov[1] = 1.0F;
  gpu_params->dims[0] = width_;
  gpu_params->dims[1] = height_;
  gpu_params->dims[2] = max_bounces_;
  // Mode 0 (legacy white) unless the config opts into the analytic sky;
  // the extra words stay zeroed, which the shader only reads in mode 1.
  // visualize_normals is a diagnostic encoding (2), not a sky, and wins
  // over the sky mode: the vis test asserts hit probes only.
  gpu_params->dims[3] =
      config.visualize_normals ? 2U : config.sky.mode;
  for (int i = 0; i < 4; ++i) {
    gpu_params->sky_sun[i] = config.sky.sun_direction[static_cast<std::size_t>(i)];
    gpu_params->sky_rayleigh[i] = config.sky.rayleigh[static_cast<std::size_t>(i)];
    gpu_params->sky_mie[i] = config.sky.mie[static_cast<std::size_t>(i)];
    gpu_params->sky_misc[i] = config.sky.misc[static_cast<std::size_t>(i)];
    gpu_params->sky_origin[i] = config.sky.origin[static_cast<std::size_t>(i)];
  }

  instance_capacity_ = static_cast<std::uint32_t>(config.instances.size());
  if (instance_capacity_ == 0U) return refuse("no instances");
  auto instance_buf = allocator.create_buffer(
      static_cast<VkDeviceSize>(instance_capacity_) * 2U * sizeof(std::array<float, 4>),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!instance_buf.is_ok()) return refuse("instance buffer failed");
  instance_allocation_ = instance_buf.value();
  for (std::uint32_t i = 0; i < instance_capacity_; ++i) {
    auto* words = static_cast<std::array<float, 4>*>(instance_allocation_.mapped) + i * 2U;
    words[0] = config.instances[i].center;
    words[1] = config.instances[i].albedo;
  }

  // ---- BLASes + TLAS, built on a private submission -------------------------
  // A BLAS must exist on the device before a descriptor can name it, so the
  // geometry builds are recorded and submitted inside create() rather than
  // deferred to the first frame.
  auto pool = warploom::render::create_command_pool(device_, graphics_family_);
  if (!pool.is_ok()) return refuse("command pool failed");
  auto build_cb_result = warploom::render::allocate_command_buffer(device_, pool.value());
  if (!build_cb_result.is_ok()) {
    vkDestroyCommandPool(device_, pool.value(), nullptr);
    return refuse("command buffer failed");
  }
  VkCommandBuffer build_cb = build_cb_result.value();
  // The pool outlives the build exactly once: every failure path below has to
  // destroy it, and the refuse() helper cannot (it runs before this handle
  // exists in the earlier checks). RAII is the honest form of "every path".
  const auto pool_guard = [&] {
    vkDestroyCommandPool(device_, pool.value(), nullptr);
  };

  const auto submit_build = [&](VkCommandBuffer cb) -> bool {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
      WARPLOOM_WARN("render", "path tracer: geometry command buffer begin failed\n");
      return false;
    }
    for (std::size_t i = 0; i < config.meshes.size(); ++i) {
      BlasBuildInput in{};
      in.vertex_buffer_address = config.meshes[i].vertex_buffer_address;
      in.triangle_count = config.meshes[i].triangle_count;
      in.max_vertex = config.meshes[i].max_vertex;
      const auto built = as_builder_.cmd_build_blas(cb, device_, blas_list_[i], in,
                                                    scratch_address_);
      if (!built.is_ok()) {
        // Name the mesh. "acceleration structure build failed" alone is what
        // made this a guessing game the first time round: BLAS, TLAS and the
        // submit were three indistinguishable causes.
        WARPLOOM_WARN("render",
                      "path tracer: BLAS %zu build failed (addr 0x%llx, %u "
                      "tris, scratch 0x%llx)\n",
                      i, static_cast<unsigned long long>(in.vertex_buffer_address),
                      in.triangle_count,
                      static_cast<unsigned long long>(scratch_address_));
        return false;
      }
      // Order EVERY consecutive build that reuses this scratch buffer --
      // INCLUDING after the last BLAS, because the TLAS build immediately
      // reuses the same memory. Omitting that last barrier is why a one-mesh
      // scene appeared to work (the hazard is latent there) while two meshes
      // lost the device inside the build with NO validation error at all: a
      // scratch-reuse race is undefined behaviour, not a spec violation the
      // validation layer can see. Unconditional for exactly that reason --
      // the "only between" version is the one that looks correct.
      VkMemoryBarrier order{};
      order.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      order.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      order.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      vkCmdPipelineBarrier(
          cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0U, 1U,
          &order, 0U, nullptr, 0U, nullptr);
    }
    // TLAS instances: one per mesh, placed from the config.
    std::vector<TlasInstance> instances(config.meshes.size());
    for (std::size_t i = 0; i < config.meshes.size(); ++i) {
      std::memcpy(instances[i].transform, config.instances[i].transform.data(),
                  sizeof(instances[i].transform));
      instances[i].instance_custom_index = config.instances[i].custom_index;
      instances[i].mask = 0xFFU;
      instances[i].sbt_offset = 0U;
      instances[i].flags = 0U;
      instances[i].blas_device_address = blas_list_[i].device_address;
      if (instances[i].blas_device_address == 0U) {
        WARPLOOM_WARN("render", "path tracer: BLAS %zu has no device address\n", i);
        return false;
      }
    }
    const auto tlas = as_builder_.cmd_build_tlas(
        cb, device_, tlas_, instances.data(),
        static_cast<std::uint32_t>(instances.size()), scratch_address_);
    if (!tlas.is_ok()) {
      WARPLOOM_WARN("render", "path tracer: TLAS build failed (%zu instances, "
                             "capacity %u)\n",
                    instances.size(), tlas_.capacity);
      return false;
    }
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
      WARPLOOM_WARN("render", "path tracer: geometry command buffer end failed\n");
      return false;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    if (vkQueueSubmit(graphics_queue_, 1U, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
      WARPLOOM_WARN("render", "path tracer: geometry submit failed\n");
      return false;
    }
    const VkResult waited = vkQueueWaitIdle(graphics_queue_);
    if (waited != VK_SUCCESS) {
      WARPLOOM_WARN("render", "path tracer: geometry queue wait failed (%d)\n",
                    static_cast<int>(waited));
      return false;
    }
    return true;
  };

  // Size every BLAS + the TLAS, acquire scratch once for the largest build,
  // then build on the private command buffer.
  for (const PathTracerMesh& mesh : config.meshes) {
    BlasBuildInput in{};
    in.vertex_buffer_address = mesh.vertex_buffer_address;
    in.triangle_count = mesh.triangle_count;
    in.max_vertex = mesh.max_vertex;
    const auto blas = as_builder_.create_blas(device_, allocator, in);
    if (!blas.is_ok()) return refuse("BLAS creation failed");
    blas_list_.push_back(blas.value());
  }
  auto tlas = as_builder_.create_tlas(device_, allocator, instance_capacity_);
  if (!tlas.is_ok()) return refuse("TLAS creation failed");
  tlas_ = tlas.value();

  const auto scratch = scratch_.acquire(allocator, device, 1U << 20U);
  if (!scratch.is_ok()) return refuse("scratch allocation failed");
  scratch_address_ = scratch.value();
  if (!submit_build(build_cb)) {
    // Found by the isolation test's leak report: the failure path returned
    // without releasing the pool, so every refused create() leaked a command
    // pool and a command buffer. A GPU fault is the case you least want a
    // leak on -- it is exactly when you will re-run the build.
    pool_guard();
    return refuse("acceleration structure build failed");
  }
  pool_guard();

  // ---- Descriptor set: 0 accum, 1 params, 2 TLAS, 3 instances, 4 moment ----
  descriptor_manager_ = std::make_unique<VulkanDescriptorManager>();
  if (!descriptor_manager_->initialize(device_).is_ok()) {
    return refuse("descriptor manager failed");
  }
  auto layout = descriptor_manager_->create_layout(
      {{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR},
       {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR},
       {0, 2, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
       {0, 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
       {0, 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR}},
      1U);
  if (!layout.is_ok()) return refuse("descriptor layout failed");
  layout_ = layout.value();
  auto set = descriptor_manager_->allocate_set(layout_);
  if (!set.is_ok()) return refuse("descriptor set failed");
  descriptor_set_ = set.value();

  VkDescriptorSetLayout set_layouts[1] = {layout_};
  VkPipelineLayoutCreateInfo pli{};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1U;
  pli.pSetLayouts = set_layouts;
  if (vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_) != VK_SUCCESS) {
    return refuse("pipeline layout failed");
  }

  // ---- RT pipeline + SBT ----------------------------------------------------
  struct StageSpec {
    const char* file;
    VkShaderStageFlagBits bit;
  };
  const StageSpec specs[3] = {
      {"pt_real.rgen.spv", VK_SHADER_STAGE_RAYGEN_BIT_KHR},
      {"pt_real.rmiss.spv", VK_SHADER_STAGE_MISS_BIT_KHR},
      {"pt_real.rchit.spv", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR}};
  std::array<VkShaderModule, 3> modules{};
  for (std::size_t i = 0; i < modules.size(); ++i) {
    modules[i] = load_spv_module(device_, shader_dir_ + "/" + specs[i].file);
    if (modules[i] == VK_NULL_HANDLE) {
      return refuse("ray tracing shader load failed");
    }
  }
  std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
  for (std::size_t i = 0; i < stages.size(); ++i) {
    stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[i].stage = specs[i].bit;
    stages[i].module = modules[i];
    stages[i].pName = "main";
  }
  const std::vector<VkPipelineShaderStageCreateInfo> stage_list(
      stages.begin(), stages.end());
  const auto created = rt_pipeline_.create(physical_device_, device_, pipeline_layout_,
                                           stage_list, /*miss_count=*/1U,
                                           /*hit_group_count=*/1U,
                                           /*max_recursion_depth=*/1U);
  for (VkShaderModule m : modules) {
    if (m != VK_NULL_HANDLE) vkDestroyShaderModule(device_, m, nullptr);
  }
  if (!created.is_ok()) return refuse("RT pipeline creation failed");
  auto handles = rt_pipeline_.fetch_handles(device_);
  if (!handles.is_ok()) return refuse("SBT handle fetch failed");
  auto sbt_buf = allocator.create_buffer(
      VulkanRtPipeline::required_sbt_bytes(physical_device_, 1U, 1U),
      VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!sbt_buf.is_ok()) return refuse("SBT buffer failed");
  sbt_allocation_ = sbt_buf.value();
  if (!rt_pipeline_.write_sbt(device_, handles.value(), sbt_allocation_).is_ok()) {
    return refuse("SBT write failed");
  }

  // ---- Descriptor writes (the AS descriptor only after the build submitted) -
  if (!descriptor_manager_
           ->write_image(descriptor_set_, 0U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, accum_view_, VK_IMAGE_LAYOUT_GENERAL, 0U)
           .is_ok() ||
      !descriptor_manager_
           ->write_buffer(descriptor_set_, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          params_allocation_.buffer, 0, VK_WHOLE_SIZE)
           .is_ok() ||
      !descriptor_manager_
           ->write_acceleration_structure(
               descriptor_set_, 2U, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
               tlas_.handle)
           .is_ok() ||
      !descriptor_manager_
           ->write_buffer(descriptor_set_, 3U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          instance_allocation_.buffer, 0, VK_WHOLE_SIZE)
           .is_ok() ||
      !descriptor_manager_
           ->write_image(descriptor_set_, 4U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, moment_view_, VK_IMAGE_LAYOUT_GENERAL, 0U)
           .is_ok()) {
    return refuse("descriptor write failed");
  }

  ready_ = true;
  return ::warploom::core::Result<void>::ok();
#else
  (void)physical_device; (void)device; (void)graphics_queue;
  (void)graphics_family; (void)allocator; (void)config;
  return ::warploom::core::Result<void>::ok();
#endif
}

void VulkanPathTracer::record_round([[maybe_unused]] VkCommandBuffer command_buffer,
                                    [[maybe_unused]] std::uint32_t frame,
                                    [[maybe_unused]] bool clear_first) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (!ready_ || command_buffer == VK_NULL_HANDLE) return;

  // The frame index is the sample seed; write it before the trace records so
  // the host upload and the GPU dispatch agree on which sample this is.
  auto* gpu_params = static_cast<TracerGpuParams*>(params_allocation_.mapped);
  gpu_params->cam_pos_frame[3] = static_cast<float>(frame);

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                    rt_pipeline_.pipeline());
  VkDescriptorSet set = descriptor_set_;
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          pipeline_layout_, 0, 1, &set, 0, nullptr);

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.image = accum_image_;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  // Both accumulation targets travel together: the moment image gets the
  // same barrier shapes as the accumulation, with only .image differing.
  // A single-image barrier here would leave the moment image's layout and
  // visibility to chance -- the same silent-race class as the missing
  // SHADER_READ bit below (no validation diagnostic, wrong picture).
  const auto emit_both = [&](const VkImageMemoryBarrier& shape,
                             VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    VkImageMemoryBarrier pair[2] = {shape, shape};
    pair[0].image = accum_image_;
    pair[1].image = moment_image_;
    vkCmdPipelineBarrier(command_buffer, src, dst, 0, 0, nullptr, 0, nullptr, 2,
                         pair);
  };
  if (clear_first) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    emit_both(barrier, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black{};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command_buffer, accum_image_, VK_IMAGE_LAYOUT_GENERAL,
                         &black, 1, &range);
    vkCmdClearColorImage(command_buffer, moment_image_, VK_IMAGE_LAYOUT_GENERAL,
                         &black, 1, &range);
    // Clear -> shader read/write so the trace both reads (nothing yet) and
    // writes the frame's contribution into GENERAL.
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    emit_both(barrier, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  } else {
    // Cross-submission visibility: the previous round's shader write must be
    // visible to this round's read-modify-write.
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    emit_both(barrier, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
              VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  }
  rt_pipeline_.trace_rays(command_buffer, width_, height_, 1U);

  // Leave the accumulation readable by a TRANSFER COPY (read_mean) and by a
  // SHADER READ.
  //
  // SHADER_READ was missing. The compose chain samples this image, and the
  // only barrier between the write and that sample was this one -- a
  // SHADER_WRITE -> TRANSFER_READ barrier does not make the write visible to
  // a shader read. The compose chain's own graph barrier cannot cover it
  // either, because the sample is an EXTERNAL image: the graph emits no
  // write->read barrier for an image it did not produce in-graph.
  //
  // The symptom was a path-traced view frozen at a plausible value while the
  // accumulation underneath it advanced: 75% of the accumulation's pixels
  // changed between 300 and 1500 samples, and 0.02% of the composed pixels
  // changed, by one LSB. No validation diagnostic -- a missing dependency is
  // not a spec violation, it is a race the driver is free to lose.
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  emit_both(barrier, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT |
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

  if (clear_first) {
    samples_ = 1U;
  } else if (samples_ > 0U) {
    ++samples_;
  }
#endif
}

void VulkanPathTracer::set_camera([[maybe_unused]] const PathTracerCamera& camera) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (params_allocation_.mapped == nullptr) return;
  auto* gpu_params = static_cast<TracerGpuParams*>(params_allocation_.mapped);
  gpu_params->cam_fov[0] = camera.tan_half_fov;
  gpu_params->cam_fov[1] = camera.aspect;
  gpu_params->cam_pos_frame[0] = camera.position[0];
  gpu_params->cam_pos_frame[1] = camera.position[1];
  gpu_params->cam_pos_frame[2] = camera.position[2];
  gpu_params->cam_basis_r[0] = camera.right[0];
  gpu_params->cam_basis_r[1] = camera.right[1];
  gpu_params->cam_basis_r[2] = camera.right[2];
  gpu_params->cam_basis_u[0] = camera.up[0];
  gpu_params->cam_basis_u[1] = camera.up[1];
  gpu_params->cam_basis_u[2] = camera.up[2];
  gpu_params->cam_basis_f[0] = camera.forward[0];
  gpu_params->cam_basis_f[1] = camera.forward[1];
  gpu_params->cam_basis_f[2] = camera.forward[2];
#endif
}

::warploom::core::Result<std::vector<float>> VulkanPathTracer::read_image_sum(
    [[maybe_unused]] VkImage image) const {
#ifdef OMNICPP_HAS_VULKAN
  if (!ready_ || allocator_ == nullptr || image == VK_NULL_HANDLE) {
    return ::warploom::core::Result<std::vector<float>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  // Copy one rgba32f image into a host-visible buffer WITHOUT dividing —
  // the shared readback behind read_mean and read_second_moment, which divide
  // by the sample count on the CPU (the same two steps the PT tests perform
  // by hand). Factored so the moment image cannot drift from the accumulation
  // path: one copy sequence, parameterized by image, not two.
  const VkDeviceSize texel = 16U;  // rgba32f
  const VkDeviceSize bytes = texel * width_ * height_;
  auto staging = allocator_->create_buffer(
      bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!staging.is_ok()) {
    return ::warploom::core::Result<std::vector<float>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  Allocation staging_alloc = staging.value();

  auto pool = warploom::render::create_command_pool(device_, graphics_family_);
  if (!pool.is_ok()) {
    allocator_->destroy_allocation(staging_alloc);
    return ::warploom::core::Result<std::vector<float>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  auto cb = warploom::render::allocate_command_buffer(device_, pool.value());
  if (!cb.is_ok()) {
    vkDestroyCommandPool(device_, pool.value(), nullptr);
    allocator_->destroy_allocation(staging_alloc);
    return ::warploom::core::Result<std::vector<float>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  VkCommandBuffer cmd = cb.value();
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);
  // The accumulation image lives in GENERAL (storage images require it); a
  // copy source must be TRANSFER_SRC_OPTIMAL. Assuming a layout instead of
  // transitioning is VUID-vkCmdCopyImage-srcImageLayout-00113, so the barrier
  // is recorded here and the image is handed back in GENERAL so a subsequent
  // record_round() sees the state it left.
  VkImageMemoryBarrier to_transfer{};
  to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  to_transfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  to_transfer.image = image;
  to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U,
                       nullptr, 1U, &to_transfer);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width_, height_, 1};
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         staging_alloc.buffer, 1, &region);
  to_transfer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  to_transfer.dstAccessMask = 0U;
  to_transfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  to_transfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0U, 0U,
                       nullptr, 0U, nullptr, 1U, &to_transfer);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1U;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(graphics_queue_, 1U, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphics_queue_);
  vkDestroyCommandPool(device_, pool.value(), nullptr);

  std::vector<float> sums(static_cast<std::size_t>(width_ * height_) * 4U, 0.0F);
  if (staging_alloc.mapped != nullptr) {
    std::memcpy(sums.data(), staging_alloc.mapped, static_cast<std::size_t>(bytes));
  }
  allocator_->destroy_allocation(staging_alloc);
  return ::warploom::core::Result<std::vector<float>>::ok(std::move(sums));
#else
  return ::warploom::core::Result<std::vector<float>>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<std::vector<float>> VulkanPathTracer::read_mean() const {
#ifdef OMNICPP_HAS_VULKAN
  auto sums = read_image_sum(accum_image_);
  if (!sums.is_ok()) {
    return ::warploom::core::Result<std::vector<float>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  std::vector<float> mean = sums.value();
  const float inv = samples_ > 0U ? 1.0F / static_cast<float>(samples_) : 0.0F;
  for (float& v : mean) v *= inv;
  return ::warploom::core::Result<std::vector<float>>::ok(std::move(mean));
#else
  return ::warploom::core::Result<std::vector<float>>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<std::vector<float>> VulkanPathTracer::read_second_moment()
    const {
#ifdef OMNICPP_HAS_VULKAN
  auto sums = read_image_sum(moment_image_);
  if (!sums.is_ok()) {
    return ::warploom::core::Result<std::vector<float>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  // E[X^2]: the mean of the squared contributions, mirroring read_mean.
  // Variance follows host-side as E[X^2] - E[X]^2 per channel.
  std::vector<float> moment = sums.value();
  const float inv = samples_ > 0U ? 1.0F / static_cast<float>(samples_) : 0.0F;
  for (float& v : moment) v *= inv;
  return ::warploom::core::Result<std::vector<float>>::ok(std::move(moment));
#else
  return ::warploom::core::Result<std::vector<float>>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanPathTracer::destroy_resources() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  ready_ = false;
  if (device_ == VK_NULL_HANDLE) return;

  rt_pipeline_.cleanup(device_);
  if (pipeline_layout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    pipeline_layout_ = VK_NULL_HANDLE;
  }
  descriptor_manager_.reset();
  layout_ = VK_NULL_HANDLE;
  descriptor_set_ = VK_NULL_HANDLE;

  if (allocator_ != nullptr) {
    // AS storage before the allocator that sub-allocated it.
    if (tlas_.handle != nullptr) {
      as_builder_.destroy_tlas(device_, *allocator_, tlas_);
    }
    for (BottomLevelAS& blas : blas_list_) {
      if (blas.handle != nullptr) {
        as_builder_.destroy_blas(device_, *allocator_, blas);
      }
    }
    scratch_.cleanup(*allocator_);
    if (params_allocation_.is_valid()) allocator_->destroy_allocation(params_allocation_);
    if (instance_allocation_.is_valid()) allocator_->destroy_allocation(instance_allocation_);
    if (sbt_allocation_.is_valid()) allocator_->destroy_allocation(sbt_allocation_);
    if (accum_allocation_.is_valid()) allocator_->destroy_allocation(accum_allocation_);
    if (moment_allocation_.is_valid()) allocator_->destroy_allocation(moment_allocation_);
  }
  blas_list_.clear();
  params_allocation_ = Allocation{};
  instance_allocation_ = Allocation{};
  sbt_allocation_ = Allocation{};
  accum_allocation_ = Allocation{};
  moment_allocation_ = Allocation{};
  tlas_ = TopLevelAS{};

  if (accum_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, accum_view_, nullptr);
    accum_view_ = VK_NULL_HANDLE;
  }
  if (accum_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, accum_image_, nullptr);
    accum_image_ = VK_NULL_HANDLE;
  }
  if (moment_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, moment_view_, nullptr);
    moment_view_ = VK_NULL_HANDLE;
  }
  if (moment_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, moment_image_, nullptr);
    moment_image_ = VK_NULL_HANDLE;
  }
#endif
  width_ = 0;
  height_ = 0;
  samples_ = 0;
}

void VulkanPathTracer::destroy() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  destroy_resources();
  device_ = VK_NULL_HANDLE;
  allocator_ = nullptr;
#endif
  failed_ = false;
  ready_ = false;
}

VulkanPathTracer::~VulkanPathTracer() { destroy(); }

}  // namespace warploom::render