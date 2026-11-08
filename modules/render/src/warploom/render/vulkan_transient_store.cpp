//! @file vulkan_transient_store.cpp
//! @brief R6b: materialize the frame-graph allocation plan as real images.
//!
//! The store owns every image it creates and every view, and borrows nothing:
//! an Aliased image binds the owner's VkDeviceMemory at the owner's offset,
//! which is sound because the planner only pairs identical descriptions
//! (verified here, not assumed) and the owner is always declared earlier
//! (allocated first). Destroy releases each owner allocation exactly once;
//! sharers release only their image and view.

#include <warploom/render/vulkan_transient_store.hpp>

#include <warploom/render/vulkan_memory_allocator.hpp>

#ifdef WARPLOOM_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

namespace {

#ifdef WARPLOOM_HAS_VULKAN
[[nodiscard]] bool image_descs_equal(const TransientImageDesc& a,
                                     const TransientImageDesc& b) noexcept {
  return a.format == b.format && a.width == b.width &&
         a.height == b.height && a.usage == b.usage;
}

[[nodiscard]] VkImageAspectFlags aspect_of(const TransientImageDesc& d) {
  if (d.aspect != 0U) return d.aspect;
  // Depth formats carry depth bits; everything the store creates otherwise
  // is color. Unknown depth/stencil combos are a caller error, not a guess.
  switch (d.format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}
#endif

}  // namespace

VulkanTransientStore::~VulkanTransientStore() { destroy(); }

::warploom::core::Result<void> VulkanTransientStore::allocate(
    VkDevice device, VkPhysicalDevice physical_device,
    VulkanMemoryAllocator* allocator,
    const std::vector<FrameResource>& resources,
    const std::vector<ResourceAllocationPlan>& plan,
    const std::vector<TransientImageDesc>& descs) {
#ifdef WARPLOOM_HAS_VULKAN
  destroy();
  if (!device || !physical_device || allocator == nullptr ||
      !allocator->is_initialized()) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  if (resources.size() != plan.size()) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  device_ = device;
  allocator_ = allocator;

  const auto find_desc = [&descs](const std::string& name)
      -> const TransientImageDesc* {
    for (const TransientImageDesc& d : descs) {
      if (d.name == name) return &d;
    }
    return nullptr;
  };
  // Owner root resolution: the planner may chain (C aliases B aliases A), so
  // the memory an entry binds is the root's. Termination is guaranteed
  // because every alias edge points strictly backwards (planKind_owner_earlier
  // in proofs/FrameAllocator.lean), but the store checks anyway: a cycle or
  // a missing owner fails loudly instead of binding a null handle.
  const auto resolve_root = [&plan](const std::string& name,
                                    std::size_t* owner_index) -> bool {
    std::string current = name;
    for (std::size_t steps = 0; steps <= plan.size(); ++steps) {
      std::size_t idx = plan.size();
      for (std::size_t i = 0; i < plan.size(); ++i) {
        if (plan[i].resource == current) {
          idx = i;
          break;
        }
      }
      if (idx >= plan.size()) return false;
      if (plan[idx].kind != ResourceAllocation::Aliased) {
        if (owner_index != nullptr) *owner_index = idx;
        return true;
      }
      current = plan[idx].shares_with;
    }
    return false;  // cycle: longer than the plan itself
  };

  for (std::size_t i = 0; i < resources.size(); ++i) {
    if (plan[i].resource != resources[i].name) {
      destroy();
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
    const TransientImageDesc* desc = find_desc(resources[i].name);
    if (desc == nullptr) continue;  // caller-owned external: plan, don't make
    if (desc->format == VK_FORMAT_UNDEFINED || desc->width == 0U ||
        desc->height == 0U || desc->usage == 0U) {
      destroy();
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }

    const bool aliased = plan[i].kind == ResourceAllocation::Aliased;
    VkDeviceMemory owner_memory = VK_NULL_HANDLE;
    VkDeviceSize owner_offset = 0U;
    VkDeviceSize owner_size = 0U;
    if (aliased) {
      // The entry records its DIRECT owner (plan-faithful shares_with), but
      // binds the ROOT's memory: intermediate owners hold no allocation of
      // their own. Both descriptions are re-checked -- the direct one for
      // plan fidelity, the root one because that memory is what the image
      // must satisfy its requirements from.
      const TransientImageDesc* direct_owner =
          find_desc(plan[i].shares_with);
      if (direct_owner == nullptr ||
          !image_descs_equal(*desc, *direct_owner)) {
        destroy();
        return ::warploom::core::Result<void>::error(
            ::warploom::core::RuntimeError::invalid_config);
      }
      std::size_t root_idx = plan.size();
      if (!resolve_root(plan[i].shares_with, &root_idx)) {
        destroy();
        return ::warploom::core::Result<void>::error(
            ::warploom::core::RuntimeError::invalid_config);
      }
      const std::string& root_name = plan[root_idx].resource;
      const TransientImageDesc* root_desc = find_desc(root_name);
      if (root_desc == nullptr || !image_descs_equal(*desc, *root_desc)) {
        destroy();
        return ::warploom::core::Result<void>::error(
            ::warploom::core::RuntimeError::invalid_config);
      }
      bool found_owner = false;
      for (const OwnerMemory& o : owners_) {
        if (o.name == root_name) {
          owner_memory = o.memory;
          owner_offset = o.offset;
          // Root allocation size, for the requirements check below.
          owner_size = o.allocation.size;
          found_owner = true;
          break;
        }
      }
      if (!found_owner || owner_memory == VK_NULL_HANDLE) {
        destroy();
        return ::warploom::core::Result<void>::error(
            ::warploom::core::RuntimeError::invalid_config);
      }
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent = {desc->width, desc->height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.format = desc->format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = desc->usage;
    // Loop28 step 1: the H-Z depth source is CONCURRENT over both families;
    // every other store image stays EXCLUSIVE. Same stack-copy rule as the
    // pyramid: vkCreateImage copies the indices synchronously.
    std::uint32_t sharing_families[2] = {desc->sharing.graphics_family,
                                         desc->sharing.compute_family};
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (desc->sharing.concurrent_ok()) {
      image_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
      image_info.queueFamilyIndexCount = 2U;
      image_info.pQueueFamilyIndices = sharing_families;
    }
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
      destroy();
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::vulkan_not_available);
    }

    VkDeviceMemory bound_memory = VK_NULL_HANDLE;
    VkDeviceSize bound_offset = 0U;
    if (aliased) {
      VkMemoryRequirements requirements{};
      vkGetImageMemoryRequirements(device, image, &requirements);
      // Same description as the owner (checked above) means same size and
      // alignment needs; the owner's offset satisfied them, so it satisfies
      // this image too. Checked, not assumed: a driver that disagrees fails
      // here instead of faulting later.
      if (requirements.size > owner_size ||
          (owner_offset % requirements.alignment) != 0U) {
        vkDestroyImage(device, image, nullptr);
        destroy();
        return ::warploom::core::Result<void>::error(
            ::warploom::core::RuntimeError::invalid_config);
      }
      if (vkBindImageMemory(device, image, owner_memory, owner_offset) !=
          VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        destroy();
        return ::warploom::core::Result<void>::error(
            ::warploom::core::RuntimeError::vulkan_not_available);
      }
      bound_memory = owner_memory;
      bound_offset = owner_offset;
    } else {
      auto allocation = allocator_->bind_image(
          image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!allocation.is_ok()) {
        vkDestroyImage(device, image, nullptr);
        destroy();
        return ::warploom::core::Result<void>::error(allocation.error());
      }
      OwnerMemory owner{};
      owner.name = desc->name;
      owner.memory = allocation.value().memory;
      owner.offset = allocation.value().offset;
      owner.allocation = allocation.value();
      owner.allocation.image = VK_NULL_HANDLE;
      owner.from_allocator = true;
      owners_.push_back(owner);
      bound_memory = owner.memory;
      bound_offset = owner.offset;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = desc->format;
    view_info.subresourceRange.aspectMask = aspect_of(*desc);
    view_info.subresourceRange.levelCount = 1U;
    view_info.subresourceRange.layerCount = 1U;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
      vkDestroyImage(device, image, nullptr);
      destroy();
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::vulkan_not_available);
    }

    TransientImage entry{};
    entry.name = desc->name;
    entry.image = image;
    entry.view = view;
    entry.memory = bound_memory;
    entry.offset = bound_offset;
    entry.aliased = aliased;
    entry.shares_with = aliased ? plan[i].shares_with : std::string{};
    images_.push_back(entry);
  }

  ready_ = true;
  ++generation_;
  return ::warploom::core::Result<void>::ok();
#else
  (void)device;
  (void)physical_device;
  (void)allocator;
  (void)resources;
  (void)plan;
  (void)descs;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanTransientStore::destroy() noexcept {
#ifdef WARPLOOM_HAS_VULKAN
  if (device_ != VK_NULL_HANDLE) {
    for (TransientImage& entry : images_) {
      if (entry.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, entry.view, nullptr);
        entry.view = VK_NULL_HANDLE;
      }
      if (entry.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, entry.image, nullptr);
        entry.image = VK_NULL_HANDLE;
      }
      entry.memory = VK_NULL_HANDLE;
    }
    // Each owner allocation released exactly once; sharers held no reference.
    for (OwnerMemory& owner : owners_) {
      if (owner.from_allocator && allocator_ != nullptr) {
        allocator_->destroy_allocation(owner.allocation);
      }
      owner.memory = VK_NULL_HANDLE;
      owner.allocation = {};
    }
  }
#endif
  images_.clear();
  owners_.clear();
  device_ = VK_NULL_HANDLE;
  allocator_ = nullptr;
  ready_ = false;
}

const TransientImage* VulkanTransientStore::find(
    const std::string& name) const noexcept {
  for (const TransientImage& entry : images_) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

VkImage VulkanTransientStore::image(const std::string& name) const noexcept {
  const TransientImage* entry = find(name);
  return entry != nullptr ? entry->image : VK_NULL_HANDLE;
}

VkImageView VulkanTransientStore::view(const std::string& name) const noexcept {
  const TransientImage* entry = find(name);
  return entry != nullptr ? entry->view : VK_NULL_HANDLE;
}

}  // namespace warploom::render
