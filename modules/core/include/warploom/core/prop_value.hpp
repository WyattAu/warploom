#pragma once

//! @file prop_value.hpp
//! @brief The typed value currency of the editor document and node graph.
//!
//! Extracted from document.hpp (M7) so the node-graph layer can include the
//! value type without depending on the whole scene-document schema. Layout
//! is unchanged: same types, same fields, same round-trip semantics — every
//! existing serialization byte stays identical.

#include <cstdint>
#include <string>
#include <utility>

namespace omnicpp::editor {

//! The typed currency of the document: every property is one of these.
struct PropValue final {
  enum class Type : std::uint8_t { Number, Bool, String, Vec3 };

  Type type{Type::Number};
  double number{0.0};
  bool boolean{false};
  std::string text{};
  double vec[3]{0.0, 0.0, 0.0};

  [[nodiscard]] static PropValue make_number(double v) {
    PropValue p;
    p.type = Type::Number;
    p.number = v;
    return p;
  }
  [[nodiscard]] static PropValue make_bool(bool v) {
    PropValue p;
    p.type = Type::Bool;
    p.boolean = v;
    return p;
  }
  [[nodiscard]] static PropValue make_string(std::string v) {
    PropValue p;
    p.type = Type::String;
    p.text = std::move(v);
    return p;
  }
  [[nodiscard]] static PropValue make_vec3(double x, double y, double z) {
    PropValue p;
    p.type = Type::Vec3;
    p.vec[0] = x;
    p.vec[1] = y;
    p.vec[2] = z;
    return p;
  }

  [[nodiscard]] bool operator==(const PropValue&) const = default;
};

}  // namespace omnicpp::editor
