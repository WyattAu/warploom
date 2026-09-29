#include "warploom/render/vulkan_descriptors.hpp"

#include <cstring>
#include <unordered_map>
#include <utility>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace omnicpp::render {

// =============================================================================
// Minimal SPIR-V reflection
// =============================================================================
//
// SPIR-V is a little-endian word stream: magic number, version, generator,
// bound, schema, then instructions as [opcode:16 | word_count:16] followed by
// operands. Types/variables use <id> references, so we resolve decoration
// targets through the Debug-type-name and Variable storage-class passes.
//
// This is deliberately a subset reflector: it covers descriptor-relevant
// storage classes and ignores anything else (push constants, UBO members,
// varying interface, etc.). No external SDK dependency.

namespace {

// All types here resolve via vulkan_types.hpp in Vulkan-off builds.

constexpr std::uint32_t kSpirvMagic = 0x07230203U;

// Op codes used by the reflector.
constexpr std::uint16_t kOpEntryPoint = 15;
constexpr std::uint16_t kOpConstant = 43;
constexpr std::uint16_t kOpDecorate = 71;
constexpr std::uint16_t kOpTypeStruct = 30;
constexpr std::uint16_t kOpTypeArray = 28;
constexpr std::uint16_t kOpTypeRuntimeArray = 29;
constexpr std::uint16_t kOpTypePointer = 32;
constexpr std::uint16_t kOpTypeSampler = 26;
constexpr std::uint16_t kOpTypeSampledImage = 27;
constexpr std::uint16_t kOpTypeImage = 25;
constexpr std::uint16_t kOpVariable = 59;

// Execution models (OpEntryPoint).
constexpr std::uint32_t kExecutionModelVertex = 0;
constexpr std::uint32_t kExecutionModelFragment = 4;
constexpr std::uint32_t kExecutionModelGeometry = 3;
constexpr std::uint32_t kExecutionModelTessellationControl = 1;
constexpr std::uint32_t kExecutionModelTessellationEvaluation = 2;
constexpr std::uint32_t kExecutionModelCompute = 5;

// Decorations.
constexpr std::uint32_t kDecorationDescriptorSet = 34;
constexpr std::uint32_t kDecorationBinding = 33;

// Storage classes (SPIR-V 1.x subset relevant to descriptors).
constexpr std::uint32_t kStorageUniformConstant = 0;
constexpr std::uint32_t kStorageUniform = 2;
constexpr std::uint32_t kStorageStorageBuffer = 12;

struct IdInfo {
  std::uint32_t kind{0};            // Classification enum below.
  std::uint32_t pointer_class{0};   // Storage class for pointer types.
  std::uint32_t pointee_id{0};      // Pointee type for pointers.
  std::uint32_t set{0};
  std::uint32_t binding{0};
  bool has_binding{false};
  std::uint32_t array_dim{1};
  bool is_array{false};             // OpTypeArray (sized).
  bool is_runtime_array{false};     // OpTypeRuntimeArray (unsized).
  std::uint32_t element_id{0};      // Element type for array types.
  std::uint32_t length_id{0};       // Length operand id for sized arrays.
  std::uint32_t constant_value{0};  // Literal for OpConstant scalars.
  bool is_constant{false};
  std::vector<std::uint32_t> member_ids;  // OpTypeStruct members.
  std::uint32_t stage_flags{0};   // VkShaderStageFlags bit contributed by entry points.
};

enum ResourceKind : std::uint32_t {
  kKindUnknown = 0,
  kKindUniformBuffer,
  kKindStorageBuffer,
  kKindSampledImage,
  kKindSampler,
  kKindStorageImage,
  kKindInputAttachment,
  kKindCombinedImageSampler,
};

[[maybe_unused]] VkDescriptorType descriptor_type_for(std::uint32_t kind) {
  switch (kind) {
    case kKindUniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case kKindStorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case kKindSampledImage: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case kKindSampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case kKindStorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case kKindInputAttachment: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    case kKindCombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
  }
}

[[maybe_unused]] std::uint32_t stage_flag_for(std::uint32_t execution_model) {
  switch (execution_model) {
    case kExecutionModelVertex: return VK_SHADER_STAGE_VERTEX_BIT;
    case kExecutionModelFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case kExecutionModelGeometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case kExecutionModelTessellationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case kExecutionModelTessellationEvaluation: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case kExecutionModelCompute: return VK_SHADER_STAGE_COMPUTE_BIT;
    default: return 0;
  }
}

} // namespace

std::vector<ReflectedBinding> reflect_spirv_resources(
    const std::uint32_t* code, std::size_t code_word_count) {
  std::vector<ReflectedBinding> out;
#ifdef OMNICPP_HAS_VULKAN
  if (!code || code_word_count < 5) return out;
  if (code[0] != kSpirvMagic) return out;

  std::unordered_map<std::uint32_t, IdInfo> ids;
  std::uint32_t entry_stage_flags = 0;

  std::size_t word = 5;
  while (word + 1 <= code_word_count) {
    const std::uint32_t instruction = code[word];
    const std::uint16_t opcode = static_cast<std::uint16_t>(instruction & 0xFFFFU);
    const std::uint16_t word_count = static_cast<std::uint16_t>(instruction >> 16U);
    if (word_count == 0 || word + word_count > code_word_count) break;

    const std::uint32_t* operands = &code[word + 1];
    const std::size_t operand_count = word_count - 1;

    switch (opcode) {
      case kOpEntryPoint: {
        // operands: exec model, entry point id, name..., interface ids...
        if (operand_count >= 1) {
          entry_stage_flags |= stage_flag_for(operands[0]);
        }
        break;
      }
      case kOpTypePointer: {
        // operands: result id, storage class, pointee type id. (The original
        // subset read operands[1] as the pointee, which silently broke every
        // pointer whose storage class word was parsed as the pointee id.)
        if (operand_count >= 3) {
          IdInfo info;
          info.pointer_class = operands[1];
          info.pointee_id = operands[2];
          ids[operands[0]] = info;
        }
        break;
      }
      case kOpTypeStruct: {
        if (operand_count >= 1) {
          IdInfo info;
          info.member_ids.assign(operands, operands + operand_count);
          ids[operands[0]] = std::move(info); // Struct members for runtime-array detection.
        }
        break;
      }
      case kOpTypeArray: {
        // operands: result id, element type id, length id.
        if (operand_count >= 3) {
          IdInfo info;
          info.is_array = true;
          info.element_id = operands[1];
          info.length_id = operands[2];
          ids[operands[0]] = info;
        }
        break;
      }
      case kOpTypeRuntimeArray: {
        // operands: result id, element type id.
        if (operand_count >= 2) {
          IdInfo info;
          info.is_runtime_array = true;
          info.element_id = operands[1];
          ids[operands[0]] = info;
        }
        break;
      }
      case kOpTypeSampler: {
        if (operand_count >= 1) ids[operands[0]].kind = kKindSampler;
        break;
      }
      case kOpTypeSampledImage: {
        if (operand_count >= 1) ids[operands[0]].kind = kKindSampledImage;
        break;
      }
      case kOpTypeImage: {
        // operands: result, sampled type, dim, depth, arrayed, ms, sampled, format
        if (operand_count >= 7) {
          IdInfo info;
          // "sampled" == 2 means storage image (read/write without sampler).
          info.kind = (operands[6] == 2U) ? kKindStorageImage : kKindUnknown;
          ids[operands[0]] = info;
        }
        break;
      }
      case kOpConstant: {
        // operands: result type id, result id, literal words (scalars only).
        if (operand_count >= 3) {
          auto& slot = ids[operands[1]];
          slot.is_constant = true;
          slot.constant_value = operands[2];
        }
        break;
      }
      case kOpDecorate: {
        if (operand_count >= 3) {
          const std::uint32_t target = operands[0];
          const std::uint32_t decoration = operands[1];
          if (decoration == kDecorationDescriptorSet) {
            ids[target].set = operands[2];
          } else if (decoration == kDecorationBinding) {
            ids[target].binding = operands[2];
            ids[target].has_binding = true;
          }
        }
        break;
      }
      case kOpVariable: {
        if (operand_count >= 3) {
          // SPIR-V operand order: [result type id, result id, storage class].
          const std::uint32_t result_type = operands[0];   // pointer type id
          const std::uint32_t result_id = operands[1];     // the variable's own id
          const std::uint32_t storage_class = operands[2];

          std::uint32_t kind = kKindUnknown;
          if (storage_class == kStorageUniform) {
            kind = kKindUniformBuffer;
            // glslang represents `readonly buffer` SSBOs with Uniform storage.
            // A block whose (array-wrapped) struct contains a runtime array is
            // an SSBO per the spec, so reclassify for correct layouts.
            std::uint32_t pointee = 0;
            const auto ptr_it = ids.find(result_type);
            if (ptr_it != ids.end()) pointee = ptr_it->second.pointee_id;
            for (std::uint32_t hop = 0; hop < 8 && pointee != 0U; ++hop) {
              const auto it = ids.find(pointee);
              if (it == ids.end()) break;
              if (it->second.is_array || it->second.is_runtime_array) {
                pointee = it->second.element_id;  // Look through array wrappers.
              } else if (it->second.member_ids.empty()) {
                break;
              } else {
                for (const std::uint32_t member : it->second.member_ids) {
                  const auto member_it = ids.find(member);
                  if (member_it != ids.end() && member_it->second.is_runtime_array) {
                    kind = kKindStorageBuffer;
                    break;
                  }
                }
                break;
              }
            }
          } else if (storage_class == kStorageStorageBuffer) {
            kind = kKindStorageBuffer;
          } else if (storage_class == kStorageUniformConstant) {
            // Resolve through the pointer's pointee type, looking through
            // array wrappers: bindless textures are runtime arrays of
            // sampled images, so the pointee is OpTypeRuntimeArray, not the
            // sampled-image type itself.
            const auto ptr_it = ids.find(result_type);
            if (ptr_it != ids.end()) {
              std::uint32_t pointee = ptr_it->second.pointee_id;
              for (std::uint32_t hop = 0; hop < 8 && pointee != 0U; ++hop) {
                const auto it = ids.find(pointee);
                if (it == ids.end()) break;
                if (it->second.is_array || it->second.is_runtime_array) {
                  pointee = it->second.element_id;  // Look through wrappers.
                  continue;
                }
                if (it->second.kind != kKindUnknown) kind = it->second.kind;
                break;
              }
            }
          } else {
            break; // Push constants, uniforms-in, etc. are not descriptors.
          }

          IdInfo var_info;
          var_info.kind = kind;
          // Resolve the descriptor array dimension from the pointee type:
          // OpTypeRuntimeArray -> 0 (runtime descriptor array); OpTypeArray ->
          // the constant length when resolvable.
          var_info.array_dim = 1;
          {
            const auto var_ptr_it = ids.find(result_type);
            if (var_ptr_it != ids.end()) {
              const auto pointee_it = ids.find(var_ptr_it->second.pointee_id);
              if (pointee_it != ids.end()) {
                if (pointee_it->second.is_runtime_array) {
                  var_info.array_dim = 0;
                } else if (pointee_it->second.is_array) {
                  const auto len_it = ids.find(pointee_it->second.length_id);
                  var_info.array_dim =
                      (len_it != ids.end() && len_it->second.is_constant)
                          ? len_it->second.constant_value
                          : 0U;
                }
              }
            }
          }
          // The variable inherits any decorations applied to its own id.
          const auto existing = ids.find(result_id);
          if (existing != ids.end()) {
            var_info.set = existing->second.set;
            var_info.binding = existing->second.binding;
            var_info.has_binding = existing->second.has_binding;
          }
          ids[result_id] = var_info;
          // Propagate set/binding from the pointer type if decorated there.
          const auto value_it = ids.find(result_type);
          if (value_it != ids.end()) {
            auto& slot = ids[result_id];
            if (value_it->second.set != 0U || value_it->second.has_binding) {
              slot.set = value_it->second.set;
            }
            if (value_it->second.has_binding) {
              slot.binding = value_it->second.binding;
              slot.has_binding = true;
            }
          }
        }
        break;
      }
      default:
        break;
    }

    word += word_count;
  }

  // Emit bindings: variables carry set/binding decorations; stage flags come
  // from the module's entry points (single-stage modules are the common case;
  // multi-stage flags merge via OR in the dedup pass below).
  for (const auto& [id, info] : ids) {
    if (info.kind == kKindUnknown || !info.has_binding) continue;
    ReflectedBinding binding;
    binding.set = info.set;
    binding.binding = info.binding;
    binding.count = info.array_dim;
    binding.stage_flags = entry_stage_flags;
    binding.type = descriptor_type_for(info.kind);
    out.push_back(binding);
  }
  // Deduplicate: variables of the same struct pointer produce identical rows.
  std::vector<ReflectedBinding> deduped;
  for (const auto& b : out) {
    bool duplicate = false;
    for (auto& existing : deduped) {
      if (existing.set == b.set && existing.binding == b.binding &&
          existing.type == b.type) {
        existing.stage_flags |= b.stage_flags; // merge stages across modules
        duplicate = true;
        break;
      }
    }
    if (!duplicate) deduped.push_back(b);
  }
  return deduped;
#else
  (void)code; (void)code_word_count;
  return out;
#endif
}

std::vector<ReflectedBinding> reflect_spirv_resources(
    const std::uint8_t* code, std::size_t code_byte_count) {
  if (!code || code_byte_count < 20 || code_byte_count % 4U != 0U) return {};
  std::uint32_t magic = 0;
  std::memcpy(&magic, code, 4);
  if (magic != 0x07230203U) return {};
  return reflect_spirv_resources(reinterpret_cast<const std::uint32_t*>(code),
                                 code_byte_count / 4U);
}

// =============================================================================
// VulkanDescriptorManager
// =============================================================================

VulkanDescriptorManager::~VulkanDescriptorManager() { cleanup(); }

omnicpp::core::Result<void> VulkanDescriptorManager::initialize(VkDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  device_ = device;
  return omnicpp::core::Result<void>::ok();
#else
  (void)device;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanDescriptorManager::cleanup() noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device_) {
    for (const auto& info : layouts_) {
      if (info.layout) vkDestroyDescriptorSetLayout(device_, info.layout, nullptr);
      if (info.pool) vkDestroyDescriptorPool(device_, info.pool, nullptr);
    }
  }
#endif
  layouts_.clear();
  device_ = VK_NULL_HANDLE;
}

const DescriptorSetLayoutInfo* VulkanDescriptorManager::find_layout(
    VkDescriptorSetLayout layout) const noexcept {
#ifdef OMNICPP_HAS_VULKAN
  for (const auto& info : layouts_) {
    if (info.layout == layout) return &info;
  }
#else
  (void)layout;
#endif
  return nullptr;
}

omnicpp::core::Result<VkDescriptorSetLayout> VulkanDescriptorManager::create_layout(
    const std::vector<ReflectedBinding>& bindings, std::uint32_t sets_to_reserve,
    bool bindless) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_) {
    return omnicpp::core::Result<VkDescriptorSetLayout>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }
  if (bindings.empty()) {
    return omnicpp::core::Result<VkDescriptorSetLayout>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }

  std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
  layout_bindings.reserve(bindings.size());
  for (const auto& binding : bindings) {
    if (binding.type == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
      return omnicpp::core::Result<VkDescriptorSetLayout>::error(
          omnicpp::core::RuntimeError::invalid_config);
    }
    VkDescriptorSetLayoutBinding lb{};
    lb.binding = binding.binding;
    lb.descriptorType = binding.type;
    lb.descriptorCount = binding.count;
    lb.stageFlags = binding.stage_flags;
    layout_bindings.push_back(lb);
  }

  // Bindless: every binding is update-after-bind and partially bound, so
  // descriptors can be null at bind time and written any time up to
  // draw/dispatch submission. Requires negotiated device support.
  // Runtime-array bindings keep a fixed bounded count in the layout (the
  // shader's unsized array inherits this bound): zero-count bindings combined
  // with update-after-bind trigger allocator bugs in validation-layer builds.
  constexpr std::uint32_t kDefaultBindlessArrayCapacity = 1024;
  std::vector<VkDescriptorBindingFlags> binding_flags;
  VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
  if (bindless) {
    for (auto& lb : layout_bindings) {
      if (lb.descriptorCount == 0U) lb.descriptorCount = kDefaultBindlessArrayCapacity;
    }
    binding_flags.resize(layout_bindings.size(),
                         VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                             VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
    binding_flags_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    binding_flags_info.bindingCount = static_cast<std::uint32_t>(binding_flags.size());
    binding_flags_info.pBindingFlags = binding_flags.data();
  }

  VkDescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (bindless) {
    // UPDATE_AFTER_BIND_POOL is the only layout-level flag required; the
    // per-binding flags above carry partially-bound + update-after-bind.
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_info.pNext = &binding_flags_info;
  }
  layout_info.bindingCount = static_cast<std::uint32_t>(layout_bindings.size());
  layout_info.pBindings = layout_bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &layout) != VK_SUCCESS) {
    return omnicpp::core::Result<VkDescriptorSetLayout>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }

  // Pool capacity from the reserved set count; one pool per layout so
  // bindless (update-after-bind) and regular layouts coexist. Bindless
  // runtime-array bindings were expanded to the fixed capacity above, so
  // pool sizing sees the same effective counts.
  VkDescriptorPool pool = VK_NULL_HANDLE;
  {
    std::unordered_map<VkDescriptorType, std::uint32_t> type_counts;
    for (const auto& lb : layout_bindings) {
      type_counts[lb.descriptorType] += lb.descriptorCount * sets_to_reserve;
    }
    std::vector<VkDescriptorPoolSize> pool_sizes;
    pool_sizes.reserve(type_counts.size());
    for (const auto& [type, count] : type_counts) {
      pool_sizes.push_back({type, count});
    }
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // Bindless sets are persistent and freed with the pool; regular sets
    // support individual free for frame reuse.
    pool_info.flags = bindless ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                               : VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = sets_to_reserve;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool) != VK_SUCCESS) {
      vkDestroyDescriptorSetLayout(device_, layout, nullptr);
      return omnicpp::core::Result<VkDescriptorSetLayout>::error(
          omnicpp::core::RuntimeError::vulkan_not_available);
    }
  }

  DescriptorSetLayoutInfo info;
  info.layout = layout;
  info.bindings = bindings;
  info.pool = pool;
  info.bindless = bindless;
  layouts_.push_back(std::move(info));
  return omnicpp::core::Result<VkDescriptorSetLayout>::ok(layout);
#else
  (void)bindings; (void)sets_to_reserve; (void)bindless;
  return omnicpp::core::Result<VkDescriptorSetLayout>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<VkDescriptorSet> VulkanDescriptorManager::allocate_set(
    VkDescriptorSetLayout layout) {
#ifdef OMNICPP_HAS_VULKAN
  const DescriptorSetLayoutInfo* info = find_layout(layout);
  if (!device_ || !info || !info->pool || !layout) {
    return omnicpp::core::Result<VkDescriptorSet>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }
  VkDescriptorSetAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc_info.descriptorPool = info->pool;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device_, &alloc_info, &set) != VK_SUCCESS) {
    return omnicpp::core::Result<VkDescriptorSet>::error(
        omnicpp::core::RuntimeError::vulkan_not_available);
  }
  return omnicpp::core::Result<VkDescriptorSet>::ok(set);
#else
  (void)layout;
  return omnicpp::core::Result<VkDescriptorSet>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanDescriptorManager::write_buffer(
    VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || !set || !buffer) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  VkDescriptorBufferInfo buffer_info{};
  buffer_info.buffer = buffer;
  buffer_info.offset = offset;
  buffer_info.range = range;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = binding;
  write.descriptorCount = 1;
  write.descriptorType = type;
  write.pBufferInfo = &buffer_info;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  return omnicpp::core::Result<void>::ok();
#else
  (void)set; (void)binding; (void)type; (void)buffer; (void)offset; (void)range;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanDescriptorManager::write_image(
    VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
    VkSampler sampler, VkImageView view, VkImageLayout layout,
    std::uint32_t array_element) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || !set || !view) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  VkDescriptorImageInfo image_info{};
  image_info.sampler = sampler;
  image_info.imageView = view;
  image_info.imageLayout = layout;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = binding;
  write.dstArrayElement = array_element;
  write.descriptorCount = 1;
  write.descriptorType = type;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  return omnicpp::core::Result<void>::ok();
#else
  (void)set; (void)binding; (void)type; (void)sampler; (void)view; (void)layout;
  return omnicpp::core::Result<void>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanDescriptorManager::write_acceleration_structure(
    VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
    VkAccelerationStructureKHR as, std::uint32_t array_element) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device_ || !set || as == VK_NULL_HANDLE ||
      type != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
    return omnicpp::core::Result<void>::error(
        omnicpp::core::RuntimeError::invalid_config);
  }
  VkWriteDescriptorSetAccelerationStructureKHR as_info{};
  as_info.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
  as_info.accelerationStructureCount = 1;
  as_info.pAccelerationStructures = &as;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = binding;
  write.dstArrayElement = array_element;
  write.descriptorCount = 1;
  write.descriptorType = type;
  write.pNext = &as_info;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  return omnicpp::core::Result<void>::ok();
#else
  (void)set; (void)binding; (void)type; (void)as; (void)array_element;
  return omnicpp::core::Result<void>::error(
      omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

} // namespace omnicpp::render
