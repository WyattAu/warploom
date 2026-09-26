//! @file property_registry.cpp
//! @brief Registry bodies + the M0→document bridge (see the header).

#include "warploom/core/property_registry.hpp"

#include <cmath>

#include "warploom/core/contract.hpp"

namespace omnicpp::editor {

std::uint32_t PropertyRegistry::register_type(
    std::string name, std::string doc, std::vector<PropertyDesc> properties) {
  OMNICPP_CONTRACT(!name.empty());
  for (const auto& existing : types_) {
    OMNICPP_CONTRACT(existing.name != name && "duplicate registry type");
  }
  OMNICPP_CONTRACT(types_.size() < UINT32_MAX);
  const auto id = static_cast<std::uint32_t>(types_.size());
  types_.push_back(
      ObjectTypeDesc{id, std::move(name), std::move(doc), std::move(properties)});
  return id;
}

const ObjectTypeDesc* PropertyRegistry::find(std::uint32_t id) const {
  return id < types_.size() ? &types_[id] : nullptr;
}

const ObjectTypeDesc* PropertyRegistry::find_by_name(
    std::string_view name) const {
  for (const auto& type : types_) {
    if (type.name == name) return &type;
  }
  return nullptr;
}

PropertyRegistry& default_registry() {
  // Function-local static: ids are assigned once, in fixed order, so they are
  // process-stable and safe to serialize as type_id. Built via a one-shot
  // flag (the registry is non-copyable, so it cannot be returned from a
  // constructing lambda).
  static PropertyRegistry registry;
  static const bool built = [] {
    registry.register_type(
        std::string(kTypeCube), "Axis-aligned unit cube scaled per-axis",
        {{"position", PropValue::make_vec3(0.0, 0.0, 0.0), "World position"},
         {"rotation", PropValue::make_vec3(0.0, 0.0, 0.0), "Euler XYZ degrees"},
         {"scale", PropValue::make_vec3(1.0, 1.0, 1.0), "Per-axis scale"},
         {"color", PropValue::make_vec3(0.8, 0.8, 0.8), "Linear albedo RGB"}});
    registry.register_type(
        std::string(kTypeSphere), "Unit sphere centered at the origin",
        {{"position", PropValue::make_vec3(0.0, 0.0, 0.0), "World position"},
         {"radius", PropValue::make_number(0.5), "Radius in meters"},
         {"color", PropValue::make_vec3(0.8, 0.8, 0.8), "Linear albedo RGB"}});
    registry.register_type(
        std::string(kTypeLight), "Omnidirectional point light",
        {{"position", PropValue::make_vec3(0.0, 2.0, 0.0), "World position"},
         {"color", PropValue::make_vec3(1.0, 1.0, 1.0), "Linear RGB"},
         {"intensity", PropValue::make_number(1.0), "Candela"}});
    registry.register_type(
        std::string(kTypeEnvironment),
        "Scene environment: singleton object holding camera and sun state",
        {{"camera_eye", PropValue::make_vec3(8.0, 3.0, 8.0), "Camera position"},
         {"camera_target", PropValue::make_vec3(0.0, 1.0, 0.0),
          "Camera look-at"},
         {"camera_fov", PropValue::make_number(60.0), "Vertical fov degrees"},
         {"sun_direction", PropValue::make_vec3(0.3, 0.65, 0.7),
          "Normalized sun vector"}});
    return true;
  }();
  (void)built;
  return registry;
}

BridgeOutcome bridge_control_command(const omnicpp::core::ControlCommand& c,
                                     const SceneDocument& doc,
                                     const PropertyRegistry& registry) {
  using CK = omnicpp::core::ControlCommand::Kind;
  using BK = BridgeOutcome::Kind;

  const auto* env = registry.find_by_name(kTypeEnvironment);
  OMNICPP_CONTRACT(env != nullptr);
  (void)env;

  switch (c.kind) {
    case CK::Ping:
    case CK::Pause:
    case CK::Resume:
    case CK::Step:
    case CK::Capture:
      return BridgeOutcome{BK::SessionOnly, nullptr, {}};

    case CK::SpawnCube: {
      const auto* cube = registry.find_by_name(kTypeCube);
      OMNICPP_CONTRACT(cube != nullptr);
      // Typed defaults from the registry, then payload overrides.
      std::map<std::string, PropValue> props;
      for (const auto& desc : cube->properties) {
        props.emplace(desc.name, desc.default_value);
      }
      const bool have_pos = c.number_count >= 3U;
      const bool have_size = c.number_count >= 4U;
      if (c.number_count > 0U && !have_pos) {
        return BridgeOutcome{BK::Rejected, nullptr,
                             "spawn_cube needs x/y/z (+ optional size)"};
      }
      if (have_pos) {
        if (!std::isfinite(c.numbers[0]) || !std::isfinite(c.numbers[1]) ||
            !std::isfinite(c.numbers[2])) {
          return BridgeOutcome{BK::Rejected, nullptr,
                               "spawn_cube position must be finite"};
        }
        props.at("position") =
            PropValue::make_vec3(c.numbers[0], c.numbers[1], c.numbers[2]);
      }
      if (have_size) {
        if (!std::isfinite(c.numbers[3]) || c.numbers[3] <= 0.0) {
          return BridgeOutcome{BK::Rejected, nullptr,
                               "spawn_cube size must be > 0"};
        }
        // "size" is uniform scale expressed as a scalar; store into scale.
        props.at("scale") = PropValue::make_vec3(c.numbers[3], c.numbers[3],
                                                 c.numbers[3]);
      }
      auto command = std::make_unique<SpawnObjectCommand>(
          doc.next_object_id, cube->id, "cube_" + std::to_string(doc.next_object_id),
          std::move(props));
      return BridgeOutcome{BK::Edited, std::move(command), {}};
    }

    case CK::SetCamera: {
      if (c.number_count < 7U) {
        return BridgeOutcome{BK::Rejected, nullptr,
                             "set_camera needs ex/ey/ez/tx/ty/tz/fov"};
      }
      for (std::uint32_t i = 0; i < 7U; ++i) {
        if (!std::isfinite(c.numbers[i])) {
          return BridgeOutcome{BK::Rejected, nullptr,
                               "set_camera values must be finite"};
        }
      }
      if (doc.find(kEnvironmentObjectId) == nullptr) {
        return BridgeOutcome{
            BK::Rejected, nullptr,
            "set_camera: environment object missing from document"};
      }
      // One atomic edit: eye + target + fov undo together.
      auto command = std::make_unique<SetPropertiesCommand>(
          kEnvironmentObjectId,
          std::vector<std::pair<std::string, PropValue>>{
              {"camera_eye",
               PropValue::make_vec3(c.numbers[0], c.numbers[1], c.numbers[2])},
              {"camera_target",
               PropValue::make_vec3(c.numbers[3], c.numbers[4], c.numbers[5])},
              {"camera_fov", PropValue::make_number(c.numbers[6])}});
      return BridgeOutcome{BK::Edited, std::move(command), {}};
    }

    case CK::SetSun: {
      if (c.number_count < 3U) {
        return BridgeOutcome{BK::Rejected, nullptr, "set_sun needs x/y/z"};
      }
      if (!std::isfinite(c.numbers[0]) || !std::isfinite(c.numbers[1]) ||
          !std::isfinite(c.numbers[2])) {
        return BridgeOutcome{BK::Rejected, nullptr,
                             "set_sun direction must be finite"};
      }
      if (doc.find(kEnvironmentObjectId) == nullptr) {
        return BridgeOutcome{
            BK::Rejected, nullptr,
            "set_sun: environment object missing from document"};
      }
      auto command = std::make_unique<SetPropertyCommand>(
          kEnvironmentObjectId, "sun_direction",
          PropValue::make_vec3(c.numbers[0], c.numbers[1], c.numbers[2]));
      return BridgeOutcome{BK::Edited, std::move(command), {}};
    }

    case CK::Unknown:
      return BridgeOutcome{BK::Rejected, nullptr, "unknown command kind"};
  }
  return BridgeOutcome{BK::Rejected, nullptr, "unknown command kind"};
}

}  // namespace omnicpp::editor
