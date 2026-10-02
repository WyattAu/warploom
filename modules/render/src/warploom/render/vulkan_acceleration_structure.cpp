//! @file vulkan_acceleration_structure.cpp
//! @brief BLAS/TLAS builds on caller command buffers (see the header).
//!
//! RT entry points are extension functions: the Vulkan loader does not export
//! them directly, so they are fetched via vkGetDeviceProcAddr and cached in a
//! per-device table (the renderer is single-threaded per device, matching the
//! rest of the codebase's contract).

#include "warploom/render/vulkan_acceleration_structure.hpp"

#include <cstring>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

#ifdef OMNICPP_HAS_VULKAN

namespace {

//! Cached extension entry points for one VkDevice.
struct RtFuncs {
  PFN_vkGetAccelerationStructureBuildSizesKHR build_sizes{nullptr};
  PFN_vkCreateAccelerationStructureKHR create_as{nullptr};
  PFN_vkDestroyAccelerationStructureKHR destroy_as{nullptr};
  PFN_vkGetAccelerationStructureDeviceAddressKHR as_address{nullptr};
  PFN_vkCmdBuildAccelerationStructuresKHR cmd_build{nullptr};
  VkDevice device{VK_NULL_HANDLE};

  bool ready() const noexcept { return device != VK_NULL_HANDLE && cmd_build != nullptr; }
};

RtFuncs g_rt_funcs;

const RtFuncs& rt_for(VkDevice device) noexcept {
  if (g_rt_funcs.device != device) {
    g_rt_funcs = RtFuncs{};
    g_rt_funcs.device = device;
    g_rt_funcs.build_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    g_rt_funcs.create_as = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    g_rt_funcs.destroy_as = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
    g_rt_funcs.as_address =
        reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    g_rt_funcs.cmd_build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
  }
  return g_rt_funcs;
}

VkAccelerationStructureBuildSizesInfoKHR query_sizes(
    VkDevice device,
    const VkAccelerationStructureBuildGeometryInfoKHR& build_info,
    std::uint32_t max_primitive_count) noexcept {
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  const RtFuncs& rt = rt_for(device);
  if (rt.build_sizes != nullptr) {
    rt.build_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                   &build_info, &max_primitive_count, &sizes);
  }
  return sizes;
}

//! Core (1.2) buffer-address query — exported by the loader directly.
VkDeviceAddress query_buffer_address(VkDevice device, VkBuffer buffer) noexcept {
  VkBufferDeviceAddressInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  info.buffer = buffer;
  return vkGetBufferDeviceAddress(device, &info);
}

}  // namespace

// ---------------------------------------------------------------------------
// VulkanScratchPool
// ---------------------------------------------------------------------------

core::Result<std::uint64_t> VulkanScratchPool::acquire(
    VulkanMemoryAllocator& allocator, VkDevice device, std::uint64_t bytes) {
  if (bytes == 0U) {
    return core::Result<std::uint64_t>::error(
        core::RuntimeError::invalid_config);
  }
  if (bytes > capacity_) {
    if (buffer_ != VK_NULL_HANDLE) {
      allocator.destroy_allocation(allocation_);
      buffer_ = VK_NULL_HANDLE;
      capacity_ = 0U;
    }
    auto alloc = allocator.create_buffer(
        static_cast<VkDeviceSize>(bytes),
        // Modern spec: scratch needs no dedicated usage bit — it is consumed
        // through build_info.scratchData, so addressability is the contract.
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!alloc.is_ok()) {
      return core::Result<std::uint64_t>::error(alloc.error());
    }
    allocation_ = alloc.value();
    buffer_ = allocation_.buffer;
    capacity_ = bytes;
  }
  const VkDeviceAddress address = query_buffer_address(device, buffer_);
  if (address == 0U) {
    return core::Result<std::uint64_t>::error(
        core::RuntimeError::vulkan_not_available);
  }
  return core::Result<std::uint64_t>::ok(static_cast<std::uint64_t>(address));
}

void VulkanScratchPool::cleanup(VulkanMemoryAllocator& allocator) noexcept {
  if (buffer_ != VK_NULL_HANDLE) {
    allocator.destroy_allocation(allocation_);
    buffer_ = VK_NULL_HANDLE;
    capacity_ = 0U;
  }
}

// ---------------------------------------------------------------------------
// Size queries
// ---------------------------------------------------------------------------

VkAccelerationStructureBuildSizesInfoKHR
VulkanAccelerationStructureBuilder::query_blas_sizes(
    VkDevice device, const BlasBuildInput& input) noexcept {
  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geometry.geometry.triangles.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  geometry.geometry.triangles.maxVertex = input.max_vertex;
  geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
  geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

  VkAccelerationStructureBuildGeometryInfoKHR build_info{};
  build_info.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build_info.geometryCount = 1U;
  build_info.pGeometries = &geometry;

  return query_sizes(device, build_info,
                     static_cast<std::uint32_t>(input.triangle_count));
}

VkAccelerationStructureBuildSizesInfoKHR
VulkanAccelerationStructureBuilder::query_tlas_sizes(
    VkDevice device, std::uint32_t instance_count) noexcept {
  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

  VkAccelerationStructureBuildGeometryInfoKHR build_info{};
  build_info.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build_info.geometryCount = 1U;
  build_info.pGeometries = &geometry;

  return query_sizes(device, build_info, instance_count);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

core::Result<std::uint64_t>
VulkanAccelerationStructureBuilder::buffer_device_address(
    VkDevice device, VkBuffer buffer) noexcept {
  const VkDeviceAddress address = query_buffer_address(device, buffer);
  if (address == 0U) {
    return core::Result<std::uint64_t>::error(
        core::RuntimeError::vulkan_not_available);
  }
  return core::Result<std::uint64_t>::ok(static_cast<std::uint64_t>(address));
}

// ---------------------------------------------------------------------------
// BLAS
// ---------------------------------------------------------------------------

core::Result<BottomLevelAS> VulkanAccelerationStructureBuilder::create_blas(
    VkDevice device, VulkanMemoryAllocator& allocator,
    const BlasBuildInput& input) const {
  if (device == VK_NULL_HANDLE || input.vertex_buffer_address == 0U ||
      input.triangle_count == 0U) {
    return core::Result<BottomLevelAS>::error(
        core::RuntimeError::invalid_config);
  }
  const RtFuncs& rt = rt_for(device);
  if (!rt.ready()) {
    return core::Result<BottomLevelAS>::error(
        core::RuntimeError::vulkan_not_available);
  }
  const auto sizes = query_blas_sizes(device, input);
  if (sizes.accelerationStructureSize == 0U) {
    return core::Result<BottomLevelAS>::error(
        core::RuntimeError::vulkan_not_available);
  }

  BottomLevelAS blas{};
  auto storage = allocator.create_buffer(
      static_cast<VkDeviceSize>(sizes.accelerationStructureSize),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!storage.is_ok()) {
    return core::Result<BottomLevelAS>::error(storage.error());
  }
  blas.storage = storage.value();
  blas.storage_buffer = blas.storage.buffer;
  blas.storage_bytes = sizes.accelerationStructureSize;

  VkAccelerationStructureCreateInfoKHR create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  create_info.buffer = blas.storage_buffer;
  create_info.offset = 0U;
  create_info.size = sizes.accelerationStructureSize;
  create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  if (rt.create_as(device, &create_info, nullptr, &blas.handle) != VK_SUCCESS) {
    allocator.destroy_allocation(blas.storage);
    return core::Result<BottomLevelAS>::error(
        core::RuntimeError::vulkan_not_available);
  }
  VkAccelerationStructureDeviceAddressInfoKHR addr_info{};
  addr_info.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
  addr_info.accelerationStructure = blas.handle;
  blas.device_address = static_cast<std::uint64_t>(
      rt.as_address(device, &addr_info));
  if (blas.device_address == 0U) {
    rt.destroy_as(device, blas.handle, nullptr);
    allocator.destroy_allocation(blas.storage);
    return core::Result<BottomLevelAS>::error(
        core::RuntimeError::vulkan_not_available);
  }
  return core::Result<BottomLevelAS>::ok(blas);
}

core::Result<void> VulkanAccelerationStructureBuilder::cmd_build_blas(
    VkCommandBuffer cmd, VkDevice device, const BottomLevelAS& blas,
    const BlasBuildInput& input, std::uint64_t scratch_address) const {
  const RtFuncs& rt = rt_for(device);
  if (cmd == VK_NULL_HANDLE || !rt.ready() || blas.handle == VK_NULL_HANDLE ||
      input.vertex_buffer_address == 0U || input.triangle_count == 0U ||
      scratch_address == 0U) {
    return core::Result<void>::error(core::RuntimeError::invalid_config);
  }

  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geometry.geometry.triangles.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  geometry.geometry.triangles.maxVertex = input.max_vertex;
  geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
  geometry.geometry.triangles.vertexData.deviceAddress =
      static_cast<VkDeviceAddress>(input.vertex_buffer_address);
  geometry.geometry.triangles.vertexStride = 3U * sizeof(float);
  geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

  VkAccelerationStructureBuildGeometryInfoKHR build_info{};
  build_info.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build_info.geometryCount = 1U;
  build_info.pGeometries = &geometry;
  build_info.srcAccelerationStructure = VK_NULL_HANDLE;
  build_info.dstAccelerationStructure = blas.handle;
  build_info.scratchData.deviceAddress =
      static_cast<VkDeviceAddress>(scratch_address);

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = static_cast<std::uint32_t>(input.triangle_count);
  range.firstVertex = 0U;
  VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
  rt.cmd_build(cmd, 1U, &build_info, &range_ptr);
  return core::Result<void>::ok();
}

// ---------------------------------------------------------------------------
// TLAS
// ---------------------------------------------------------------------------

core::Result<TopLevelAS> VulkanAccelerationStructureBuilder::create_tlas(
    VkDevice device, VulkanMemoryAllocator& allocator,
    std::uint32_t instance_count) const {
  if (device == VK_NULL_HANDLE || instance_count == 0U) {
    return core::Result<TopLevelAS>::error(core::RuntimeError::invalid_config);
  }
  const RtFuncs& rt = rt_for(device);
  if (!rt.ready()) {
    return core::Result<TopLevelAS>::error(
        core::RuntimeError::vulkan_not_available);
  }
  const auto sizes = query_tlas_sizes(device, instance_count);
  if (sizes.accelerationStructureSize == 0U) {
    return core::Result<TopLevelAS>::error(
        core::RuntimeError::vulkan_not_available);
  }
  TopLevelAS tlas{};
  auto storage = allocator.create_buffer(
      static_cast<VkDeviceSize>(sizes.accelerationStructureSize),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!storage.is_ok()) {
    return core::Result<TopLevelAS>::error(storage.error());
  }
  tlas.storage = storage.value();
  tlas.storage_buffer = tlas.storage.buffer;
  tlas.storage_bytes = sizes.accelerationStructureSize;
  tlas.capacity = instance_count;

  VkAccelerationStructureCreateInfoKHR create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  create_info.buffer = tlas.storage_buffer;
  create_info.offset = 0U;
  create_info.size = sizes.accelerationStructureSize;
  create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  if (rt.create_as(device, &create_info, nullptr, &tlas.handle) != VK_SUCCESS) {
    allocator.destroy_allocation(tlas.storage);
    return core::Result<TopLevelAS>::error(
        core::RuntimeError::vulkan_not_available);
  }

  // Host-visible, host-coherent instance buffer: the CPU writes instance
  // structs each frame and the build reads them by device address.
  const VkDeviceSize instance_bytes =
      static_cast<VkDeviceSize>(instance_count) *
      sizeof(VkAccelerationStructureInstanceKHR);
  auto instances = allocator.create_buffer(
      instance_bytes,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!instances.is_ok()) {
    rt.destroy_as(device, tlas.handle, nullptr);
    allocator.destroy_allocation(tlas.storage);
    return core::Result<TopLevelAS>::error(instances.error());
  }
  tlas.instances = instances.value();
  tlas.instances_buffer = tlas.instances.buffer;
  tlas.instance_buffer_address =
      static_cast<std::uint64_t>(query_buffer_address(device, tlas.instances_buffer));
  if (tlas.instance_buffer_address == 0U) {
    rt.destroy_as(device, tlas.handle, nullptr);
    allocator.destroy_allocation(tlas.instances);
    allocator.destroy_allocation(tlas.storage);
    return core::Result<TopLevelAS>::error(
        core::RuntimeError::vulkan_not_available);
  }
  return core::Result<TopLevelAS>::ok(tlas);
}

core::Result<void> VulkanAccelerationStructureBuilder::cmd_build_tlas(
    VkCommandBuffer cmd, VkDevice device, const TopLevelAS& tlas,
    const TlasInstance* instances, std::uint32_t instance_count,
    std::uint64_t scratch_address) const {
  const RtFuncs& rt = rt_for(device);
  if (cmd == VK_NULL_HANDLE || !rt.ready() || tlas.handle == VK_NULL_HANDLE ||
      instances == nullptr || instance_count == 0U ||
      instance_count > tlas.capacity || scratch_address == 0U ||
      tlas.instances.mapped == nullptr) {
    return core::Result<void>::error(core::RuntimeError::invalid_config);
  }

  auto* out = static_cast<VkAccelerationStructureInstanceKHR*>(
      tlas.instances.mapped);
  for (std::uint32_t i = 0; i < instance_count; ++i) {
    const TlasInstance& in = instances[i];
    if (in.blas_device_address == 0U) {
      return core::Result<void>::error(core::RuntimeError::invalid_config);
    }
    VkAccelerationStructureInstanceKHR& dst = out[i];
    std::memcpy(dst.transform.matrix, in.transform,
                sizeof(dst.transform.matrix));
    dst.instanceCustomIndex = in.instance_custom_index;
    dst.mask = in.mask;
    dst.instanceShaderBindingTableRecordOffset = in.sbt_offset;
    dst.flags = in.flags;
    dst.accelerationStructureReference =
        static_cast<VkDeviceAddress>(in.blas_device_address);
  }
  // HOST_COHERENT: no flush needed.

  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry.geometry.instances.data.deviceAddress =
      static_cast<VkDeviceAddress>(tlas.instance_buffer_address);
  geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

  VkAccelerationStructureBuildGeometryInfoKHR build_info{};
  build_info.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build_info.geometryCount = 1U;
  build_info.pGeometries = &geometry;
  build_info.srcAccelerationStructure = VK_NULL_HANDLE;
  build_info.dstAccelerationStructure = tlas.handle;
  build_info.scratchData.deviceAddress =
      static_cast<VkDeviceAddress>(scratch_address);

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = instance_count;
  VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
  rt.cmd_build(cmd, 1U, &build_info, &range_ptr);
  return core::Result<void>::ok();
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void VulkanAccelerationStructureBuilder::destroy_blas(
    VkDevice device, VulkanMemoryAllocator& allocator,
    BottomLevelAS& blas) noexcept {
  const RtFuncs& rt = rt_for(device);
  if (blas.handle != VK_NULL_HANDLE && rt.destroy_as != nullptr) {
    rt.destroy_as(device, blas.handle, nullptr);
    blas.handle = VK_NULL_HANDLE;
  }
  if (blas.storage.is_valid()) {
    allocator.destroy_allocation(blas.storage);
    blas.storage_buffer = VK_NULL_HANDLE;
  }
  blas.device_address = 0U;
  blas.storage_bytes = 0U;
}

void VulkanAccelerationStructureBuilder::destroy_tlas(
    VkDevice device, VulkanMemoryAllocator& allocator,
    TopLevelAS& tlas) noexcept {
  const RtFuncs& rt = rt_for(device);
  if (tlas.handle != VK_NULL_HANDLE && rt.destroy_as != nullptr) {
    rt.destroy_as(device, tlas.handle, nullptr);
    tlas.handle = VK_NULL_HANDLE;
  }
  if (tlas.instances.is_valid()) {
    allocator.destroy_allocation(tlas.instances);
    tlas.instances_buffer = VK_NULL_HANDLE;
  }
  if (tlas.storage.is_valid()) {
    allocator.destroy_allocation(tlas.storage);
    tlas.storage_buffer = VK_NULL_HANDLE;
  }
  tlas.instance_buffer_address = 0U;
  tlas.storage_bytes = 0U;
  tlas.capacity = 0U;
}

#endif  // OMNICPP_HAS_VULKAN

}  // namespace warploom::render
