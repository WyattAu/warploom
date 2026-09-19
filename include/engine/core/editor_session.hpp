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

#include "engine/core/control_server.hpp"
#include "engine/core/document.hpp"
#include "engine/core/property_registry.hpp"

namespace omnicpp::editor {

class EditorSession final : public omnicpp::core::ControlHost {
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

  // -- ControlHost ---------------------------------------------------------
  [[nodiscard]] omnicpp::core::ControlReply on_control(
      const omnicpp::core::ControlCommand& command) override;
  [[nodiscard]] std::string snapshot_json() const override;

 private:
  [[nodiscard]] omnicpp::core::ControlReply handle_session(
      const omnicpp::core::ControlCommand& command);
  //! Returns true when the kind was a query (reply filled).
  [[nodiscard]] bool handle_query(
      const omnicpp::core::ControlCommand& command,
      omnicpp::core::ControlReply& reply);
  //! Returns false when the kind is not a document edit.
  [[nodiscard]] bool handle_edit(
      const omnicpp::core::ControlCommand& command,
      omnicpp::core::ControlReply& reply);

  SceneDocument doc_{};
  CommandStack stack_{doc_};
  std::uint64_t selected_id_{0};
};

}  // namespace omnicpp::editor
