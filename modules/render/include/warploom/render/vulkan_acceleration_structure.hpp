#pragma once

/**
 * @file vulkan_acceleration_structure.hpp
 * @brief Bottom-level (BLAS) and top-level (TLAS) acceleration-structure
 *        builders for ray queries.
 *
 * Contract summary:
 * - BLAS geometry reads packed triangle positions through a SHADER_DEVICE_
 *   ADDRESS buffer owned by the CALLER (`BlasBuildInput.vertex_buffer_address`).
 *   The caller uploads the float triples and keeps the buffer alive for the
 *   build (and for any rebuild).
 * - TLAS instances are written from host-visible memory each frame; each
 *   instance references its BLAS by device address (`TlasInstance.blas_device_
 *   address`, obtained from BottomLevelAS::device_address).
 * - Scratch memory comes from VulkanScratchPool, sized to the largest build
 *   and reused across builds. All builds record onto the caller's command
 *   buffer; the caller submits and (in tests) waits before reading results.
 *
 * Memory follows the two-buffer rule: AS storage is
 * VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR (device-local),
 * scratch is VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_SCRATCH_BIT_KHR
 * (device-local) with SHADER_DEVICE_ADDRESS so the build can take its
 * address. TLAS rebuilds (not refits) every frame — preferred at our scale.
 */

#include "warploom/render/vulkan_memory_allocator.hpp"

#include <cstdint>

struct VkAccelerationStructureKHR_T;

namespace omnicpp::render {

class VulkanContext;

using VkAccelerationStructure = VkAccelerationStructureKHR_T*;

//! One triangle-geometry mesh (AABB procedural prims come later).
struct BlasBuildInput {
  //! Device address of packed triangle positions: 9 floats per triangle
  //! (3 vertices * xyz), tightly packed (stride implied 9 floats). The
  //! buffer must be alive when cmd_build_blas records, and created with
  //! SHADER_DEVICE_ADDRESS.
  std::uint64_t vertex_buffer_address{0};
  std::uint64_t triangle_count{0};
  //! Highest vertex index referenced (needed by the geometry description);
  //! for non-indexed builds: triangle_count * 3 - 1.
  std::uint32_t max_vertex{0};
};

//! A BLAS + its backing storage. destroy_blas tears it down.
struct BottomLevelAS {
  VkAccelerationStructure handle{nullptr};
  Allocation storage{};
  VkBuffer storage_buffer{nullptr};
  std::uint64_t device_address{0};
  //! Storage size actually allocated (for diagnostics/telemetry).
  std::uint64_t storage_bytes{0};
};

//! Per-instance data for the frame's TLAS.
struct TlasInstance {
  //! Row-major 3x4 affine transform (Vulkan instance layout: rows are the
  //! matrix rows, last row is translation).
  float transform[12];
  std::uint32_t instance_custom_index{0};
  std::uint32_t mask{0xFFu};
  std::uint32_t sbt_offset{0};
  std::uint32_t flags{0};
  //! Device address of the referenced BLAS (BottomLevelAS::device_address).
  std::uint64_t blas_device_address{0};
};

//! The frame's TLAS + its buffers.
struct TopLevelAS {
  VkAccelerationStructure handle{nullptr};
  Allocation storage{};
  VkBuffer storage_buffer{nullptr};
  std::uint64_t storage_bytes{0};
  //! Host-visible, host-coherent instance buffer written by cmd_build_tlas.
  Allocation instances{};
  VkBuffer instances_buffer{nullptr};
  std::uint64_t instance_buffer_address{0};
  //! Capacity (instances) the storage was sized for.
  std::uint32_t capacity{0};
};

//! Shared scratch pool: sized to the largest build so far, reused by all.
class VulkanScratchPool final {
 public:
  VulkanScratchPool() = default;
  ~VulkanScratchPool() = default;
  VulkanScratchPool(const VulkanScratchPool&) = delete;
  VulkanScratchPool& operator=(const VulkanScratchPool&) = delete;

  //! Ensures a device-local scratch buffer of at least `bytes`; returns its
  //! device address (buffer_device_address feature required).
  [[nodiscard]] core::Result<std::uint64_t> acquire(
      VulkanMemoryAllocator& allocator, VkDevice device, std::uint64_t bytes);
  void cleanup(VulkanMemoryAllocator& allocator) noexcept;

  [[nodiscard]] VkBuffer buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }

 private:
  Allocation allocation_{};
  VkBuffer buffer_{nullptr};
  std::uint64_t capacity_{0};
};

//! Builds BLASes and the per-frame TLAS on caller command buffers.
class VulkanAccelerationStructureBuilder final {
 public:
  VulkanAccelerationStructureBuilder() = default;
  ~VulkanAccelerationStructureBuilder() = default;
  VulkanAccelerationStructureBuilder(const VulkanAccelerationStructureBuilder&) = delete;
  VulkanAccelerationStructureBuilder& operator=(const VulkanAccelerationStructureBuilder&) = delete;

  //! Queries build sizes and allocates BLAS storage. Records nothing.
  [[nodiscard]] core::Result<BottomLevelAS> create_blas(
      VkDevice device, VulkanMemoryAllocator& allocator,
      const BlasBuildInput& input) const;

  //! Records the BLAS build. `scratch_bytes` is scratch needed (from
  //! create time); acquire that much from VulkanScratchPool first.
  [[nodiscard]] core::Result<void> cmd_build_blas(
      VkCommandBuffer cmd, VkDevice device, const BottomLevelAS& blas,
      const BlasBuildInput& input, std::uint64_t scratch_address) const;

  //! Builds sizes for the input (test/introspection + scratch sizing).
  [[nodiscard]] static VkAccelerationStructureBuildSizesInfoKHR query_blas_sizes(
      VkDevice device, const BlasBuildInput& input) noexcept;

  //! Creates TLAS storage sized for `instance_count` + host instance buffer.
  [[nodiscard]] core::Result<TopLevelAS> create_tlas(
      VkDevice device, VulkanMemoryAllocator& allocator,
      std::uint32_t instance_count) const;

  //! Writes `instances` into the host buffer and records the TLAS build.
  [[nodiscard]] core::Result<void> cmd_build_tlas(
      VkCommandBuffer cmd, VkDevice device, const TopLevelAS& tlas,
      const TlasInstance* instances, std::uint32_t instance_count,
      std::uint64_t scratch_address) const;

  [[nodiscard]] static VkAccelerationStructureBuildSizesInfoKHR query_tlas_sizes(
      VkDevice device, std::uint32_t instance_count) noexcept;

  void destroy_blas(VkDevice device, VulkanMemoryAllocator& allocator,
                    BottomLevelAS& blas) noexcept;
  void destroy_tlas(VkDevice device, VulkanMemoryAllocator& allocator,
                    TopLevelAS& tlas) noexcept;

 private:
  static core::Result<std::uint64_t> buffer_device_address(
      VkDevice device, VkBuffer buffer) noexcept;
};

}  // namespace omnicpp::render
