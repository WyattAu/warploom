/**
 * @file vulkan_context.cpp
 * @brief Vulkan instance and device abstraction implementation.
 * Extracted from the legacy renderer.cpp. All Qt dependencies removed.
 */

#include "warploom/render/vulkan_context.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace omnicpp::render {

VulkanContext::~VulkanContext() { cleanup(); }

omnicpp::core::Result<void> VulkanContext::initialize(
    const std::string& app_name, bool enable_validation) {
#ifdef OMNICPP_HAS_VULKAN
  if (initialized_) return omnicpp::core::Result<void>::ok();
  if (!is_available()) return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);

  validation_enabled_ = enable_validation;
  validation_warning_count_.store(0, std::memory_order_relaxed);
  validation_error_count_.store(0, std::memory_order_relaxed);
  if (validation_enabled_ && !check_validation_layer_support()) {
    validation_enabled_ = false;
  }

  auto extensions = get_required_extensions(validation_enabled_);

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = app_name.c_str();
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "OmniCpp Engine";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  const std::uint32_t instance_api_version = get_highest_api_version();
  app_info.apiVersion = instance_api_version;

  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;
  create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
  create_info.ppEnabledExtensionNames = extensions.data();

  VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};
  if (validation_enabled_) {
    const char* validation_layers[] = {"VK_LAYER_KHRONOS_validation"};
    create_info.enabledLayerCount = 1;
    create_info.ppEnabledLayerNames = validation_layers;

    debug_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug_create_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_create_info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_create_info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT type,
                                            const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                            void* user_data) -> VkBool32 {
      auto* context = static_cast<VulkanContext*>(user_data);
      if (context) {
        context->record_validation_message(
            static_cast<std::uint32_t>(severity),
            static_cast<std::uint32_t>(type),
            callback_data ? callback_data->pMessage : "<missing validation message>");
      }
      return VK_FALSE;
    };
    debug_create_info.pUserData = this;
    create_info.pNext = &debug_create_info;
  }

  VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
  if (result != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  if (validation_enabled_) {
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (func) func(instance_, &debug_create_info, nullptr, &debug_messenger_);
  }

  // Enumerate physical devices
  std::uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
  if (device_count == 0) { cleanup(); return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available); }

  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

  physical_device_ = nullptr;
  std::uint32_t best_score = 0;
  for (const auto& dev : devices) {
    const std::uint32_t score = rate_device_suitability(dev);
    if (score > best_score) {
      physical_device_ = dev;
      best_score = score;
    }
  }
  if (!physical_device_) {
    cleanup();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical_device_, &props);
  device_properties_.name = props.deviceName;
  device_properties_.api_version = props.apiVersion;
  device_properties_.driver_version = props.driverVersion;
  device_properties_.vendor_id = props.vendorID;
  device_properties_.device_id = props.deviceID;
  device_properties_.is_discrete_gpu = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
  device_properties_.max_image_dimension_2d = props.limits.maxImageDimension2D;
  device_properties_.timestamp_period_ns = props.limits.timestampPeriod;

  queue_families_ = find_queue_families(physical_device_, nullptr);
  if (!queue_families_.is_complete()) { cleanup(); return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available); }

  std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
  std::set<std::int32_t> unique_families = {queue_families_.graphics_family, queue_families_.present_family};
  if (queue_families_.has_dedicated_compute()) {
    unique_families.insert(queue_families_.compute_family);
  }
  float priority = 1.0f;
  for (std::int32_t fam : unique_families) {
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = static_cast<std::uint32_t>(fam);
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    queue_create_infos.push_back(qci);
  }

  // Effective API version: min(instance request, device support). Feature
  // structs for versions above the effective API must not appear in the
  // vkCreateDevice pNext chain (VUID-VkDeviceCreateInfo-pNext-pNext).
  const std::uint32_t effective_api =
      instance_api_version < props.apiVersion ? instance_api_version : props.apiVersion;
  const bool is_vulkan13 = effective_api >= VK_API_VERSION_1_3;
  const bool is_vulkan12 = effective_api >= VK_API_VERSION_1_2;

  VkPhysicalDeviceFeatures supported_features{};
  vkGetPhysicalDeviceFeatures(physical_device_, &supported_features);
  VkPhysicalDeviceFeatures device_features{};
  // Anisotropy is optional for this renderer; only enable supported features.
  device_features.samplerAnisotropy = supported_features.samplerAnisotropy;
  // Vertex pulling (SSBO reads in the vertex stage) requires this feature;
  // enable when the device supports it so compute-generated geometry works.
  device_features.vertexPipelineStoresAndAtomics =
      supported_features.vertexPipelineStoresAndAtomics;
  // GPU-driven draws: vkCmdDrawIndexedIndirect with drawCount > 1 (one
  // command per object, degenerate commands cull) needs multiDrawIndirect;
  // per-command firstInstance lets gl_InstanceIndex address the object
  // payload SSBO slot for each draw command.
  device_features.multiDrawIndirect = supported_features.multiDrawIndirect;
  device_features.drawIndirectFirstInstance =
      supported_features.drawIndirectFirstInstance;

  if (!check_device_extension_support(physical_device_)) {
    cleanup();
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  // Vulkan 1.3 feature negotiation: enable sync2 and timeline semaphores only
  // when the selected device advertises them, and chain the struct explicitly.
  synchronization2_enabled_ = false;
  timeline_semaphores_enabled_ = false;
  descriptor_indexing_enabled_ = false;
  VkPhysicalDeviceVulkan13Features vulkan13_features{};
  vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  VkPhysicalDeviceVulkan12Features vulkan12_features{};
  vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceVulkan11Features vulkan11_features{};
  vulkan11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  // RT structs join the PROBE chain (tail) whenever the device has the RT
  // extensions, so one vkGetPhysicalDeviceFeatures2 call fills them in.
  const bool rt_extensions_present =
      is_vulkan12 && device_supports_ray_tracing(physical_device_);
  if (rt_extensions_present) {
    as_features_.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    ray_query_features_.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rt_pipeline_features_.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
  }
  if (is_vulkan13 || is_vulkan12) {
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features = device_features;
    // Chain: features2 -> 1.3 -> 1.2 -> 1.1 -> [AS -> rayQuery -> rtPipeline].
    if (is_vulkan13) {
      features2.pNext = &vulkan13_features;
      if (is_vulkan12) {
        vulkan13_features.pNext = &vulkan12_features;
        vulkan12_features.pNext = &vulkan11_features;
        if (rt_extensions_present) {
          vulkan11_features.pNext = &as_features_;
          as_features_.pNext = &ray_query_features_;
          ray_query_features_.pNext = &rt_pipeline_features_;
        }
      }
    }
    vkGetPhysicalDeviceFeatures2(physical_device_, &features2);
  }

  std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkBaseOutStructure* features_chain = nullptr;
  if (is_vulkan13) {
    synchronization2_enabled_ = vulkan13_features.synchronization2 == VK_TRUE;
    if (synchronization2_enabled_) {
      device_extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }
  }
  // Ray tracing negotiation: the probe above filled the feature structs;
  // enable the extensions only when both features are supported. RT also
  // requires bufferDeviceAddress (scratch/instance/geometry addressing).
  ray_tracing_enabled_ = false;
  rt_pipeline_enabled_ = false;
  bool buffer_device_address_enabled = false;
  if (rt_extensions_present) {
    as_properties_.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &as_properties_;
    vkGetPhysicalDeviceProperties2(physical_device_, &props2);
    buffer_device_address_enabled =
        is_vulkan12 && vulkan12_features.bufferDeviceAddress == VK_TRUE;
    ray_tracing_enabled_ =
        buffer_device_address_enabled &&
        as_features_.accelerationStructure == VK_TRUE &&
        ray_query_features_.rayQuery == VK_TRUE &&
        as_properties_.maxGeometryCount > 0U;
    // Full RT pipelines (vkCmdTraceRaysKHR + SBT) additionally need the
    // VK_KHR_ray_tracing_pipeline extension and its feature bit.
    rt_pipeline_enabled_ =
        ray_tracing_enabled_ && rt_pipeline_features_.rayTracingPipeline == VK_TRUE &&
        device_supports_rt_pipeline_extension(physical_device_);
    if (ray_tracing_enabled_) {
      as_features_.accelerationStructure = VK_TRUE;
      ray_query_features_.rayQuery = VK_TRUE;
      vulkan12_features.bufferDeviceAddress = VK_TRUE;
      device_extensions.push_back(
          VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
      device_extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
      device_extensions.push_back(
          VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
      if (rt_pipeline_enabled_) {
        rt_pipeline_features_.rayTracingPipeline = VK_TRUE;
        device_extensions.push_back(
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
      }
    }
  }
  if (is_vulkan12) {
    timeline_semaphores_enabled_ = vulkan12_features.timelineSemaphore == VK_TRUE;
    if (timeline_semaphores_enabled_) {
      vulkan12_features.timelineSemaphore = VK_TRUE;
    }
    // Bindless descriptor indexing: require the full combination used by the
    // renderer (runtime-sized arrays, partially-bound sets, update-after-bind
    // for sampled images and storage buffers, and non-uniform indexing).
    descriptor_indexing_enabled_ =
        vulkan12_features.descriptorIndexing == VK_TRUE &&
        vulkan12_features.runtimeDescriptorArray == VK_TRUE &&
        vulkan12_features.descriptorBindingPartiallyBound == VK_TRUE &&
        vulkan12_features.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
        vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE &&
        vulkan12_features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
        vulkan12_features.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE;
    if (descriptor_indexing_enabled_) {
      vulkan12_features.descriptorIndexing = VK_TRUE;
      vulkan12_features.runtimeDescriptorArray = VK_TRUE;
      vulkan12_features.descriptorBindingPartiallyBound = VK_TRUE;
      vulkan12_features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
      vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
      vulkan12_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
      vulkan12_features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
      device_extensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }
    // shaderDrawParameters (Vulkan 1.1 feature struct): makes
    // gl_InstanceIndex / gl_BaseInstance include the draw's firstInstance.
    // GPU-driven draws rely on per-command firstInstance to address the
    // object payload SSBO slot.
    if (vulkan11_features.shaderDrawParameters == VK_TRUE) {
      vulkan11_features.shaderDrawParameters = VK_TRUE;
    }
  }
  // bufferDeviceAddress is enabled whenever supported: RT needs it, and the
  // allocator must set VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT on memory that
  // backs SHADER_DEVICE_ADDRESS buffers (kept consistent with the allocator's
  // support probe, which cannot observe per-context enablement).
  if (is_vulkan12 && vulkan12_features.bufferDeviceAddress == VK_TRUE) {
    vulkan12_features.bufferDeviceAddress = VK_TRUE;
  }
  // Enable the negotiated feature structs on the device.
  if (ray_tracing_enabled_) {
    // Chain order: rt_pipeline -> ray_query -> as_features -> <rest> (built
    // from the tail so each node's pNext points at the node below it).
    if (rt_pipeline_enabled_) {
      rt_pipeline_features_.pNext = features_chain;
      features_chain = reinterpret_cast<VkBaseOutStructure*>(&rt_pipeline_features_);
    }
    ray_query_features_.pNext = features_chain;
    features_chain = reinterpret_cast<VkBaseOutStructure*>(&ray_query_features_);
    as_features_.pNext = features_chain;
    features_chain = reinterpret_cast<VkBaseOutStructure*>(&as_features_);
  }
  // Join the level structs UNCONDITIONALLY when the API level is present:
  // dropping a level (e.g. no sync2) would silently discard every feature
  // negotiated through it (descriptor indexing, RT, bufferDeviceAddress).
  if (is_vulkan13) {
    vulkan13_features.pNext = features_chain;
    features_chain = reinterpret_cast<VkBaseOutStructure*>(&vulkan13_features);
  }
  if (is_vulkan12) {
    vulkan12_features.pNext = features_chain;
    features_chain = reinterpret_cast<VkBaseOutStructure*>(&vulkan12_features);
  }
  if (is_vulkan12 && vulkan11_features.shaderDrawParameters == VK_TRUE) {
    vulkan11_features.pNext = features_chain;
    features_chain = reinterpret_cast<VkBaseOutStructure*>(&vulkan11_features);
  }

  VkDeviceCreateInfo device_create_info{};
  device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_create_info.pNext = features_chain;
  device_create_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_create_infos.size());
  device_create_info.pQueueCreateInfos = queue_create_infos.data();
  device_create_info.pEnabledFeatures = &device_features;
  device_create_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
  device_create_info.ppEnabledExtensionNames = device_extensions.data();

  result = vkCreateDevice(physical_device_, &device_create_info, nullptr, &device_);
  if (result != VK_SUCCESS) { cleanup(); return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available); }

  vkGetDeviceQueue(device_, static_cast<std::uint32_t>(queue_families_.graphics_family), 0, &graphics_queue_);
  vkGetDeviceQueue(device_, static_cast<std::uint32_t>(queue_families_.present_family), 0, &present_queue_);
  if (queue_families_.has_dedicated_compute()) {
    vkGetDeviceQueue(device_, static_cast<std::uint32_t>(queue_families_.compute_family), 0, &compute_queue_);
  }

  // Headless surface support is optional and checked after instance creation.
  std::uint32_t extension_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
  std::vector<VkExtensionProperties> available_extensions(extension_count);
  vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extensions.data());
  headless_surface_enabled_ = false;
  for (const auto& extension : available_extensions) {
    if (std::strcmp(extension.extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0) {
      headless_surface_enabled_ = true;
      break;
    }
  }

  initialized_ = true;
  return omnicpp::core::Result<void>::ok();
#else
  (void)app_name;
  (void)enable_validation;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanContext::create_surface(VkSurfaceKHR& surface) {
#ifdef OMNICPP_HAS_VULKAN
  if (!initialized_ || !instance_ || !headless_surface_enabled_) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  auto create_headless = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
      vkGetInstanceProcAddr(instance_, "vkCreateHeadlessSurfaceEXT"));
  if (!create_headless) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  VkHeadlessSurfaceCreateInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
  const VkResult result = create_headless(instance_, &info, nullptr, &surface);
  return result == VK_SUCCESS
      ? omnicpp::core::Result<void>::ok()
      : omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#else
  (void)surface;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanContext::destroy_surface(VkSurfaceKHR& surface) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (surface && instance_) {
    vkDestroySurfaceKHR(instance_, surface, nullptr);
  }
#endif
  surface = VK_NULL_HANDLE;
}

void VulkanContext::cleanup() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device_) { vkDestroyDevice(device_, nullptr); device_ = nullptr; }
  if (debug_messenger_ && instance_) {
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (func) func(instance_, debug_messenger_, nullptr);
    debug_messenger_ = nullptr;
  }
  if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = nullptr; }
  physical_device_ = nullptr;
  graphics_queue_ = nullptr;
  present_queue_ = nullptr;
  initialized_ = false;
  headless_surface_enabled_ = false;
#endif
}

bool VulkanContext::is_available() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "OmniCpp Probe";
  app_info.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;

  VkInstance probe = nullptr;
  VkResult result = vkCreateInstance(&create_info, nullptr, &probe);
  if (result == VK_SUCCESS && probe) { vkDestroyInstance(probe, nullptr); return true; }
  return false;
#else
  return false;
#endif
}

bool VulkanContext::check_validation_layer_support() {
#ifdef OMNICPP_HAS_VULKAN
  std::uint32_t layer_count;
  vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
  std::vector<VkLayerProperties> available_layers(layer_count);
  vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());
  for (const char* name : {"VK_LAYER_KHRONOS_validation"}) {
    bool found = false;
    for (const auto& l : available_layers) {
      if (std::strcmp(name, l.layerName) == 0) { found = true; break; }
    }
    if (!found) return false;
  }
  return true;
#else
  return false;
#endif
}

std::vector<const char*> VulkanContext::get_required_extensions(bool validation) {
  std::vector<const char*> extensions;
#ifdef OMNICPP_HAS_VULKAN
  std::uint32_t extension_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
  std::vector<VkExtensionProperties> available(extension_count);
  vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available.data());

  const auto supported = [&available](const char* name) {
    for (const auto& extension : available) {
      if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
  };

  if (supported(VK_KHR_SURFACE_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  }
#ifdef VK_USE_PLATFORM_XCB_KHR
  if (supported(VK_KHR_XCB_SURFACE_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
  }
#endif
  headless_surface_enabled_ = supported(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
  if (headless_surface_enabled_) {
    extensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
  }
  if (validation && supported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }
#endif
#ifndef OMNICPP_HAS_VULKAN
  (void)validation;
#endif
  return extensions;
}

bool VulkanContext::check_device_extension_support(VkPhysicalDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) return false;
  std::uint32_t extension_count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> available(extension_count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, available.data()) != VK_SUCCESS) {
    return false;
  }
  for (const auto& extension : available) {
    if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
      return true;
    }
  }
#endif
#ifndef OMNICPP_HAS_VULKAN
  (void)device;
#endif
  return false;
}

std::uint32_t VulkanContext::rate_device_suitability(VkPhysicalDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !check_device_extension_support(device)) return 0;
  const auto queue_families = find_queue_families(device, nullptr);
  if (!queue_families.is_complete()) return 0;
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(device, &properties);
  std::uint32_t score = 1;
  if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
  score += static_cast<std::uint32_t>(properties.limits.maxImageDimension2D);
  return score;
#else
  (void)device;
  return 0;
#endif
}

QueueFamilyIndices VulkanContext::find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
#ifdef OMNICPP_HAS_VULKAN
  QueueFamilyIndices indices;
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
  for (std::uint32_t i = 0; i < count; ++i) {
    const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    const bool compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    if (graphics) indices.graphics_family = static_cast<std::int32_t>(i);
    // Dedicated async compute: COMPUTE-capable family with no GRAPHICS bit.
    // Prefer the deepest (largest queue count) such family.
    if (compute && !graphics &&
        (indices.compute_family < 0 ||
         families[i].queueCount > families[static_cast<std::size_t>(indices.compute_family)].queueCount)) {
      indices.compute_family = static_cast<std::int32_t>(i);
    }
    if (surface) {
      VkBool32 present_support = false;
      vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
      if (present_support) indices.present_family = static_cast<std::int32_t>(i);
    } else {
      indices.present_family = indices.graphics_family;
    }
    // Scan ALL families: the dedicated compute family typically sits after
    // the graphics one (e.g. NVIDIA: 0=GRAPHICS|COMPUTE, 1=COMPUTE, 2=TRANSFER),
    // so an early break once graphics+present are found would miss it.
  }
  return indices;
#else
  (void)device;
  (void)surface;
  return {};
#endif
}

std::uint32_t VulkanContext::get_highest_api_version() {
#ifdef OMNICPP_HAS_VULKAN
  auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
  if (enumerate_version) {
    std::uint32_t version = VK_API_VERSION_1_0;
    if (enumerate_version(&version) == VK_SUCCESS) {
      // Request up to 1.3: the effective API is min(instance, device), so a
      // 1.3-capable loader is required to legally use 1.3 feature structs.
      if (version >= VK_API_VERSION_1_3) return VK_API_VERSION_1_3;
      if (version >= VK_API_VERSION_1_2) return VK_API_VERSION_1_2;
      if (version >= VK_API_VERSION_1_1) return VK_API_VERSION_1_1;
    }
  }
  return VK_API_VERSION_1_0;
#else
  return 0;
#endif
}

void VulkanContext::record_validation_message(std::uint32_t severity,
                                               std::uint32_t type,
                                               const char* message) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  // The loader reports its explicit-layer notice through the debug callback;
  // exclude only those known informational notices, never an entire category.
  const bool forced_layer_notice = message && (std::strstr(message, "forced enabled due to env var") != nullptr ||
      std::strstr(message, "defined and adding layers") != nullptr);
  if (!forced_layer_notice &&
      (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U) {
    validation_error_count_.fetch_add(1, std::memory_order_relaxed);
  } else if (!forced_layer_notice &&
             (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0U) {
    validation_warning_count_.fetch_add(1, std::memory_order_relaxed);
  }
  (void)type;
  std::fprintf(stderr, "[Vulkan validation] %s\n", message ? message : "<null>");
#else
  (void)severity;
  (void)type;
  (void)message;
#endif
}

bool VulkanContext::device_supports_ray_tracing(VkPhysicalDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) return false;
  std::uint32_t extension_count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count,
                                           nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> available(extension_count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count,
                                           available.data()) != VK_SUCCESS) {
    return false;
  }
  int found = 0;
  for (const auto& extension : available) {
    if (std::strcmp(extension.extensionName,
                    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) {
      found |= 1;
    } else if (std::strcmp(extension.extensionName,
                           VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) {
      found |= 2;
    } else if (std::strcmp(extension.extensionName,
                           VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) ==
               0) {
      found |= 4;
    }
  }
  return found == 7;
#else
  (void)device;
  return false;
#endif
}

bool VulkanContext::device_supports_rt_pipeline_extension(VkPhysicalDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) return false;
  std::uint32_t extension_count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count,
                                           nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> available(extension_count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count,
                                           available.data()) != VK_SUCCESS) {
    return false;
  }
  for (const auto& extension : available) {
    if (std::strcmp(extension.extensionName,
                    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0) {
      return true;
    }
  }
  return false;
#else
  (void)device;
  return false;
#endif
}

} // namespace omnicpp::render
