#pragma once

/**
 * @file vulkan_frame_allocator.hpp
 * @brief Frame-graph resource lifetimes and the allocator derived from them (R6).
 *
 * The render graph already computes barriers between passes; what it cannot do
 * is decide which images could share memory, because nothing declares a
 * resource's LIFETIME. Every caller today hand-manages "create this target in
 * ensure(), destroy it on resize" -- the duplication that forced the capture
 * chain to restate the whole compose sequence and made the bloom toggle
 * initially a rebuild.
 *
 * This file is the pure half: no Vulkan, no allocation, just the interval
 * computation. Given a declared resource list and a pass sequence it answers
 * "what is live where, and what may alias what". The Vulkan side (create,
 * alias, destroy) is a separate step; keeping the derivation testable without
 * a device is the point.
 *
 * ## The rule that matters: history is never aliased
 *
 * A resource read across the frame boundary -- the H-Z packed pyramid, the
 * path tracer's accumulation image that the capture chain samples after the
 * live frame, exposure/compose history -- holds the PREVIOUS frame's contents
 * and must survive every moment of this frame's execution. Sandu & Shcherbakov
 * ("A Resource Allocation Algorithm for a History-Aware Frame Graph", Journal
 * of WSCG 31(1-2):63-70, 2023) measured the cost of getting this wrong the
 * other way: the naive rule of never aliasing history costs ~10% of GPU memory
 * in their engine (81 nodes, 27 resources, 14 with history reads). They built a
 * cyclic-time allocator to recover it. At our scale the cost is negligible,
 * which is exactly why the SAFE rule is the right one to adopt before any
 * allocator exists: a transient image aliased over the pyramid is not a slow
 * path, it is a rendering bug with no validation error.
 *
 * The rule is therefore stated as an invariant and enforced structurally: a
 * history resource occupies [0, T] (the whole timeline plus the boundary) and
 * so overlaps everything, while a pure-transient resource occupies only its
 * declared [first_use, last_use].
 */

// STL only (no Vulkan) so the planner is testable without a device.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace warploom::render {

//! One declared frame-graph resource. Lifetime is in PASS INDICES, which is
//! the unit both the graph and the recorder already speak.
struct FrameResource {
  std::string name;
  //! First pass that touches it. Inclusive.
  std::uint32_t first_use{0};
  //! Last pass that touches it. Inclusive; equal to first_use for a
  //! single-pass resource.
  std::uint32_t last_use{0};
  //! Read or written by a LATER frame as well (pyramid, PT accumulation,
  //! any temporal history). Such a resource is never a reuse candidate.
  bool history{false};
  //! Allocation size in bytes. The planner does not interpret it; it is
  //! carried so the Vulkan side can allocate without a second declaration.
  std::uint64_t size_bytes{0};
  //! Opaque tag for the caller (e.g. a VulkanFormat + flags pair packed by the
  //! owner). Compared byte-wise only when deciding whether two resources MAY
  //! alias: aliasing requires identical size AND identical layout/format.
  std::uint64_t layout_tag{0};
};

//! Outcome for one declared resource.
//!
//! Deliberately has no HistoryReserved member. "A history resource is never
//! aliased" is not a rule this enum enforces -- it is a property of the
//! interval arithmetic (history spans the whole timeline, so it overlaps
//! everything and can never find a partner), and an enum value that asserted
//! it would be a second, unprovable guard. The caller that needs to assert the
//! invariant checks plan[i].kind == Dedicated for every FrameResource with
//! history == true; tests/unit/test_frame_allocator.cpp does exactly that,
//! validated by sabotage.
enum class ResourceAllocation : std::uint8_t {
  //! Own allocation; no other resource shares this memory.
  Dedicated = 0,
  //! Shares memory with the named resource's allocation. The SHARED-WITH
  //! resource is the one that owns the allocation; this one must not be
  //! written or read while that one is live (guaranteed by construction).
  Aliased = 1,
};

struct ResourceAllocationPlan {
  std::string resource;
  ResourceAllocation kind{ResourceAllocation::Dedicated};
  //! For Aliased: the name of the resource whose allocation is shared.
  std::string shares_with;
  //! The byte offset within the owning allocation (for suballocation).
  std::uint64_t offset_bytes{0};
};

//! Plan resource allocations over one frame given the declared lifetimes.
//!
//! `timeline` is the number of passes (the timeline length T in the paper).
//! History resources are treated as spanning [0, T] (whole frame) and are
//! never aliased. Transient resources reuse the allocation of the
//! highest-indexed earlier resource that is compatible (same size and layout)
//! and whose lifetime does not overlap, chosen deterministically by descending
//! index so the result does not depend on iteration accidents.
[[nodiscard]] std::vector<ResourceAllocationPlan> plan_resource_allocations(
    const std::vector<FrameResource>& resources, std::uint32_t timeline);

}  // namespace warploom::render
