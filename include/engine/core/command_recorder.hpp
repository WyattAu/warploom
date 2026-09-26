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

#include "engine/core/control_server.hpp"
#include "engine/core/document.hpp"

namespace omnicpp::editor {

//! One frame-stamped command log entry: the parsed command in positional
//! form (numbers array + non-empty texts), NOT the raw wire line — replay
//! rebuilds the ControlCommand directly, so recording can never drift from
//! the parser.
struct RecordedCommand final {
  std::uint64_t frame{0};
  omnicpp::core::ControlCommand command{};
};

//! Session-side recorder. Owned by EditorSession; driven from on_control.
class CommandRecorder final {
 public:
  //! Begins recording at logical `frame`. Embeds the opening checkpoint
  //! (capture-start + explicit-snapshots density, per the format spec).
  //! Returns false + error when already recording or the document fails to
  //! serialize.
  [[nodiscard]] bool start(std::uint64_t frame, const SceneDocument& doc,
                           const std::string& scene, std::string& error);

  //! Ends recording and writes the file. Embeds the closing checkpoint at
  //! the current logical frame. Returns false + error when not recording,
  //! the path is empty, the document fails to serialize, or the write
  //! fails; recording STOPS only on success (a failed stop keeps the
  //! session intact — caller may retry). Clears the log on success.
  [[nodiscard]] bool stop(const std::string& path, const SceneDocument& doc,
                          std::string& error);

  //! Records a command whose reply was already ok. Enforces the spec's
  //! recorded-kinds set and updates the logical frame for step commands.
  void record(const omnicpp::core::ControlCommand& command);

  //! Embeds a checkpoint at an EXPLICIT frame: `start` uses its start
  //! frame, the session's scrub_start handler uses the scrub target frame
  //! (checkpoints must be warpable targets, not logical arrival frames),
  //! `stop` uses the final logical frame. Explicit frames never collide,
  //! so same-frame replacement stays meaningful.
  void embed_checkpoint(const SceneDocument& doc, std::uint64_t frame);

  //! True while a capture is active.
  [[nodiscard]] bool active() const noexcept { return active_; }
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
  };

  std::vector<RecordedCommand> log_{};
  std::vector<CheckpointLine> checkpoints_{};
  bool active_{false};
  std::uint64_t frame_{0};
  std::string scene_{};
};

}  // namespace omnicpp::editor
