//! @file gltf_json.hpp
//! @brief Internal glTF decoding infrastructure shared by the asset importers.
//!
//! Provides the strict RFC 8259 JSON parser, base64/data-URI decoding, glTF
//! accessor/bufferView decoding, and small JSON access helpers used by
//! import_gltf_mesh / import_gltf_scene and the skeletal-animation importer.
//! Everything here is `inline` (or an internal-linkage constexpr) so a
//! single inclusion per translation unit is the only requirement; include
//! this header from exactly one TU per binary, or freely from several since
//! no definition can clash.

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omnicpp::asset::gltf_detail {

// ============================================================================
// Minimal RFC 8259 JSON parser (recursive descent, strict, no exceptions)
// ============================================================================

struct Json {
  enum class Kind { Null, Bool, Int, Real, String, Array, Object };
  Kind kind{Kind::Null};
  bool boolean{false};
  std::int64_t integer{0};
  double real{0.0};
  std::string string{};
  std::vector<Json> items{};                          // Array
  //! Object members as a named struct: std::vector<std::pair<string, Json>>
  //! instantiates std::pair<string, Json> while Json is still incomplete,
  //! which Clang rejects (the P0735 vector<Incomplete> exemption does not
  //! apply), and even a vector<Json::Member> of a forward-declared nested
  //! struct trips the eager sizeof/alignof trait queries on Clang+libstdc++.
  //! unique_ptr indirection is the portable shape: sizeof(unique_ptr<Member>)
  //! is independent of Member's completeness, and Member is completed
  //! immediately after Json so every destructor instantiation sees it.
  struct Member;  // defined right after Json (holds a Json by value)
  std::vector<std::unique_ptr<Member>> members{};  // Object
};

struct Json::Member {
  std::string key;
  Json value;
};

constexpr std::size_t kMaxJsonDepth = 96;

class JsonParser final {
public:
  JsonParser(const char* data, std::size_t size, std::string& error)
      : data_(data), size_(size), error_(error) {}

  [[nodiscard]] bool parse(Json& root) {
    skip_ws();
    if (!parse_value(root, 0)) return false;
    skip_ws();
    if (pos_ != size_) return fail("trailing content after JSON document");
    return true;
  }

private:
  const char* data_;
  std::size_t size_;
  std::size_t pos_{0};
  std::string& error_;

  [[nodiscard]] bool fail(std::string_view message) {
    if (error_.empty()) {
      error_ = "JSON parse error at byte ";
      error_ += std::to_string(pos_);
      error_ += ": ";
      error_.append(message.data(), message.size());
    }
    return false;
  }

  void skip_ws() {
    while (pos_ < size_ && (data_[pos_] == ' ' || data_[pos_] == '\t' ||
                            data_[pos_] == '\n' || data_[pos_] == '\r')) {
      ++pos_;
    }
  }

  [[nodiscard]] bool parse_value(Json& out, std::size_t depth) {
    if (depth > kMaxJsonDepth) return fail("nesting exceeds depth limit");
    skip_ws();
    if (pos_ >= size_) return fail("unexpected end of document");
    const char c = data_[pos_];
    switch (c) {
      case '{': return parse_object(out, depth);
      case '[': return parse_array(out, depth);
      case '"': return parse_string_value(out);
      case 't': return parse_literal("true", out, Json::Kind::Bool, true);
      case 'f': return parse_literal("false", out, Json::Kind::Bool, false);
      case 'n': return parse_literal("null", out, Json::Kind::Null, false);
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        return fail("unexpected character");
    }
  }

  [[nodiscard]] bool parse_literal(const char* literal, Json& out,
                                   Json::Kind kind, bool boolean) {
    const std::size_t len = std::strlen(literal);
    if (pos_ + len > size_ ||
        std::memcmp(data_ + pos_, literal, len) != 0) {
      return fail("invalid literal");
    }
    pos_ += len;
    out.kind = kind;
    out.boolean = boolean;
    return true;
  }

  [[nodiscard]] bool parse_object(Json& out, std::size_t depth) {
    out.kind = Json::Kind::Object;
    ++pos_;  // consume '{'
    skip_ws();
    if (pos_ < size_ && data_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      skip_ws();
      if (pos_ >= size_ || data_[pos_] != '"') return fail("expected member name");
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (pos_ >= size_ || data_[pos_] != ':') return fail("expected ':'");
      ++pos_;
      Json value;
      if (!parse_value(value, depth + 1)) return false;
      auto member = std::make_unique<Json::Member>();
      member->key = std::move(key);
      member->value = std::move(value);
      out.members.push_back(std::move(member));
      skip_ws();
      if (pos_ >= size_) return fail("unterminated object");
      if (data_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (data_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  [[nodiscard]] bool parse_array(Json& out, std::size_t depth) {
    out.kind = Json::Kind::Array;
    ++pos_;  // consume '['
    skip_ws();
    if (pos_ < size_ && data_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      Json value;
      if (!parse_value(value, depth + 1)) return false;
      out.items.push_back(std::move(value));
      skip_ws();
      if (pos_ >= size_) return fail("unterminated array");
      if (data_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (data_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  [[nodiscard]] bool parse_string_value(Json& out) {
    std::string value;
    if (!parse_string(value)) return false;
    out.kind = Json::Kind::String;
    out.string = std::move(value);
    return true;
  }

  //! Parse a quoted string starting at data_[pos_]. Advances past the closing
  //! quote. Handles every standard escape including \uXXXX with surrogate
  //! pairs, producing UTF-8.
  [[nodiscard]] bool parse_string(std::string& out) {
    if (pos_ >= size_ || data_[pos_] != '"') return fail("expected string");
    ++pos_;
    out.clear();
    out.reserve(16);
    while (pos_ < size_) {
      const unsigned char c = static_cast<unsigned char>(data_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c < 0x20) return fail("unescaped control character in string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++pos_;
        continue;
      }
      // Escape sequence.
      ++pos_;
      if (pos_ >= size_) return fail("unterminated escape");
      const char esc = data_[pos_];
      ++pos_;
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t codepoint = 0;
          if (!read_hex4(codepoint)) return fail("invalid \\u escape");
          if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
            // High surrogate: a low surrogate must follow.
            if (pos_ + 2 > size_ || data_[pos_] != '\\' ||
                data_[pos_ + 1] != 'u') {
              return fail("unpaired surrogate in string");
            }
            pos_ += 2;
            std::uint32_t low = 0;
            if (!read_hex4(low) || low < 0xDC00U || low > 0xDFFFU) {
              return fail("invalid low surrogate in string");
            }
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                        (low - 0xDC00U);
          } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
            return fail("unpaired low surrogate in string");
          }
          append_utf8(out, codepoint);
          break;
        }
        default:
          return fail("invalid escape sequence");
      }
    }
    return fail("unterminated string");
  }

  [[nodiscard]] bool read_hex4(std::uint32_t& out) {
    if (pos_ + 4 > size_) return false;
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = data_[pos_ + static_cast<std::size_t>(i)];
      value <<= 4U;
      if (h >= '0' && h <= '9') {
        value |= static_cast<std::uint32_t>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        value |= static_cast<std::uint32_t>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        value |= static_cast<std::uint32_t>(h - 'A' + 10);
      } else {
        return false;
      }
    }
    pos_ += 4;
    out = value;
    return true;
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7FU) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
      out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
      out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
  }

  //! Grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  [[nodiscard]] bool parse_number(Json& out) {
    const std::size_t start = pos_;
    if (pos_ < size_ && data_[pos_] == '-') ++pos_;
    if (pos_ >= size_) return fail("truncated number");
    if (data_[pos_] == '0') {
      ++pos_;
      // Leading zeros are illegal: 01, -01.
      if (pos_ < size_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
        return fail("leading zero in number");
      }
    } else if (data_[pos_] >= '1' && data_[pos_] <= '9') {
      while (pos_ < size_ && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
    } else {
      return fail("invalid number");
    }
    bool is_real = false;
    if (pos_ < size_ && data_[pos_] == '.') {
      is_real = true;
      ++pos_;
      if (pos_ >= size_ || data_[pos_] < '0' || data_[pos_] > '9') {
        return fail("malformed fraction in number");
      }
      while (pos_ < size_ && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
    }
    if (pos_ < size_ && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
      is_real = true;
      ++pos_;
      if (pos_ < size_ && (data_[pos_] == '+' || data_[pos_] == '-')) ++pos_;
      if (pos_ >= size_ || data_[pos_] < '0' || data_[pos_] > '9') {
        return fail("malformed exponent in number");
      }
      while (pos_ < size_ && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
    }
    const char* first = data_ + start;
    const char* last = data_ + pos_;
    if (!is_real) {
      std::int64_t value = 0;
      const auto result = std::from_chars(first, last, value);
      if (result.ec == std::errc{} && result.ptr == last) {
        out.kind = Json::Kind::Int;
        out.integer = value;
        return true;
      }
      // Overflowing integers fall through to double so large literals in
      // unused fields never hard-fail the document.
      is_real = true;
    }
    double value = 0.0;
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
      return fail("number out of range");
    }
    out.kind = Json::Kind::Real;
    out.real = value;
    return true;
  }
};

// ============================================================================
// JSON access helpers
// ============================================================================

inline const Json* find_member(const Json& object, const char* key) {
  if (object.kind != Json::Kind::Object) return nullptr;
  const std::string_view wanted{key};
  for (const auto& member : object.members) {
    if (member->key == wanted) return &member->value;
  }
  return nullptr;
}

inline bool fail_asset(std::string& error, const std::string& message) {
  if (error.empty()) error = message;
  return false;
}

//! Integer-typed JSON value (spec integers like indices, counts, modes).
inline bool as_int(const Json& value, std::int64_t& out,
                   std::string& error, const std::string& context) {
  if (value.kind == Json::Kind::Int) {
    out = value.integer;
    return true;
  }
  return fail_asset(error, context + " must be an integer");
}

//! Real-typed JSON value (spec floats like baseColorFactor).
inline bool as_real(const Json& value, double& out, std::string& error,
                    const std::string& context) {
  if (value.kind == Json::Kind::Int) {
    out = static_cast<double>(value.integer);
    return true;
  }
  if (value.kind == Json::Kind::Real) {
    out = value.real;
    return true;
  }
  return fail_asset(error, context + " must be a number");
}

//! Optional unsigned member with default.
inline bool member_uint(const Json& object, const char* key,
                        std::size_t default_value, std::size_t& out,
                        std::string& error, const std::string& context,
                        bool& present) {
  present = false;
  const Json* member = find_member(object, key);
  if (member == nullptr) {
    out = default_value;
    return true;
  }
  present = true;
  std::int64_t raw = 0;
  if (!as_int(*member, raw, error, context + "." + key)) return false;
  if (raw < 0) return fail_asset(error, context + "." + key + " is negative");
  out = static_cast<std::size_t>(raw);
  return true;
}

// ============================================================================
// Base64 (glTF data URIs, RFC 4648 with padding)
// ============================================================================

inline int base64_value(unsigned char c) {
  // Integer promotion of `unsigned char` yields int, so no cast is needed.
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

//! Decode a base64 payload (padding optional but accepted). Returns false on
//! any invalid character or on trailing bits that are not all zero.
inline bool decode_base64(std::string_view text,
                          std::vector<std::uint8_t>& out,
                          std::string& error) {
  out.clear();
  out.reserve((text.size() / 4U) * 3U);
  std::uint32_t accumulator = 0;
  int bits = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '=') {
      // Padding is only valid at the very end.
      for (std::size_t j = i + 1; j < text.size(); ++j) {
        if (text[j] != '=') return fail_asset(error, "invalid base64 padding");
      }
      break;
    }
    const int value = base64_value(static_cast<unsigned char>(c));
    if (value < 0) return fail_asset(error, "invalid base64 character");
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xFFU));
    }
  }
  if (bits != 0 && ((accumulator & ((1U << bits) - 1U)) != 0U)) {
    return fail_asset(error, "non-zero base64 trailing bits");
  }
  return true;
}

// ============================================================================
// glTF 2.0 decoding
// ============================================================================

constexpr std::int64_t kComponentByte = 5120;
constexpr std::int64_t kComponentUByte = 5121;
constexpr std::int64_t kComponentShort = 5122;
constexpr std::int64_t kComponentUShort = 5123;
constexpr std::int64_t kComponentUInt = 5125;
constexpr std::int64_t kComponentFloat = 5126;
constexpr std::int64_t kPrimitiveTriangles = 4;

//! glTF type string -> element component count (1 for SCALAR .. 16 for MAT4).
inline bool type_components(const std::string& type,
                            std::size_t& components,
                            std::string& error,
                            const std::string& context) {
  if (type == "SCALAR") components = 1;
  else if (type == "VEC2") components = 2;
  else if (type == "VEC3") components = 3;
  else if (type == "VEC4") components = 4;
  else if (type == "MAT2") components = 4;
  else if (type == "MAT3") components = 9;
  else if (type == "MAT4") components = 16;
  else {
    return fail_asset(error, context + " has unknown type \"" + type + "\"");
  }
  return true;
}

struct BufferSource {
  const std::uint8_t* data{nullptr};
  std::size_t size{0};
};

struct View {
  std::size_t buffer_index{0};
  std::size_t byte_offset{0};
  std::size_t byte_length{0};
  std::size_t byte_stride{0};
  bool has_stride{false};
};

struct AccessorInfo {
  bool has_view{false};
  std::size_t view_index{0};
  std::size_t byte_offset{0};
  std::size_t count{0};
  std::int64_t component_type{0};
  std::size_t component_size{0};  // bytes per scalar component
  std::size_t components{1};      // scalars per element
};

//! Parse one accessor. `buffer_views` and `buffers` are validated while
//! building `sources`/`views`, so lookups here can trust index bounds.
inline bool parse_accessor(const Json& accessor_json,
                           const std::vector<View>& views,
                           AccessorInfo& out, std::string& error,
                           const std::string& context) {
  if (find_member(accessor_json, "sparse") != nullptr) {
    return fail_asset(error, context + ": sparse accessors are not supported");
  }
  std::int64_t component_type = 0;
  const Json* component_value = find_member(accessor_json, "componentType");
  if (component_value == nullptr) {
    return fail_asset(error, context + " is missing componentType");
  }
  if (!as_int(*component_value, component_type, error,
              context + ".componentType")) {
    return false;
  }
  std::int64_t count_value = 0;
  const Json* count_member = find_member(accessor_json, "count");
  if (count_member == nullptr) {
    return fail_asset(error, context + " is missing count");
  }
  if (!as_int(*count_member, count_value, error, context + ".count")) {
    return false;
  }
  if (count_value < 0) {
    return fail_asset(error, context + ".count is negative");
  }
  const Json* type_member = find_member(accessor_json, "type");
  if (type_member == nullptr || type_member->kind != Json::Kind::String) {
    return fail_asset(error, context + " is missing type");
  }
  std::size_t components = 0;
  if (!type_components(type_member->string, components, error, context)) {
    return false;
  }
  out.count = static_cast<std::size_t>(count_value);
  out.components = components;
  out.component_type = component_type;

  bool offset_present = false;
  if (!member_uint(accessor_json, "byteOffset", 0, out.byte_offset, error,
                   context, offset_present)) {
    return false;
  }
  const Json* view_member = find_member(accessor_json, "bufferView");
  if (view_member != nullptr) {
    std::int64_t view_index = 0;
    if (!as_int(*view_member, view_index, error, context + ".bufferView")) {
      return false;
    }
    if (view_index < 0 ||
        static_cast<std::size_t>(view_index) >= views.size()) {
      return fail_asset(error, context + ".bufferView is out of range");
    }
    out.has_view = true;
    out.view_index = static_cast<std::size_t>(view_index);
  }
  switch (component_type) {
    case kComponentUByte:
      out.component_size = 1;
      break;
    case kComponentUShort:
      out.component_size = 2;
      break;
    case kComponentUInt:
      out.component_size = 4;
      break;
    case kComponentFloat:
      out.component_size = 4;
      break;
    default:
      return fail_asset(
          error, context + ": componentType " +
                     std::to_string(component_type) + " is not supported");
  }
  return true;
}

//! Read a little-endian unsigned integer of `size` bytes at `data`.
inline std::uint64_t read_le_unsigned(const std::uint8_t* data,
                                      std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i) {
    value |= static_cast<std::uint64_t>(data[i]) << (i * 8U);
  }
  return value;
}

inline bool decode_data_uri(std::string_view uri, BufferSource& out,
                            std::vector<std::vector<std::uint8_t>>&
                                embedded_storage,
                            std::string& error) {
  constexpr std::string_view kPrefix = "data:";
  if (uri.size() < kPrefix.size() ||
      uri.substr(0, kPrefix.size()) != kPrefix) {
    return fail_asset(error, "unsupported buffer URI (only data: and the "
                             "single external buffer are supported)");
  }
  const std::size_t comma = uri.find(',');
  if (comma == std::string_view::npos) {
    return fail_asset(error, "malformed data: URI");
  }
  const std::string_view meta = uri.substr(5, comma - 5);
  const std::size_t base64_pos = meta.find(";base64");
  if (base64_pos == std::string_view::npos) {
    return fail_asset(error, "only base64 data: URIs are supported");
  }
  embedded_storage.emplace_back();
  if (!decode_base64(uri.substr(comma + 1), embedded_storage.back(), error)) {
    return false;
  }
  out.data = embedded_storage.back().data();
  out.size = embedded_storage.back().size();
  return true;
}

//! out = a * b for column-major 4x4 matrices.
inline void mat_mul(const float a[16], const float b[16], float out[16]) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a[r + 4 * k] * b[k + 4 * c];
      }
      out[r + 4 * c] = sum;
    }
  }
}

//! Compose local = translation * rotation(quaternion xyzw) * scale into
//! a column-major matrix.
inline void trs_matrix(const float* translation, const float* quaternion,
                const float* scale, float out[16]) {
  float r[16]{};
  r[0] = 1.0f;
  r[5] = 1.0f;
  r[10] = 1.0f;
  r[15] = 1.0f;
  if (quaternion != nullptr) {
    const float x = quaternion[0];
    const float y = quaternion[1];
    const float z = quaternion[2];
    const float w = quaternion[3];
    r[0] = 1.0f - 2.0f * (y * y + z * z);
    r[1] = 2.0f * (x * y + z * w);
    r[2] = 2.0f * (x * z - y * w);
    r[4] = 2.0f * (x * y - z * w);
    r[5] = 1.0f - 2.0f * (x * x + z * z);
    r[6] = 2.0f * (y * z + x * w);
    r[8] = 2.0f * (x * z + y * w);
    r[9] = 2.0f * (y * z - x * w);
    r[10] = 1.0f - 2.0f * (x * x + y * y);
  }
  float rs[16]{};
  if (scale != nullptr) {
    float s[16]{};
    s[0] = scale[0];
    s[5] = scale[1];
    s[10] = scale[2];
    s[15] = 1.0f;
    mat_mul(r, s, rs);
  } else {
    for (int i = 0; i < 16; ++i) rs[i] = r[i];
  }
  if (translation != nullptr) {
    for (int i = 0; i < 16; ++i) out[i] = rs[i];
    out[12] = rs[12] + translation[0];
    out[13] = rs[13] + translation[1];
    out[14] = rs[14] + translation[2];
  } else {
    for (int i = 0; i < 16; ++i) out[i] = rs[i];
  }
}

//! Parsed GLB container: the JSON chunk as a string and a view of the
//! optional BIN chunk (pointing into the caller's buffer, which must
//! outlive the import).
struct GlbContents {
  std::string json{};
  const std::uint8_t* bin{nullptr};
  std::size_t bin_size{0};
};

//! Parse the GLB 2.0 container (magic "glTF", version 2): a JSON chunk
//! followed by an optional BIN chunk. Chunk lengths are validated against
//! the container; padding bytes are not interpreted. Returns false with
//! `error` set on any malformed container.
[[nodiscard]] inline bool parse_glb(const std::uint8_t* data,
                                    std::size_t size, GlbContents& out,
                                    std::string& error) {
  constexpr std::uint32_t kGlbMagic = 0x46546C67U;    // "glTF"
  constexpr std::uint32_t kChunkJson = 0x4E4F534AU;   // "JSON"
  constexpr std::uint32_t kChunkBin = 0x004E4942U;    // "BIN\0"
  if (data == nullptr || size < 12U) {
    return fail_asset(error, "GLB container is smaller than its header");
  }
  std::uint32_t magic = 0;
  std::memcpy(&magic, data, 4);
  if (magic != kGlbMagic) {
    return fail_asset(error, "GLB container magic is not \"glTF\"");
  }
  std::uint32_t version = 0;
  std::memcpy(&version, data + 4, 4);
  if (version != 2U) {
    return fail_asset(error, "GLB container version must be 2");
  }
  std::uint32_t declared_length = 0;
  std::memcpy(&declared_length, data + 8, 4);
  if (declared_length < 12U || declared_length > size) {
    return fail_asset(error, "GLB header length exceeds the data size");
  }
  std::size_t cursor = 12U;
  bool json_seen = false;
  while (cursor < declared_length) {
    if (declared_length - cursor < 8U) {
      return fail_asset(error, "GLB chunk header is truncated");
    }
    std::uint32_t chunk_length = 0;
    std::uint32_t chunk_type = 0;
    std::memcpy(&chunk_length, data + cursor, 4);
    std::memcpy(&chunk_type, data + cursor + 4, 4);
    cursor += 8U;
    if (chunk_length > declared_length - cursor) {
      return fail_asset(error, "GLB chunk length exceeds the container");
    }
    if (chunk_type == kChunkJson) {
      if (json_seen) {
        return fail_asset(error, "GLB has more than one JSON chunk");
      }
      json_seen = true;
      out.json.assign(reinterpret_cast<const char*>(data + cursor),
                      chunk_length);
    } else if (chunk_type == kChunkBin) {
      if (out.bin != nullptr) {
        return fail_asset(error, "GLB has more than one BIN chunk");
      }
      out.bin = data + cursor;
      out.bin_size = chunk_length;
    } else {
      return fail_asset(error, "GLB contains an unknown chunk type");
    }
    cursor += chunk_length;
  }
  if (!json_seen) {
    return fail_asset(error, "GLB is missing its JSON chunk");
  }
  return true;
}

//! Common prologue result: the JSON text to parse (owned by the caller via
//! `container_json` when the input was a GLB), the effective BIN payload,
//! and whether the document may reference buffer 0 without a uri.
struct DocumentPrologue {
  const char* json_bytes{nullptr};
  std::size_t json_len{0};
  const std::uint8_t* bin_bytes{nullptr};
  std::size_t bin_len{0};
  bool allow_uriless_buffer0{false};
};

//! Auto-detect the container: raw JSON (starts with a non-GLB byte) or a GLB
//! 2.0 container (magic "glTF"), whose JSON chunk replaces the input text and
//! whose BIN chunk (if any) becomes the effective buffer-0 payload. Returns
//! false with `error` set on malformed GLB containers. `container_json` must
//! outlive the import (it owns the GLB JSON chunk text).
[[nodiscard]] inline bool parse_gltf_document_prologue(
    const char* json_bytes, std::size_t json_len,
    const std::uint8_t* bin_bytes, std::size_t bin_len,
    std::string& container_json, DocumentPrologue& out, std::string& error) {
  constexpr std::uint32_t kGlbMagic = 0x46546C67U;  // "glTF"
  if (json_bytes == nullptr || json_len < 4U) {
    return fail_asset(error, "empty glTF document");
  }
  // JSON documents must start with '{'; anything else is either a GLB
  // container (magic "glTF") or garbage worth a precise diagnostic.
  constexpr char kJsonStart = '{';
  std::uint32_t magic = 0;
  std::memcpy(&magic, json_bytes, 4);
  if (magic != kGlbMagic && json_bytes[0] == kJsonStart) {
    out.json_bytes = json_bytes;
    out.json_len = json_len;
    out.bin_bytes = bin_bytes;
    out.bin_len = bin_len;
    out.allow_uriless_buffer0 = false;
    return true;
  }
  if (magic != kGlbMagic) {
    return fail_asset(
        error, "input is neither a JSON glTF document (must start with '{') "
               "nor a GLB container (magic \"glTF\")");
  }
  GlbContents glb{};
  if (!parse_glb(reinterpret_cast<const std::uint8_t*>(json_bytes), json_len,
                 glb, error)) {
    return false;
  }
  container_json = std::move(glb.json);
  out.json_bytes = container_json.data();
  out.json_len = container_json.size();
  out.bin_bytes = glb.bin != nullptr ? glb.bin : bin_bytes;
  out.bin_len = glb.bin != nullptr ? glb.bin_size : bin_len;
  out.allow_uriless_buffer0 = true;
  return true;
}

//! Shared prologue of every glTF importer: decodes the `buffers` array
//! (external buffer 0 is served from bin_bytes / bin_len; others must be
//! data: URIs), the `bufferViews` array (ranges proven in-bounds, strides
//! validated), and the `accessors` array (component types restricted to the
//! supported subset, sparse accessors rejected). Returns false with `error`
//! set on any malformed input; on success the out-vectors are filled.
//! LIFETIME: `sources` holds pointers into `embedded_storage`, which must
//! therefore be owned by (and outlive) the caller's import scope.
[[nodiscard]] inline bool parse_gltf_buffers_views_accessors(
    const Json& document, const std::uint8_t* bin_bytes,
    std::size_t bin_len, std::vector<BufferSource>& sources,
    std::vector<View>& views, std::vector<AccessorInfo>& accessors,
    std::vector<std::vector<std::uint8_t>>& embedded_storage,
    std::string& error, bool allow_uriless_buffer0 = false) {
  const Json* buffers = find_member(document, "buffers");
  const Json* views_json = find_member(document, "bufferViews");
  const Json* accessors_json = find_member(document, "accessors");
  if (buffers == nullptr || buffers->kind != Json::Kind::Array) {
    return fail_asset(error, "document is missing buffers array");
  }
  if (views_json == nullptr || views_json->kind != Json::Kind::Array) {
    return fail_asset(error, "document is missing bufferViews array");
  }
  if (accessors_json == nullptr ||
      accessors_json->kind != Json::Kind::Array) {
    return fail_asset(error, "document is missing accessors array");
  }

  // ------------------------------------------------------------------ buffers
  sources.reserve(buffers->items.size());
  embedded_storage.reserve(buffers->items.size());
  for (std::size_t i = 0; i < buffers->items.size(); ++i) {
    const Json& buffer_json = buffers->items[i];
    std::size_t byte_length = 0;
    bool length_present = false;
    if (!member_uint(buffer_json, "byteLength", 0, byte_length, error,
                     "buffers[" + std::to_string(i) + "]", length_present)) {
      return false;
    }
    if (!length_present) {
      return fail_asset(
          error, "buffers[" + std::to_string(i) + "] is missing byteLength");
    }
    const Json* uri_member = find_member(buffer_json, "uri");
    BufferSource source{};
    if (uri_member == nullptr) {
      // GLB containers carry buffer 0 in the BIN chunk; callers pass the
      // BIN view as bin_bytes / bin_len and set allow_uriless_buffer0.
      if (allow_uriless_buffer0 && i == 0U) {
        source.data = bin_bytes;
        source.size = bin_len;
        if (source.size != byte_length) {
          return fail_asset(
              error, "GLB BIN chunk size " + std::to_string(source.size) +
                         " does not match buffers[0].byteLength " +
                         std::to_string(byte_length));
        }
        sources.push_back(source);
        continue;
      }
      return fail_asset(
          error, "buffers[" + std::to_string(i) +
                     "] has no uri (GLB-style buffers are only supported "
                     "through the *_glb entry points)");
    }
    if (uri_member->kind != Json::Kind::String) {
      return fail_asset(error, "buffers[" + std::to_string(i) +
                                   "].uri must be a string");
    }
    const std::string_view uri = uri_member->string;
    if (uri.size() >= 5U && uri.substr(0, 5U) == "data:") {
      if (!decode_data_uri(uri, source, embedded_storage, error)) {
        return false;
      }
    } else {
      // External file: only buffer 0 may be external, served from the
      // caller-provided bytes.
      if (i != 0U) {
        return fail_asset(
            error, "buffers[" + std::to_string(i) +
                       "] is external; only the first external buffer is "
                       "supported (embed with data: URIs instead)");
      }
      source.data = bin_bytes;
      source.size = bin_len;
    }
    if (source.size != byte_length) {
      return fail_asset(
          error, "buffers[" + std::to_string(i) + "] data length " +
                     std::to_string(source.size) +
                     " does not match declared byteLength " +
                     std::to_string(byte_length));
    }
    sources.push_back(source);
  }

  // ------------------------------------------------------------ bufferViews
  views.reserve(views_json->items.size());
  for (std::size_t i = 0; i < views_json->items.size(); ++i) {
    const Json& view_json = views_json->items[i];
    View view;
    std::size_t buffer_index = 0;
    bool buffer_present = false;
    bool offset_present = false;
    bool length_present = false;
    if (!member_uint(view_json, "buffer", 0, buffer_index, error,
                     "bufferViews[" + std::to_string(i) + "]",
                     buffer_present)) {
      return false;
    }
    if (!buffer_present) {
      return fail_asset(
          error,
          "bufferViews[" + std::to_string(i) + "] is missing buffer");
    }
    if (!member_uint(view_json, "byteOffset", 0, view.byte_offset, error,
                     "bufferViews[" + std::to_string(i) + "]",
                     offset_present) ||
        !member_uint(view_json, "byteLength", 0, view.byte_length, error,
                     "bufferViews[" + std::to_string(i) + "]",
                     length_present)) {
      return false;
    }
    if (!length_present) {
      return fail_asset(
          error, "bufferViews[" + std::to_string(i) +
                     "] is missing byteLength");
    }
    if (buffer_index >= sources.size()) {
      return fail_asset(
          error, "bufferViews[" + std::to_string(i) + "].buffer " +
                     std::to_string(buffer_index) +
                     " is out of range (declared " +
                     std::to_string(sources.size()) + " buffers)");
    }
    bool stride_present = false;
    std::size_t stride = 0;
    if (!member_uint(view_json, "byteStride", 0, stride, error,
                     "bufferViews[" + std::to_string(i) + "]", stride_present)) {
      return false;
    }
    view.buffer_index = buffer_index;
    view.has_stride = stride_present;
    view.byte_stride = stride;
    if (stride_present &&
        (stride < 4U || stride % 4U != 0U)) {
      return fail_asset(
          error, "bufferViews[" + std::to_string(i) +
                     "].byteStride must be a multiple of 4 (was " +
                     std::to_string(stride) + ")");
    }
    // Whole-view range must fit inside the referenced buffer.
    if (view.byte_offset > sources[buffer_index].size ||
        view.byte_length > sources[buffer_index].size - view.byte_offset) {
      return fail_asset(
          error, "bufferViews[" + std::to_string(i) +
                     "] range exceeds buffers[" + std::to_string(buffer_index) +
                     "] (size " +
                     std::to_string(sources[buffer_index].size) + ")");
    }
    views.push_back(view);
  }

  // ------------------------------------------------------------- accessors
  accessors.reserve(accessors_json->items.size());
  for (std::size_t i = 0; i < accessors_json->items.size(); ++i) {
    AccessorInfo info;
    if (!parse_accessor(accessors_json->items[i], views, info, error,
                        "accessors[" + std::to_string(i) + "]")) {
      return false;
    }
    accessors.push_back(info);
  }
  return true;
}

}  // namespace omnicpp::asset::gltf_detail
