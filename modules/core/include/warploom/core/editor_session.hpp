#pragma once

//! @file editor_session.hpp
//! @brief M3 editor session: authoritative document behind the control
//!        protocol (query + edit + undo/redo).
//!
//! `EditorSession` implements `ControlHost` on top of the M1 document:
//!   - scene-editing commands run through the bridge (typed registry
//!     defaults, validation) and execute on the CommandStack — every edit
//!     is undoable and the document stays byte-serializable;
//!   - query commands (list_objects / get_object / schema) serialize
//!     document/registry state back to the client;
//!   - session commands (ping/pause/step/capture) pass through untouched.
//!
//! The viewport embeds this session and mirrors document state into its
//! render structures; tests use it headless. The session never touches
//! GPU/renderer state — that keeps the protocol fully headless-testable.

#include <string>
#include <vector>

#include "warploom/core/command_recorder.hpp"
#include "warploom/core/control_server.hpp"
#include "warploom/core/document.hpp"
#include "warploom/core/property_registry.hpp"
#include "warploom/core/replay_scrubber.hpp"

namespace warploom::editor {

class EditorSession final : public ::warploom::core::ControlHost {
 public:
  EditorSession();

  [[nodiscard]] SceneDocument& document() { return doc_; }
  [[nodiscard]] const SceneDocument& document() const { return doc_; }
  [[nodiscard]] CommandStack& stack() { return stack_; }
  [[nodiscard]] const PropertyRegistry& registry() const {
    return default_registry();
  }

  //! Editor selection (mirrored by hosts for outline/highlight rendering;
  //! 0 = nothing selected). Set via the `select` protocol command.
  [[nodiscard]] std::uint64_t selected_id() const noexcept {
    return selected_id_;
  }

  // -- Graph -> scene bridge (M9) -----------------------------------------
  //! Binds one node output pin to one object property. When `sync_graph()`
  //! runs (once per editor tick), each binding writes the pin's CURRENT
  //! evaluated value into the object property — direct application, NOT a
  //! command (graphs drive state every frame; undoing a driven property is
  //! the graph's job, via node params). Rejected at bind time when object,
  //! property, or pin does not exist or the value type mismatches.
  struct PropertyBinding {
    std::uint64_t node_id{0};
    std::string out_pin{};
    std::uint64_t object_id{0};
    std::string property{};
  };
  //! Returns false + `error` when the binding is invalid (never half-bound).
  [[nodiscard]] bool bind_property(std::uint64_t node_id,
                                   std::string out_pin,
                                   std::uint64_t object_id,
                                   std::string property,
                                   std::string& error);
  //! Removes the binding on an object property (false when none).
  [[nodiscard]] bool unbind_property(std::uint64_t object_id,
                                     const std::string& property);
  [[nodiscard]] const std::vector<PropertyBinding>& bindings() const noexcept {
    return bindings_;
  }
  //! Replaces the document wholesale (file load): the loaded document becomes
  //! authoritative and history is cleared (undo across a load boundary is
  //! meaningless). Bindings and selection reset; borrowed graph pointers
  //! stay valid (the document object's address does not change).
  void reset_from(SceneDocument&& loaded) {
    doc_ = std::move(loaded);
    stack_.clear();
    bindings_.clear();
    selected_id_ = 0;
    recording_ = false;  // G3: armed clip intent does not survive a replace
    playing_ = false;
  }
  //! Runs the graph evaluation, then writes every binding's current pin
  //! value into its bound object property. Returns the number of bindings
  //! applied. Deterministic: bindings apply in insertion order.
  [[nodiscard]] std::size_t sync_graph(std::string& error);

  // -- Replay scrubbing (W1, protocol v1.6) --------------------------------
  //! Captures a checkpoint of the CURRENT document at sim frame `frame`.
  //! Checkpoints live in a bounded ring (see ReplayScrubber); restoring is
  //! hash-verified two ways (bytes -> capture hash, parse -> re-serialize
  //! byte equality) so a scrub is provably lossless.
  [[nodiscard]] bool scrub_start(std::uint64_t frame, std::string& error);
  //! Restores the checkpoint captured at sim frame `frame`. The document is
  //! replaced by the captured state and history CLEARS — undo cannot cross
  //! a time warp (same semantics as a document load). Selection survives
  //! only when the restored document still contains the object.
  [[nodiscard]] bool scrub_to(std::uint64_t frame, std::string& error);
  [[nodiscard]] const ReplayScrubber& scrubber() const noexcept {
    return scrubber_;
  }

  // -- Timeline clips (G3, protocol v1.8) ----------------------------------
  //! ONE session tick of the timeline: record-into-armed-clip, then
  //! play-from-armed-clip. Direct application (NOT commands — driven values
  //! are the bindings model; W1 scrubbing is the recovery path). Call once
  //! per sim tick AFTER sync_graph (bindings write first; the clip wins
  //! that frame when both drive the same property — documented order).
  //! The current document value is read AFTER playback applied, so a clip
  //! that plays into the same track it records from reproduces itself.
  void tick_timeline(std::uint64_t frame);
  [[nodiscard]] const TimelineClip* recording_target() const noexcept {
    return recording_ ? doc_.find_clip(recording_clip_) : nullptr;
  }
  [[nodiscard]] const TimelineClip* playing_clip() const noexcept {
    return playing_ ? doc_.find_clip(playing_clip_) : nullptr;
  }

  // -- Protocol record/replay (W2, protocol v1.7) ---------------------------
  //! Begins recording protocol commands into a warploom-replay-v1 file
  //! (docs/replay-format.md). Embeds the opening checkpoint at `frame`.
  [[nodiscard]] bool capture_start(std::uint64_t frame,
                                   const std::string& scene,
                                   std::string& error) {
    return recorder_.start(frame, doc_, scene, error);
  }
  //! Ends recording and writes the file atomically (tmp+rename, 0600).
  //! Embeds the closing checkpoint at the current logical frame.
  [[nodiscard]] bool capture_stop(const std::string& path,
                                  std::string& error) {
    return recorder_.stop(path, doc_, error);
  }
  [[nodiscard]] const CommandRecorder& recorder() const noexcept {
    return recorder_;
  }
  //! Records a command that was handled OUTSIDE the session (the viewport
  //! host owns pause/resume/step — the sim loop — so those never reach
  //! on_control). Call after the host handled the command successfully;
  //! recording semantics are identical (recorded-kinds set, frame stamps).
  void record_external(const ::warploom::core::ControlCommand& command) {
    if (!replaying_) recorder_.record(command);
  }
  //! Loads a warploom-replay-v1 file: hash-verified checkpoint hydration
  //! + command-log re-apply in seq order (spec "Load semantics"), with
  //! ONE full session tick (sync_graph + tick_timeline) per recorded step
  //! tick — sample capture re-executes deterministically.
  [[nodiscard]] bool load_replay(const std::string& path,
                                 std::string& error);

  // -- ControlHost ---------------------------------------------------------
  [[nodiscard]] ::warploom::core::ControlReply on_control(
      const ::warploom::core::ControlCommand& command) override;
  [[nodiscard]] std::string snapshot_json() const override;

 private:
  [[nodiscard]] ::warploom::core::ControlReply handle_session(
      const ::warploom::core::ControlCommand& command);
  //! Returns true when the kind was a query (reply filled).
  [[nodiscard]] bool handle_query(
      const ::warploom::core::ControlCommand& command,
      ::warploom::core::ControlReply& reply);
  //! Returns false when the kind is not a document edit.
  [[nodiscard]] bool handle_edit(
      const ::warploom::core::ControlCommand& command,
      ::warploom::core::ControlReply& reply);

  SceneDocument doc_{};
  CommandStack stack_{doc_};
  std::uint64_t selected_id_{0};
  CommandRecorder recorder_{};  //!< W2 capture (frame-thread-only)
  bool replaying_{false};       //!< suppresses re-recording during load_replay
  std::vector<PropertyBinding> bindings_{};
  // G3: armed clip state (session-side intent; at most one of each).
  bool recording_{false};       //!< armed record target exists
  std::uint64_t recording_clip_{0};
  std::uint64_t recording_object_{0};
  std::string recording_property_{};
  std::uint64_t recorded_samples_{0};  //!< next sample offset (per arm)
  bool playing_{false};         //!< armed playback clip exists
  std::uint64_t playing_clip_{0};
  std::uint64_t play_started_{0};  //!< frame playback was armed at
  //! W1: checkpoint ring for hash-verified scrubbing. Frame-thread-only
  //! (all access arrives via on_control), matching the session's model.
  ReplayScrubber scrubber_{};
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
