//! @file replay_scrubber.cpp
//! @brief Checkpoint ring + hash-verified restore (see the header).
//!
//! Verification is two-layered on restore: the captured BYTES must still
//! hash to the capture-time hash (storage integrity), and the parsed
//! document must re-serialize to the exact same bytes (schema round-trip
//! integrity). Together they make a scrub provably lossless.

#include "engine/core/replay_scrubber.hpp"

#include <utility>

#include "engine/core/node_graph.hpp"

namespace omnicpp::editor {

std::uint64_t fnv1a64(const char* data, std::size_t size) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::uint64_t state_hash(const SceneDocument& doc) noexcept {
  const std::string json = doc.to_json();
  return fnv1a64(json.data(), json.size());
}

ReplayScrubber::ReplayScrubber(std::size_t capacity)
    : capacity_(capacity == 0U ? 1U : capacity) {
  ring_.reserve(capacity_);
}

bool ReplayScrubber::capture(std::uint64_t frame, const SceneDocument& doc,
                             std::string& /*error*/) {
  // to_json is byte-deterministic and total (contracts in document.hpp);
  // the error parameter stays for API stability.
  ReplayCheckpoint cp;
  cp.frame = frame;
  cp.json = doc.to_json();
  cp.hash = fnv1a64(cp.json.data(), cp.json.size());
  // Re-capturing the same frame replaces the old checkpoint (newest state
  // wins — auto-capture and manual markers can target the same frame).
  for (auto& existing : ring_) {
    if (existing.frame == frame) {
      existing = std::move(cp);
      return true;
    }
  }
  if (ring_.size() < capacity_) {
    ring_.push_back(std::move(cp));
  } else {
    ring_[write_index_] = std::move(cp);
    write_index_ = (write_index_ + 1U) % capacity_;
  }
  return true;
}

void ReplayScrubber::insert(std::uint64_t frame, std::uint64_t hash,
                            std::string json) {
  ReplayCheckpoint cp;
  cp.frame = frame;
  cp.hash = hash;
  cp.json = std::move(json);
  for (auto& existing : ring_) {
    if (existing.frame == frame) {
      existing = std::move(cp);
      return;
    }
  }
  if (ring_.size() < capacity_) {
    ring_.push_back(std::move(cp));
  } else {
    ring_[write_index_] = std::move(cp);
    write_index_ = (write_index_ + 1U) % capacity_;
  }
}

bool ReplayScrubber::restore(std::uint64_t frame, SceneDocument& doc,
                             std::string& error) {
  const ReplayCheckpoint* cp = find_checkpoint(frame);
  if (cp == nullptr) {
    error = "scrub: no checkpoint at frame " + std::to_string(frame);
    return false;
  }
  if (fnv1a64(cp->json.data(), cp->json.size()) != cp->hash) {
    error = "scrub: checkpoint at frame " + std::to_string(frame) +
            " failed hash verification";
    return false;
  }
  SceneDocument parsed;
  // Node types validate against the target's registry: seed the builtins so
  // graph-bearing documents restore (same contract as LoadDocument).
  register_builtin_node_types(parsed.node_graph);
  if (!SceneDocument::from_json(cp->json, parsed, error)) {
    error = "scrub: checkpoint at frame " + std::to_string(frame) +
            " failed to parse: " + error;
    return false;
  }
  if (parsed.to_json() != cp->json) {
    error = "scrub: checkpoint at frame " + std::to_string(frame) +
            " failed round-trip verification";
    return false;
  }
  doc = std::move(parsed);
  return true;
}

bool ReplayScrubber::verify(std::uint64_t frame) const {
  const ReplayCheckpoint* cp = find_checkpoint(frame);
  if (cp == nullptr) {
    return false;
  }
  return fnv1a64(cp->json.data(), cp->json.size()) == cp->hash;
}

bool ReplayScrubber::has(std::uint64_t frame) const {
  return find_checkpoint(frame) != nullptr;
}

std::vector<std::uint64_t> ReplayScrubber::frames() const {
  std::vector<std::uint64_t> out;
  out.reserve(ring_.size());
  if (ring_.size() < capacity_) {
    for (const auto& cp : ring_) {
      out.push_back(cp.frame);
    }
  } else {
    // Full ring: oldest entry sits at write_index_.
    for (std::size_t i = 0; i < ring_.size(); ++i) {
      out.push_back(ring_[(write_index_ + i) % ring_.size()].frame);
    }
  }
  return out;
}

const ReplayCheckpoint* ReplayScrubber::find_checkpoint(
    std::uint64_t frame) const {
  for (const auto& cp : ring_) {
    if (cp.frame == frame) {
      return &cp;
    }
  }
  return nullptr;
}

}  // namespace omnicpp::editor
