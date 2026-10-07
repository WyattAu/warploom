#pragma once

//! @file vulkan_command.hpp
//! @brief Command-pool / command-buffer allocation, shared infrastructure.
//!
//! Were VulkanRenderer public statics. Extracted so the compose chain (and
//! anything else that records its own one-shot work) can allocate without
//! depending on the renderer type -- the same promotion B3b did for the
//! full-screen pass. The renderer's statics remain as thin wrappers over these
//! so ~60 existing call sites in tests, examples and engine code do not move.

#include <cstdint>

#include <warploom/core/deterministic_runtime.hpp>
#include <warploom/render/vulkan_types.hpp>

namespace warploom::render {

//! TRANSIENT-pool? No: RESET_COMMAND_BUFFER_BIT, because each allocated
//! primary buffer is re-recorded every frame rather than reset in bulk.
[[nodiscard]] ::warploom::core::Result<VkCommandPool> create_command_pool(
    VkDevice device, std::uint32_t queue_family_index);

[[nodiscard]] ::warploom::core::Result<VkCommandBuffer> allocate_command_buffer(
    VkDevice device, VkCommandPool pool);

}  // namespace warploom::render
