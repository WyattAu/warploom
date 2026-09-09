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
  std::vector<std::pair<std::string, Json>> members{};  // Object
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
      out.members.emplace_back(std::move(key), std::move(value));
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
    if (member.first == wanted) return &member.second;
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

}  // namespace omnicpp::asset::gltf_detail
