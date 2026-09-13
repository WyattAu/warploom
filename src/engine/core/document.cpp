//! @file document.cpp
//! @brief SceneDocument serialization + reversible commands (see the header).
//!
//! The writer is a hand-rolled emitter (map order + %.17g doubles give
//! byte-determinism); the reader is a strict recursive-descent JSON parser
//! sized exactly to the document schema — unknown keys, duplicate keys, and
//! out-of-range numbers are rejected with byte-offset diagnostics rather
//! than silently tolerated. Document size is editor-scale, so clarity beats
//! micro-optimization here; the hot path (per-frame telemetry) lives
//! elsewhere.

#include "engine/core/document.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "engine/core/contract.hpp"

namespace omnicpp::editor {

namespace {

// ============================================================================
// JSON string escaping (writer side)
// ============================================================================

void write_escaped(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  out += '"';
  for (const char c : text) {
    const auto uc = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (uc < 0x20U) {
          // Other control characters must be escaped to stay valid JSON.
          out += "\\u00";
          out += kHex[(uc >> 4U) & 0xFU];
          out += kHex[uc & 0xFU];
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

//! Round-trip-safe double formatting: %.17g always re-parses to the same
//! bits (shortest-form would too but varies across libc versions; we choose
//! determinism over brevity).
void write_number(std::string& out, double v) {
  char buf[64];
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
  OMNICPP_CONTRACT(n > 0 && static_cast<std::size_t>(n) < sizeof(buf));
  out.append(buf, static_cast<std::size_t>(n));
}

// ============================================================================
// Strict JSON reader (recursive descent over the document schema)
// ============================================================================

class JsonReader final {
 public:
  JsonReader(const char* begin, const char* end)
      : cur_(begin), begin_(begin), end_(end) {}

  [[nodiscard]] bool at_end() const noexcept { return cur_ >= end_; }

  void check_invariant(const char* site) const {
    if (cur_ < begin_ || cur_ > end_) {
      std::fprintf(stderr, "INVARIANT VIOLATION at %s: cur out of range\n", site);
      std::abort();
    }
  }

  void skip_ws() {
    while (!at_end() &&
           (*cur_ == ' ' || *cur_ == '\t' || *cur_ == '\n' || *cur_ == '\r')) {
      ++cur_;
    }
  }

  [[nodiscard]] bool peek_is(char c) const noexcept {
    return !at_end() && *cur_ == c;
  }

  //! Skips whitespace, then expects `c`.
  [[nodiscard]] bool expect(char c, std::string& error) {
    check_invariant("expect");
    skip_ws();
    if (at_end() || *cur_ != c) {
      fail(error, std::string("expected '") + c + "'");
      return false;
    }
    ++cur_;
    return true;
  }

  //! Reads a JSON string into `out` (handles the standard escapes; \u
  //! surrogates decode to UTF-8).
  [[nodiscard]] bool read_string(std::string& out, std::string& error) {
    check_invariant("read_string");
    skip_ws();
    if (at_end() || *cur_ != '"') {
      fail(error, "expected string");
      return false;
    }
    ++cur_;
    out.clear();
    while (!at_end()) {
      check_invariant("read_string:loop");
      const char c = *cur_++;
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (at_end()) break;
      const char esc = *cur_++;
      switch (esc) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = 0;
          if (!read_hex4(cp, error)) return false;
          if (cp >= 0xD800U && cp <= 0xDBFFU && cur_ + 1 < end_ &&
              cur_[0] == '\\' && cur_[1] == 'u') {
            cur_ += 2;
            unsigned lo = 0;
            if (!read_hex4(lo, error)) return false;
            if (lo >= 0xDC00U && lo <= 0xDFFFU) {
              cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
            } else {
              fail(error, "invalid low surrogate");
              return false;
            }
          }
          append_utf8(out, cp);
          break;
        }
        default:
          fail(error, std::string("bad escape '\\") + esc + "'");
          return false;
      }
    }
    fail(error, "unterminated string");
    return false;
  }

  //! Reads a number via std::from_chars (locale-free, strict).
  [[nodiscard]] bool read_number(double& out, std::string& error) {
    check_invariant("read_number");
    skip_ws();
    const char* start = cur_;
    if (!at_end() && (*cur_ == '-' || *cur_ == '+')) ++cur_;
    while (!at_end() &&
           ((*cur_ >= '0' && *cur_ <= '9') || *cur_ == '.' ||
            *cur_ == 'e' || *cur_ == 'E' || *cur_ == '+' || *cur_ == '-')) {
      ++cur_;
    }
    if (cur_ == start) {
      fail(error, "expected number");
      return false;
    }
    const char* first = start;
    const char* last = cur_;
    // from_chars rejects a leading '+'; normalize it away.
    if (*first == '+') ++first;
    const auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec != std::errc{} || ptr != last || !std::isfinite(out)) {
      fail(error, "malformed number");
      return false;
    }
    return true;
  }

  [[nodiscard]] bool read_bool(bool& out, std::string& error) {
    check_invariant("read_bool");
    skip_ws();
    if (cur_ + 4 <= end_ && std::string_view(cur_, 4) == "true") {
      cur_ += 4;
      out = true;
      return true;
    }
    if (cur_ + 5 <= end_ && std::string_view(cur_, 5) == "false") {
      cur_ += 5;
      out = false;
      return true;
    }
    fail(error, "expected bool");
    return false;
  }

  [[nodiscard]] bool read_key(std::string& out, std::string& error) {
    check_invariant("read_key");
    return read_string(out, error);
  }

  void fail(std::string& error, const std::string& message) const {
    error = "parse error at byte " + std::to_string(cur_ - begin_) + ": " +
            message;
  }

  [[nodiscard]] bool read_hex4(unsigned& out, std::string& error) {
    if (cur_ + 4 > end_) {
      fail(error, "truncated \\u escape");
      return false;
    }
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *cur_++;
      out <<= 4U;
      if (c >= '0' && c <= '9') {
        out |= static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        out |= static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        out |= static_cast<unsigned>(c - 'A' + 10);
      } else {
        fail(error, "bad hex digit in \\u escape");
        return false;
      }
    }
    return true;
  }

  static void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80U) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800U) {
      out += static_cast<char>(0xC0U | (cp >> 6U));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000U) {
      out += static_cast<char>(0xE0U | (cp >> 12U));
      out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else {
      out += static_cast<char>(0xF0U | (cp >> 18U));
      out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
      out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    }
  }

  const char* cur_;
  const char* begin_;
  const char* end_;
};

[[nodiscard]] bool read_prop_value(JsonReader& r, PropValue& out,
                                   std::string& error);

//! Reads one `{ "type": "...", ...payload }` prop value. Object form keeps
//! the on-disk shape self-describing.
[[nodiscard]] bool read_prop_object(JsonReader& r, PropValue& out,
                                    std::string& error) {
  if (!r.expect('{', error)) return false;
  std::string type_key;
  if (!r.read_key(type_key, error)) return false;
  if (type_key != "type") {
    r.fail(error, "expected \"type\" key in prop value, got \"" +
                      type_key + "\"");
    return false;
  }
  if (!r.expect(':', error)) return false;
  std::string type;
  if (!r.read_string(type, error)) return false;
  if (type == "number") {
    out.type = PropValue::Type::Number;
    if (!r.expect(',', error)) return false;
    std::string key;
    if (!r.read_key(key, error)) return false;
    if (key != "v") {
      r.fail(error, "unknown key \"" + key + "\" in number prop");
      return false;
    }
    return r.expect(':', error) && r.read_number(out.number, error) &&
           r.expect('}', error);
  }
  if (type == "bool") {
    out.type = PropValue::Type::Bool;
    if (!r.expect(',', error)) return false;
    std::string key;
    if (!r.read_key(key, error)) return false;
    if (key != "v") {
      r.fail(error, "unknown key \"" + key + "\" in bool prop");
      return false;
    }
    return r.expect(':', error) && r.read_bool(out.boolean, error) &&
           r.expect('}', error);
  }
  if (type == "string") {
    out.type = PropValue::Type::String;
    if (!r.expect(',', error)) return false;
    std::string key;
    if (!r.read_key(key, error)) return false;
    if (key != "v") {
      r.fail(error, "unknown key \"" + key + "\" in string prop");
      return false;
    }
    return r.expect(':', error) && r.read_string(out.text, error) &&
           r.expect('}', error);
  }
  if (type == "vec3") {
    out.type = PropValue::Type::Vec3;
    // Index-based loop: pointer arithmetic on the string LITERALS (axis -
    // kAxes[0]) yields character distances between rodata strings, not the
    // element index — the classic bug this loop shape avoids.
    static constexpr const char* kAxes[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
      if (!r.expect(',', error)) return false;
      std::string key;
      if (!r.read_key(key, error)) return false;
      if (key != kAxes[i]) {
        r.fail(error, std::string("expected \"") + kAxes[i] +
                          "\" in vec3 prop, got \"" + key + "\"");
        return false;
      }
      if (!r.expect(':', error)) return false;
      if (!r.read_number(out.vec[i], error)) return false;
    }
    return r.expect('}', error);
  }
  r.fail(error, "unknown prop type \"" + type + "\"");
  return false;
}

[[nodiscard]] bool read_prop_value(JsonReader& r, PropValue& out,
                                   std::string& error) {
  return read_prop_object(r, out, error);
}

//! Reads a `properties` object into the sorted map (duplicate keys rejected).
[[nodiscard]] bool read_properties(JsonReader& r,
                                   std::map<std::string, PropValue>& out,
                                   std::string& error) {
  if (!r.expect('{', error)) return false;
  r.skip_ws();
  if (!r.at_end() && r.peek_is('}')) {
    return r.expect('}', error);
  }
  for (;;) {
    std::string key;
    if (!r.read_key(key, error)) return false;
    if (out.contains(key)) {
      r.fail(error, "duplicate property key \"" + key + "\"");
      return false;
    }
    if (!r.expect(':', error)) return false;
    PropValue value;
    if (!read_prop_value(r, value, error)) return false;
    out.emplace(std::move(key), std::move(value));
    r.skip_ws();
    if (r.at_end()) {
      r.fail(error, "unterminated properties object");
      return false;
    }
    if (r.peek_is('}')) return r.expect('}', error);
    if (!r.expect(',', error)) return false;
  }
}

}  // namespace

// ============================================================================
// SceneDocument
// ============================================================================

SceneObject* SceneDocument::find(std::uint64_t id) {
  for (auto& object : objects) {
    if (object.id == id) return &object;
  }
  return nullptr;
}

const SceneObject* SceneDocument::find(std::uint64_t id) const {
  for (const auto& object : objects) {
    if (object.id == id) return &object;
  }
  return nullptr;
}

std::string SceneDocument::to_json() const {
  std::string out;
  out.reserve(256U + objects.size() * 96U);
  out += "{\"schema_version\":";
  write_number(out, static_cast<double>(schema_version));
  out += ",\"next_object_id\":";
  write_number(out, static_cast<double>(next_object_id));
  out += ",\"objects\":[";
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const SceneObject& object = objects[i];
    if (i != 0) out += ',';
    out += "{\"id\":";
    write_number(out, static_cast<double>(object.id));
    out += ",\"type_id\":";
    write_number(out, static_cast<double>(object.type_id));
    out += ",\"name\":";
    write_escaped(out, object.name);
    out += ",\"properties\":{";
    bool first = true;
    for (const auto& [key, value] : object.properties) {
      if (!first) out += ',';
      first = false;
      write_escaped(out, key);
      out += ':';
      switch (value.type) {
        case PropValue::Type::Number:
          out += "{\"type\":\"number\",\"v\":";
          write_number(out, value.number);
          out += '}';
          break;
        case PropValue::Type::Bool:
          out += "{\"type\":\"bool\",\"v\":";
          out += value.boolean ? "true" : "false";
          out += '}';
          break;
        case PropValue::Type::String:
          out += "{\"type\":\"string\",\"v\":";
          write_escaped(out, value.text);
          out += '}';
          break;
        case PropValue::Type::Vec3:
          out += "{\"type\":\"vec3\",\"x\":";
          write_number(out, value.vec[0]);
          out += ",\"y\":";
          write_number(out, value.vec[1]);
          out += ",\"z\":";
          write_number(out, value.vec[2]);
          out += '}';
          break;
      }
    }
    out += "}}";
  }
  out += "]}";
  return out;
}

bool SceneDocument::from_json(std::string_view text, SceneDocument& out,
                              std::string& error) {
  SceneDocument parsed{};
  JsonReader r(text.data(), text.data() + text.size());

  if (!r.expect('{', error)) return false;
  bool seen_version = false;
  bool seen_next_id = false;
  bool seen_objects = false;
  r.skip_ws();
  if (!r.at_end() && r.peek_is('}')) {
    r.fail(error, "empty document object");
    return false;
  }
  for (;;) {
    std::string key;
    if (!r.read_key(key, error)) return false;
    if (key == "schema_version") {
      if (seen_version) {
        r.fail(error, "duplicate key \"schema_version\"");
        return false;
      }
      double v = 0.0;
      if (!r.expect(':', error) || !r.read_number(v, error)) return false;
      if (v != static_cast<double>(static_cast<std::uint32_t>(v)) ||
          v < 1.0) {
        r.fail(error, "schema_version must be a positive integer");
        return false;
      }
      parsed.schema_version = static_cast<std::uint32_t>(v);
      if (parsed.schema_version > kDocumentSchemaVersion) {
        r.fail(error, "document schema v" +
                          std::to_string(parsed.schema_version) +
                          " is newer than this build (v" +
                          std::to_string(kDocumentSchemaVersion) + ")");
        return false;
      }
      seen_version = true;
    } else if (key == "next_object_id") {
      if (seen_next_id) {
        r.fail(error, "duplicate key \"next_object_id\"");
        return false;
      }
      double v = 0.0;
      if (!r.expect(':', error) || !r.read_number(v, error)) return false;
      if (v < 1.0 || v != std::floor(v) ||
          v > static_cast<double>(UINT64_MAX)) {
        r.fail(error, "next_object_id must be a positive integer");
        return false;
      }
      parsed.next_object_id = static_cast<std::uint64_t>(v);
      seen_next_id = true;
    } else if (key == "objects") {
      if (seen_objects) {
        r.fail(error, "duplicate key \"objects\"");
        return false;
      }
      if (!r.expect(':', error) || !r.expect('[', error)) return false;
      r.skip_ws();
      if (!r.at_end() && r.peek_is(']')) {
        if (!r.expect(']', error)) return false;
      } else {
        for (;;) {
          SceneObject object{};
          if (!r.expect('{', error)) return false;
          bool seen_id = false;
          bool seen_type = false;
          bool seen_name = false;
          bool seen_props = false;
          r.skip_ws();
          if (!r.at_end() && r.peek_is('}')) {
            r.fail(error, "empty object entry");
            return false;
          }
          for (;;) {
            std::string okey;
            if (!r.read_key(okey, error)) return false;
            if (okey == "id") {
              double v = 0.0;
              if (!r.expect(':', error) || !r.read_number(v, error)) {
                return false;
              }
              if (v < 1.0 || v != std::floor(v) ||
                  v > static_cast<double>(UINT64_MAX)) {
                r.fail(error, "object id must be a positive integer");
                return false;
              }
              object.id = static_cast<std::uint64_t>(v);
              seen_id = true;
            } else if (okey == "type_id") {
              double v = 0.0;
              if (!r.expect(':', error) || !r.read_number(v, error)) {
                return false;
              }
              if (v < 0.0 || v != std::floor(v) ||
                  v > static_cast<double>(UINT32_MAX)) {
                r.fail(error, "type_id must be a non-negative integer");
                return false;
              }
              object.type_id = static_cast<std::uint32_t>(v);
              seen_type = true;
            } else if (okey == "name") {
              if (!r.expect(':', error) ||
                  !r.read_string(object.name, error)) {
                return false;
              }
              seen_name = true;
            } else if (okey == "properties") {
              if (!r.expect(':', error)) return false;
              if (!read_properties(r, object.properties, error)) return false;
              seen_props = true;
            } else {
              r.fail(error, "unknown object key \"" + okey + "\"");
              return false;
            }
            r.skip_ws();
            if (r.at_end()) {
              r.fail(error, "unterminated object entry");
              return false;
            }
            if (r.peek_is('}')) break;
            if (!r.expect(',', error)) return false;
          }
          if (!r.expect('}', error)) return false;
          if (!seen_id || !seen_type || !seen_name || !seen_props) {
            r.fail(error, "object entry missing required key(s): " +
                              std::string(seen_id ? "" : "id ") +
                              std::string(seen_type ? "" : "type_id ") +
                              std::string(seen_name ? "" : "name ") +
                              std::string(seen_props ? "" : "properties"));
            return false;
          }
          parsed.objects.push_back(std::move(object));
          r.skip_ws();
          if (r.at_end()) {
            r.fail(error, "unterminated objects array");
            return false;
          }
          if (r.peek_is(']')) break;
          if (!r.expect(',', error)) return false;
        }
        if (!r.expect(']', error)) return false;
      }
      seen_objects = true;
    } else {
      r.fail(error, "unknown document key \"" + key + "\"");
      return false;
    }
    r.skip_ws();
    if (r.at_end()) {
      r.fail(error, "unterminated document object");
      return false;
    }
    if (r.peek_is('}')) break;
    if (!r.expect(',', error)) return false;
  }
  if (!r.expect('}', error)) return false;
  r.skip_ws();
  if (!r.at_end()) {
    r.fail(error, "trailing content after document");
    return false;
  }
  if (!seen_version || !seen_next_id || !seen_objects) {
    error = std::string("document missing required key(s): ") +
            (seen_version ? "" : "schema_version ") +
            (seen_next_id ? "" : "next_object_id ") +
            (seen_objects ? "" : "objects");
    return false;
  }
  out = std::move(parsed);
  return true;
}

// ============================================================================
// Commands
// ============================================================================

SetPropertyCommand::SetPropertyCommand(std::uint64_t object_id,
                                       std::string key, PropValue value)
    : object_id_(object_id), key_(std::move(key)), value_(std::move(value)) {}

bool SetPropertyCommand::apply(SceneDocument& doc, std::string& error) {
  SceneObject* object = doc.find(object_id_);
  if (object == nullptr) {
    error = "set_property: no object " + std::to_string(object_id_);
    return false;
  }
  const auto it = object->properties.find(key_);
  existed_ = it != object->properties.end();
  if (existed_) {
    old_value_ = it->second;
    it->second = value_;
  } else {
    old_value_ = PropValue{};
    object->properties.emplace(key_, value_);
  }
  return true;
}

void SetPropertyCommand::undo(SceneDocument& doc) {
  SceneObject* object = doc.find(object_id_);
  OMNICPP_CONTRACT(object != nullptr);
  if (existed_) {
    object->properties.insert_or_assign(key_, old_value_);
  } else {
    object->properties.erase(key_);
  }
}

std::string SetPropertyCommand::describe() const {
  return "set \"" + key_ + "\" on object " + std::to_string(object_id_);
}

SpawnObjectCommand::SpawnObjectCommand(std::uint64_t object_id,
                                       std::uint32_t type_id, std::string name,
                                       std::map<std::string, PropValue> props)
    : object_id_(object_id),
      type_id_(type_id),
      name_(std::move(name)),
      properties_(std::move(props)) {}

bool SpawnObjectCommand::apply(SceneDocument& doc, std::string& error) {
  if (doc.find(object_id_) != nullptr) {
    error = "spawn: object " + std::to_string(object_id_) + " already exists";
    return false;
  }
  SceneObject object;
  object.id = object_id_;
  object.type_id = type_id_;
  object.name = name_;
  object.properties = properties_;
  doc.objects.push_back(std::move(object));
  if (object_id_ >= doc.next_object_id) {
    doc.next_object_id = object_id_ + 1U;
    bumped_id_ = true;
  }
  return true;
}

void SpawnObjectCommand::undo(SceneDocument& doc) {
  for (std::size_t i = doc.objects.size(); i-- > 0;) {
    if (doc.objects[i].id == object_id_) {
      doc.objects.erase(doc.objects.begin() +
                        static_cast<std::ptrdiff_t>(i));
      if (bumped_id_) {
        doc.next_object_id = object_id_;  // restore the pre-apply counter
        bumped_id_ = false;
      }
      return;
    }
  }
  OMNICPP_CONTRACT(false && "spawn undo: object vanished");
}

std::string SpawnObjectCommand::describe() const {
  return "spawn object " + std::to_string(object_id_) + " (" + name_ + ")";
}

SetPropertiesCommand::SetPropertiesCommand(
    std::uint64_t object_id,
    std::vector<std::pair<std::string, PropValue>> entries)
    : object_id_(object_id), entries_(std::move(entries)) {}

bool SetPropertiesCommand::apply(SceneDocument& doc, std::string& error) {
  SceneObject* object = doc.find(object_id_);
  if (object == nullptr) {
    error = "set_properties: no object " + std::to_string(object_id_);
    return false;
  }
  // Validate all entries before mutating any (atomicity on failure).
  existed_.assign(entries_.size(), false);
  old_values_.assign(entries_.size(), PropValue{});
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const auto it = object->properties.find(entries_[i].first);
    if (it != object->properties.end()) {
      existed_[i] = true;
      old_values_[i] = it->second;
    }
  }
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    object->properties.insert_or_assign(entries_[i].first, entries_[i].second);
  }
  return true;
}

void SetPropertiesCommand::undo(SceneDocument& doc) {
  SceneObject* object = doc.find(object_id_);
  OMNICPP_CONTRACT(object != nullptr);
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (existed_[i]) {
      object->properties.insert_or_assign(entries_[i].first, old_values_[i]);
    } else {
      object->properties.erase(entries_[i].first);
    }
  }
}

std::string SetPropertiesCommand::describe() const {
  return "set " + std::to_string(entries_.size()) + " properties on object " +
         std::to_string(object_id_);
}

DestroyObjectCommand::DestroyObjectCommand(std::uint64_t object_id)
    : object_id_(object_id) {}

bool DestroyObjectCommand::apply(SceneDocument& doc, std::string& error) {
  for (std::size_t i = 0; i < doc.objects.size(); ++i) {
    if (doc.objects[i].id == object_id_) {
      index_ = i;
      captured_ = doc.objects[i];
      doc.objects.erase(doc.objects.begin() +
                        static_cast<std::ptrdiff_t>(i));
      applied_ = true;
      return true;
    }
  }
  error = "destroy: no object " + std::to_string(object_id_);
  return false;
}

void DestroyObjectCommand::undo(SceneDocument& doc) {
  OMNICPP_CONTRACT(applied_);
  const auto at = doc.objects.begin() +
                  static_cast<std::ptrdiff_t>(std::min(index_, doc.objects.size()));
  doc.objects.insert(at, captured_);
}

std::string DestroyObjectCommand::describe() const {
  return "destroy object " + std::to_string(object_id_);
}

// ============================================================================
// CommandStack
// ============================================================================

bool CommandStack::execute(std::unique_ptr<Command> command,
                           std::string& error) {
  OMNICPP_CONTRACT(command != nullptr);
  if (!command->apply(*doc_, error)) {
    return false;
  }
  undo_.push_back(std::move(command));
  redo_.clear();
  return true;
}

bool CommandStack::undo(std::string& error) {
  if (undo_.empty()) {
    error = "nothing to undo";
    return false;
  }
  undo_.back()->undo(*doc_);
  redo_.push_back(std::move(undo_.back()));
  undo_.pop_back();
  return true;
}

bool CommandStack::redo(std::string& error) {
  if (redo_.empty()) {
    error = "nothing to redo";
    return false;
  }
  // A stored redo command was applied successfully before; a failure here
  // would mean document corruption, so fail loudly but do not abort.
  if (!redo_.back()->apply(*doc_, error)) {
    return false;
  }
  undo_.push_back(std::move(redo_.back()));
  redo_.pop_back();
  return true;
}

}  // namespace omnicpp::editor
