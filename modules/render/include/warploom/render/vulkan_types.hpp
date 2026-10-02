#pragma once

#include <cstdint>

// S5-B phase 2 name normalization (docs/warploom-identity-plan.md): the
// new primary name is WARPLOOM_HAS_VULKAN; the legacy name is honored so
// existing build definitions keep working (removed post-0.1).
#if !defined(WARPLOOM_HAS_VULKAN) && defined(OMNICPP_HAS_VULKAN)
#define WARPLOOM_HAS_VULKAN
#endif

#if defined(WARPLOOM_HAS_VULKAN)
#include <vulkan/vulkan.h>
#define WARPLOOM_VULKAN_TYPES_AVAILABLE 1
#else
#define WARPLOOM_VULKAN_TYPES_AVAILABLE 0

typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
typedef struct VkSwapchainKHR_T* VkSwapchainKHR;
typedef struct VkImageView_T* VkImageView;
typedef struct VkImage_T* VkImage;
typedef struct VkDeviceMemory_T* VkDeviceMemory;
typedef struct VkRenderPass_T* VkRenderPass;
typedef struct VkFramebuffer_T* VkFramebuffer;
typedef struct VkPipeline_T* VkPipeline;
typedef struct VkPipelineLayout_T* VkPipelineLayout;
typedef struct VkShaderModule_T* VkShaderModule;
typedef struct VkCommandPool_T* VkCommandPool;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef struct VkFence_T* VkFence;
typedef struct VkSemaphore_T* VkSemaphore;
typedef struct VkEvent_T* VkEvent;
typedef struct VkBuffer_T* VkBuffer;
typedef struct VkBufferView_T* VkBufferView;
typedef struct VkDescriptorPool_T* VkDescriptorPool;
typedef struct VkDescriptorSetLayout_T* VkDescriptorSetLayout;
typedef struct VkDescriptorSet_T* VkDescriptorSet;
typedef struct VkSampler_T* VkSampler;
typedef struct VkQueryPool_T* VkQueryPool;
typedef struct VkDebugUtilsMessengerEXT_T* VkDebugUtilsMessengerEXT;

using VkDeviceSize = std::uint64_t;
constexpr VkDeviceSize VK_WHOLE_SIZE = ~static_cast<VkDeviceSize>(0);
using VkBufferUsageFlags = std::uint32_t;
using VkMemoryPropertyFlags = std::uint32_t;
using VkDescriptorType = std::uint32_t;
using VkShaderStageFlags = std::uint32_t;
using VkImageLayout = std::uint32_t;
using VkDescriptorSetLayoutCreateFlags = std::uint32_t;
using VkDescriptorPoolCreateFlags = std::uint32_t;
using VkDescriptorBindingFlags = std::uint32_t;

constexpr VkDescriptorType VK_DESCRIPTOR_TYPE_MAX_ENUM = 0x7FFFFFFF;
constexpr VkDescriptorType VK_DESCRIPTOR_TYPE_SAMPLER = 0;
constexpr VkDescriptorType VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1;
constexpr VkDescriptorType VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE = 2;
constexpr VkDescriptorType VK_DESCRIPTOR_TYPE_STORAGE_IMAGE = 3;
constexpr VkDescriptorType VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6;
constexpr VkDescriptorType VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7;
constexpr VkDescriptorType VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT = 10;
constexpr VkShaderStageFlags VK_SHADER_STAGE_VERTEX_BIT = 0x00000001;
constexpr VkShaderStageFlags VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT = 0x00000002;
constexpr VkShaderStageFlags VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT = 0x00000004;
constexpr VkShaderStageFlags VK_SHADER_STAGE_GEOMETRY_BIT = 0x00000008;
constexpr VkShaderStageFlags VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010;
constexpr VkShaderStageFlags VK_SHADER_STAGE_COMPUTE_BIT = 0x00000020;
constexpr VkDescriptorSetLayoutCreateFlags
    VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT = 0x00000002;
constexpr VkDescriptorPoolCreateFlags
    VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT = 0x00000002;
constexpr VkDescriptorBindingFlags
    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT = 0x00000001;
constexpr VkDescriptorBindingFlags
    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT = 0x00000004;

//! Ray-tracing handle stand-in (headless builds): the AS write API takes a
//! typed handle; stubbed to an opaque pointer so headers compile unchanged.
typedef struct VkAccelerationStructureKHR_T* VkAccelerationStructureKHR;

//! Pipeline shader-stage descriptor stand-in (headless builds): appears in
//! graphics/RT pipeline create APIs; never constructed without a device.
//! pSpecializationInfo is carried as const void* (layout-compatible).
struct VkPipelineShaderStageCreateInfo {
  const void* sType;
  const void* pNext;
  std::uint32_t flags;
  std::uint32_t stage;
  VkShaderModule module;
  const char* pName;
  const void* pSpecializationInfo;
};

//! Additional flag/enum stand-ins used by render headers in headless builds.
using VkImageUsageFlags = std::uint32_t;
using VkImageCreateFlags = std::uint32_t;
using VkPipelineStageFlags = std::uint32_t;
using VkAccessFlags = std::uint32_t;
using VkImageViewType = std::uint32_t;
using VkSamplerMipmapMode = std::uint32_t;
using VkFilter = std::uint32_t;
using VkSamplerAddressMode = std::uint32_t;
using VkComponentMapping = std::uint32_t;
using VkFormatFeatureFlags = std::uint32_t;
using VkResult = std::int32_t;
using VkQueryResultFlags = std::uint32_t;
using VkBuildAccelerationStructureFlagsKHR = std::uint32_t;
using VkAccelerationStructureTypeKHR = std::uint32_t;
using VkGeometryTypeKHR = std::uint32_t;

constexpr VkResult VK_SUCCESS = 0;
constexpr VkQueryResultFlags VK_QUERY_RESULT_64_BIT = 0x00000001;
constexpr VkQueryResultFlags VK_QUERY_RESULT_WAIT_BIT = 0x00000002;

//! RT struct stand-ins (headless builds): size-queried via vkGet... paths
//! that never execute without a device; members mirror the real Vulkan
//! layout so code compiles unchanged.
struct VkStructureTypePlaceholder;
struct VkAccelerationStructureBuildSizesInfoKHR {
  const VkStructureTypePlaceholder* sType;
  const void* pNext;
  std::uint64_t accelerationStructureSize;
  std::uint64_t updateScratchSize;
  std::uint64_t buildScratchSize;
};

using VkFormat = std::uint32_t;
using VkImageLayout = std::uint32_t;
using VkPresentModeKHR = std::uint32_t;

struct VkExtent2D { std::uint32_t width; std::uint32_t height; };
struct VkOffset2D { std::int32_t x; std::int32_t y; };
struct VkRect2D { VkOffset2D offset; VkExtent2D extent; };
struct VkClearValue { float color[4]; };
struct VkSurfaceFormatKHR { VkFormat format; std::uint32_t colorSpace; };
struct VkSurfaceCapabilitiesKHR {
  std::uint32_t minImageCount;
  std::uint32_t maxImageCount;
  VkExtent2D currentExtent;
  VkExtent2D minImageExtent;
  VkExtent2D maxImageExtent;
  std::uint32_t minImageArrayLayers;
  std::uint32_t maxImageArrayLayers;
  std::uint32_t supportedTransforms;
  std::uint32_t currentTransform;
  std::uint32_t supportedCompositeAlpha;
  std::uint32_t supportedUsageFlags;
};

constexpr VkFormat VK_FORMAT_UNDEFINED = 0;
constexpr VkBufferUsageFlags VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x00000001;
constexpr VkBufferUsageFlags VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x00000002;
constexpr VkMemoryPropertyFlags VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x00000001;
constexpr VkMemoryPropertyFlags VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x00000002;
constexpr VkMemoryPropertyFlags VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x00000040;
constexpr VkFormat VK_FORMAT_B8G8R8A8_UNORM = 44;
constexpr VkFormat VK_FORMAT_B8G8R8A8_SRGB = 50;
constexpr VkFormat VK_FORMAT_D32_SFLOAT = 126;
constexpr VkPresentModeKHR VK_PRESENT_MODE_FIFO_KHR = 2;
#ifndef VK_NULL_HANDLE
#define VK_NULL_HANDLE nullptr
#endif
constexpr VkPresentModeKHR VK_PRESENT_MODE_MAILBOX_KHR = 1;
constexpr VkImageLayout VK_IMAGE_LAYOUT_UNDEFINED = 0;
constexpr VkImageLayout VK_IMAGE_LAYOUT_GENERAL = 1;
constexpr VkImageLayout VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2;
constexpr VkImageLayout VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3;
constexpr VkImageLayout VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5;
constexpr VkImageLayout VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6;
constexpr VkImageLayout VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7;
constexpr VkImageLayout VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002;
constexpr std::uint32_t VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0;

#endif

// S5-B phase 2 shim: legacy availability flag aliases the new one
// (docs/warploom-identity-plan.md); removed post-0.1.
#ifndef OMNICPP_VULKAN_TYPES_AVAILABLE
#define OMNICPP_VULKAN_TYPES_AVAILABLE WARPLOOM_VULKAN_TYPES_AVAILABLE
#endif
