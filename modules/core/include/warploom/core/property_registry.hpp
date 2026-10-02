#pragma once

//! @file property_registry.hpp
//! @brief Reflection-lite property registry + M0-protocol → document bridge.
//!
//! The registry assigns stable ids to editor object *types* and defines each
//! type's properties with typed defaults — one source of truth that later
//! feeds the inspector UI, the `.scene` schema, and protocol validation.
//! (C++26 static reflection replaces the hand registration once Clang lands
//! P2996; the registry API is designed to survive that swap.)
//!
//! The bridge maps M0 ControlCommands to document edits:
//!   spawn_cube  → SpawnObjectCommand (registry type "cube", typed defaults
//!                  overridden by the command payload)
//!   set_camera  → SetPropertyCommand on the singleton "environment" object
//!   set_sun     → SetPropertyCommand on "environment"
//!   ping/pause/resume/step/capture → session state, no document edit

#include <string>

#include "warploom/core/control_server.hpp"
#include "warploom/core/document.hpp"

namespace warploom::editor {

//! One registered property: typed default + description.
struct PropertyDesc final {
  std::string name;
  PropValue default_value;
  std::string doc;  // inspector tooltip text
};

//! One registered object type.
struct ObjectTypeDesc final {
  std::uint32_t id{0};
  std::string name;
  std::string doc;
  std::vector<PropertyDesc> properties{};
};

//! Global registry. Registration order assigns ids; names are unique.
class PropertyRegistry final {
 public:
  PropertyRegistry() = default;
  PropertyRegistry(const PropertyRegistry&) = delete;
  PropertyRegistry& operator=(const PropertyRegistry&) = delete;

  //! Registers a type; returns its assigned id. Duplicate names are a
  //! programming error (contract).
  [[nodiscard]] std::uint32_t register_type(
      std::string name, std::string doc,
      std::vector<PropertyDesc> properties);

  [[nodiscard]] const ObjectTypeDesc* find(std::uint32_t id) const;
  [[nodiscard]] const ObjectTypeDesc* find_by_name(
      std::string_view name) const;
  [[nodiscard]] std::size_t type_count() const noexcept {
    return types_.size();
  }

 private:
  std::vector<ObjectTypeDesc> types_{};
};

//! The engine's shared registry: cube, sphere, light, environment. Ids are
//! process-stable because registration order is fixed.
[[nodiscard]] PropertyRegistry& default_registry();

//! Built-in type names (registry lookups + tests).
inline constexpr std::string_view kTypeCube = "cube";
inline constexpr std::string_view kTypeSphere = "sphere";
inline constexpr std::string_view kTypeLight = "light";
inline constexpr std::string_view kTypeEnvironment = "environment";

//! Singleton environment object id (camera/sun properties live here).
inline constexpr std::uint64_t kEnvironmentObjectId = 1;

//! Result of bridging one control command to the document.
struct BridgeOutcome final {
  enum class Kind : std::uint8_t {
    //! Document was mutated; `command` holds the executed edit.
    Edited,
    //! Session-state command (ping/pause/resume/step/capture): no edit.
    SessionOnly,
    //! Invalid payload for the target type: `error` explains.
    Rejected,
  };

  Kind kind{Kind::SessionOnly};
  std::unique_ptr<Command> command{};  // valid when kind == Edited
  std::string error{};                 // set when kind == Rejected
};

//! Maps one M0 control command to a document edit (or session-only outcome).
//! Pure: inspects the document, never mutates it — the caller executes the
//! returned command on its CommandStack so undo stays in one place.
[[nodiscard]] BridgeOutcome bridge_control_command(
    const ::warploom::core::ControlCommand& command, const SceneDocument& doc,
    const PropertyRegistry& registry);

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
