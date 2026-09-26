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

#include "engine/core/document.hpp"

namespace omnicpp::editor {

//! FNV-1a 64-bit over bytes (shared with the mesh table's determinism
//! proofs). Exposed for tests; the scrubber uses it via state_hash().
[[nodiscard]] std::uint64_t fnv1a64(const char* data,
                                    std::size_t size) noexcept;

//! Hash of the document's authoritative serialization. Byte-deterministic.
[[nodiscard]] std::uint64_t state_hash(const SceneDocument& doc) noexcept;

//! One checkpoint: the document bytes captured at a sim frame, with the
//! hash taken at capture time. Restore re-hashes the bytes and re-serializes
//! the parsed document; both must match.
struct ReplayCheckpoint final {
  std::uint64_t frame{0};  //!< sim frame index at capture
  std::uint64_t hash{0};   //!< fnv1a64 over `json` at capture time
  std::string json{};      //!< byte-deterministic document serialization
};

//! Ring of checkpoints with hash-verified restore. Capacity is bounded
//! (oldest evicted) so long sessions can never grow unbounded.
class ReplayScrubber final {
 public:
  explicit ReplayScrubber(
      std::size_t capacity = kDefaultCheckpointCapacity);

  //! Captures the CURRENT document state at `frame`. Overwrites the oldest
  //! checkpoint when the ring is full. Returns false + `error` when the
  //! document fails to serialize (never captures half state).
  [[nodiscard]] bool capture(std::uint64_t frame, const SceneDocument& doc,
                             std::string& error);

  //! Restores the checkpoint at sim frame `frame`: loads the captured bytes
  //! into `doc` and verifies BOTH (a) the bytes still hash to the capture-
  //! time hash and (b) the parsed document re-serializes to the exact same
  //! bytes. Returns false + `error` when the frame is not checkpointed, a
  //! check fails, or parsing fails; `doc` untouched on failure. NOTE: this
  //! does NOT clear the session's command history — protocol-level scrub
  //! (v1.6) resets the stack so post-restore undo cannot cross a time warp.
  [[nodiscard]] bool restore(std::uint64_t frame, SceneDocument& doc,
                             std::string& error);

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
  //! the bytes and their capture-time hash; no document needed. Same-frame
  //! insertion replaces (newest wins), the ring evicts oldest when full —
  //! identical ring semantics to capture().
  void insert(std::uint64_t frame, std::uint64_t hash, std::string json);

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

}  // namespace omnicpp::editor
