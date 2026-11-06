// R6a: frame-graph resource lifetime planner.
//
// The pure derivation, tested without a device, because its wrong answers are
// rendering bugs with NO validation error: a transient image aliased over the
// H-Z pyramid or the path tracer's accumulation image produces a picture that
// is wrong and a driver that is happy.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "warploom/render/vulkan_frame_allocator.hpp"
#include "warploom/render/vulkan_graph_resources.hpp"
#include "warploom/render/vulkan_render_graph.hpp"

namespace {

using warploom::render::FrameResource;
using warploom::render::plan_resource_allocations;
using warploom::render::ResourceAllocation;

// The invariant the planner must never break, checked the way it is actually
// specified:
//
//   1. No two resources whose live intervals overlap may share an allocation.
//   2. No history resource may be aliased AT ALL -- neither as the sharer nor
//      as the owner. This is the one that matters, and it is NOT implied by
//      (1): the intervals of a history resource and a disjoint transient are
//      disjoint, so sharing them violates no interval rule while destroying
//      the previous frame's contents.
//
// A first version of this helper checked only (1). That check is a tautology:
// aliasing is only ever chosen for disjoint intervals, so it can never fail no
// matter what the planner does. It sat there looking like a property test and
// proved nothing -- the targeted test caught the sabotage, this one passed it.
bool invariant_violated(const std::vector<FrameResource>& resources,
                        std::uint32_t timeline, std::string* why) {
  const auto plan = plan_resource_allocations(resources, timeline);

  // Owner's index by name. The OWNER is always declared EARLIER than the
  // sharer (the reuse loop walks backwards), so this map is what makes the
  // check well-formed. A first version compared plan[i].shares_with against
  // resources[j] for j > i only -- which can never match, so it reported a
  // clean plan no matter what the planner did. Measured, not assumed: with the
  // history sabotage the planner aliases a history resource in 84 of 200
  // random configurations, and that check passed every one of them.
  std::vector<std::size_t> index_of(resources.size(), resources.size());
  for (std::size_t k = 0; k < resources.size(); ++k) {
    for (std::size_t n = 0; n < resources.size(); ++n) {
      if (resources[n].name == resources[k].name) index_of[k] = n;
    }
  }

  for (std::size_t i = 0; i < resources.size(); ++i) {
    if (plan[i].kind != ResourceAllocation::Aliased) continue;
    std::size_t owner = resources.size();
    for (std::size_t n = 0; n < resources.size(); ++n) {
      if (resources[n].name == plan[i].shares_with) owner = n;
    }
    if (owner >= resources.size()) {
      *why = "resource " + plan[i].resource + " claims to share with '" +
             plan[i].shares_with + "', which does not exist";
      return true;
    }

    // (2) The rule that matters: history is never aliased, in either
    // direction. Not implied by interval disjointness -- a history resource
    // and a disjoint transient DO have disjoint intervals, so sharing them
    // breaks no interval rule while destroying the previous frame's contents.
    if (resources[i].history || resources[owner].history) {
      *why = "history resource " +
             (resources[i].history ? resources[i].name : resources[owner].name) +
             " was aliased (" + plan[i].resource + " onto " +
             plan[i].shares_with + ")";
      return true;
    }

    // (1) Belt and braces: overlapping lives must not share either.
    const bool overlap = resources[i].first_use <= resources[owner].last_use &&
                         resources[owner].first_use <= resources[i].last_use;
    if (overlap) {
      *why = "overlapping resources " + resources[i].name + " and " +
             resources[owner].name + " shared an allocation";
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(FrameAllocator, DisjointTransientResourcesShareMemory) {
  // Two same-size, same-layout transients live on disjoint passes: they CAN
  // share, and should.
  std::vector<FrameResource> resources = {
      {"hdr", 0, 1, false, 4096, 7},
      {"bloom", 2, 3, false, 4096, 7},
  };
  std::string why;
  const auto plan = plan_resource_allocations(resources, 4U);
  ASSERT_EQ(plan.size(), 2U);
  // hdr owns the allocation; bloom aliases it.
  EXPECT_EQ(plan[0].kind, ResourceAllocation::Dedicated);
  EXPECT_EQ(plan[1].kind, ResourceAllocation::Aliased);
  EXPECT_EQ(plan[1].shares_with, "hdr");
  EXPECT_FALSE(invariant_violated(resources, 4U, &why));
}

TEST(FrameAllocator, OverlappingTransientsNeverShare) {
  // Same size and layout, but live at the SAME time: must NOT share.
  std::vector<FrameResource> resources = {
      {"hdr", 0, 3, false, 4096, 7},
      {"bloom", 1, 2, false, 4096, 7},
  };
  std::string why;
  const auto plan = plan_resource_allocations(resources, 4U);
  EXPECT_NE(plan[1].kind, ResourceAllocation::Aliased);
  EXPECT_FALSE(invariant_violated(resources, 4U, &why));
}

TEST(FrameAllocator, IncompatibleLayoutsNeverShare) {
  // Disjoint lives but different layout (e.g. a sampled target vs a storage
  // one): aliasing them would be a descriptor/layout bug.
  std::vector<FrameResource> resources = {
      {"hdr", 0, 1, false, 4096, 7},
      {"accum", 2, 3, false, 4096, 99},  // different layout tag
  };
  std::string why;
  const auto plan = plan_resource_allocations(resources, 4U);
  EXPECT_EQ(plan[1].kind, ResourceAllocation::Dedicated);
  EXPECT_FALSE(invariant_violated(resources, 4U, &why)) << why;
}

TEST(FrameAllocator, HistoryResourceIsNeverAliased) {
  // THE invariant. A history resource is read after the frame boundary (a
  // later frame, or the capture chain after the live frame), so its contents
  // must be intact for the whole frame and across the boundary. Even if a
  // transient sits on disjoint passes and matches size+layout, it may NOT be
  // aliased onto the history allocation.
  std::vector<FrameResource> resources = {
      {"hiz_pyramid", 0, 1, /*history=*/true, 4096, 7},
      {"transient", 2, 3, /*history=*/false, 4096, 7},  // disjoint + compatible
  };
  std::string why;
  const auto plan = plan_resource_allocations(resources, 4U);
  EXPECT_EQ(plan[0].kind, ResourceAllocation::Dedicated)
      << "a history resource must never be aliased, so it keeps its own allocation";
  EXPECT_EQ(plan[1].kind, ResourceAllocation::Dedicated)
      << "a transient was aliased onto a history resource -- exactly the bug "
         "the never-alias-history rule exists to prevent";
  EXPECT_FALSE(invariant_violated(resources, 4U, &why));
}

TEST(FrameAllocator, HistoryResourceNeverOwnsASharedAllocation) {
  // Nor may a transient be aliased ONTO a history resource (the reverse
  // direction), nor may two transients both pile onto it.
  std::vector<FrameResource> resources = {
      {"a", 0, 0, false, 4096, 7},
      {"pt_accum", 1, 3, true, 4096, 7},
      {"b", 2, 2, false, 4096, 7},
      {"c", 4, 4, false, 4096, 7},
  };
  const auto plan = plan_resource_allocations(resources, 5U);
  // pt_accum keeps its own allocation; b and c may be Dedicated or aliased to
  // a TRANSIENT, never to pt_accum.
  for (const auto& entry : plan) {
    if (entry.kind == ResourceAllocation::Aliased) {
      EXPECT_NE(entry.shares_with, "pt_accum")
          << "resource " << entry.resource
          << " aliased onto the history resource";
    }
  }
}

TEST(FrameAllocator, RealComposeChainScenario) {
  // Mirrors the actual PT frame: the accumulation image is history (read by
  // the capture chain after the live frame), the HDR target and bloom target
  // are transients that may share when disjoint and compatible.
  std::vector<FrameResource> resources = {
      {"pt_accum", 0, 2, true, 65536, 42},      // history, storage layout
      {"hdr", 1, 1, false, 65536, 7},           // raster target
      {"bloom_down", 2, 2, false, 16384, 7},    // quarter res, smaller
  };
  std::string why;
  const auto plan = plan_resource_allocations(resources, 3U);
  EXPECT_EQ(plan[0].kind, ResourceAllocation::Dedicated)
      << "a history resource must never be aliased, so it keeps its own allocation";
  EXPECT_EQ(plan[1].kind, ResourceAllocation::Dedicated);
  EXPECT_FALSE(invariant_violated(resources, 3U, &why)) << why;
}

// The strongest available check: sweep many random-but-deterministic lifetime
// configurations and assert the one invariant that must hold for all of them --
// no two overlapping resources ever share an allocation.
TEST(FrameAllocator, NoOverlappingShareAcrossManyConfigurations) {
  std::uint32_t seed = 12345U;
  const auto next = [&seed]() {
    // Deterministic LCG (no <random> dependence on implementation).
    seed = seed * 1664525U + 1013904223U;
    return seed;
  };
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<FrameResource> resources;
    const std::uint32_t count = 3U + next() % 8U;
    const std::uint32_t timeline = 6U;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t a = next() % timeline;
      const std::uint32_t b = next() % timeline;
      FrameResource r;
      r.name = "r" + std::to_string(i);
      r.first_use = std::min(a, b);
      r.last_use = std::max(a, b);
      r.history = (next() % 4U) == 0U;
      // One size class on purpose: aliasing requires a size AND layout match,
      // so two size classes make random pairs almost always incompatible and
      // the sweep stops exercising the reuse path at all. A first version of
      // this sweep had two size classes and did NOT catch the
      // history-does-not-span sabotage, while the targeted test did.
      r.size_bytes = 4096U;
      r.layout_tag = 7U;
      resources.push_back(r);
    }
    std::string why;
    ASSERT_FALSE(invariant_violated(resources, timeline, &why))
        << "trial " << trial << ": " << why;
  }
}

// Loop 13 (research loop 2 of 10): lifetimes derived from the node sequence
// instead of hand-declared -- the prerequisite R6b was gated behind.
// Helpers are file-local: a named namespace in a .cpp still has external
// linkage, which GCC reports as -Wmissing-declarations in the Vulkan-off
// tree (clang does not enable that warning, which is why only the sanitizer
// presets saw it).
namespace loop13 {

static VkImage img(std::uintptr_t id) { return reinterpret_cast<VkImage>(id); }

static std::uint64_t size_of(VkImage image) noexcept {
  switch (reinterpret_cast<std::uintptr_t>(image)) {
    case 1U:
    case 6U:
      return 65536U;  // hdr target, swapchain image
    case 2U:
    case 3U:
      return 16384U;  // bloom chain (quarter res)
    default:
      return 4096U;  // scratch + history metadata images
  }
}
static bool is_history(VkImage image) noexcept {
  // Caller knowledge the graph cannot supply (RPS external/persistent): the
  // swapchain image belongs to the next frame's present, the pyramid and the
  // PT accumulation are read by a later frame.
  const auto id = reinterpret_cast<std::uintptr_t>(image);
  return id == 6U || id == 7U || id == 8U;
}
static std::uint64_t tag_of(VkImage) noexcept { return 7U; }

// Five passes shaped like the compose frame: scene -> bloom x2 -> present,
// plus a compute H-Z pass. First-touch order fixes the generated names.
struct Scene {
  std::vector<warploom::render::GraphPass> passes;
  std::vector<warploom::render::GraphComputePass> computes;
  std::vector<warploom::render::GraphNode> nodes;
};
static Scene make_scene() {
  using namespace warploom::render;
  Scene s;
  s.passes.resize(4);
  s.computes.resize(1);
  // pass 0: scene writes hdr + pt accumulation.
  s.passes[0].name = "scene";
  s.passes[0].attachments = {RenderPassAttachment{}, RenderPassAttachment{}};
  s.passes[0].attachments[0].image = img(1U);
  s.passes[0].attachments[1].image = img(8U);
  // pass 1: bloom down samples hdr, writes bloomA + scratch1.
  s.passes[1].name = "bloom_down";
  s.passes[1].attachments = {RenderPassAttachment{}, RenderPassAttachment{}};
  s.passes[1].attachments[0].image = img(2U);
  s.passes[1].attachments[1].image = img(4U);
  s.passes[1].sampled_images = {GraphSampledImage{}};
  s.passes[1].sampled_images[0].image = img(1U);
  // pass 2: bloom up samples bloomA, writes bloomB (overlaps bloomA at 2).
  s.passes[2].name = "bloom_up";
  s.passes[2].attachments = {RenderPassAttachment{}};
  s.passes[2].attachments[0].image = img(3U);
  s.passes[2].sampled_images = {GraphSampledImage{}};
  s.passes[2].sampled_images[0].image = img(2U);
  // pass 3: present samples hdr + bloomB, writes scratch2 + swapchain.
  s.passes[3].name = "present";
  s.passes[3].attachments = {RenderPassAttachment{}, RenderPassAttachment{}};
  s.passes[3].attachments[0].image = img(5U);
  s.passes[3].attachments[1].image = img(6U);
  s.passes[3].sampled_images = {GraphSampledImage{}, GraphSampledImage{}};
  s.passes[3].sampled_images[0].image = img(1U);
  s.passes[3].sampled_images[1].image = img(3U);
  // pass 4 (compute): H-Z reduction reads depth into the pyramid, samples
  // the accumulation for the capture-chain contract.
  s.computes[0].name = "hiz";
  s.computes[0].image_uses = {GraphImageUse{}, GraphImageUse{}};
  s.computes[0].image_uses[0].image = img(7U);
  s.computes[0].image_uses[1].image = img(8U);
  for (const GraphPass& p : s.passes) s.nodes.push_back(GraphNode::from_render(p));
  // NOTE: nodes holds pointers into s.passes/s.computes; the Scene owns
  // both, so the pointers stay valid for the Scene's lifetime.
  s.nodes.push_back(GraphNode::from_compute(s.computes[0]));
  return s;
}

static const FrameResource* find(const std::vector<FrameResource>& rs,
                          const std::string& name) {
  for (const FrameResource& r : rs) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

}  // namespace loop13

TEST(FrameAllocator, Loop13DeriveLifetimesFromNodes) {
  const loop13::Scene s = loop13::make_scene();
  const auto derived = warploom::render::derive_frame_resources(
      s.nodes, loop13::size_of, loop13::is_history, loop13::tag_of);
  ASSERT_EQ(derived.size(), 8U);  // 8 distinct images, nulls skipped
  // First-touch order: hdr, ptAccum, bloomA, scratch1, bloomB, scratch2,
  // swap, pyramid.
  const std::uint32_t first[] = {0U, 0U, 1U, 1U, 2U, 3U, 3U, 4U};
  const std::uint32_t last[] = {3U, 4U, 2U, 1U, 3U, 3U, 3U, 4U};
  for (std::size_t i = 0; i < derived.size(); ++i) {
    EXPECT_EQ(derived[i].name, "graph_img_" + std::to_string(i));
    EXPECT_EQ(derived[i].first_use, first[i]) << derived[i].name;
    EXPECT_EQ(derived[i].last_use, last[i]) << derived[i].name;
  }
  // Caller knowledge carried through, not inferred.
  EXPECT_TRUE(loop13::find(derived, "graph_img_6")->history);   // swap
  EXPECT_TRUE(loop13::find(derived, "graph_img_7")->history);   // pyramid
  EXPECT_TRUE(loop13::find(derived, "graph_img_1")->history);   // ptAccum
  EXPECT_FALSE(loop13::find(derived, "graph_img_0")->history);  // hdr
  EXPECT_EQ(loop13::find(derived, "graph_img_0")->size_bytes, 65536U);
  EXPECT_EQ(loop13::find(derived, "graph_img_3")->size_bytes, 4096U);
}

TEST(FrameAllocator, Loop13DerivedPlanElidesAndProtectsHistory) {
  const loop13::Scene s = loop13::make_scene();
  const auto derived = warploom::render::derive_frame_resources(
      s.nodes, loop13::size_of, loop13::is_history, loop13::tag_of);
  const auto plan = warploom::render::plan_resource_allocations(
      derived, static_cast<std::uint32_t>(s.nodes.size()));
  ASSERT_EQ(plan.size(), derived.size());
  const auto* scratch2 = loop13::find(derived, "graph_img_5");
  ASSERT_NE(scratch2, nullptr);
  const std::size_t idx =
      static_cast<std::size_t>(scratch2 - derived.data());
  // scratch2 [3,3] reuses scratch1 [1,1]: same size, disjoint, most recent.
  EXPECT_EQ(plan[idx].kind, warploom::render::ResourceAllocation::Aliased);
  EXPECT_EQ(plan[idx].shares_with, "graph_img_3");
  // bloomB [2,3] overlaps bloomA [1,2] at pass 2: no share despite same size.
  const auto* bloom_b = loop13::find(derived, "graph_img_4");
  const std::size_t bidx =
      static_cast<std::size_t>(bloom_b - derived.data());
  EXPECT_EQ(plan[bidx].kind, warploom::render::ResourceAllocation::Dedicated);
  // Every history resource keeps its own allocation -- the invariant that
  // motivated the whole R6 line, now over derived (not hand-written) input.
  for (const auto& entry : plan) {
    const FrameResource* r = loop13::find(derived, entry.resource);
    ASSERT_NE(r, nullptr);
    if (r->history) {
      EXPECT_EQ(entry.kind, warploom::render::ResourceAllocation::Dedicated)
          << entry.resource;
    }
    if (entry.kind == warploom::render::ResourceAllocation::Aliased) {
      EXPECT_NE(entry.shares_with, "graph_img_1");  // ptAccum
      EXPECT_NE(entry.shares_with, "graph_img_6");  // swap
      EXPECT_NE(entry.shares_with, "graph_img_7");  // pyramid
    }
  }
  std::string why;
  EXPECT_FALSE(invariant_violated(
      derived, static_cast<std::uint32_t>(s.nodes.size()), &why))
      << why;
}

TEST(FrameAllocator, Loop13DerivationIsDeterministicAndSkipsNull) {
  const loop13::Scene s = loop13::make_scene();
  const auto once = warploom::render::derive_frame_resources(
      s.nodes, loop13::size_of, loop13::is_history, loop13::tag_of);
  const auto twice = warploom::render::derive_frame_resources(
      s.nodes, loop13::size_of, loop13::is_history, loop13::tag_of);
  ASSERT_EQ(once.size(), twice.size());
  for (std::size_t i = 0; i < once.size(); ++i) {
    EXPECT_EQ(once[i].name, twice[i].name);
    EXPECT_EQ(once[i].first_use, twice[i].first_use);
    EXPECT_EQ(once[i].last_use, twice[i].last_use);
  }
  // A pass with only null handles contributes no resource.
  warploom::render::GraphPass empty;
  empty.name = "empty";
  empty.attachments = {warploom::render::RenderPassAttachment{}};
  const std::vector<warploom::render::GraphNode> nodes = {
      warploom::render::GraphNode::from_render(empty)};
  EXPECT_TRUE(warploom::render::derive_frame_resources(
                    nodes, loop13::size_of, loop13::is_history,
                    loop13::tag_of)
                    .empty());
  EXPECT_TRUE(warploom::render::derive_frame_resources(
                    {}, loop13::size_of, loop13::is_history,
                    loop13::tag_of)
                    .empty());
}
