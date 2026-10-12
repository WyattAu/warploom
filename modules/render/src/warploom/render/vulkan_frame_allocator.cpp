//! @file vulkan_frame_allocator.cpp
//! @brief Deterministic interval-based allocator for frame-graph resources (R6).
//!
//! The whole file is one pure function plus a helper. It allocates no Vulkan
//! memory and touches no device: it answers "can these two resources share an
//! allocation", which is the question whose wrong answer produces a rendering
//! bug with no validation error when a transient image is aliased over a
//! history image (the H-Z pyramid, the PT accumulation).

#include <warploom/render/vulkan_frame_allocator.hpp>

#include <utility>

namespace warploom::render {

namespace {

//! Does [a.first, a.last] overlap [b.first, b.last]? Inclusive on both ends:
//! two resources live at the same pass index overlap.
[[nodiscard]] bool overlaps(std::uint32_t a_first, std::uint32_t a_last,
                            std::uint32_t b_first, std::uint32_t b_last) noexcept {
  return a_first <= b_last && b_first <= a_last;
}

//! Two resources may share memory only if they are the same size AND layout.
[[nodiscard]] bool compatible(const FrameResource& a,
                              const FrameResource& b) noexcept {
  return a.size_bytes == b.size_bytes && a.layout_tag == b.layout_tag;
}

}  // namespace

std::vector<ResourceAllocationPlan> plan_resource_allocations(
    const std::vector<FrameResource>& resources, std::uint32_t timeline) {
  std::vector<ResourceAllocationPlan> plan;
  plan.reserve(resources.size());

  // Interval each resource is live for. This is the ONLY place the
  // never-alias-history rule is enforced, and it is enforced structurally:
  // a history resource spans [0, timeline] (the whole frame plus the
  // boundary), which overlaps every possible interval, so it can neither own
  // a shared allocation nor be aliased onto one -- not because a flag says so,
  // but because the same overlap test that governs transients governs it.
  //
  // Earlier drafts ALSO skipped history resources by name in two separate
  // loops. That was defence in depth until it became defence against the
  // tests: with three guards, disabling any one of them changed nothing, so a
  // sabotage could not fail the suite and the guards were untested. One
  // mechanism, provable by one sabotage.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> live(resources.size());
  for (std::size_t i = 0; i < resources.size(); ++i) {
    const FrameResource& r = resources[i];
    live[i] = r.history ? std::pair<std::uint32_t, std::uint32_t>{0U, timeline}
                        : std::pair<std::uint32_t, std::uint32_t>{r.first_use,
                                                                 r.last_use};
  }

  // Greedy: process in declaration order; each resource tries to share with
  // the most recent compatible earlier resource whose live interval does not
  // overlap. "Most recent" (highest declaration index) rather than "largest"
  // or "first fit" keeps the choice local and the plan stable as unrelated
  // resources are added -- a new resource should not reshuffle everything.
  for (std::size_t i = 0; i < resources.size(); ++i) {
    const FrameResource& r = resources[i];
    ResourceAllocationPlan entry;
    entry.resource = r.name;

    bool found = false;
    std::size_t owner = 0U;
    for (std::size_t j = i; j-- > 0U;) {
      if (!compatible(resources[j], r)) continue;
      if (overlaps(live[j].first, live[j].second, live[i].first, live[i].second)) {
        continue;
      }
      owner = j;
      found = true;
      break;
    }

    // No branch consults r.history. A history resource cannot be `found`
    // with, because it spans [0, timeline] and overlaps every interval, so
    // "never alias history" is not enforced here at all -- it falls out of the
    // interval arithmetic. That is deliberate: every earlier attempt also
    // special-cased history in the decision, which meant three guards for one
    // invariant, which meant no sabotage could fail the tests (removing any
    // single guard was masked by the other two). One mechanism, one sabotage.
    if (found) {
      entry.kind = ResourceAllocation::Aliased;
      entry.shares_with = resources[owner].name;
      entry.offset_bytes = 0U;  // same size + same layout -> offset 0
    } else {
      entry.kind = ResourceAllocation::Dedicated;
      entry.shares_with.clear();
      entry.offset_bytes = 0U;
    }
    plan.push_back(std::move(entry));
  }

  return plan;
}

}  // namespace warploom::render
