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
  // ---- First-hit G-buffer image (rgba32f storage, same creation path) ----
  // Geometry signal for the T2 denoiser: normal + depth overwritten every
  // unmasked round. A second copy of the creation block would be a second
  // place for the usage flags to drift, so it goes through the same helper.
  if (!make_storage_image(gbuffer_image_, gbuffer_view_, gbuffer_allocation_)) {
    return ::warploom::core::Result<void>::ok();  // refuse() already ran
  }
  // ---- Per-texel count image (r32ui storage) + adaptive mask buffer ------
  // The count travels with the sums (cleared/incremented together) so the
  // uniform path carries per-pixel counts equal to sample_count() by
  // construction. Masked rounds skip held texels entirely; their counts
  // stay behind, which is what makes read_adaptive_mean() exact.
  {
    VkImageCreateInfo count_info{};
    count_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    count_info.imageType = VK_IMAGE_TYPE_2D;
    count_info.format = VK_FORMAT_R32_UINT;
    count_info.extent = {width_, height_, 1};
    count_info.mipLevels = 1;
    count_info.arrayLayers = 1;
    count_info.samples = VK_SAMPLE_COUNT_1_BIT;
    count_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    count_info.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    count_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &count_info, nullptr, &count_image_) !=
        VK_SUCCESS) {
      return refuse("count image creation failed");
    }
    auto mem = allocator.bind_image(count_image_,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!mem.is_ok()) return refuse("count image memory failed");
    count_allocation_ = mem.value();
    VkImageViewCreateInfo count_view_info{};
    count_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    count_view_info.image = count_image_;
    count_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    count_view_info.format = VK_FORMAT_R32_UINT;
    count_view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                        1};
    if (vkCreateImageView(device_, &count_view_info, nullptr,
                          &count_view_) != VK_SUCCESS) {
      return refuse("count image view failed");
    }
    const VkDeviceSize mask_bytes = static_cast<VkDeviceSize>(width_) *
                                    static_cast<VkDeviceSize>(height_) *
                                    sizeof(std::uint32_t);
    auto mask_buf = allocator.create_buffer(
        mask_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mask_buf.is_ok()) return refuse("adaptive mask buffer failed");
    mask_allocation_ = mask_buf.value();
    std::memset(mask_allocation_.mapped, 0,
                static_cast<std::size_t>(mask_bytes));
  }
  adaptive_enabled_ = false;

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
    // TLAS instances: one per config instance, each selecting its mesh.
    // mesh_index 0xFFFFFFFF is the identity mapping (instance i -> mesh i);
    // anything else shares one BLAS across instances. An out-of-range index
    // refuses cleanly: a TLAS entry naming no BLAS would fault the build
    // with no validation error to explain it.
    std::vector<TlasInstance> instances(config.instances.size());
    for (std::size_t i = 0; i < config.instances.size(); ++i) {
      const std::uint32_t mesh = config.instances[i].mesh_index == 0xFFFFFFFFU
                                     ? static_cast<std::uint32_t>(i)
                                     : config.instances[i].mesh_index;
      if (mesh >= static_cast<std::uint32_t>(config.meshes.size())) {
        WARPLOOM_WARN("render", "path tracer: instance %zu names mesh %u of %zu\n", i,
                      mesh, config.meshes.size());
        return false;
      }
      std::memcpy(instances[i].transform, config.instances[i].transform.data(),
                  sizeof(instances[i].transform));
      instances[i].instance_custom_index = config.instances[i].custom_index;
      instances[i].mask = 0xFFU;
      instances[i].sbt_offset = 0U;
      instances[i].flags = 0U;
      instances[i].blas_device_address = blas_list_[mesh].device_address;
      if (instances[i].blas_device_address == 0U) {
        WARPLOOM_WARN("render", "path tracer: BLAS %u has no device address\n", mesh);
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
  // T2 adaptive: 5 = mask storage buffer (raygen), 6 = count storage image
  // (raygen). T2 G-buffer: 7 = first-hit normal+depth storage image
  // (raygen, overwritten per round). The rgen skips masked-out texels and
  // increments the count exactly when it adds to the sums.
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
        VK_SHADER_STAGE_RAYGEN_BIT_KHR},
       {0, 5, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR},
       {0, 6, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR},
       {0, 7, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
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

  // ---- Denoised-mean output image (same creation path: the tonemap will ----
  // ---- sample it, so it needs the SAMPLED bit like the accumulation) -----
  if (!make_storage_image(denoise_image_, denoise_view_, denoise_allocation_)) {
    return ::warploom::core::Result<void>::ok();  // refuse() already ran
  }

  // ---- Denoise compute pipeline + descriptor set ---------------------------
  // Bindings 0-3 mirror the spike shader's (sums, moment, G-buffer, out);
  // binding 4 is the count image, the one way this shader differs from
  // denoise_edge_atrous: per-texel divisors instead of scalar counts.
  auto denoise_layout = descriptor_manager_->create_layout(
      {{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_SHADER_STAGE_COMPUTE_BIT},
       {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_SHADER_STAGE_COMPUTE_BIT},
       {0, 2, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_SHADER_STAGE_COMPUTE_BIT},
       {0, 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_SHADER_STAGE_COMPUTE_BIT},
       {0, 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_SHADER_STAGE_COMPUTE_BIT}},
      1U);
  if (!denoise_layout.is_ok()) return refuse("denoise layout failed");
  denoise_layout_ = denoise_layout.value();
  auto denoise_set = descriptor_manager_->allocate_set(denoise_layout_);
  if (!denoise_set.is_ok()) return refuse("denoise set failed");
  denoise_set_ = denoise_set.value();
  if (!denoise_pipeline_
           .load_shader_stage_file(
               device_, shader_dir_ + "/denoise_edge_counts.comp.spv",
               "compute")
           .is_ok()) {
    return refuse("denoise shader load failed");
  }
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28U};
    if (!denoise_pipeline_
             .create_pipeline_layout(device_, &denoise_layout_, 1, &push)
             .is_ok()) {
      return refuse("denoise pipeline layout failed");
    }
  }
  if (!denoise_pipeline_.create_compute_pipeline(device_, denoise_pipeline_.pipeline_layout())
           .is_ok()) {
    return refuse("denoise pipeline creation failed");
  }
  if (!descriptor_manager_
           ->write_image(denoise_set_, 0U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, accum_view_, VK_IMAGE_LAYOUT_GENERAL,
                         0U)
           .is_ok() ||
      !descriptor_manager_
           ->write_image(denoise_set_, 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, moment_view_, VK_IMAGE_LAYOUT_GENERAL,
                         0U)
           .is_ok() ||
      !descriptor_manager_
           ->write_image(denoise_set_, 2U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, gbuffer_view_, VK_IMAGE_LAYOUT_GENERAL,
                         0U)
           .is_ok() ||
      !descriptor_manager_
           ->write_image(denoise_set_, 3U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, denoise_view_, VK_IMAGE_LAYOUT_GENERAL,
                         0U)
           .is_ok() ||
      !descriptor_manager_
           ->write_image(denoise_set_, 4U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, count_view_, VK_IMAGE_LAYOUT_GENERAL,
                         0U)
           .is_ok()) {
    return refuse("denoise descriptor write failed");
  }
  denoise_laid_out_ = false;

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
           .is_ok() ||
      !descriptor_manager_
           ->write_buffer(descriptor_set_, 5U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          mask_allocation_.buffer, 0, VK_WHOLE_SIZE)
           .is_ok() ||
      !descriptor_manager_
           ->write_image(descriptor_set_, 6U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, count_view_, VK_IMAGE_LAYOUT_GENERAL, 0U)
           .is_ok() ||
      !descriptor_manager_
           ->write_image(descriptor_set_, 7U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         VK_NULL_HANDLE, gbuffer_view_, VK_IMAGE_LAYOUT_GENERAL, 0U)
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

  // Live-PT stepping discipline: a camera move since the last round restarts
  // the sequence even when the caller forgot clear_first (the smear is
  // unrepresentable through this API). Consume the flag here, before the
  // hold check, so a move also releases a held-complete sequence: holding
  // an image for a pose it was not traced from is the same defect.
  const bool camera_moved = camera_dirty_;
  camera_dirty_ = false;

  // R1 stepped-PT hold: at target the accumulation is complete, so further
  // rounds no-op instead of drifting the mean. A clear always restarts the
  // sequence (viewport wrap-around, deterministic re-runs), and the
  // viewport additionally skips the submit while held, so this is the
  // backstop, not the hot path.
  if (!clear_first && !camera_moved && sample_target_ > 0U &&
      samples_ >= sample_target_) {
    return;
  }

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
  // All accumulation targets travel together: the moment + count + G-buffer
  // images get the same barrier shapes as the accumulation, with only
  // .image differing. A single-image barrier here would leave the moment
  // image's layout and visibility to chance -- the same silent-race class
  // as the missing SHADER_READ bit below (no validation diagnostic, wrong
  // picture). The count image joins the same group: it is cleared and
  // incremented exactly when the sums are. The G-buffer joins it too: it
  // is overwritten (not accumulated) every unmasked round, so it needs
  // the same layout transitions and write visibility, just no add.
  const auto emit_all = [&](const VkImageMemoryBarrier& shape,
                            VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    VkImageMemoryBarrier group[4] = {shape, shape, shape, shape};
    group[0].image = accum_image_;
    group[1].image = moment_image_;
    group[2].image = count_image_;
    group[3].image = gbuffer_image_;
    vkCmdPipelineBarrier(command_buffer, src, dst, 0, 0, nullptr, 0, nullptr,
                         4, group);
  };
  // A camera move clears exactly like clear_first: same barriers, same
  // zeroes, same count restart. One code path for both, so the restarted
  // image cannot differ from a caller-cleared one.
  if (clear_first || camera_moved) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    emit_all(barrier, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black{};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command_buffer, accum_image_, VK_IMAGE_LAYOUT_GENERAL,
                         &black, 1, &range);
    vkCmdClearColorImage(command_buffer, moment_image_, VK_IMAGE_LAYOUT_GENERAL,
                         &black, 1, &range);
    // G-buffer clear = the miss encoding: no normal, t = -1. A round that
    // traces then overwrites every unmasked texel, so this value survives
    // only where nothing was traced.
    VkClearColorValue gclear{};
    gclear.float32[0] = 0.0F;
    gclear.float32[1] = 0.0F;
    gclear.float32[2] = 0.0F;
    gclear.float32[3] = -1.0F;
    vkCmdClearColorImage(command_buffer, gbuffer_image_, VK_IMAGE_LAYOUT_GENERAL,
                         &gclear, 1, &range);
    VkClearColorValue zero_count{};
    zero_count.uint32[0] = 0U;
    vkCmdClearColorImage(command_buffer, count_image_, VK_IMAGE_LAYOUT_GENERAL,
                         &zero_count, 1, &range);
    // Clear -> shader read/write so the trace both reads (nothing yet) and
    // writes the frame's contribution into GENERAL.
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    emit_all(barrier, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  } else {
    // Cross-submission visibility: the previous round's shader write must be
    // visible to this round's read-modify-write.
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    emit_all(barrier, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
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
  emit_all(barrier, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
           VK_PIPELINE_STAGE_TRANSFER_BIT |
               VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

  if (clear_first || camera_moved) {
    samples_ = 1U;
  } else if (samples_ > 0U) {
    ++samples_;
  }
#endif
}

void VulkanPathTracer::record_denoise([[maybe_unused]] VkCommandBuffer command_buffer) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (!ready_ || command_buffer == VK_NULL_HANDLE) return;

  // Input scope: the round's trace writes (storage images, GENERAL). The
  // count image was cleared by TRANSFER like the sums, so its last write
  // may be either -- cover both source stages, exactly like the spike's
  // barrier_inputs (RT | TRANSFER -> COMPUTE read).
  VkImageMemoryBarrier in{};
  in.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  in.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  in.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  in.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  in.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  in.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  in.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  in.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier ins[4] = {in, in, in, in};
  ins[0].image = accum_image_;
  ins[1].image = moment_image_;
  ins[2].image = gbuffer_image_;
  ins[3].image = count_image_;
  vkCmdPipelineBarrier(command_buffer,
                       VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                       0, nullptr, 4, ins);

  // First dispatch lays the output out of UNDEFINED; it persists GENERAL
  // across frames (same lifetime rule as the accumulation), so later
  // dispatches only need the execution edge, not the transition.
  if (!denoise_laid_out_) {
    VkImageMemoryBarrier prep{};
    prep.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prep.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    prep.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    prep.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    prep.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prep.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prep.image = denoise_image_;
    prep.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &prep);
    denoise_laid_out_ = true;
  }

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    denoise_pipeline_.pipeline());
  VkDescriptorSet set = denoise_set_;
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          denoise_pipeline_.pipeline_layout(), 0, 1, &set, 0,
                          nullptr);
  // Measured-best loop30 widths (k=2.0, spatial=1.5; normal stop inert on
  // planar scenes so 0.1 is the sweep middle; depth=1.0 separates
  // silhouettes), stride 1: the single-iteration pin. No tuning API: the
  // values are a measured result, baked where the measurement lives.
  struct DenoisePush {
    std::uint32_t w, h;
    float k, sigma, ns, ds;
    std::uint32_t stride;
  };
  static_assert(sizeof(DenoisePush) == 28U, "push layout drifted");
  const DenoisePush p{width_, height_, 2.0F, 1.5F, 0.1F, 1.0F, 1U};
  vkCmdPushConstants(command_buffer, denoise_pipeline_.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DenoisePush), &p);
  vkCmdDispatch(command_buffer, (width_ + 7U) / 8U, (height_ + 7U) / 8U, 1U);

  // Output scope: GENERAL -> GENERAL, compute-write visible to a later
  // shader read (the tonemap samples it) and to transfer (read_denoised).
  // The compose chain's own source-scope barrier (updated to the compute
  // stage when the viewport points the external source here) chains the
  // cross-submit capture read; this barrier keeps the object safe for
  // standalone callers and orders the same-submit tonemap.
  VkImageMemoryBarrier out{};
  out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  out.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  out.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  out.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  out.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  out.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  out.image = denoise_image_;
  out.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &out);
#endif
}

::warploom::core::Result<std::vector<float>> VulkanPathTracer::read_denoised() const {
  // The denoised image holds a filtered MEAN, not sums: copy without
  // dividing, exactly like read_gbuffer.
  return read_image_sum(denoise_image_);
}

void VulkanPathTracer::set_camera([[maybe_unused]] const PathTracerCamera& camera) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (params_allocation_.mapped == nullptr) return;
  auto* gpu_params = static_cast<TracerGpuParams*>(params_allocation_.mapped);
  // Change detection BEFORE the write: any field-wise difference in the
  // pose/fov the trace reads marks the sequence stale. Field-wise == (not
  // memcmp) so -0.0 vs +0.0 does not restart; the frame seed word
  // (cam_pos_frame[3]) is excluded -- record_round owns it and it advances
  // every round by construction.
  // Exact comparison is correct here: change detection needs bitwise-level
  // sensitivity, and != (not memcmp) so -0.0 vs +0.0 does not restart.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
  const bool moved =
      gpu_params->cam_fov[0] != camera.tan_half_fov ||
      gpu_params->cam_fov[1] != camera.aspect ||
      gpu_params->cam_pos_frame[0] != camera.position[0] ||
      gpu_params->cam_pos_frame[1] != camera.position[1] ||
      gpu_params->cam_pos_frame[2] != camera.position[2] ||
      gpu_params->cam_basis_r[0] != camera.right[0] ||
      gpu_params->cam_basis_r[1] != camera.right[1] ||
      gpu_params->cam_basis_r[2] != camera.right[2] ||
      gpu_params->cam_basis_u[0] != camera.up[0] ||
      gpu_params->cam_basis_u[1] != camera.up[1] ||
      gpu_params->cam_basis_u[2] != camera.up[2] ||
      gpu_params->cam_basis_f[0] != camera.forward[0] ||
      gpu_params->cam_basis_f[1] != camera.forward[1] ||
      gpu_params->cam_basis_f[2] != camera.forward[2];
#pragma GCC diagnostic pop
  if (moved) camera_dirty_ = true;
  gpu_params->cam_fov[0] = camera.tan_half_fov;
  gpu_params->cam_fov[1] = camera.aspect;
  // Adaptive flag (cam_fov[2]) survives camera uploads: set_camera owns the
  // pose/fov words, the adaptive toggle owns the enable word. Overwriting
  // it here would silently disable masking on every camera hold.
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

::warploom::core::Result<std::vector<float>> VulkanPathTracer::read_gbuffer()
    const {
#ifdef OMNICPP_HAS_VULKAN
  // No divide: the G-buffer is overwritten per round, not accumulated.
  // read_image_sum copies WITHOUT dividing, which is exactly the raw
  // texel contract here -- shared path, not a second copy sequence.
  return read_image_sum(gbuffer_image_);
#else
  return ::warploom::core::Result<std::vector<float>>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanPathTracer::set_adaptive_enabled(
    [[maybe_unused]] bool enabled) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  adaptive_enabled_ = enabled;
  if (params_allocation_.mapped != nullptr) {
    auto* gpu_params = static_cast<TracerGpuParams*>(params_allocation_.mapped);
    gpu_params->cam_fov[2] = enabled ? 1.0F : 0.0F;
  }
#else
  (void)enabled;
#endif
}

bool VulkanPathTracer::upload_adaptive_mask(
    [[maybe_unused]] const std::vector<std::uint32_t>& mask) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  const std::size_t texels =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  if (mask.size() != texels || mask_allocation_.mapped == nullptr) return false;
  std::memcpy(mask_allocation_.mapped, mask.data(),
              texels * sizeof(std::uint32_t));
  return true;
#else
  (void)mask;
  return false;
#endif
}

::warploom::core::Result<std::vector<std::uint32_t>>
VulkanPathTracer::read_per_pixel_counts() const {
#ifdef OMNICPP_HAS_VULKAN
  if (!ready_ || allocator_ == nullptr || count_image_ == VK_NULL_HANDLE) {
    return ::warploom::core::Result<std::vector<std::uint32_t>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  const VkDeviceSize texel = 4U;  // r32ui
  const VkDeviceSize bytes =
      texel * static_cast<VkDeviceSize>(width_) * static_cast<VkDeviceSize>(height_);
  auto staging = allocator_->create_buffer(
      bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!staging.is_ok()) {
    return ::warploom::core::Result<std::vector<std::uint32_t>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  Allocation staging_alloc = staging.value();
  auto pool = warploom::render::create_command_pool(device_, graphics_family_);
  if (!pool.is_ok()) {
    allocator_->destroy_allocation(staging_alloc);
    return ::warploom::core::Result<std::vector<std::uint32_t>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  auto cb = warploom::render::allocate_command_buffer(device_, pool.value());
  if (!cb.is_ok()) {
    vkDestroyCommandPool(device_, pool.value(), nullptr);
    allocator_->destroy_allocation(staging_alloc);
    return ::warploom::core::Result<std::vector<std::uint32_t>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  VkCommandBuffer cmd = cb.value();
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);
  VkImageMemoryBarrier to_transfer{};
  to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  to_transfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  to_transfer.image = count_image_;
  to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U,
                       nullptr, 1U, &to_transfer);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width_, height_, 1};
  vkCmdCopyImageToBuffer(cmd, count_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
  std::vector<std::uint32_t> counts(static_cast<std::size_t>(width_) *
                                    static_cast<std::size_t>(height_),
                                    0U);
  if (staging_alloc.mapped != nullptr) {
    std::memcpy(counts.data(), staging_alloc.mapped,
                static_cast<std::size_t>(bytes));
  }
  allocator_->destroy_allocation(staging_alloc);
  return ::warploom::core::Result<std::vector<std::uint32_t>>::ok(
      std::move(counts));
#else
  return ::warploom::core::Result<std::vector<std::uint32_t>>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<std::vector<float>> VulkanPathTracer::read_adaptive_mean()
    const {
#ifdef OMNICPP_HAS_VULKAN
  auto sums = read_image_sum(accum_image_);
  auto counts = read_per_pixel_counts();
  if (!sums.is_ok() || !counts.is_ok()) {
    return ::warploom::core::Result<std::vector<float>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  std::vector<float> mean = sums.value();
  const std::vector<std::uint32_t>& counts_value = counts.value();
  const std::size_t texels = counts_value.size();
  if (mean.size() != texels * 4U) {
    return ::warploom::core::Result<std::vector<float>>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  for (std::size_t t = 0; t < texels; ++t) {
    const float inv = counts_value[t] > 0U
                          ? 1.0F / static_cast<float>(counts_value[t])
                          : 0.0F;
    for (std::size_t c = 0; c < 4U; ++c) mean[t * 4U + c] *= inv;
  }
  return ::warploom::core::Result<std::vector<float>>::ok(std::move(mean));
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
    if (denoise_allocation_.is_valid()) allocator_->destroy_allocation(denoise_allocation_);
    if (moment_allocation_.is_valid()) allocator_->destroy_allocation(moment_allocation_);
    if (count_allocation_.is_valid()) allocator_->destroy_allocation(count_allocation_);
    if (gbuffer_allocation_.is_valid()) allocator_->destroy_allocation(gbuffer_allocation_);
    if (mask_allocation_.is_valid()) allocator_->destroy_allocation(mask_allocation_);
  }
  denoise_pipeline_.cleanup(device_);
  denoise_layout_ = VK_NULL_HANDLE;
  denoise_set_ = VK_NULL_HANDLE;
  blas_list_.clear();
  params_allocation_ = Allocation{};
  instance_allocation_ = Allocation{};
  sbt_allocation_ = Allocation{};
  accum_allocation_ = Allocation{};
  denoise_allocation_ = Allocation{};
  moment_allocation_ = Allocation{};
  count_allocation_ = Allocation{};
  gbuffer_allocation_ = Allocation{};
  mask_allocation_ = Allocation{};
  tlas_ = TopLevelAS{};

  if (accum_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, accum_view_, nullptr);
    accum_view_ = VK_NULL_HANDLE;
  }
  if (accum_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, accum_image_, nullptr);
    accum_image_ = VK_NULL_HANDLE;
  }
  if (denoise_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, denoise_view_, nullptr);
    denoise_view_ = VK_NULL_HANDLE;
  }
  if (denoise_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, denoise_image_, nullptr);
    denoise_image_ = VK_NULL_HANDLE;
  }
  if (moment_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, moment_view_, nullptr);
    moment_view_ = VK_NULL_HANDLE;
  }
  if (moment_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, moment_image_, nullptr);
    moment_image_ = VK_NULL_HANDLE;
  }
  if (count_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, count_view_, nullptr);
    count_view_ = VK_NULL_HANDLE;
  }
  if (count_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, count_image_, nullptr);
    count_image_ = VK_NULL_HANDLE;
  }
  if (gbuffer_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, gbuffer_view_, nullptr);
    gbuffer_view_ = VK_NULL_HANDLE;
  }
  if (gbuffer_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, gbuffer_image_, nullptr);
    gbuffer_image_ = VK_NULL_HANDLE;
  }
#endif
  width_ = 0;
  height_ = 0;
  samples_ = 0;
  camera_dirty_ = false;
  adaptive_enabled_ = false;
  denoise_enabled_ = false;
  denoise_laid_out_ = false;
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