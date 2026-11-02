//! @file vulkan_graph_resources.cpp
//! @brief Derive FrameResource lifetimes from a GraphNode sequence (R6b
//!        prerequisite, Loop 13).
//!
//! One linear walk, no device. The ONLY policy in this file is the touch
//! order (attachments, sampled, compute uses) and first-touch naming; both
//! are fixed, so the output is a pure function of (nodes, callbacks).

#include "warploom/render/vulkan_graph_resources.hpp"

#include <string>
#include <utility>

namespace warploom::render {

std::vector<FrameResource> derive_frame_resources(
    const std::vector<GraphNode>& nodes, GraphImageSizeFn size_of,
    GraphImageHistoryFn is_history, GraphImageTagFn tag_of) {
  std::vector<FrameResource> out;
  // Image -> index into out. Linear search, deliberately: node sequences are
  // a handful of passes, and an unordered_map would trade a deterministic
  // walk for bucket order the derivation must then sort away (the same
  // lesson as rebuild_candidate_pairs: hash iteration order is unspecified).
  std::vector<std::pair<VkImage, std::size_t>> known;
  out.reserve(nodes.size());
  known.reserve(nodes.size());

  const auto touch = [&](VkImage image, std::uint32_t pass) {
    if (image == VK_NULL_HANDLE) return;
    for (const auto& [seen, idx] : known) {
      if (seen == image) {
        out[idx].last_use = pass;
        return;
      }
    }
    FrameResource r;
    r.name = "graph_img_" + std::to_string(out.size());
    r.first_use = pass;
    r.last_use = pass;
    r.history = is_history(image);
    r.size_bytes = size_of(image);
    r.layout_tag = tag_of(image);
    out.push_back(std::move(r));
    known.emplace_back(image, out.size() - 1U);
  };

  for (std::size_t n = 0; n < nodes.size(); ++n) {
    const std::uint32_t pass = static_cast<std::uint32_t>(n);
    const GraphNode& node = nodes[n];
    if (node.render != nullptr) {
      for (const RenderPassAttachment& a : node.render->attachments) {
        touch(a.image, pass);
      }
      for (const GraphSampledImage& s : node.render->sampled_images) {
        touch(s.image, pass);
      }
    } else if (node.dynamic != nullptr) {
      // Same touch order as render (target, then samples): lifetimes are
      // framing-independent, so the planner sees the identical sequence
      // whether a stage records baked or dynamic.
      touch(node.dynamic->image, pass);
      for (const GraphSampledImage& s : node.dynamic->sampled_images) {
        touch(s.image, pass);
      }
    } else if (node.compute != nullptr) {
      for (const GraphImageUse& u : node.compute->image_uses) {
        touch(u.image, pass);
      }
    }
  }
  return out;
}

}  // namespace warploom::render
