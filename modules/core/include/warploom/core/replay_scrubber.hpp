#pragma once

//! @file replay_scrubber.hpp
//! @brief W1 replay scrubber: hash-verified checkpoint timeline for the
//!        deterministic simulation (docs/roadmap.md, track W).
//!
//! A scrub session captures the editor document at chosen sim frames into a
//! bounded ring of checkpoints. Restoring a checkpoint reloads the exact
//! bytes that were captured, then verifies the state hash — so a scrub can
//! never silently load corrupted or divergent state: the same guarantee as
//! the runtime's replay system, applied to the editor document.
//!
//! Threading: the scrubber is frame-thread-only, exactly like the
//! EditorSession it wraps. Protocol commands reach it through the session's
//! on_control (single mutation authority, M10); the viewport drives it from
//! the frame loop. No locks — callers serialize.
//!
//! Determinism: the document JSON is byte-deterministic (sorted keys,
//! round-trip-safe numbers), so identical state yields identical snapshots
//! and identical hashes on every machine.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "warploom/core/document.hpp"

namespace warploom::editor {

//! FNV-1a 64-bit over bytes (shared with the mesh table's determinism
//! proofs). Exposed for tests; the scrubber uses it via state_hash().
[[nodiscard]] std::uint64_t fnv1a64(const char* data,
                                    std::size_t size) noexcept;

//! Hash of the document's authoritative serialization. Byte-deterministic.
[[nodiscard]] std::uint64_t state_hash(const SceneDocument& doc) noexcept;

//! One checkpoint: the document bytes captured at a sim frame, with the
//! hash taken at capture time, PLUS the T4a physics snapshot (solver state
//! as bytes, same capture moment). Restore re-hashes both byte strings
//! and re-serializes the parsed document; all must match. An empty
//! physics string means "no physics state" and restores an empty world —
//! deterministic, never stale (pre-physics files hydrate this way). This
//! keeps old checkpoints loadable: absent physics and empty physics are
//! the same defined state, not two cases.
struct ReplayCheckpoint final {
  std::uint64_t frame{0};  //!< sim frame index at capture
  std::uint64_t hash{0};   //!< fnv1a64 over `json` at capture time
  std::string json{};      //!< byte-deterministic document serialization
  std::uint64_t physics_hash{0};  //!< fnv1a64 over `physics_json`
  std::string physics_json{};     //!< physics_snapshot codec bytes (may be empty)
};

//! Ring of checkpoints with hash-verified restore. Capacity is bounded
//! (oldest evicted) so long sessions can never grow unbounded.
class ReplayScrubber final {
 public:
  explicit ReplayScrubber(
      std::size_t capacity = kDefaultCheckpointCapacity);

  //! Captures the CURRENT document state at `frame`, plus the CURRENT
  //! physics bytes (already serialized by the caller via the
  //! physics_snapshot codec — the ring stores opaque bytes, never solver
  //! types). Overwrites the oldest checkpoint when the ring is full.
  //! Returns false + `error` when the document fails to serialize (never
  //! captures half state).
  [[nodiscard]] bool capture(std::uint64_t frame, const SceneDocument& doc,
                             const std::string& physics_json,
                             std::string& error);
  //! Back-compatible capture without physics (stores empty physics bytes:
  //! restores to an empty world, per the struct contract).
  [[nodiscard]] bool capture(std::uint64_t frame, const SceneDocument& doc,
                             std::string& error) {
    return capture(frame, doc, std::string{}, error);
  }

  //! Restores the checkpoint at sim frame `frame`: loads the captured bytes
  //! into `doc` and verifies BOTH (a) the bytes still hash to the capture-
  //! time hash and (b) the parsed document re-serializes to the exact same
  //! bytes. Returns false + `error` when the frame is not checkpointed, a
  //! check fails, or parsing fails; `doc` untouched on failure. NOTE: this
  //! restores the DOCUMENT only — the caller restores physics separately
  //! from `physics_for(frame)` (same two rules: hash must match, schema
  //! must validate), and the session's `scrub_to` additionally resets the
  //! command stack, disarms clip record/play, and drops selections the
  //! restored document no longer has, so post-restore undo cannot cross
  //! a time warp.
  [[nodiscard]] bool restore(std::uint64_t frame, SceneDocument& doc,
                             std::string& error);

  //! Physics bytes stored at `frame` (empty when the checkpoint predates
  //! physics or captured an empty world). The caller validates them through
  //! physics_restore_json; the hash check happens in restore()/verify().
  [[nodiscard]] bool physics_for(std::uint64_t frame,
                                 std::string& out) const;

  //! Restores the document from checkpoint BYTES (not a ring lookup), with
  //! the same two-layer verification as restore(), plus the physics-bytes
  //! hash check. load_replay uses this for the opening checkpoint: opening
  //! and closing can share a frame (a capture with no Step between start
  //! and stop), and same-frame ring insertion replaces newest-wins — so
  //! restoring the opening frame from the ring can return the CLOSING bytes
  //! and the log then re-applies on top of them.
  [[nodiscard]] bool restore_bytes(const std::string& json,
                                   std::uint64_t hash,
                                   const std::string& physics_json,
                                   std::uint64_t physics_hash,
                                   SceneDocument& doc, std::string& error);

  //! Hash check only (no mutation): is a checkpoint present for `frame`,
  //! and does its bytes still hash to the stored hash?
  [[nodiscard]] bool verify(std::uint64_t frame) const;

  //! True when a checkpoint exists at `frame`.
  [[nodiscard]] bool has(std::uint64_t frame) const;

  //! Checkpoint frames in capture order (ring order, oldest first).
  [[nodiscard]] std::vector<std::uint64_t> frames() const;

  [[nodiscard]] std::size_t size() const noexcept { return ring_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return ring_.empty(); }
  void clear() noexcept { ring_.clear(); }

  //! W2: inserts a checkpoint captured ELSEWHERE (e.g. hydrated from a
  //! warploom-replay-v1 file) without re-serializing. The caller supplies
  //! the bytes and their capture-time hashes; no document needed.
  //! Same-frame insertion replaces (newest wins), the ring evicts oldest
  //! when full — identical ring semantics to capture().
  void insert(std::uint64_t frame, std::uint64_t hash, std::string json,
              std::uint64_t physics_hash, std::string physics_json);

  //! Read-only ring view (ring order, oldest first) for writers/tests.
  [[nodiscard]] const std::vector<ReplayCheckpoint>& items() const noexcept {
    return ring_;
  }

  //! Default ring size: 4096 checkpoints of a typical 2-8 KiB document is
  //! 8-32 MiB worst case — bounded, documented, and generous for scrubbing.
  static constexpr std::size_t kDefaultCheckpointCapacity = 4096;

 private:
  [[nodiscard]] const ReplayCheckpoint* find_checkpoint(
      std::uint64_t frame) const;

  std::size_t capacity_{0};
  std::size_t write_index_{0};  //!< next eviction slot when full
  std::vector<ReplayCheckpoint> ring_{};
};

}  // namespace warploom::editor
// S2-B compat footer: legacy `omnicpp::*` spellings keep resolving
// during the transition (removed with the S5 identity pass -
// docs/warploom-core-plan.md, phase B). A using-directive in a
// namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types, and legally coexists
// with real `omnicpp::core` extension blocks elsewhere (extension
// blocks merge). One directive per namespace THIS header declares,
// each under its OWN guard (a shared guard would suppress later
// headers' distinct directives).
#ifndef WARPLOOM_COMPAT_EDITOR_NS
#define WARPLOOM_COMPAT_EDITOR_NS
namespace omnicpp::editor {
    using namespace ::warploom::editor;
}
#endif  // WARPLOOM_COMPAT_EDITOR_NS
