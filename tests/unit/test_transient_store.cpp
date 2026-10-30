//! @file test_transient_store.cpp
//! @brief R6b on-device proofs for the VulkanTransientStore.
//!
//! The pure planner is proven in test_frame_allocator.cpp (no device). These
//! tests prove the Vulkan side: the plan materializes into real images with
//! real shared memory, disjoint pairs actually elide (4-of-10 here), history
//! never shares memory (deterministic fuzz), aliased images read and write
//! correctly through their bindings, and the compose chain's per-frame gate
//! accepts the shapes it planned and rejects an unknown image.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "warploom/render/vulkan_compose_chain.hpp"
#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_frame_allocator.hpp"
#include "warploom/render/vulkan_graph_resources.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_transient_store.hpp"
#include "warploom/render/vulkan_types.hpp"

#if defined(WARPLOOM_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace {

using warploom::render::FrameResource;
using warploom::render::ResourceAllocation;
using warploom::render::TransientImageDesc;
using warploom::render::VulkanComposeChain;
using warploom::render::VulkanMemoryAllocator;
using warploom::render::VulkanTransientStore;

#if defined(WARPLOOM_HAS_VULKAN)

// Two size classes with fixed extents, so alias-compatibility implies
// identical creation parameters (mirrors the chain's kind rule: one desc
// per tag). 4096 = 32x32 RGBA8, 8192 = 32x32 RGBA16F.
constexpr std::uint32_t kSizeA = 4096U;
constexpr std::uint32_t kSizeB = 8192U;
constexpr std::uint64_t kTagA = 7U;
constexpr std::uint64_t kTagB = 9U;
constexpr std::uint64_t kTagC = 11U;

TransientImageDesc desc_for(const std::string& name, std::uint64_t size,
                            std::uint64_t tag) {
  TransientImageDesc d;
  d.name = name;
  if (size == kSizeA) {
    d.format = VK_FORMAT_R8G8B8A8_UNORM;
    d.width = 32U;
    d.height = 32U;
  } else {
    d.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    d.width = 32U;
    d.height = 32U;
  }
  if (tag == kTagC) {
    // Same byte size, different tag: never aliasable despite the size match.
    d.format = VK_FORMAT_B8G8R8A8_UNORM;
    d.width = 64U;
    d.height = 16U;
  }
  d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  return d;
}

FrameResource res(const std::string& name, std::uint32_t first,
                  std::uint32_t last, bool history, std::uint64_t size,
                  std::uint64_t tag) {
  FrameResource r;
  r.name = name;
  r.first_use = first;
  r.last_use = last;
  r.history = history;
  r.size_bytes = size;
  r.layout_tag = tag;
  return r;
}

// The 10-resource frame: 2 history + 8 transients engineered for exactly 4
// alias pairs (a1->a0, b1->b0, a2->a1, b2->b1). c0/c1 share a size class
// with nothing compatible-alive: c1 overlaps c0, so both stay Dedicated.
std::vector<FrameResource> ten_resources() {
  return {
      res("a0", 0U, 0U, false, kSizeA, kTagA),
      res("a1", 2U, 2U, false, kSizeA, kTagA),
      res("b0", 0U, 1U, false, kSizeB, kTagB),
      res("b1", 3U, 4U, false, kSizeB, kTagB),
      res("h0", 0U, 0U, true, kSizeA, kTagA),
      res("a2", 5U, 5U, false, kSizeA, kTagA),
      res("h1", 0U, 0U, true, kSizeB, kTagB),
      res("b2", 6U, 7U, false, kSizeB, kTagB),
      res("c0", 1U, 6U, false, kSizeA, kTagC),
      res("c1", 6U, 6U, false, kSizeA, kTagC),
  };
}

struct DeviceHarness {
  warploom::render::VulkanContext context;
  VulkanMemoryAllocator allocator;
  VkCommandPool pool{VK_NULL_HANDLE};

  bool initialize() {
    if (!warploom::render::VulkanContext::is_available()) return false;
    if (!context.initialize("TransientStoreTest", true).is_ok()) return false;
    if (!allocator.initialize(context.device(), context.physical_device())
             .is_ok()) {
      context.cleanup();
      return false;
    }
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = static_cast<std::uint32_t>(
        context.queue_families().graphics_family);
    if (vkCreateCommandPool(context.device(), &pool_info, nullptr, &pool) !=
        VK_SUCCESS) {
      allocator.cleanup();
      context.cleanup();
      return false;
    }
    return true;
  }
  void cleanup() {
    if (pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(context.device(), pool, nullptr);
      pool = VK_NULL_HANDLE;
    }
    allocator.cleanup();
    context.cleanup();
  }
};

#endif  // defined(WARPLOOM_HAS_VULKAN)

}  // namespace

#if defined(WARPLOOM_HAS_VULKAN)

// 4-of-10 elision, on a real device: the plan says 4 share, the store binds
// them to their owners' memory, history keeps private memory, and every
// image -- including the aliased ones -- clears and copies back correctly.
TEST(TransientStore, ElidesDisjointPairsOnDevice) {
  DeviceHarness h;
  if (!h.initialize()) GTEST_SKIP() << "no Vulkan device";
  const auto resources = ten_resources();
  const auto plan =
      warploom::render::plan_resource_allocations(resources, 8U);
  ASSERT_EQ(plan.size(), 10U);
  std::uint32_t aliased = 0U;
  for (const auto& entry : plan) {
    if (entry.kind == ResourceAllocation::Aliased) ++aliased;
  }
  // The exit criterion, stated as a number: exactly 4 elided.
  ASSERT_EQ(aliased, 4U) << "scaffold drift: re-tune ten_resources()";

  std::vector<TransientImageDesc> descs;
  for (const auto& r : resources) {
    descs.push_back(desc_for(r.name, r.size_bytes, r.layout_tag));
  }
  VulkanTransientStore store;
  ASSERT_TRUE(store.allocate(h.context.device(), h.context.physical_device(),
                             &h.allocator, resources, plan, descs)
                  .is_ok());
  EXPECT_TRUE(store.ready());
  EXPECT_EQ(store.generation(), 1U);

  // The elision number, measured in backing allocations, not planner claims:
  // 10 images over 6 owner allocations. (The allocator sub-allocates from
  // shared blocks, so VkDeviceMemory handles repeat across owners; the
  // (handle, offset) pair is the identity, and the owner COUNT is the
  // elision proof.)
  EXPECT_EQ(store.owner_allocation_count(), 6U);
  const auto same_binding = [](const warploom::render::TransientImage* a,
                               const warploom::render::TransientImage* b) {
    return a->memory == b->memory && a->offset == b->offset;
  };
  // Memory topology: every Aliased entry binds its root owner's (memory,
  // offset); history entries share a binding with nothing.
  for (const auto& entry : plan) {
    const auto* img = store.find(entry.resource);
    ASSERT_NE(img, nullptr);
    if (entry.kind != ResourceAllocation::Aliased) {
      EXPECT_FALSE(img->aliased) << entry.resource;
      continue;
    }
    EXPECT_TRUE(img->aliased) << entry.resource;
    EXPECT_EQ(img->shares_with, entry.shares_with);
    // Resolve to the Dedicated root (the planner may chain: C->B->A).
    std::string root = entry.shares_with;
    for (std::size_t steps = 0U; steps <= plan.size(); ++steps) {
      const auto* root_img = store.find(root);
      ASSERT_NE(root_img, nullptr) << "dangling owner " << root;
      if (!root_img->aliased) break;
      root = root_img->shares_with;
    }
    const auto* root_img = store.find(root);
    ASSERT_NE(root_img, nullptr);
    EXPECT_FALSE(root_img->aliased);
    EXPECT_TRUE(same_binding(img, root_img))
        << entry.resource << " does not share its root's binding";
  }
  // h0/h1 private: no other entry may share their (memory, offset).
  for (const char* hist : {"h0", "h1"}) {
    const auto* himg = store.find(hist);
    ASSERT_NE(himg, nullptr);
    for (const auto& r : resources) {
      if (r.name == hist) continue;
      EXPECT_FALSE(same_binding(store.find(r.name), himg))
          << r.name << " shares memory with history " << hist;
    }
  }

  // Functional proof: clear every image to a distinct 0/1 pattern and copy
  // it back BEFORE the next clear. Aliased images share memory, so only
  // barrier-separated sequential use is sound -- which is exactly what this
  // records. A bad binding (wrong memory, wrong size) fails here, not in a
  // planner unit test.
  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = h.pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1U;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  ASSERT_EQ(
      vkAllocateCommandBuffers(h.context.device(), &alloc_info, &cb),
      VK_SUCCESS);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);
  struct Copy {
    warploom::render::Allocation buffer;
    std::uint64_t size_bytes;
    std::uint64_t tag;
  };
  std::vector<Copy> copies;
  for (std::size_t i = 0; i < resources.size(); ++i) {
    const auto* img = store.find(resources[i].name);
    ASSERT_NE(img, nullptr);
    // UNDEFINED -> TRANSFER_DST for the clear.
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = img->image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    to_dst.srcAccessMask = 0U;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U,
                         nullptr, 1U, &to_dst);
    // Distinct 0/1 pattern from the index bits: exact in both UNORM8 and
    // HALF_FLOAT, so the readback comparison is bit-exact, not tolerant.
    VkClearColorValue clear{};
    clear.float32[0] = ((i & 1U) != 0U) ? 1.0F : 0.0F;
    clear.float32[1] = ((i & 2U) != 0U) ? 1.0F : 0.0F;
    clear.float32[2] = ((i & 4U) != 0U) ? 1.0F : 0.0F;
    clear.float32[3] = ((i & 8U) != 0U) ? 1.0F : 0.0F;
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1U;
    range.layerCount = 1U;
    vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1U, &range);
    // TRANSFER_DST -> TRANSFER_SRC for the copy.
    VkImageMemoryBarrier to_src = to_dst;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U,
                         nullptr, 1U, &to_src);
    auto buf = h.allocator.create_buffer(
        static_cast<VkDeviceSize>(resources[i].size_bytes),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(buf.is_ok());
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
    // Extent matches the desc (c0/c1 are 64x16; everything else 32x32).
    region.imageExtent = {descs[i].width, descs[i].height, 1U};
    vkCmdCopyImageToBuffer(cb, img->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf.value().buffer, 1U, &region);
    // Copy -> host read needs no barrier (coherent + queue wait below), but
    // the NEXT image's clear must not overlap this copy: same-queue submit
    // order already guarantees it.
    copies.push_back(
        {buf.value(), resources[i].size_bytes, resources[i].layout_tag});
  }
  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(h.context.graphics_queue(), 1U, &submit,
                          VK_NULL_HANDLE),
            VK_SUCCESS);
  ASSERT_EQ(vkQueueWaitIdle(h.context.graphics_queue()), VK_SUCCESS);

  for (std::size_t i = 0; i < copies.size(); ++i) {
    const auto* mapped =
        static_cast<const std::uint8_t*>(copies[i].buffer.mapped);
    ASSERT_NE(mapped, nullptr);
    const float expect[4] = {
        ((i & 1U) != 0U) ? 1.0F : 0.0F, ((i & 2U) != 0U) ? 1.0F : 0.0F,
        ((i & 4U) != 0U) ? 1.0F : 0.0F, ((i & 8U) != 0U) ? 1.0F : 0.0F};
    if (copies[i].tag == kTagB) {
      // HALF_FLOAT: 1.0h = 0x3C00, 0.0h = 0x0000, little-endian.
      const auto* half = reinterpret_cast<const std::uint16_t*>(mapped);
      for (int c = 0; c < 4; ++c) {
        const std::uint16_t want = (expect[c] > 0.5F) ? 0x3C00U : 0x0000U;
        EXPECT_EQ(half[c], want) << "image " << i << " channel " << c;
      }
    } else if (copies[i].tag == kTagC) {
      // B8G8R8A8: bytes are B,G,R,A. 1.0 -> 255 exactly, 0.0 -> 0 exactly.
      const int order[4] = {2, 1, 0, 3};
      for (int c = 0; c < 4; ++c) {
        const std::uint8_t want = (expect[order[c]] > 0.5F) ? 255U : 0U;
        EXPECT_EQ(mapped[c], want) << "image " << i << " channel " << c;
      }
    } else {
      // R8G8B8A8: bytes are R,G,B,A. 1.0 -> 255 exactly, 0.0 -> 0 exactly.
      for (int c = 0; c < 4; ++c) {
        const std::uint8_t want = (expect[c] > 0.5F) ? 255U : 0U;
        EXPECT_EQ(mapped[c], want) << "image " << i << " channel " << c;
      }
    }
    warploom::render::Allocation live = copies[i].buffer;
    h.allocator.destroy_allocation(live);
  }
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  vkFreeCommandBuffers(h.context.device(), h.pool, 1U, &cb);
  store.destroy();
  h.cleanup();
}

// Deterministic fuzz: random lifetime configurations, planned and
// materialized on-device. The invariant is structural -- no history image
// may share a VkDeviceMemory with anything, and every alias edge must
// resolve to a Dedicated root with identical memory -- so ANY configuration
// must satisfy it, not just the scaffold above.
TEST(TransientStore, FuzzNeverAliasesHistoryOnDevice) {
  DeviceHarness h;
  if (!h.initialize()) GTEST_SKIP() << "no Vulkan device";
  std::uint32_t seed = 987654321U;
  const auto next = [&seed]() -> std::uint32_t {
    seed = seed * 1664525U + 1013904223U;
    return seed;
  };
  constexpr std::uint32_t kTrials = 40U;
  for (std::uint32_t trial = 0U; trial < kTrials; ++trial) {
    const std::uint32_t count = 3U + next() % 6U;
    std::vector<FrameResource> resources;
    for (std::uint32_t i = 0U; i < count; ++i) {
      const std::uint32_t a = next() % 6U;
      const std::uint32_t b = next() % 6U;
      const bool size_b = (next() % 2U) == 0U;
      FrameResource r;
      r.name = "f" + std::to_string(i);
      r.first_use = (a < b) ? a : b;
      r.last_use = (a < b) ? b : a;
      r.history = (next() % 4U) == 0U;
      r.size_bytes = size_b ? kSizeB : kSizeA;
      r.layout_tag = size_b ? kTagB : kTagA;
      resources.push_back(r);
    }
    const auto plan =
        warploom::render::plan_resource_allocations(resources, 6U);
    ASSERT_EQ(plan.size(), resources.size()) << "trial " << trial;
    std::vector<TransientImageDesc> descs;
    for (const auto& r : resources) {
      descs.push_back(desc_for(r.name, r.size_bytes, r.layout_tag));
    }
    VulkanTransientStore store;
    ASSERT_TRUE(store.allocate(h.context.device(),
                               h.context.physical_device(), &h.allocator,
                               resources, plan, descs)
                    .is_ok())
        << "trial " << trial;
    // Invariant 1: history bindings are private (handle AND offset: the
    // allocator sub-allocates, so handles repeat across owners by design).
    const auto same_binding = [](
        const warploom::render::TransientImage* a,
        const warploom::render::TransientImage* b) {
      return a->memory == b->memory && a->offset == b->offset;
    };
    for (const auto& r : resources) {
      if (!r.history) continue;
      const auto* himg = store.find(r.name);
      ASSERT_NE(himg, nullptr) << "trial " << trial;
      EXPECT_FALSE(himg->aliased) << "trial " << trial << " " << r.name;
      for (const auto& o : resources) {
        if (o.name == r.name) continue;
        EXPECT_FALSE(same_binding(store.find(o.name), himg))
            << "trial " << trial << ": " << o.name
            << " shares history memory of " << r.name;
      }
    }
    // Invariant 2: every alias edge resolves to a Dedicated root whose
    // binding it actually shares.
    for (const auto& entry : plan) {
      if (entry.kind != ResourceAllocation::Aliased) continue;
      std::string root = entry.shares_with;
      for (std::size_t steps = 0U; steps <= plan.size(); ++steps) {
        const auto* root_img = store.find(root);
        ASSERT_NE(root_img, nullptr) << "trial " << trial;
        if (!root_img->aliased) break;
        root = root_img->shares_with;
      }
      const auto* root_img = store.find(root);
      const auto* img = store.find(entry.resource);
      ASSERT_NE(root_img, nullptr) << "trial " << trial;
      ASSERT_NE(img, nullptr) << "trial " << trial;
      EXPECT_FALSE(root_img->aliased) << "trial " << trial;
      EXPECT_TRUE(same_binding(img, root_img)) << "trial " << trial;
    }
    store.destroy();
  }
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  h.cleanup();
}

// The plan skeleton behind the chain's per-frame gate, checked against the
// REAL derivation: the skeleton nodes derive_frame_resources walks must
// produce the same lifetimes the chain plans from. The on-device half of
// the gate (accept planned shapes, reject unknown handles) runs inside
// every golden-test frame through renderer.record_commands.
TEST(TransientStore, ChainSkeletonMatchesDerivedLifetimes) {
  for (bool include_bloom : {false, true}) {
    std::vector<warploom::render::GraphPass> skeleton;
    VulkanComposeChain::build_plan_skeleton_nodes(include_bloom, skeleton);
    const auto kinds = [](VkImage image) noexcept -> std::uint64_t {
      return static_cast<std::uint64_t>(
          VulkanComposeChain::skeleton_kind_of(image));
    };
    const auto sizes = [](VkImage image) noexcept -> std::uint64_t {
      const int kind = VulkanComposeChain::skeleton_kind_of(image);
      if (kind == VulkanComposeChain::kKindHdrColor) return 1280U * 720U * 8U;
      if (kind == VulkanComposeChain::kKindHdrDepth) return 1280U * 720U * 4U;
      if (kind == VulkanComposeChain::kKindBloomColor) return 640U * 360U * 8U;
      return 1280U * 720U * 16U;
    };
    const auto history = [](VkImage image) noexcept -> bool {
      return VulkanComposeChain::skeleton_kind_of(image) ==
             VulkanComposeChain::kKindExternal;
    };
    const auto derived = warploom::render::derive_frame_resources(
        [&]() {
          std::vector<warploom::render::GraphNode> nodes;
          for (const auto& p : skeleton) {
            nodes.push_back(warploom::render::GraphNode::from_render(p));
          }
          return nodes;
        }(),
        sizes, history, kinds);
    // First-touch order is fixed by construction: hdr, depth, [bloom,]
    // external.
    ASSERT_EQ(derived.size(), include_bloom ? 4U : 3U);
    EXPECT_EQ(derived[0].layout_tag,
              static_cast<std::uint64_t>(VulkanComposeChain::kKindHdrColor));
    EXPECT_EQ(derived[1].layout_tag,
              static_cast<std::uint64_t>(VulkanComposeChain::kKindHdrDepth));
    EXPECT_EQ(derived[0].first_use, 0U);
    EXPECT_EQ(derived[1].first_use, 0U);
    EXPECT_EQ(derived[1].last_use, 0U);
    if (include_bloom) {
      EXPECT_EQ(derived[2].layout_tag,
                static_cast<std::uint64_t>(
                    VulkanComposeChain::kKindBloomColor));
      EXPECT_EQ(derived[2].first_use, 1U);
      EXPECT_EQ(derived[2].last_use, 2U);
      EXPECT_EQ(derived[3].layout_tag,
                static_cast<std::uint64_t>(VulkanComposeChain::kKindExternal));
      EXPECT_TRUE(derived[3].history);
      EXPECT_EQ(derived[0].last_use, 2U);
    } else {
      EXPECT_EQ(derived[2].layout_tag,
                static_cast<std::uint64_t>(VulkanComposeChain::kKindExternal));
      EXPECT_TRUE(derived[2].history);
      EXPECT_EQ(derived[0].last_use, 1U);
    }
  }
}

#else  // !defined(WARPLOOM_HAS_VULKAN)

TEST(TransientStore, SkippedWithoutVulkan) { GTEST_SKIP(); }

#endif
