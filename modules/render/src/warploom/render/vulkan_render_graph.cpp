#include "warploom/render/vulkan_render_graph.hpp"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace {

#ifdef OMNICPP_HAS_VULKAN
[[nodiscard]] VkImageAspectFlags barrier_aspect_mask(
    VkImageLayout layout, std::uint32_t explicit_aspect) noexcept {
  if (explicit_aspect != 0U) return explicit_aspect;
  switch (layout) {
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
      return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}
#endif

struct ImageSubresourceKey {
  VkImage image{VK_NULL_HANDLE};
  std::uint32_t base_mip{0};
  friend bool operator==(const ImageSubresourceKey& lhs,
                         const ImageSubresourceKey& rhs) noexcept {
    return lhs.image == rhs.image && lhs.base_mip == rhs.base_mip;
  }
};

struct ImageSubresourceKeyHash {
  std::size_t operator()(const ImageSubresourceKey& key) const noexcept {
    const auto image_bits = reinterpret_cast<std::uintptr_t>(key.image);
    return std::hash<std::uintptr_t>{}(image_bits) ^
           (std::hash<std::uint32_t>{}(key.base_mip) << 1U);
  }
};

} // namespace

namespace warploom::render {

// Attachment builder helpers. Values mirror the Vulkan constants so the
// Vulkan-off shim build resolves them; real Vulkan builds see the same bits.

RenderPassAttachment color_attachment(
    VkImage image, VkImageView view, VkFormat format,
    VkImageLayout final_layout) {
  RenderPassAttachment a;
  a.image = image;
  a.view = view;
  a.format = format;
  a.used_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  a.final_layout = final_layout;
#ifdef OMNICPP_HAS_VULKAN
  a.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  a.stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
#else
  constexpr std::uint32_t kColorWriteAccess = 0x100;  // COLOR_ATTACHMENT_WRITE
  constexpr std::uint32_t kColorOutStage = 0x200;
  a.access = kColorWriteAccess;
  a.stage = kColorOutStage;
#endif
  a.is_depth = false;
  return a;
}

RenderPassAttachment depth_attachment(
    VkImage image, VkImageView view, VkFormat format,
    VkImageLayout final_layout) {
  RenderPassAttachment a;
  a.image = image;
  a.view = view;
  a.format = format;
  a.used_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  a.final_layout = final_layout;
#ifdef OMNICPP_HAS_VULKAN
  a.access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  a.stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
#else
  constexpr std::uint32_t kDepthWriteAccess = 0x400;  // DEPTH_STENCIL_ATTACHMENT_WRITE
  constexpr std::uint32_t kEarlyFrag = 0x1000;
  constexpr std::uint32_t kLateFrag = 0x2000;
  a.access = kDepthWriteAccess;
  a.stage = kEarlyFrag | kLateFrag;
#endif
  a.is_depth = true;
  return a;
}

// =============================================================================
// Compiler
// =============================================================================

namespace {

struct ImageState {
  VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
  std::uint32_t access{0};
  std::uint32_t stage{0};
  bool valid{false};   // False until the image's first declaration.
};

//! Previous-pass access vs. this pass's access: barrier needed when the
//! ordering matters (write-after-read, read-after-write, write-after-write).
bool access_ordering_matters(std::uint32_t prev_access, std::uint32_t prev_stage,
                             std::uint32_t next_access, std::uint32_t next_stage) {
  // Vulkan access-mask values are stable across the core API and the project's
  // Vulkan-off shim. Treat every attachment/transfer/shader write as an
  // ordering edge; this is conservative and, importantly, recognizes
  // VK_ACCESS_SHADER_WRITE_BIT (0x40), not the old placeholder bit.
  constexpr std::uint32_t kShaderWrite = 0x40U;
  constexpr std::uint32_t kColorWrite = 0x100U;
  constexpr std::uint32_t kDepthWrite = 0x400U;
  constexpr std::uint32_t kTransferWrite = 0x1000U;
  constexpr std::uint32_t write_mask =
      kShaderWrite | kColorWrite | kDepthWrite | kTransferWrite;
  const bool prev_writes = (prev_access & write_mask) != 0U;
  const bool next_writes = (next_access & write_mask) != 0U;
  (void)prev_stage; (void)next_stage;
  return prev_writes || next_writes;
}

} // namespace

CompiledRenderGraph compile_render_graph(const std::vector<GraphPass>& passes) {
  CompiledRenderGraph out;
  out.barriers_per_pass.resize(passes.size());
  std::unordered_map<VkImage, ImageState> states;

  for (std::size_t p = 0; p < passes.size(); ++p) {
    const GraphPass& pass = passes[p];
    std::vector<GraphBarrier>& barriers = out.barriers_per_pass[p];

    for (const RenderPassAttachment& att : pass.attachments) {
      if (!att.image) continue;
      ImageState& state = states[att.image];

      const bool needs_barrier =
          !state.valid ||
          state.layout != att.used_layout ||
          access_ordering_matters(state.access, state.stage,
                                  att.access, att.stage);

      if (needs_barrier) {
        GraphBarrier barrier;
        barrier.image = att.image;
        barrier.old_layout = state.valid ? state.layout : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.new_layout = att.used_layout;
        barrier.src_access = state.access;
        barrier.dst_access = att.access;
        barrier.src_stage = state.valid ? state.stage : 0U;
        barrier.dst_stage = att.stage;
        barriers.push_back(barrier);
      }
      state.layout = att.final_layout;
      state.access = att.access;
      state.stage = att.stage;
      state.valid = true;
    }
  }

  return out;
}

// =============================================================================
// Executor
// =============================================================================

void execute_render_graph(
    VkCommandBuffer command_buffer,
    const std::vector<GraphPass>& passes,
    const CompiledRenderGraph& compiled,
    void (*record_pass)(VkCommandBuffer, const GraphPass&, void*)) {
  if (!command_buffer) return;

#ifdef OMNICPP_HAS_VULKAN
  for (std::size_t p = 0; p < passes.size() && p < compiled.barriers_per_pass.size(); ++p) {
    const auto& barriers = compiled.barriers_per_pass[p];
    if (!barriers.empty()) {
      // Reused scratch storage: zero heap operations on the steady-state frame
      // path. thread_local keeps concurrent recording on separate command
      // buffers safe.
      thread_local std::vector<VkImageMemoryBarrier> vk_barriers;
      vk_barriers.clear();
      vk_barriers.reserve(barriers.size());
      VkPipelineStageFlags src_stage_mask = 0;
      VkPipelineStageFlags dst_stage_mask = 0;
      for (const auto& barrier : barriers) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = barrier.old_layout;
        b.newLayout = barrier.new_layout;
        b.srcAccessMask = barrier.src_access;
        b.dstAccessMask = barrier.dst_access;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = barrier.image;
        b.subresourceRange.aspectMask = barrier_aspect_mask(barrier.new_layout, barrier.aspect);
        b.subresourceRange.baseMipLevel = barrier.base_mip;
        b.subresourceRange.levelCount = barrier.level_count;
        b.subresourceRange.layerCount = 1;
        vk_barriers.push_back(b);
        src_stage_mask |= barrier.src_stage;
        dst_stage_mask |= barrier.dst_stage;
      }
      // UNDEFINED -> X transitions need no src access; ensure non-zero masks.
      if (src_stage_mask == 0) src_stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      if (dst_stage_mask == 0) dst_stage_mask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      vkCmdPipelineBarrier(command_buffer, src_stage_mask, dst_stage_mask,
                           0, 0, nullptr, 0, nullptr,
                           static_cast<std::uint32_t>(vk_barriers.size()),
                           vk_barriers.data());
    }

    const GraphPass& pass = passes[p];

    VkRenderPassBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = pass.render_pass;
    begin_info.framebuffer = pass.framebuffer;
    begin_info.renderArea.extent = {pass.width, pass.height};
    begin_info.clearValueCount = pass.clear_value_count;
    begin_info.pClearValues = pass.clear_values;
    vkCmdBeginRenderPass(command_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

    if (record_pass) {
      record_pass(command_buffer, pass, pass.user_data);
    }

    vkCmdEndRenderPass(command_buffer);
  }
#else
  (void)passes; (void)compiled; (void)record_pass;
#endif
}

// =============================================================================
// Mixed render/compute graph
// =============================================================================

CompiledGraph compile_graph(const std::vector<GraphNode>& nodes) {
  CompiledGraph out;
  out.barriers_per_node.resize(nodes.size());
  out.buffer_edges_per_node.resize(nodes.size());
  std::unordered_map<ImageSubresourceKey, ImageState, ImageSubresourceKeyHash> states;

  for (std::size_t p = 0; p < nodes.size(); ++p) {
    const GraphNode& node = nodes[p];
    const GraphPass* render = node.render;
    if (render != nullptr) {
      std::vector<GraphBarrier>& barriers = out.barriers_per_node[p];
#ifdef OMNICPP_HAS_VULKAN
      constexpr std::uint32_t kShaderRead = VK_ACCESS_SHADER_READ_BIT;
      constexpr std::uint32_t kFragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
#else
      constexpr std::uint32_t kShaderRead = 0x20;   // SHADER_READ
      constexpr std::uint32_t kFragStage = 0x80;    // FRAGMENT_SHADER
#endif
      for (const RenderPassAttachment& att : render->attachments) {
        if (!att.image) continue;
        ImageState& state = states[{att.image, 0U}];
        const bool needs_barrier =
            !state.valid ||
            state.layout != att.used_layout ||
            access_ordering_matters(state.access, state.stage, att.access, att.stage);
        if (needs_barrier) {
          GraphBarrier barrier;
          barrier.image = att.image;
          barrier.old_layout = state.valid ? state.layout : VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.new_layout = att.used_layout;
          barrier.src_access = state.access;
          barrier.dst_access = att.access;
          barrier.src_stage = state.valid ? state.stage : 0U;
          barrier.dst_stage = att.stage;
          barrier.aspect = 0U;
          barriers.push_back(barrier);
        }
        state.layout = att.final_layout;
        state.access = att.access;
        state.stage = att.stage;
        state.valid = true;
      }

      // Sampled (non-attachment) images: track the layout transition from
      // whatever an earlier graph pass left behind into the sampled layout.
      // A depth attachment -> shader read (shadow map) carries the
      // write->read execution barrier inside the layout transition.
      for (const GraphSampledImage& sampled : render->sampled_images) {
        if (!sampled.image) continue;
        ImageState& state = states[{sampled.image, 0U}];
        if (!state.valid) {
          if (sampled.initial_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            // No in-graph producer and the caller asserts the image is
            // already in used_layout with no outstanding writes. Record the
            // state and emit nothing.
            state.layout = sampled.used_layout;
            state.access = kShaderRead;
            state.stage = kFragStage;
            state.valid = true;
            continue;
          }
          // An external producer left the image somewhere else. Emit the
          // transition from where it really is, the way the compute path
          // already does with GraphImageUse::initial_layout.
          GraphBarrier barrier;
          barrier.image = sampled.image;
          barrier.old_layout = sampled.initial_layout;
          barrier.new_layout = sampled.used_layout;
          barrier.src_access = sampled.initial_access;
          barrier.dst_access = kShaderRead;
          barrier.src_stage = sampled.initial_stage;
          barrier.dst_stage = kFragStage;
          barrier.aspect = sampled.aspect;
          barriers.push_back(barrier);
          state.layout = sampled.used_layout;
          state.access = kShaderRead;
          state.stage = kFragStage;
          state.valid = true;
          continue;
        }
        constexpr std::uint32_t kSampleAccess = kShaderRead;
        constexpr std::uint32_t kSampleStage = kFragStage;
        if (state.layout != sampled.used_layout ||
            access_ordering_matters(state.access, state.stage,
                                    kSampleAccess, kSampleStage)) {
          GraphBarrier barrier;
          barrier.image = sampled.image;
          barrier.old_layout = state.layout;
          barrier.new_layout = sampled.used_layout;
          barrier.src_access = state.access;
          barrier.dst_access = kSampleAccess;
          barrier.src_stage = state.stage;
          barrier.dst_stage = kSampleStage;
          barrier.aspect = sampled.aspect;
          barriers.push_back(barrier);
        }
        // Sampling is a read: state keeps the sampled layout with read
        // access so a later writer sees the WAR edge.
        state.layout = sampled.used_layout;
        state.access = kSampleAccess;
        state.stage = kSampleStage;
      }
    }

    if (node.compute != nullptr) {
      for (const GraphImageUse& use : node.compute->image_uses) {
        if (!use.image || use.level_count == 0U) continue;
        for (std::uint32_t level = 0; level < use.level_count; ++level) {
          ImageState& state = states[{use.image, use.base_mip + level}];
          const bool needs_barrier =
              !state.valid || state.layout != use.used_layout ||
              access_ordering_matters(state.access, state.stage, use.access, use.stage);
          if (needs_barrier) {
            GraphBarrier barrier;
            barrier.image = use.image;
            barrier.old_layout = state.valid ? state.layout : use.initial_layout;
            barrier.new_layout = use.used_layout;
            barrier.src_access = state.valid ? state.access : use.initial_access;
            barrier.dst_access = use.access;
            barrier.src_stage = state.valid ? state.stage : use.initial_stage;
            barrier.dst_stage = use.stage;
            barrier.base_mip = use.base_mip + level;
            barrier.level_count = 1U;
            barrier.aspect = use.aspect;
            out.barriers_per_node[p].push_back(barrier);
          }
          state.layout = use.final_layout;
          state.access = use.access;
          state.stage = use.stage;
          state.valid = true;
        }
      }
    }
    // Buffer edges are declared on the CONSUMER node: the barrier must be
    // recorded before the consumer's work. Copy them into the compiled plan.
    if (node.compute != nullptr) {
      out.buffer_edges_per_node[p] = node.compute->buffer_edges;
    } else if (node.render != nullptr) {
      out.buffer_edges_per_node[p] = node.render->buffer_edges;
    }
  }
  return out;
}

void execute_graph(
    VkCommandBuffer command_buffer,
    const std::vector<GraphNode>& nodes,
    const CompiledGraph& compiled,
    void (*record_render)(VkCommandBuffer, const GraphPass&, void*),
    void (*record_compute)(VkCommandBuffer, const GraphComputePass&, void*),
    std::uint32_t current_family) {
  if (!command_buffer) return;
#ifdef OMNICPP_HAS_VULKAN
  // Staging vectors for the steady-state frame path (no heap operations);
  // thread_local keeps concurrent recording on separate buffers safe.
  thread_local std::vector<VkImageMemoryBarrier> vk_barriers;
  thread_local std::vector<VkImageMemoryBarrier> vk_post_barriers;
  thread_local std::vector<VkBufferMemoryBarrier> vk_buffer_barriers;
  thread_local std::vector<VkBufferMemoryBarrier> vk_foreign_releases;

  for (std::size_t p = 0; p < nodes.size() && p < compiled.barriers_per_node.size(); ++p) {
    const GraphNode& node = nodes[p];
    const auto& barriers = compiled.barriers_per_node[p];
    const auto& edges = (p < compiled.buffer_edges_per_node.size())
                            ? compiled.buffer_edges_per_node[p]
                            : std::vector<GraphBufferEdge>{};

    vk_barriers.clear();
    vk_post_barriers.clear();
    vk_buffer_barriers.clear();
    vk_foreign_releases.clear();

    for (const GraphBarrier& barrier : barriers) {
      VkImageMemoryBarrier b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.oldLayout = barrier.old_layout;
      b.newLayout = barrier.new_layout;
      b.srcAccessMask = barrier.src_access;
      b.dstAccessMask = barrier.dst_access;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = barrier.image;
      b.subresourceRange.aspectMask = barrier_aspect_mask(barrier.new_layout, barrier.aspect);
      b.subresourceRange.baseMipLevel = barrier.base_mip;
      b.subresourceRange.levelCount = barrier.level_count;
      b.subresourceRange.layerCount = 1;
      vk_barriers.push_back(b);
    }

    for (const GraphBufferEdge& edge : edges) {
      if (!edge.buffer || edge.producer_stage == 0 || edge.consumer_stage == 0) {
        continue;  // Malformed edge: skip rather than emit a no-op barrier.
      }
      VkBufferMemoryBarrier b{};
      b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      b.dstAccessMask = edge.consumer_access;
      b.srcQueueFamilyIndex = edge.producer_family;
      b.dstQueueFamilyIndex = edge.consumer_family;
      b.buffer = edge.buffer;
      b.offset = 0;
      b.size = VK_WHOLE_SIZE;
      if (edge.producer_family != VK_QUEUE_FAMILY_IGNORED &&
          edge.consumer_family != VK_QUEUE_FAMILY_IGNORED &&
          edge.producer_family != edge.consumer_family) {
        if (edge.producer_family == current_family) {
          // This queue produced the data: record the release half here.
          b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          b.dstAccessMask = 0;
          vk_foreign_releases.push_back(b);
        } else {
          // Another queue produced it: this is the acquire half.
          b.srcQueueFamilyIndex = edge.producer_family;
          b.srcAccessMask = 0;
          b.dstQueueFamilyIndex = current_family;
          vk_buffer_barriers.push_back(b);
        }
        continue;
      }
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      vk_buffer_barriers.push_back(b);
    }

    VkPipelineStageFlags src_stage_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;
    for (const auto& barrier : barriers) {
      src_stage_mask |= barrier.src_stage;
      dst_stage_mask |= barrier.dst_stage;
    }
    for (const auto& e : edges) {
      if (!e.buffer) continue;
      src_stage_mask |= e.producer_stage;
      dst_stage_mask |= e.consumer_stage;
    }
    if (src_stage_mask == 0) src_stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (dst_stage_mask == 0) dst_stage_mask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (!vk_foreign_releases.empty()) {
      vkCmdPipelineBarrier(command_buffer, src_stage_mask, dst_stage_mask, 0,
                           0, nullptr, static_cast<std::uint32_t>(vk_foreign_releases.size()),
                           vk_foreign_releases.data(), 0, nullptr);
    }
    if (!vk_barriers.empty() || !vk_buffer_barriers.empty()) {
      vkCmdPipelineBarrier(command_buffer, src_stage_mask, dst_stage_mask, 0,
                           0, nullptr,
                           static_cast<std::uint32_t>(vk_buffer_barriers.size()),
                           vk_buffer_barriers.empty() ? nullptr : vk_buffer_barriers.data(),
                           static_cast<std::uint32_t>(vk_barriers.size()),
                           vk_barriers.empty() ? nullptr : vk_barriers.data());
    }

    if (node.render != nullptr) {
      const GraphPass& pass = *node.render;
      VkRenderPassBeginInfo begin_info{};
      begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      begin_info.renderPass = pass.render_pass;
      begin_info.framebuffer = pass.framebuffer;
      begin_info.renderArea.extent = {pass.width, pass.height};
      begin_info.clearValueCount = pass.clear_value_count;
      begin_info.pClearValues = pass.clear_values;
      vkCmdBeginRenderPass(command_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
      if (record_render) record_render(command_buffer, pass, pass.user_data);
      vkCmdEndRenderPass(command_buffer);
    } else if (node.compute != nullptr && record_compute) {
      record_compute(command_buffer, *node.compute, node.compute->user_data);

      // A compute pass declares both the layout used during dispatch and the
      // layout it leaves behind. Render passes get their final-layout
      // transition from VkRenderPass; compute passes need the executor to
      // materialize it explicitly.
      for (const GraphImageUse& use : node.compute->image_uses) {
        if (!use.image || use.level_count == 0U ||
            use.final_layout == use.used_layout) {
          continue;
        }
        for (std::uint32_t level = 0; level < use.level_count; ++level) {
          VkImageMemoryBarrier post{};
          post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          post.srcAccessMask = use.access;
          post.dstAccessMask =
              use.final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                  ? VkAccessFlags{VK_ACCESS_SHADER_READ_BIT}
                  : use.access;
          post.oldLayout = use.used_layout;
          post.newLayout = use.final_layout;
          post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          post.image = use.image;
          post.subresourceRange.aspectMask =
              barrier_aspect_mask(use.final_layout, use.aspect);
          post.subresourceRange.baseMipLevel = use.base_mip + level;
          post.subresourceRange.levelCount = 1U;
          post.subresourceRange.layerCount = 1U;
          vk_post_barriers.push_back(post);
        }
      }
    }

    if (!vk_post_barriers.empty()) {
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr,
                           static_cast<std::uint32_t>(vk_post_barriers.size()),
                           vk_post_barriers.data());
    }
  }
#else
  (void)nodes; (void)compiled; (void)record_render; (void)record_compute; (void)current_family;
#endif
}

} // namespace warploom::render
