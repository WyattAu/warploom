#pragma once

//! @file command_recorder.hpp
//! @brief W2 protocol record/replay: session-side recorder producing
//!        warploom-replay-v1 files (docs/replay-format.md).
//!
//! The recorder OBSERVES commands post-parse — it never mutates the
//! document and adds no nondeterminism. While active, every command whose
//! reply was ok is appended to a frame-stamped, positional-args command
//! log; checkpoints (start, scrub_start, stop) embed the byte-deterministic
//! document serialization. stop_capture writes the file atomically
//! (tmp+rename, 0600) — the G1 save discipline.
//!
//! Threading: frame-thread-only, exactly like the EditorSession it wraps;
//! callers serialize. No locks.

#include <cstdint>
#include <string>
#include <vector>

#include "warploom/core/control_server.hpp"
#include "warploom/core/document.hpp"

namespace warploom::editor {

//! One frame-stamped command log entry: the parsed command in positional
//! form (numbers array + non-empty texts), NOT the raw wire line — replay
//! rebuilds the ControlCommand directly, so recording can never drift from
//! the parser.
struct RecordedCommand final {
  std::uint64_t frame{0};
  ::warploom::core::ControlCommand command{};
};

//! Session-side recorder. Owned by EditorSession; driven from on_control.
class CommandRecorder final {
 public:
  //! Begins recording at logical `frame`. Embeds the opening checkpoint
  //! (capture-start + explicit-snapshots density, per the format spec).
  //! `physics_json` is the T4a solver snapshot at the same moment (opaque
  //! bytes); `fixed_dt` is the host's fixed step recorded into the header
  //! so load_replay re-simulates physics with the same increment.
  //! Returns false + error when already recording or the document fails to
  //! serialize.
  [[nodiscard]] bool start(std::uint64_t frame, const SceneDocument& doc,
                           const std::string& physics_json, double fixed_dt,
                           const std::string& scene, std::string& error);

  //! Ends recording and writes the file. Embeds the closing checkpoint at
  //! the current logical frame. Returns false + error when not recording,
  //! the path is empty, the document fails to serialize, or the write
  //! fails; recording STOPS only on success (a failed stop keeps the
  //! session intact — caller may retry). Clears the log on success.
  [[nodiscard]] bool stop(const std::string& path, const SceneDocument& doc,
                          const std::string& physics_json, std::string& error);

  //! Records a command whose reply was already ok. Enforces the spec's
  //! recorded-kinds set and updates the logical frame for step commands.
  void record(const ::warploom::core::ControlCommand& command);

  //! Embeds a checkpoint at an EXPLICIT frame: `start` uses its start
  //! frame, the session's scrub_start handler uses the scrub target frame
  //! (checkpoints must be warpable targets, not logical arrival frames),
  //! `stop` uses the final logical frame. Explicit frames never collide,
  //! so same-frame replacement stays meaningful. Physics travels with the
  //! document (T4a); both hashes land in the marker line.
  void embed_checkpoint(const SceneDocument& doc,
                        const std::string& physics_json, std::uint64_t frame);

  //! True while a capture is active.
  [[nodiscard]] bool active() const noexcept { return active_; }
  //! True when a checkpoint is already embedded at `frame` (the scrub_to
  //! handler uses it to avoid duplicating bytes the scrub_start handler
  //! already embedded for the same warp target).
  [[nodiscard]] bool has_checkpoint(std::uint64_t frame) const noexcept {
    for (const auto& c : checkpoints_) {
      if (c.frame == frame) return true;
    }
    return false;
  }
  //! Current logical frame (start frame + accumulated step ticks).
  [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }
  //! Number of recorded commands so far.
  [[nodiscard]] std::size_t command_count() const noexcept {
    return log_.size();
  }
  //! Number of embedded checkpoints so far.
  [[nodiscard]] std::size_t checkpoint_count() const noexcept {
    return checkpoints_.size();
  }

 private:
  struct CheckpointLine final {
    std::uint64_t frame{0};
    std::uint64_t hash{0};
    std::string json{};
    std::uint64_t physics_hash{0};
    std::string physics_json{};
  };

  std::vector<RecordedCommand> log_{};
  std::vector<CheckpointLine> checkpoints_{};
  bool active_{false};
  std::uint64_t frame_{0};
  std::string scene_{};
  //! Host fixed step latched at start (T4a): re-simulation increment.
  //! 0 in files predating the header field = unknown, do not re-simulate.
  double fixed_dt_{0.0};
};

}  // namespace warploom::editor