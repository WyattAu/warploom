#pragma once

/**
 * @file vulkan_context.hpp
 * @brief Vulkan instance and device abstraction.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace omnicpp::render {

struct QueueFamilyIndices {
  std::int32_t graphics_family{-1};
  std::int32_t present_family{-1};
  //! Family with COMPUTE but no GRAPHICS bit (true async compute). -1 when
  //! the device offers none; callers then fall back to the graphics family.
  std::int32_t compute_family{-1};
  [[nodiscard]] bool is_complete() const noexcept {
    return graphics_family >= 0 && present_family >= 0;
  }
  [[nodiscard]] bool has_dedicated_compute() const noexcept { return compute_family >= 0; }
};

struct DeviceProperties {
  std::string name;
  std::uint32_t api_version{0};
  std::uint32_t driver_version{0};
  std::uint32_t vendor_id{0};
  std::uint32_t device_id{0};
  bool is_discrete_gpu{false};
  std::uint64_t max_image_dimension_2d{0};
  //! Timestamp period in nanoseconds per tick (0 when unsupported).
  float timestamp_period_ns{0.0f};
};

class VulkanContext final {
public:
  VulkanContext() = default;
  ~VulkanContext();

  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;
  VulkanContext(VulkanContext&&) = delete;
  VulkanContext& operator=(VulkanContext&&) = delete;

  [[nodiscard]] omnicpp::core::Result<void> initialize(
      const std::string& app_name, bool enable_validation = false);
  //! Create a VK_EXT_headless_surface surface for GPU-only validation.
  [[nodiscard]] omnicpp::core::Result<void> create_surface(VkSurfaceKHR& surface);
  //! Destroy a surface created by this context.
  void destroy_surface(VkSurfaceKHR& surface) noexcept;
  void cleanup() noexcept;

  [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
  [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
  [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return physical_device_; }
  [[nodiscard]] VkDevice device() const noexcept { return device_; }
  [[nodiscard]] VkQueue graphics_queue() const noexcept { return graphics_queue_; }
  [[nodiscard]] VkQueue present_queue() const noexcept { return present_queue_; }
  //! Dedicated async-compute queue (COMPUTE-only family) or null when the
  //! device has no such family — callers must fall back to the graphics queue.
  [[nodiscard]] VkQueue compute_queue() const noexcept { return compute_queue_; }
  //! Family of compute_queue(); falls back to the graphics family otherwise.
  [[nodiscard]] std::uint32_t compute_family_index() const noexcept {
    return queue_families_.has_dedicated_compute()
               ? static_cast<std::uint32_t>(queue_families_.compute_family)
               : static_cast<std::uint32_t>(queue_families_.graphics_family);
  }
  //! True when a COMPUTE-only queue family was found and a queue was fetched.
  [[nodiscard]] bool has_dedicated_compute() const noexcept { return compute_queue_ != nullptr; }
  [[nodiscard]] const QueueFamilyIndices& queue_families() const noexcept { return queue_families_; }
  [[nodiscard]] const DeviceProperties& device_properties() const noexcept { return device_properties_; }
  [[nodiscard]] bool has_validation() const noexcept { return validation_enabled_; }
  //! True when the device supports Vulkan 1.3 and Synchronization 2 was enabled.
  [[nodiscard]] bool has_synchronization2() const noexcept { return synchronization2_enabled_; }
  //! True when the device supports Vulkan 1.3 and timeline semaphores were enabled.
  [[nodiscard]] bool has_timeline_semaphores() const noexcept { return timeline_semaphores_enabled_; }
  //! True when descriptor indexing (bindless: runtime arrays, partially bound,
  //! update-after-bind) was negotiated and enabled on the device.
  [[nodiscard]] bool has_descriptor_indexing() const noexcept { return descriptor_indexing_enabled_; }
  //! True when the RT extensions (VK_KHR_acceleration_structure,
  //! VK_KHR_ray_query, VK_KHR_deferred_host_operations) and the
  //! accelerationStructure + rayQuery device features were all negotiated
  //! and enabled. Gates every ray-tracing code path.
  [[nodiscard]] bool has_ray_tracing() const noexcept { return ray_tracing_enabled_; }
  //! True when VK_KHR_ray_tracing_pipeline + the rayTracingPipeline feature
  //! were additionally negotiated (vkCmdTraceRaysKHR / SBT pipelines). A
  //! superset of has_ray_tracing(): acceleration structures and ray queries
  //! are always available when this returns true.
  [[nodiscard]] bool has_ray_tracing_pipeline() const noexcept { return rt_pipeline_enabled_; }
  //! scratch/AS size limits from the RT properties (0 when absent).
  [[nodiscard]] std::uint64_t max_ray_tracing_scratch() const noexcept {
#if defined(OMNICPP_HAS_VULKAN)
    return as_properties_.maxGeometryCount > 0U ? 1U : 0U;
#else
    return 0U;
#endif
  }
  [[nodiscard]] std::uint64_t max_acceleration_structure_geometry_count() const noexcept {
#if defined(OMNICPP_HAS_VULKAN)
    return as_properties_.maxGeometryCount;
#else
    return 0U;
#endif
  }
  [[nodiscard]] std::uint32_t validation_warning_count() const noexcept {
    return validation_warning_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint32_t validation_error_count() const noexcept {
    return validation_error_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] static bool is_available() noexcept;

private:
  bool check_validation_layer_support();
  std::vector<const char*> get_required_extensions(bool validation);
  std::uint32_t rate_device_suitability(VkPhysicalDevice device);
  bool check_device_extension_support(VkPhysicalDevice device);
  QueueFamilyIndices find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface);
  std::uint32_t get_highest_api_version();
  void record_validation_message(std::uint32_t severity, std::uint32_t type,
                                 const char* message) noexcept;

  bool initialized_{false};
  bool validation_enabled_{false};
  bool headless_surface_enabled_{false};
  bool synchronization2_enabled_{false};
  bool timeline_semaphores_enabled_{false};
  bool descriptor_indexing_enabled_{false};
  bool ray_tracing_enabled_{false};
  bool rt_pipeline_enabled_{false};
#if defined(OMNICPP_HAS_VULKAN)
  VkPhysicalDeviceAccelerationStructurePropertiesKHR as_properties_{};
  //! Feature-struct members for the negotiation chain (RT-capable devices).
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features_{};
  VkPhysicalDeviceRayQueryFeaturesKHR ray_query_features_{};
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_pipeline_features_{};
#endif
  //! Probes RT extension + feature availability on `device` (no enabling).
  static bool device_supports_ray_tracing(VkPhysicalDevice device);
  //! Probes VK_KHR_ray_tracing_pipeline extension presence (no enabling).
  static bool device_supports_rt_pipeline_extension(VkPhysicalDevice device);

#if !defined(OMNICPP_HAS_VULKAN)
  //! Opaque stand-ins keep the header compiling without Vulkan (the members
  //! above are compiled out; the negotiation code is entirely #ifdef'd).
  struct VkPhysicalDeviceAccelerationStructurePropertiesKHR {};
#endif
  VkInstance instance_{nullptr};
  VkPhysicalDevice physical_device_{nullptr};
  VkDevice device_{nullptr};
  VkQueue graphics_queue_{nullptr};
  VkQueue present_queue_{nullptr};
  VkQueue compute_queue_{nullptr};
  VkDebugUtilsMessengerEXT debug_messenger_{nullptr};
  QueueFamilyIndices queue_families_{};
  DeviceProperties device_properties_{};
  std::atomic<std::uint32_t> validation_warning_count_{0};
  std::atomic<std::uint32_t> validation_error_count_{0};
};

} // namespace omnicpp::render
