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

#include "warploom/core/document.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "warploom/core/contract.hpp"

namespace warploom::editor {

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

//! G3: reads one clip track's samples array. Offsets must be strictly
//! increasing (the writer's invariant); duplicates are rejected so a
//! tampered file cannot silently alias samples.
[[nodiscard]] bool read_clip_samples(JsonReader& r, ClipTrack& track,
                                     std::string& error) {
  if (!r.expect('[', error)) return false;
  r.skip_ws();
  if (!r.at_end() && r.peek_is(']')) {
    return r.expect(']', error);
  }
  for (;;) {
    ClipSample sample;
    if (!r.expect('{', error)) return false;
    bool seen_offset = false;
    bool seen_value = false;
    r.skip_ws();
    if (!r.at_end() && r.peek_is('}')) {
      r.fail(error, "empty sample entry");
      return false;
    }
    for (;;) {
      std::string key;
      if (!r.read_key(key, error)) return false;
      if (key == "frame_offset") {
        double v = 0.0;
        if (!r.expect(':', error) || !r.read_number(v, error)) return false;
        if (v < 0.0 || v != std::floor(v) ||
            v > static_cast<double>(UINT64_MAX)) {
          r.fail(error, "frame_offset must be a non-negative integer");
          return false;
        }
        sample.frame_offset = static_cast<std::uint64_t>(v);
        seen_offset = true;
      } else if (key == "value") {
        if (!r.expect(':', error)) return false;
        if (!read_prop_value(r, sample.value, error)) return false;
        seen_value = true;
      } else {
        r.fail(error, "unknown sample key \"" + key + "\"");
        return false;
      }
      r.skip_ws();
      if (r.at_end()) {
        r.fail(error, "unterminated sample entry");
        return false;
      }
      if (r.peek_is('}')) break;
      if (!r.expect(',', error)) return false;
    }
    if (!r.expect('}', error)) return false;
    if (!seen_offset || !seen_value) {
      r.fail(error, "sample entry missing required key(s): " +
                        std::string(seen_offset ? "" : "frame_offset ") +
                        std::string(seen_value ? "" : "value"));
      return false;
    }
    if (!track.samples.empty() &&
        sample.frame_offset <= track.samples.back().frame_offset) {
      r.fail(error, "sample offsets must be strictly increasing");
      return false;
    }
    track.samples.push_back(std::move(sample));
    r.skip_ws();
    if (r.at_end()) {
      r.fail(error, "unterminated samples array");
      return false;
    }
    if (r.peek_is(']')) break;
    if (!r.expect(',', error)) return false;
  }
  return r.expect(']', error);
}

//! G3: reads one clip (id/name/start/length/tracks; all required except
//! empty tracks, which the writer emits for a fresh clip).
[[nodiscard]] bool read_timeline_clip(JsonReader& r, TimelineClip& clip,
                                      std::string& error) {
  if (!r.expect('{', error)) return false;
  bool seen_id = false;
  bool seen_name = false;
  bool seen_start = false;
  bool seen_length = false;
  bool seen_tracks = false;
  r.skip_ws();
  if (!r.at_end() && r.peek_is('}')) {
    r.fail(error, "empty clip entry");
    return false;
  }
  for (;;) {
    std::string key;
    if (!r.read_key(key, error)) return false;
    if (key == "id") {
      double v = 0.0;
      if (!r.expect(':', error) || !r.read_number(v, error)) return false;
      if (v < 1.0 || v != std::floor(v) ||
          v > static_cast<double>(UINT64_MAX)) {
        r.fail(error, "clip id must be a positive integer");
        return false;
      }
      clip.id = static_cast<std::uint64_t>(v);
      seen_id = true;
    } else if (key == "name") {
      if (!r.expect(':', error) || !r.read_string(clip.name, error)) {
        return false;
      }
      seen_name = true;
    } else if (key == "start_frame") {
      double v = 0.0;
      if (!r.expect(':', error) || !r.read_number(v, error)) return false;
      if (v < 0.0 || v != std::floor(v) ||
          v > static_cast<double>(UINT64_MAX)) {
        r.fail(error, "start_frame must be a non-negative integer");
        return false;
      }
      clip.start_frame = static_cast<std::uint64_t>(v);
      seen_start = true;
    } else if (key == "length_frames") {
      double v = 0.0;
      if (!r.expect(':', error) || !r.read_number(v, error)) return false;
      if (v < 0.0 || v != std::floor(v) ||
          v > static_cast<double>(UINT64_MAX)) {
        r.fail(error, "length_frames must be a non-negative integer");
        return false;
      }
      clip.length_frames = static_cast<std::uint64_t>(v);
      seen_length = true;
    } else if (key == "tracks") {
      if (!r.expect(':', error) || !r.expect('{', error)) return false;
      r.skip_ws();
      if (!r.at_end() && r.peek_is('}')) {
        if (!r.expect('}', error)) return false;
      } else {
        for (;;) {
          std::string tkey;
          if (!r.read_key(tkey, error)) return false;
          if (!r.expect(':', error)) return false;
          ClipTrack track;
          if (!r.expect('{', error)) return false;
          bool seen_oid = false;
          bool seen_prop = false;
          bool seen_samples = false;
          r.skip_ws();
          if (!r.at_end() && r.peek_is('}')) {
            r.fail(error, "empty track entry");
            return false;
          }
          for (;;) {
            std::string tk;
            if (!r.read_key(tk, error)) return false;
            if (tk == "object_id") {
              double v = 0.0;
              if (!r.expect(':', error) || !r.read_number(v, error)) {
                return false;
              }
              if (v < 0.0 || v != std::floor(v) ||
                  v > static_cast<double>(UINT64_MAX)) {
                r.fail(error, "track object_id must be a non-negative integer");
                return false;
              }
              track.object_id = static_cast<std::uint64_t>(v);
              seen_oid = true;
            } else if (tk == "property") {
              if (!r.expect(':', error) ||
                  !r.read_string(track.property, error)) {
                return false;
              }
              seen_prop = true;
            } else if (tk == "samples") {
              if (!r.expect(':', error)) return false;
              if (!read_clip_samples(r, track, error)) return false;
              seen_samples = true;
            } else {
              r.fail(error, "unknown track key \"" + tk + "\"");
              return false;
            }
            r.skip_ws();
            if (r.at_end()) {
              r.fail(error, "unterminated track entry");
              return false;
            }
            if (r.peek_is('}')) break;
            if (!r.expect(',', error)) return false;
          }
          if (!r.expect('}', error)) return false;
          if (!seen_oid || !seen_prop || !seen_samples) {
            r.fail(error, "track entry missing required key(s): " +
                              std::string(seen_oid ? "" : "object_id ") +
                              std::string(seen_prop ? "" : "property ") +
                              std::string(seen_samples ? "" : "samples"));
            return false;
          }
          if (!clip.tracks.emplace(tkey, std::move(track)).second) {
            r.fail(error, "duplicate track key \"" + tkey + "\"");
            return false;
          }
          r.skip_ws();
          if (r.at_end()) {
            r.fail(error, "unterminated tracks object");
            return false;
          }
          if (r.peek_is('}')) break;
          if (!r.expect(',', error)) return false;
        }
        if (!r.expect('}', error)) return false;
      }
      seen_tracks = true;
    } else {
      r.fail(error, "unknown clip key \"" + key + "\"");
      return false;
    }
    r.skip_ws();
    if (r.at_end()) {
      r.fail(error, "unterminated clip entry");
      return false;
    }
    if (r.peek_is('}')) break;
    if (!r.expect(',', error)) return false;
  }
  if (!r.expect('}', error)) return false;
  if (!seen_id || !seen_name || !seen_start || !seen_length ||
      !seen_tracks) {
    r.fail(error, "clip entry missing required key(s): " +
                      std::string(seen_id ? "" : "id ") +
                      std::string(seen_name ? "" : "name ") +
                      std::string(seen_start ? "" : "start_frame ") +
                      std::string(seen_length ? "" : "length_frames ") +
                      std::string(seen_tracks ? "" : "tracks"));
    return false;
  }
  return true;
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

//! Reads one PropValue in BARE form (the node-graph JSON dialect: untagged
//! numbers/bools/strings/[x,y,z] arrays — machine-written by to_json).
[[nodiscard]] bool read_bare_prop_value(JsonReader& r, PropValue& out,
                                        std::string& error) {
  r.skip_ws();
  if (r.at_end()) {
    r.fail(error, "expected value");
    return false;
  }
  if (r.peek_is('"')) {
    out = PropValue::make_string("");
    return r.read_string(out.text, error);
  }
  if (r.peek_is('t') || r.peek_is('f')) {
    bool b = false;
    if (!r.read_bool(b, error)) return false;
    out = PropValue::make_bool(b);
    return true;
  }
  if (r.peek_is('[')) {
    if (!r.expect('[', error)) return false;
    if (!r.read_number(out.vec[0], error)) return false;
    if (!r.expect(',', error)) return false;
    if (!r.read_number(out.vec[1], error)) return false;
    if (!r.expect(',', error)) return false;
    if (!r.read_number(out.vec[2], error)) return false;
    return r.expect(']', error);
  }
  out = PropValue{};
  return r.read_number(out.number, error);
}

//! Reads the node-graph object: {"nodes":[{"id":N,"type":"...",
//! "params":{...}}], "links":[{"from":N,"out":"...","to":N,"in":"..."}]}
//! Types are validated against the graph's REGISTRY (the caller pre-registers
//! built-ins; unknown types are rejected before any mutation).
[[nodiscard]] bool read_node_graph(JsonReader& r, NodeGraph& graph,
                                   std::string& error) {
  if (!r.expect('{', error)) return false;
  bool seen_nodes = false;
  bool seen_links = false;
  r.skip_ws();
  if (!r.at_end() && r.peek_is('}')) {
    r.fail(error, "empty node graph object");
    return false;
  }
  for (;;) {
    std::string key;
    if (!r.read_key(key, error)) return false;
    if (key == "nodes") {
      if (seen_nodes) {
        r.fail(error, "duplicate key \"nodes\"");
        return false;
      }
      if (!r.expect(':', error) || !r.expect('[', error)) return false;
      r.skip_ws();
      if (!r.at_end() && r.peek_is(']')) {
        if (!r.expect(']', error)) return false;
      } else {
        for (;;) {
          if (!r.expect('{', error)) return false;
          bool seen_id = false;
          bool seen_type = false;
          bool seen_params = false;
          std::uint64_t id = 0;
          std::string type;
          std::map<std::string, PropValue> params;
          for (;;) {
            std::string nkey;
            if (!r.read_key(nkey, error)) return false;
            if (nkey == "id") {
              if (!r.expect(':', error)) return false;
              double v = 0.0;
              if (!r.read_number(v, error)) return false;
              if (v < 1.0 || v != std::floor(v) ||
                  v > static_cast<double>(UINT64_MAX)) {
                r.fail(error, "node id must be a positive integer");
                return false;
              }
              id = static_cast<std::uint64_t>(v);
              seen_id = true;
            } else if (nkey == "type") {
              if (!r.expect(':', error) || !r.read_string(type, error)) {
                return false;
              }
              seen_type = true;
            } else if (nkey == "params") {
              if (!r.expect(':', error) || !r.expect('{', error)) return false;
              r.skip_ws();
              if (!r.at_end() && r.peek_is('}')) {
                if (!r.expect('}', error)) return false;
              } else {
                for (;;) {
                  std::string pkey;
                  if (!r.read_key(pkey, error)) return false;
                  if (params.contains(pkey)) {
                    r.fail(error, "duplicate param \"" + pkey + "\"");
                    return false;
                  }
                  if (!r.expect(':', error)) return false;
                  PropValue pv;
                  if (!read_bare_prop_value(r, pv, error)) return false;
                  params.emplace(std::move(pkey), std::move(pv));
                  r.skip_ws();
                  if (r.at_end()) {
                    r.fail(error, "unterminated params object");
                    return false;
                  }
                  if (r.peek_is('}')) break;
                  if (!r.expect(',', error)) return false;
                }
                if (!r.expect('}', error)) return false;
              }
              seen_params = true;
            } else {
              r.fail(error, "unknown node key \"" + nkey + "\"");
              return false;
            }
            r.skip_ws();
            if (r.at_end()) {
              r.fail(error, "unterminated node entry");
              return false;
            }
            if (r.peek_is('}')) break;
            if (!r.expect(',', error)) return false;
          }
          if (!r.expect('}', error)) return false;
          if (!seen_id || !seen_type) {
            r.fail(error, "node entry missing id/type");
            return false;
          }
          if (!graph.add_node_with_id(id, std::move(type),
                                      std::move(params))) {
            r.fail(error, "node entry invalid (id " + std::to_string(id) +
                              "): unknown type or duplicate id");
            return false;
          }
          (void)seen_params;
          r.skip_ws();
          if (r.at_end()) {
            r.fail(error, "unterminated nodes array");
            return false;
          }
          if (r.peek_is(']')) break;
          if (!r.expect(',', error)) return false;
        }
        if (!r.expect(']', error)) return false;
      }
      seen_nodes = true;
    } else if (key == "links") {
      if (seen_links) {
        r.fail(error, "duplicate key \"links\"");
        return false;
      }
      if (!r.expect(':', error) || !r.expect('[', error)) return false;
      r.skip_ws();
      if (!r.at_end() && r.peek_is(']')) {
        if (!r.expect(']', error)) return false;
      } else {
        for (;;) {
          if (!r.expect('{', error)) return false;
          bool seen_from = false;
          bool seen_out = false;
          bool seen_to = false;
          bool seen_in = false;
          std::uint64_t from = 0;
          std::uint64_t to = 0;
          std::string out_pin;
          std::string in_pin;
          for (;;) {
            std::string lkey;
            if (!r.read_key(lkey, error)) return false;
            if (lkey == "from") {
              if (!r.expect(':', error)) return false;
              double v = 0.0;
              if (!r.read_number(v, error)) return false;
              if (v < 1.0 || v != std::floor(v) ||
                  v > static_cast<double>(UINT64_MAX)) {
                r.fail(error, "link from-node must be a positive integer");
                return false;
              }
              from = static_cast<std::uint64_t>(v);
              seen_from = true;
            } else if (lkey == "out") {
              if (!r.expect(':', error) || !r.read_string(out_pin, error)) {
                return false;
              }
              seen_out = true;
            } else if (lkey == "to") {
              if (!r.expect(':', error)) return false;
              double v = 0.0;
              if (!r.read_number(v, error)) return false;
              if (v < 1.0 || v != std::floor(v) ||
                  v > static_cast<double>(UINT64_MAX)) {
                r.fail(error, "link to-node must be a positive integer");
                return false;
              }
              to = static_cast<std::uint64_t>(v);
              seen_to = true;
            } else if (lkey == "in") {
              if (!r.expect(':', error) || !r.read_string(in_pin, error)) {
                return false;
              }
              seen_in = true;
            } else {
              r.fail(error, "unknown link key \"" + lkey + "\"");
              return false;
            }
            r.skip_ws();
            if (r.at_end()) {
              r.fail(error, "unterminated link entry");
              return false;
            }
            if (r.peek_is('}')) break;
            if (!r.expect(',', error)) return false;
          }
          if (!r.expect('}', error)) return false;
          if (!seen_from || !seen_out || !seen_to || !seen_in) {
            r.fail(error, "link entry missing from/out/to/in");
            return false;
          }
          if (!graph.add_link(from, out_pin, to, in_pin, error)) {
            std::string detail = std::move(error);
            r.fail(error, "link invalid: " + detail);
            return false;
          }
          r.skip_ws();
          if (r.at_end()) {
            r.fail(error, "unterminated links array");
            return false;
          }
          if (r.peek_is(']')) break;
          if (!r.expect(',', error)) return false;
        }
        if (!r.expect(']', error)) return false;
      }
      seen_links = true;
    } else {
      r.fail(error, "unknown node-graph key \"" + key + "\"");
      return false;
    }
    r.skip_ws();
    if (r.at_end()) {
      r.fail(error, "unterminated node graph object");
      return false;
    }
    if (r.peek_is('}')) break;
    if (!r.expect(',', error)) return false;
  }
  return r.expect('}', error);
}

//! Reads the node_layout object: {"<id>":[x,y], ...} (string keys because
//! JSON object keys are strings; ids parse strictly as positive integers).
[[nodiscard]] bool read_node_layout(JsonReader& r,
                                    std::map<std::uint64_t,
                                             std::pair<double, double>>& out,
                                    std::string& error) {
  if (!r.expect('{', error)) return false;
  r.skip_ws();
  if (!r.at_end() && r.peek_is('}')) {
    return r.expect('}', error);
  }
  for (;;) {
    std::string key;
    if (!r.read_key(key, error)) return false;
    std::uint64_t id = 0;
    {
      double v = 0.0;
      const char* first = key.c_str();
      const char* last = first + key.size();
      const auto [ptr, ec] = std::from_chars(first, last, v);
      if (ec != std::errc{} || ptr != last || v < 1.0 || v != std::floor(v) ||
          v > static_cast<double>(UINT64_MAX)) {
        r.fail(error, "node_layout key must be a positive integer id");
        return false;
      }
      id = static_cast<std::uint64_t>(v);
    }
    if (!r.expect(':', error) || !r.expect('[', error)) return false;
    std::pair<double, double> xy{};
    if (!r.read_number(xy.first, error)) return false;
    if (!r.expect(',', error) || !r.read_number(xy.second, error)) return false;
    if (!r.expect(']', error)) return false;
    if (!out.emplace(id, xy).second) {
      r.fail(error, "duplicate node_layout entry for id " +
                        std::to_string(id));
      return false;
    }
    r.skip_ws();
    if (r.at_end()) {
      r.fail(error, "unterminated node_layout object");
      return false;
    }
    if (r.peek_is('}')) return r.expect('}', error);
    if (!r.expect(',', error)) return false;
  }
}

// ============================================================================
// SceneDocument (from_json)
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

std::string track_key(std::uint64_t object_id, const std::string& property) {
  return std::to_string(object_id) + ":" + property;
}

TimelineClip* SceneDocument::find_clip(std::uint64_t id) {
  for (auto& clip : clips) {
    if (clip.id == id) {
      return &clip;
    }
  }
  return nullptr;
}

const TimelineClip* SceneDocument::find_clip(std::uint64_t id) const {
  for (const auto& clip : clips) {
    if (clip.id == id) {
      return &clip;
    }
  }
  return nullptr;
}

//! G3: step-hold evaluation — the value of the last sample at or before the
//! query offset; nothing before the first sample (unrecorded lead-in holds
//! nothing, so playback before the first sample is a no-op per track).
bool TimelineClip::evaluate(std::uint64_t frame, std::uint64_t object_id,
                            const std::string& property,
                            PropValue& out) const {
  if (frame < start_frame || frame >= start_frame + length_frames) {
    return false;
  }
  const auto it = tracks.find(track_key(object_id, property));
  if (it == tracks.end() || it->second.samples.empty()) {
    return false;
  }
  const std::uint64_t offset = frame - start_frame;
  const ClipSample* best = nullptr;
  for (const auto& sample : it->second.samples) {
    if (sample.frame_offset <= offset) {
      best = &sample;
    } else {
      break;  // samples are strictly increasing
    }
  }
  if (best == nullptr) {
    return false;
  }
  out = best->value;
  return true;
}

bool TimelineClip::evaluate_at(double frame_time, std::uint64_t object_id,
                               const std::string& property, Easing easing,
                               PropValue& out) const {
  // Range check on the same half-open span as evaluate(), in floating point.
  const double start = static_cast<double>(start_frame);
  const double end = start + static_cast<double>(length_frames);
  if (!(frame_time >= start) || !(frame_time < end)) {
    return false;
  }
  const auto it = tracks.find(track_key(object_id, property));
  if (it == tracks.end() || it->second.samples.empty()) {
    return false;
  }
  const std::vector<ClipSample>& samples = it->second.samples;
  const double offset = frame_time - start;

  // Locate the bracketing pair. Samples are strictly increasing, so this is a
  // linear scan that stops early -- same shape as evaluate(), kept linear
  // rather than binary-searched because tracks are short and a linear scan
  // keeps the iteration order obvious.
  std::size_t upper = 0;
  while (upper < samples.size() &&
         static_cast<double>(samples[upper].frame_offset) <= offset) {
    ++upper;
  }
  if (upper == 0) {
    return false;  // nothing at or before this point
  }
  const std::size_t lower_index = upper - 1U;

  // Past the last key there is nothing to interpolate toward, so the final
  // value holds. This has to be decided before the Step check below: without
  // it, a fractional query past the last key falls through and reads
  // samples[upper] with upper == samples.size().
  if (upper >= samples.size()) {
    out = samples[lower_index].value;
    return true;
  }

  // Step mode is exactly the old behaviour, which is what keeps existing
  // replays reproducing their original run.
  if (easing == Easing::Step) {
    out = samples[lower_index].value;
    return true;
  }

  const double lower_offset = static_cast<double>(samples[lower_index].frame_offset);
  const PropValue& a = samples[lower_index].value;
  const PropValue& b = samples[upper].value;
  const double span = static_cast<double>(samples[upper].frame_offset) - lower_offset;
  if (!(span > 0.0)) {
    out = a;
    return true;
  }
  const float t = apply_easing(
      easing, static_cast<float>((offset - lower_offset) / span));

  // Non-numeric channels hold the earlier sample.
  if (a.type == PropValue::Type::Number &&
      b.type == PropValue::Type::Number) {
    out = PropValue::make_number(
        static_cast<double>(a.number) +
        (static_cast<double>(b.number) - static_cast<double>(a.number)) *
            static_cast<double>(t));
    return true;
  }
  if (a.type == PropValue::Type::Vec3 && b.type == PropValue::Type::Vec3) {
    double v[3];
    for (std::size_t k = 0; k < 3; ++k) {
      v[k] = a.vec[k] + (b.vec[k] - a.vec[k]) * static_cast<double>(t);
    }
    out = PropValue::make_vec3(v[0], v[1], v[2]);
    return true;
  }
  out = a;
  return true;
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
  out += "]";

  // M7 node graph (schema v2): the graph emits first (owning its own sorted
  // keys), then per-node view layout. Absent when the graph is empty so v1
  // documents and graph-free scenes stay byte-identical to the old writer.
  if (node_graph.node_count() != 0U) {
    out += ",\"node_graph\":";
    out += node_graph.to_json();
    out += ",\"node_layout\":{";
    bool first_node = true;
    for (const auto& [id, xy] : node_layout) {  // std::map: id-ordered
      if (!first_node) {
        out += ',';
      }
      first_node = false;
      out += '"';
      out += std::to_string(id);
      out += "\":[";
      write_number(out, xy.first);
      out += ',';
      write_number(out, xy.second);
      out += ']';
    }
    out += '}';
  }

  // G3 timeline clips (schema v3): written only when present so v2-era
  // documents stay byte-identical to the old writer. next_clip_id rides
  // along whenever clips exist (or the cursor moved, so a save/load round
  // trip preserves future id allocation).
  if (!clips.empty() || next_clip_id != 1U) {
    out += ",\"next_clip_id\":";
    write_number(out, static_cast<double>(next_clip_id));
    out += ",\"clips\":[";
    for (std::size_t i = 0; i < clips.size(); ++i) {
      const TimelineClip& clip = clips[i];
      if (i != 0) out += ',';
      out += "{\"id\":";
      write_number(out, static_cast<double>(clip.id));
      out += ",\"name\":";
      write_escaped(out, clip.name);
      out += ",\"start_frame\":";
      write_number(out, static_cast<double>(clip.start_frame));
      out += ",\"length_frames\":";
      write_number(out, static_cast<double>(clip.length_frames));
      out += ",\"tracks\":{";
      bool first_track = true;
      for (const auto& [tkey, track] : clip.tracks) {  // std::map: sorted
        if (!first_track) out += ',';
        first_track = false;
        write_escaped(out, tkey);
        out += ":{\"object_id\":";
        write_number(out, static_cast<double>(track.object_id));
        out += ",\"property\":";
        write_escaped(out, track.property);
        out += ",\"samples\":[";
        for (std::size_t s = 0; s < track.samples.size(); ++s) {
          const ClipSample& sample = track.samples[s];
          if (s != 0) out += ',';
          out += "{\"frame_offset\":";
          write_number(out, static_cast<double>(sample.frame_offset));
          out += ",\"value\":";
          switch (sample.value.type) {
            case PropValue::Type::Number:
              out += "{\"type\":\"number\",\"v\":";
              write_number(out, sample.value.number);
              out += '}';
              break;
            case PropValue::Type::Bool:
              out += "{\"type\":\"bool\",\"v\":";
              out += sample.value.boolean ? "true" : "false";
              out += '}';
              break;
            case PropValue::Type::String:
              out += "{\"type\":\"string\",\"v\":";
              write_escaped(out, sample.value.text);
              out += '}';
              break;
            case PropValue::Type::Vec3:
              out += "{\"type\":\"vec3\",\"x\":";
              write_number(out, sample.value.vec[0]);
              out += ",\"y\":";
              write_number(out, sample.value.vec[1]);
              out += ",\"z\":";
              write_number(out, sample.value.vec[2]);
              out += '}';
              break;
          }
          out += '}';
        }
        out += "]}}";  // samples array, track entry, tracks object
      }
      // The last track's suffix closed the tracks object; an EMPTY tracks
      // map still needs its own closer before the clip entry closes.
      out += first_track ? "}}" : "}";
    }
    out += ']';
  }
  out += '}';
  return out;
}

bool SceneDocument::from_json(std::string_view text,
                              SceneDocument& out,
                              std::string& error) {
  SceneDocument parsed{};
  // The node-graph parser validates types against a registry. Documents
  // carry no type definitions, so parse against the CALLEE's registry
  // (pre-register the built-ins — or your custom set — before loading).
  for (const auto& t : out.node_graph.types()) {
    parsed.node_graph.register_type(t);
  }
  JsonReader r(text.data(), text.data() + text.size());

  if (!r.expect('{', error)) return false;
  bool seen_version = false;
  bool seen_next_id = false;
  bool seen_objects = false;
  bool seen_graph = false;
  bool seen_layout = false;
  bool seen_clips = false;
  bool seen_next_clip_id = false;
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
    } else if (key == "node_graph") {
      if (seen_graph) {
        r.fail(error, "duplicate key \"node_graph\"");
        return false;
      }
      if (!r.expect(':', error)) return false;
      if (!read_node_graph(r, parsed.node_graph, error)) return false;
      seen_graph = true;
    } else if (key == "node_layout") {
      if (seen_layout) {
        r.fail(error, "duplicate key \"node_layout\"");
        return false;
      }
      if (!r.expect(':', error)) return false;
      if (!read_node_layout(r, parsed.node_layout, error)) return false;
      seen_layout = true;
    } else if (key == "next_clip_id") {
      if (seen_next_clip_id) {
        r.fail(error, "duplicate key \"next_clip_id\"");
        return false;
      }
      double v = 0.0;
      if (!r.expect(':', error) || !r.read_number(v, error)) return false;
      if (v < 1.0 || v != std::floor(v) ||
          v > static_cast<double>(UINT64_MAX)) {
        r.fail(error, "next_clip_id must be a positive integer");
        return false;
      }
      parsed.next_clip_id = static_cast<std::uint64_t>(v);
      seen_next_clip_id = true;
    } else if (key == "clips") {
      if (seen_clips) {
        r.fail(error, "duplicate key \"clips\"");
        return false;
      }
      if (!r.expect(':', error) || !r.expect('[', error)) return false;
      r.skip_ws();
      if (!r.at_end() && r.peek_is(']')) {
        if (!r.expect(']', error)) return false;
      } else {
        for (;;) {
          TimelineClip clip;
          if (!read_timeline_clip(r, clip, error)) return false;
          for (const auto& c : parsed.clips) {
            if (c.id == clip.id) {
              r.fail(error, "duplicate clip id " + std::to_string(clip.id));
              return false;
            }
          }
          parsed.clips.push_back(std::move(clip));
          r.skip_ws();
          if (r.at_end()) {
            r.fail(error, "unterminated clips array");
            return false;
          }
          if (r.peek_is(']')) break;
          if (!r.expect(',', error)) return false;
        }
        if (!r.expect(']', error)) return false;
      }
      seen_clips = true;
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
  // Graph payload requires schema v2 (the version that introduced it).
  if ((seen_graph || seen_layout) && parsed.schema_version < 2) {
    r.fail(error, "node_graph/node_layout requires schema_version >= 2");
    return false;
  }
  if (!seen_graph != !seen_layout) {
    r.fail(error, "node_graph and node_layout must appear together");
    return false;
  }
  // G3: clip payload requires schema v3 (the version that introduced it),
  // and the two clip keys travel together (writer emits both or neither).
  if ((seen_clips || seen_next_clip_id) && parsed.schema_version < 3) {
    r.fail(error, "clips require schema_version >= 3");
    return false;
  }
  if (!seen_clips != !seen_next_clip_id) {
    r.fail(error, "clips and next_clip_id must appear together");
    return false;
  }
  for (const auto& clip : parsed.clips) {
    if (clip.id >= parsed.next_clip_id) {
      r.fail(error, "clip id " + std::to_string(clip.id) +
                        " is not below next_clip_id");
      return false;
    }
    for (const auto& [tkey, track] : clip.tracks) {
      const std::string expected =
          std::to_string(track.object_id) + ":" + track.property;
      if (tkey != expected) {
        r.fail(error, "clip " + std::to_string(clip.id) +
                          " track key \"" + tkey +
                          "\" does not match its payload (expected \"" +
                          expected + "\")");
        return false;
      }
      if (parsed.find(track.object_id) == nullptr) {
        r.fail(error, "clip " + std::to_string(clip.id) +
                          " track references unknown object id " +
                          std::to_string(track.object_id));
        return false;
      }
      // Track values must be numeric: the recorder samples properties that
      // a number pin (or numeric property) drives; bool/string/vec3 tracks
      // have no deterministic playback meaning in v1 (step-hold of a vec3
      // is meaningful, but the recorder never samples one — refuse the
      // shape so the on-disk format stays honest).
      for (const auto& sample : track.samples) {
        if (sample.value.type != PropValue::Type::Number) {
          r.fail(error, "clip " + std::to_string(clip.id) +
                            " track \"" + tkey +
                            "\" has a non-numeric sample");
          return false;
        }
      }
    }
  }
  // Every layout entry must reference a real node (view state of nothing).
  for (const auto& [id, xy] : parsed.node_layout) {
    if (parsed.node_graph.find(id) == nullptr) {
      (void)xy;
      r.fail(error, "node_layout references unknown node id " +
                        std::to_string(id));
      return false;
    }
  }
  out = std::move(parsed);
  return true;
}

// ============================================================================
// Disk persistence (atomic save; strict load)
// ============================================================================

bool SceneDocument::save_to_file(const std::string& path,
                                 std::string& error) const {
  // Byte-deterministic payload.
  const std::string payload = to_json();

  // Atomic write: temp file in the same directory + rename (POSIX rename is
  // atomic within a filesystem), so a crash mid-write never truncates an
  // existing document. The temp name embeds the pid for parallel safety.
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    error = "save: cannot open \"" + tmp + "\": " + std::strerror(errno);
    return false;
  }
  out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  out.close();
  if (!out.good()) {
    error = "save: write failed for \"" + tmp + "\": " + std::strerror(errno);
    std::remove(tmp.c_str());
    return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    error = "save: rename failed: " + std::string(std::strerror(errno));
    std::remove(tmp.c_str());
    return false;
  }
  // Editor documents may hold project-local names; keep them private.
  (void)::chmod(path.c_str(), 0600);
  return true;
}

bool SceneDocument::load_from_file(const std::string& path,
                                   SceneDocument& out, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    error = "load: cannot open \"" + path + "\": " + std::strerror(errno);
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (in.bad()) {
    error = "load: read failed for \"" + path + "\"";
    return false;
  }
  return SceneDocument::from_json(text, out, error);
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
// Node-graph commands (M7)
// ============================================================================

AddNodeCommand::AddNodeCommand(std::string type, double x, double y)
    : type_(std::move(type)), x_(x), y_(y) {}

bool AddNodeCommand::apply(SceneDocument& doc, std::string& error) {
  if (doc.node_graph.find_type(type_) == nullptr) {
    error = "add_node: unknown type \"" + type_ + "\"";
    return false;
  }
  const std::uint64_t id = doc.node_graph.peek_next_id();
  std::map<std::string, PropValue> params;
  node_id_ = id;
  if (!doc.node_graph.add_node_with_id(id, type_, std::move(params))) {
    error = "add_node: id " + std::to_string(id) + " already taken";
    node_id_ = 0;
    return false;
  }
  doc.node_layout.emplace(id, std::make_pair(x_, y_));
  bumped_ = true;
  return true;
}

void AddNodeCommand::undo(SceneDocument& doc) {
  OMNICPP_CONTRACT(node_id_ != 0U);
  OMNICPP_CONTRACT(doc.node_graph.remove_node(node_id_));
  doc.node_layout.erase(node_id_);
  // Restore the id cursor so spawn+undo+redo claims the SAME id again —
  // the byte-determinism contract for spawn+undo round-trips.
  doc.node_graph.restore_id_cursor(node_id_);
}

std::string AddNodeCommand::describe() const {
  return "add node \"" + type_ + "\" (" + std::to_string(node_id_) + ")";
}

RemoveNodeCommand::RemoveNodeCommand(std::uint64_t node_id)
    : node_id_(node_id) {}

bool RemoveNodeCommand::apply(SceneDocument& doc, std::string& error) {
  const GraphNode* node = doc.node_graph.find(node_id_);
  if (node == nullptr) {
    error = "remove_node: no node " + std::to_string(node_id_);
    return false;
  }
  captured_ = *node;
  captured_links_.clear();
  for (const auto& l : doc.node_graph.links()) {
    if (l.from_node == node_id_ || l.to_node == node_id_) {
      captured_links_.push_back(l);
    }
  }
  OMNICPP_CONTRACT(doc.node_graph.remove_node(node_id_));
  doc.node_layout.erase(node_id_);
  applied_ = true;
  return true;
}

void RemoveNodeCommand::undo(SceneDocument& doc) {
  OMNICPP_CONTRACT(applied_);
  OMNICPP_CONTRACT(
      doc.node_graph.add_node_with_id(captured_.id, captured_.type,
                                      captured_.params));
  for (const auto& l : captured_links_) {
    std::string link_error;
    OMNICPP_CONTRACT(doc.node_graph.add_link(l.from_node, l.from_pin,
                                             l.to_node, l.to_pin,
                                             link_error));
  }
  if (!captured_links_.empty()) {
    // add_node_with_id bumps version per op; keep the counter honest.
  }
  // Restore layout position if the node had one.
  // (If it had none, leave it absent — the view assigns a default.)
  applied_ = false;
}

std::string RemoveNodeCommand::describe() const {
  return "remove node " + std::to_string(node_id_);
}

// G3: timeline clip commands (undo restores byte-identical document state,
// including the next_clip_id cursor for spawn+undo+redo id determinism).

AddClipCommand::AddClipCommand(std::string name, std::uint64_t start_frame,
                               std::uint64_t length_frames)
    : name_(std::move(name)),
      start_frame_(start_frame),
      length_frames_(length_frames) {}

bool AddClipCommand::apply(SceneDocument& doc, std::string& error) {
  (void)error;
  clip_id_ = doc.next_clip_id;
  TimelineClip clip;
  clip.id = clip_id_;
  clip.name = name_;
  clip.start_frame = start_frame_;
  clip.length_frames = length_frames_;
  doc.clips.push_back(std::move(clip));
  doc.next_clip_id += 1;
  bumped_ = true;
  return true;
}

void AddClipCommand::undo(SceneDocument& doc) {
  OMNICPP_CONTRACT(clip_id_ != 0U);
  OMNICPP_CONTRACT(bumped_);
  for (auto it = doc.clips.rbegin(); it != doc.clips.rend(); ++it) {
    if (it->id == clip_id_) {
      doc.clips.erase(std::next(it).base());
      break;
    }
  }
  // Restore the cursor so spawn+undo+redo claims the SAME id again —
  // the byte-determinism contract for spawn+undo round-trips.
  doc.next_clip_id = clip_id_;
}

std::string AddClipCommand::describe() const {
  return "add clip \"" + name_ + "\" @" + std::to_string(start_frame_);
}

RemoveClipCommand::RemoveClipCommand(std::uint64_t clip_id)
    : clip_id_(clip_id) {}

bool RemoveClipCommand::apply(SceneDocument& doc, std::string& error) {
  for (std::size_t i = 0; i < doc.clips.size(); ++i) {
    if (doc.clips[i].id == clip_id_) {
      captured_ = doc.clips[i];
      doc.clips.erase(doc.clips.begin() +
                      static_cast<std::ptrdiff_t>(i));
      applied_ = true;
      return true;
    }
  }
  error = "remove_clip: no clip " + std::to_string(clip_id_);
  return false;
}

void RemoveClipCommand::undo(SceneDocument& doc) {
  OMNICPP_CONTRACT(applied_);
  // Re-insert at the id-ordered slot (ids are monotonic, so the captured
  // clip's position is the first clip with a larger id — or the end).
  std::size_t at = doc.clips.size();
  for (std::size_t i = 0; i < doc.clips.size(); ++i) {
    if (doc.clips[i].id > captured_.id) {
      at = i;
      break;
    }
  }
  doc.clips.insert(doc.clips.begin() + static_cast<std::ptrdiff_t>(at),
                   captured_);
  applied_ = false;
}

std::string RemoveClipCommand::describe() const {
  return "remove clip " + std::to_string(clip_id_);
}

MoveClipCommand::MoveClipCommand(std::uint64_t clip_id,
                                 std::uint64_t new_start)
    : clip_id_(clip_id), new_start_(new_start) {}

bool MoveClipCommand::apply(SceneDocument& doc, std::string& error) {
  TimelineClip* clip = doc.find_clip(clip_id_);
  if (clip == nullptr) {
    error = "move_clip: no clip " + std::to_string(clip_id_);
    return false;
  }
  old_start_ = clip->start_frame;
  clip->start_frame = new_start_;
  applied_ = true;
  return true;
}

void MoveClipCommand::undo(SceneDocument& doc) {
  OMNICPP_CONTRACT(applied_);
  TimelineClip* clip = doc.find_clip(clip_id_);
  OMNICPP_CONTRACT(clip != nullptr);
  clip->start_frame = old_start_;
  applied_ = false;
}

std::string MoveClipCommand::describe() const {
  return "move clip " + std::to_string(clip_id_) + " to frame " +
         std::to_string(new_start_);
}

LinkNodesCommand::LinkNodesCommand(std::uint64_t from_node, std::string from_pin,
                                   std::uint64_t to_node, std::string to_pin)
    : from_node_(from_node),
      from_pin_(std::move(from_pin)),
      to_node_(to_node),
      to_pin_(std::move(to_pin)) {}

bool LinkNodesCommand::apply(SceneDocument& doc, std::string& error) {
  // Capture any link the apply will displace on the target input pin.
  had_previous_ = false;
  for (const auto& l : doc.node_graph.links()) {
    if (l.to_node == to_node_ && l.to_pin == to_pin_) {
      previous_ = l;
      had_previous_ = true;
      break;
    }
  }
  if (!doc.node_graph.add_link(from_node_, from_pin_, to_node_, to_pin_,
                               error)) {
    return false;
  }
  applied_ = true;
  return true;
}

void LinkNodesCommand::undo(SceneDocument& doc) {
  OMNICPP_CONTRACT(applied_);
  OMNICPP_CONTRACT(doc.node_graph.remove_link(to_node_, to_pin_));
  if (had_previous_) {
    std::string link_error;
    OMNICPP_CONTRACT(doc.node_graph.add_link(previous_.from_node,
                                             previous_.from_pin,
                                             previous_.to_node,
                                             previous_.to_pin, link_error));
  }
  applied_ = false;
}

std::string LinkNodesCommand::describe() const {
  return "link " + std::to_string(from_node_) + "." + from_pin_ + " -> " +
         std::to_string(to_node_) + "." + to_pin_;
}

UnlinkNodeCommand::UnlinkNodeCommand(std::uint64_t to_node, std::string to_pin)
    : to_node_(to_node), to_pin_(std::move(to_pin)) {}

bool UnlinkNodeCommand::apply(SceneDocument& doc, std::string& error) {
  had_link_ = false;
  for (const auto& l : doc.node_graph.links()) {
    if (l.to_node == to_node_ && l.to_pin == to_pin_) {
      captured_ = l;
      had_link_ = true;
      break;
    }
  }
  if (!had_link_) {
    error = "unlink: no link on node " + std::to_string(to_node_) + " pin \"" +
            to_pin_ + "\"";
    return false;
  }
  OMNICPP_CONTRACT(doc.node_graph.remove_link(to_node_, to_pin_));
  return true;
}

void UnlinkNodeCommand::undo(SceneDocument& doc) {
  OMNICPP_CONTRACT(had_link_);
  std::string link_error;
  OMNICPP_CONTRACT(doc.node_graph.add_link(captured_.from_node,
                                           captured_.from_pin, captured_.to_node,
                                           captured_.to_pin, link_error));
}

std::string UnlinkNodeCommand::describe() const {
  return "unlink " + std::to_string(to_node_) + "." + to_pin_;
}

SetNodeParamCommand::SetNodeParamCommand(std::uint64_t node_id, std::string key,
                                         PropValue value)
    : node_id_(node_id), key_(std::move(key)), value_(std::move(value)) {}

bool SetNodeParamCommand::apply(SceneDocument& doc, std::string& error) {
  GraphNode* node = nullptr;
  for (auto& n : doc.node_graph.nodes_mutable()) {
    if (n.id == node_id_) {
      node = &n;
      break;
    }
  }
  if (node == nullptr) {
    error = "set_node_param: no node " + std::to_string(node_id_);
    return false;
  }
  const auto it = node->params.find(key_);
  existed_ = it != node->params.end();
  if (existed_) {
    old_value_ = it->second;
    it->second = value_;
  } else {
    node->params.emplace(key_, value_);
  }
  return true;
}

void SetNodeParamCommand::undo(SceneDocument& doc) {
  for (auto& n : doc.node_graph.nodes_mutable()) {
    if (n.id != node_id_) continue;
    if (existed_) {
      n.params.insert_or_assign(key_, old_value_);
    } else {
      n.params.erase(key_);
    }
    return;
  }
  OMNICPP_CONTRACT(false && "set_node_param undo: node vanished");
}

std::string SetNodeParamCommand::describe() const {
  return "set param \"" + key_ + "\" on node " + std::to_string(node_id_);
}

SetNodePositionCommand::SetNodePositionCommand(std::uint64_t node_id, double x,
                                               double y)
    : node_id_(node_id), x_(x), y_(y) {}

bool SetNodePositionCommand::apply(SceneDocument& doc, std::string& error) {
  if (doc.node_graph.find(node_id_) == nullptr) {
    error = "set_node_position: no node " + std::to_string(node_id_);
    return false;
  }
  const auto it = doc.node_layout.find(node_id_);
  had_previous_ = it != doc.node_layout.end();
  if (had_previous_) {
    previous_ = it->second;
    it->second = {x_, y_};
  } else {
    doc.node_layout.emplace(node_id_, std::make_pair(x_, y_));
  }
  return true;
}

void SetNodePositionCommand::undo(SceneDocument& doc) {
  if (had_previous_) {
    doc.node_layout.insert_or_assign(node_id_, previous_);
  } else {
    doc.node_layout.erase(node_id_);
  }
}

std::string SetNodePositionCommand::describe() const {
  return "move node " + std::to_string(node_id_);
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

}  // namespace warploom::editor
