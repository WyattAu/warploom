//! @file jpeg_decoder.cpp
//! @brief Strict self-contained baseline sequential JPEG decoder.
//! See jpeg_decoder.hpp.

#include "warploom/asset/jpeg_decoder.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace warploom::asset {

namespace {

using ::warploom::core::RuntimeError;

// ============================================================================
// Marker codes
// ============================================================================

constexpr std::uint8_t kSof0 = 0xC0;  // baseline DCT
constexpr std::uint8_t kSof1 = 0xC1;  // extended sequential (12-bit)
constexpr std::uint8_t kSof2 = 0xC2;  // progressive
constexpr std::uint8_t kDht = 0xC4;
constexpr std::uint8_t kSos = 0xDA;
constexpr std::uint8_t kDqt = 0xDB;
constexpr std::uint8_t kDnl = 0xDC;
constexpr std::uint8_t kDri = 0xDD;
constexpr std::uint8_t kEoi = 0xD9;
constexpr std::uint8_t kRst0 = 0xD0;

//! True for the SOFn frame markers (0xC0..0xCF excluding DHT/DAC segments).
constexpr bool is_sof_marker(std::uint8_t marker) noexcept {
  return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xCC;
}

// ============================================================================
// Zig-zag scan order: entry k is the k-th coefficient in scan order
// ============================================================================

constexpr std::uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

//! Inverse of kZigzag: natural position n sits at scan index kInverseZigzag[n].
constexpr std::array<std::uint8_t, 64> kInverseZigzag = [] {
  std::array<std::uint8_t, 64> inverse{};
  for (int i = 0; i < 64; ++i) {
    inverse[kZigzag[i]] = static_cast<std::uint8_t>(i);
  }
  return inverse;
}();

// ============================================================================
// Byte cursor over the encoded file (single cursor shared with the entropy
// reader, so marker dispatch continues exactly where scan data ended)
// ============================================================================

class JpegInput final {
public:
  JpegInput(const std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - pos_; }
  [[nodiscard]] bool empty() const noexcept { return pos_ >= size_; }
  [[nodiscard]] std::size_t pos() const noexcept { return pos_; }

  [[nodiscard]] bool read_byte(std::uint8_t& out) noexcept {
    if (empty()) return false;
    out = data_[pos_++];
    return true;
  }

  //! Skip `count` raw bytes (bounds-checked).
  [[nodiscard]] bool skip(std::size_t count) noexcept {
    if (count > remaining()) return false;
    pos_ += count;
    return true;
  }

  //! Consume a marker's 0xFF prefix (and any 0xFF fill bytes) and return the
  //! marker byte. Returns false when no marker stands at the cursor.
  [[nodiscard]] bool marker_prefix(std::uint8_t& marker) noexcept {
    while (!empty()) {
      if (data_[pos_] != 0xFF) return false;
      while (!empty() && data_[pos_] == 0xFF) ++pos_;
      if (empty()) return false;
      marker = data_[pos_++];
      return true;
    }
    return false;
  }

  //! The caller has already consumed a marker byte (or the pending-marker
  //! path delivered one); read its length-prefixed payload.
  [[nodiscard]] bool read_payload(const std::uint8_t*& payload,
                                  std::size_t& payload_size,
                                  std::string& error) noexcept {
    if (remaining() < 2U) {
      if (error.empty()) error = "truncated segment header";
      return false;
    }
    const std::uint32_t length =
        (static_cast<std::uint32_t>(data_[pos_]) << 8U) |
        static_cast<std::uint32_t>(data_[pos_ + 1U]);
    if (length < 2U || static_cast<std::size_t>(length) - 2U > remaining()) {
      if (error.empty()) error = "segment length is inconsistent";
      return false;
    }
    payload = data_ + pos_ + 2U;
    payload_size = static_cast<std::size_t>(length) - 2U;
    return skip(2U + payload_size);
  }

private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_{0};
};

//! Records the first diagnostic and signals a bool failure (used by helpers
//! that report success as `bool`).
[[nodiscard]] bool record_fail(std::string& error, const std::string& message) {
  if (error.empty()) error = message;
  return false;
}

// ============================================================================
// Huffman table (canonical codes from a DHT segment)
// ============================================================================

struct HuffmanTable {
  //! count[l] = number of codes of length l (l in 1..16).
  std::uint16_t count[17]{};
  //! Symbols grouped by code length, in code order within a length.
  std::uint8_t symbols[17][256]{};
  bool defined{false};
  //! Canonical ranges: a code of length l is valid when
  //! min_code[l] <= code <= max_code[l]; its index within the length is
  //! code - min_code[l]. -1 means no codes of that length.
  std::int32_t min_code[17];
  std::int32_t max_code[17];

  HuffmanTable() noexcept {
    for (int l = 0; l < 17; ++l) {
      min_code[l] = -1;
      max_code[l] = -1;
    }
  }

  //! Compute canonical ranges from the counts and validate the code space
  //! (Kraft sum must not exceed 1). Canonical codes are assigned exactly as in
  //! JPEG Annex F.1.2.2: the next code of length l+1 is
  //! (code + count[l]) << 1.
  [[nodiscard]] bool build() noexcept {
    std::uint64_t kraft = 0;
    for (int len = 1; len <= 16; ++len) {
      kraft += static_cast<std::uint64_t>(count[len])
               << static_cast<unsigned>(16 - len);
      if (kraft > 0x10000U) return false;  // over-subscribed code space
    }
    std::uint32_t code = 0;
    for (int len = 1; len <= 16; ++len) {
      if (count[len] != 0U) {
        min_code[len] = static_cast<std::int32_t>(code);
        max_code[len] =
            static_cast<std::int32_t>(code + count[len] - 1U);
      }
      code = (code + count[len]) << 1;
    }
    return true;
  }
};

// ============================================================================
// Entropy-coded bit reader (MSB-first, 0xFF00 stuffing, marker detection).
// Shares the JpegInput cursor.
// ============================================================================

class EntropyReader final {
public:
  explicit EntropyReader(JpegInput& input) noexcept : input_(input) {}

  //! Pull bytes into the cache. Returns false (leaving any marker byte
  //! unconsumed and recorded in `marker_`) when a marker or end of input is
  //! reached.
  [[nodiscard]] bool refill() noexcept {
    if (marker_ != 0U) return false;
    std::uint8_t b = 0;
    while (input_.read_byte(b)) {
      if (b != 0xFF) {
        cache_ = (cache_ << 8U) | b;
        bits_ += 8;
        return true;
      }
      // 0xFF followed by 0x00 is a stuffed data byte.
      std::uint8_t next = 0;
      if (!input_.read_byte(next)) {
        marker_ = 0xFF;  // file ended inside a fill run
        return false;
      }
      if (next == 0x00) {
        cache_ = (cache_ << 8U) | 0xFFU;
        bits_ += 8;
        return true;
      }
      // next is a marker or another fill 0xFF; consume fill bytes, then the
      // marker byte, and record it.
      while (next == 0xFF) {
        if (!input_.read_byte(next)) {
          marker_ = 0xFF;
          return false;
        }
      }
      marker_ = next;
      return false;
    }
    marker_ = 0xFF;
    return false;
  }

  //! Read exactly `count` bits (0..24), MSB-first.
  [[nodiscard]] bool read_bits(int count, std::uint32_t& out) noexcept {
    out = 0;
    while (count > 0) {
      while (bits_ == 0) {
        if (!refill()) return false;
      }
      const int take = count < bits_ ? count : bits_;
      const int shift = bits_ - take;
      out = (out << take) |
            ((cache_ >> shift) & ((std::uint32_t{1} << take) - 1U));
      bits_ -= take;
      cache_ &= bits_ != 0 ? (std::uint32_t{1} << bits_) - 1U : 0U;
      count -= take;
    }
    return true;
  }

  [[nodiscard]] bool marker_pending() const noexcept {
    return marker_ != 0U && marker_ != 0xFF;
  }
  [[nodiscard]] std::uint8_t pending_marker() const noexcept { return marker_; }
  void clear_marker() noexcept { marker_ = 0; }

  //! Drop any partially buffered byte so the next code starts at a byte
  //! boundary (restart semantics).
  void byte_align() noexcept {
    cache_ = 0;
    bits_ = 0;
  }

  //! Consume padding bits until a marker is recorded (used after the final
  //! MCU and before restart markers). Returns false only at end of input.
  [[nodiscard]] bool drain_to_marker() noexcept {
    while (marker_ == 0U) {
      if (bits_ != 0) {
        cache_ = 0;
        bits_ = 0;
        continue;
      }
      std::uint32_t ignored = 0;
      if (!read_bits(1, ignored)) {
        if (marker_ == 0xFF) return false;
      }
    }
    return marker_ != 0xFF;
  }

private:
  JpegInput& input_;
  std::uint32_t cache_{0};
  int bits_{0};
  std::uint8_t marker_{0};
};

// ============================================================================
// IDCT (separable, floating point)
// ============================================================================

void idct_8x8(double block[64]) noexcept {
  // Basis C[i][k] = c(k) * cos((2i+1) * k * pi / 16), c(0) = sqrt(0.5).
  static const std::array<std::array<double, 8>, 8> kCos = [] {
    std::array<std::array<double, 8>, 8> c{};
    const double pi = 3.14159265358979323846;
    for (int k = 0; k < 8; ++k) {
      const double scale = k == 0 ? std::sqrt(0.5) : 1.0;
      for (int i = 0; i < 8; ++i) {
        c[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] =
            scale * std::cos((2.0 * static_cast<double>(i) + 1.0) *
                             static_cast<double>(k) * pi / 16.0);
      }
    }
    return c;
  }();

  // Rows.
  for (int y = 0; y < 8; ++y) {
    double tmp[8]{};
    for (int x = 0; x < 8; ++x) {
      double sum = 0.0;
      for (int k = 0; k < 8; ++k) {
        sum += kCos[static_cast<std::size_t>(x)][static_cast<std::size_t>(k)] *
               block[y * 8 + k];
      }
      tmp[x] = sum;
    }
    std::memcpy(&block[y * 8], tmp, sizeof(tmp));
  }
  // Columns.
  for (int x = 0; x < 8; ++x) {
    double tmp[8]{};
    for (int y = 0; y < 8; ++y) {
      double sum = 0.0;
      for (int k = 0; k < 8; ++k) {
        sum += kCos[static_cast<std::size_t>(y)][static_cast<std::size_t>(k)] *
               block[k * 8 + x];
      }
      tmp[y] = sum;
    }
    for (int y = 0; y < 8; ++y) {
      block[y * 8 + x] = tmp[y] * 0.25;  // 0.5 per 1D transform
    }
  }
}

//! Round + clamp a floating sample into an output byte.
[[nodiscard]] std::uint8_t clamp_u8(double value) noexcept {
  if (value < 0.0) return 0;
  if (value > 255.0) return 255;
  return static_cast<std::uint8_t>(value + 0.5);
}

//! Extend a `size`-bit magnitude to signed (JPEG Annex F.2.2.3):
//! negative values are stored as `2^size - 1 + x` in `size` bits, so
//! recovery is `value - (2^size - 1)` = `value - 2^size + 1`. The +1 is
//! essential: without it every negative coefficient decodes one unit too
//! negative and DC predictors drift.
[[nodiscard]] std::int32_t extend_value(std::uint32_t value,
                                        std::uint32_t size) noexcept {
  if (size == 0U) return 0;
  const std::uint32_t half = std::uint32_t{1} << (size - 1U);
  if (value >= half) return static_cast<std::int32_t>(value);
  return static_cast<std::int32_t>(value) -
             static_cast<std::int32_t>(std::uint32_t{1} << size) +
         1;
}

struct FrameSpec {
  std::uint32_t width{0};
  std::uint32_t height{0};
  int component_count{0};
  struct Component {
    std::uint8_t id{0};
    std::uint8_t h{1};
    std::uint8_t v{1};
    std::uint8_t quant_id{0};
    std::uint8_t h_ratio{1};
    std::uint8_t v_ratio{1};
  };
  Component components[3];
  std::uint8_t hmax{1};
  std::uint8_t vmax{1};
};

}  // namespace

// ============================================================================
// decode_jpeg
// ============================================================================

::warploom::core::Result<DecodedImage> decode_jpeg(
    const std::uint8_t* bytes, std::size_t length,
    std::string* error_detail) {
  std::string error;
  //! Result-flavoured failure (records the first diagnostic and copies it to
  //! the caller's error_detail, which is otherwise only cleared on success).
  auto failf = [&](const std::string& message)
      -> ::warploom::core::Result<DecodedImage> {
    if (error.empty()) error = message;
    if (error_detail != nullptr) *error_detail = error;
    return ::warploom::core::Result<DecodedImage>::error(
        RuntimeError::malformed_asset);
  };

  if (bytes == nullptr && length != 0U) {
    return failf("bytes is null");
  }
  if (length < 4U) {
    return failf("file is too short to be a JPEG");
  }
  if (bytes[0] != 0xFF || bytes[1] != 0xD8) {
    return failf("missing JPEG SOI marker");
  }

  JpegInput input(bytes, length);
  if (!input.skip(2U)) return failf("truncated file");  // SOI

  HuffmanTable dc_tables[4];
  HuffmanTable ac_tables[4];
  double quant_tables[4][64]{};
  bool quant_defined[4]{};
  std::uint32_t restart_interval = 0;
  FrameSpec frame;
  bool frame_seen = false;
  bool saw_scan = false;
  std::uint8_t scan_frame_index[3]{};
  std::uint8_t scan_dc_selector[3]{};
  std::uint8_t scan_ac_selector[3]{};
  DecodedImage image;

  auto parse_sof0 = [&](const std::uint8_t* payload,
                        std::size_t payload_size) -> bool {
    if (payload_size < 6U) return record_fail(error, "SOF payload is too short");
    const std::uint8_t precision = payload[0];
    if (precision != 8U) {
      return record_fail(error, "sample precision must be 8 bits (got " +
                                    std::to_string(precision) + ")");
    }
    frame.height = (static_cast<std::uint32_t>(payload[1]) << 8U) |
                   static_cast<std::uint32_t>(payload[2]);
    frame.width = (static_cast<std::uint32_t>(payload[3]) << 8U) |
                  static_cast<std::uint32_t>(payload[4]);
    if (frame.width == 0U || frame.height == 0U) {
      return record_fail(error, "image dimensions must be non-zero");
    }
    if (static_cast<std::uint64_t>(frame.width) * frame.height * 4U >
        kMaxDecodedJpegBytes) {
      return record_fail(error, "decoded image exceeds the memory cap");
    }
    const std::uint8_t nf = payload[5];
    if (nf != 1U && nf != 3U) {
      return record_fail(error, "only grayscale (1) and YCbCr (3) component "
                                "frames are supported (got " +
                                    std::to_string(nf) + ")");
    }
    if (payload_size < 6U + static_cast<std::size_t>(nf) * 3U) {
      return record_fail(error, "SOF payload is too short for its components");
    }
    frame.component_count = nf;
    if (nf == 3) {
      // Colour frames follow the JFIF convention: ids 1, 2, 3 name Y, Cb, Cr
      // (scan order is free, conversion keys off the ids).
      bool have[4]{false, false, false, false};
      for (int c = 0; c < nf; ++c) {
        const std::uint8_t id = payload[6U + static_cast<std::size_t>(c) * 3U];
        if (id == 0U || id > 3U) {
          return record_fail(error, "colour frame components must use JFIF "
                                    "ids 1 (Y), 2 (Cb), 3 (Cr)");
        }
        have[id] = true;
      }
      if (!have[1] || !have[2] || !have[3]) {
        return record_fail(error, "colour frame must declare Y, Cb and Cr "
                                  "components");
      }
    }
    std::uint8_t hmax = 0;
    std::uint8_t vmax = 0;
    bool id_used[256]{};
    for (int c = 0; c < nf; ++c) {
      const std::uint8_t* entry = payload + 6U + static_cast<std::size_t>(c) * 3U;
      const std::uint8_t id = entry[0];
      const std::uint8_t sampling = entry[1];
      const std::uint8_t tq = entry[2];
      if (id == 0U) return record_fail(error, "component id 0 is reserved");
      if (id_used[id]) {
        return record_fail(error, "duplicate component id " +
                                      std::to_string(id));
      }
      id_used[id] = true;
      const std::uint8_t h = static_cast<std::uint8_t>(sampling >> 4U);
      const std::uint8_t v = static_cast<std::uint8_t>(sampling & 0x0FU);
      if (h == 0U || h > 4U || v == 0U || v > 4U) {
        return record_fail(error, "sampling factors must be in 1..4");
      }
      if (tq > 3U) return record_fail(error, "quantisation table id out of range");
      frame.components[c].id = id;
      frame.components[c].h = h;
      frame.components[c].v = v;
      frame.components[c].quant_id = tq;
      if (h > hmax) hmax = h;
      if (v > vmax) vmax = v;
    }
    for (int c = 0; c < nf; ++c) {
      if (hmax % frame.components[c].h != 0U ||
          vmax % frame.components[c].v != 0U) {
        return record_fail(error, "unsupported fractional upsampling ratio "
                                  "(each component's sampling factor must "
                                  "divide the frame maximum)");
      }
      frame.components[c].h_ratio =
          static_cast<std::uint8_t>(hmax / frame.components[c].h);
      frame.components[c].v_ratio =
          static_cast<std::uint8_t>(vmax / frame.components[c].v);
    }
    frame.hmax = hmax;
    frame.vmax = vmax;
    frame_seen = true;
    return true;
  };

  std::uint8_t pending_marker = 0;
  bool has_pending_marker = false;

  while (true) {
    std::uint8_t marker = 0;
    if (has_pending_marker) {
      marker = pending_marker;
      has_pending_marker = false;
    } else if (!input.marker_prefix(marker)) {
      return failf(error.empty() ? "truncated file before a marker" : error);
    }

    if (marker == kEoi) {
      if (!saw_scan) return failf("EOI before any scan");
      if (error_detail != nullptr) *error_detail = std::string();
      return ::warploom::core::Result<DecodedImage>::ok(std::move(image));
    }

    if (marker == kDqt) {
      if (saw_scan) {
        return failf("quantisation tables after the scan are not supported");
      }
      const std::uint8_t* payload = nullptr;
      std::size_t payload_size = 0;
      if (!input.read_payload(payload, payload_size, error)) {
        return failf(error.empty() ? "truncated DQT" : error);
      }
      std::size_t done = 0;
      while (done < payload_size) {
        if (payload_size - done < 65U) {
          return failf("DQT payload is truncated");
        }
        const std::uint8_t pq_tq = payload[done];
        const std::uint8_t precision =
            static_cast<std::uint8_t>(pq_tq >> 4U);
        const std::uint8_t id = static_cast<std::uint8_t>(pq_tq & 0x0FU);
        if (precision != 0U) {
          return failf("16-bit quantisation tables are not supported");
        }
        if (id > 3U) return failf("quantisation table id out of range");
        // Payload values arrive in zig-zag (scan) order; store them keyed by
        // the natural coefficient index so dequantization reads
        // block[natural] * quant[natural].
        for (std::size_t i = 0; i < 64U; ++i) {
          quant_tables[id][i] =
              payload[done + 1U +
                      static_cast<std::size_t>(kInverseZigzag[i])];
        }
        quant_defined[id] = true;
        done += 65U;
      }
      continue;
    }

    if (marker == kDht) {
      if (saw_scan) {
        return failf("Huffman tables after the scan are not supported");
      }
      const std::uint8_t* payload = nullptr;
      std::size_t payload_size = 0;
      if (!input.read_payload(payload, payload_size, error)) {
        return failf(error.empty() ? "truncated DHT" : error);
      }
      std::size_t done = 0;
      while (done < payload_size) {
        if (payload_size - done < 17U) {
          return failf("DHT payload is truncated");
        }
        const std::uint8_t tc_th = payload[done];
        const std::uint8_t table_class =
            static_cast<std::uint8_t>(tc_th >> 4U);
        const std::uint8_t id = static_cast<std::uint8_t>(tc_th & 0x0FU);
        if (table_class > 1U) return failf("invalid Huffman table class");
        if (id > 3U) return failf("Huffman table id out of range");
        std::uint32_t total = 0;
        HuffmanTable& table =
            table_class == 0U ? dc_tables[id] : ac_tables[id];
        table = HuffmanTable{};
        for (int len = 1; len <= 16; ++len) {
          const std::uint8_t count = payload[done + static_cast<std::size_t>(len)];
          table.count[len] = count;
          total += count;
        }
        if (total > 256U) return failf("Huffman table has too many codes");
        if (total == 0U) return failf("Huffman table has no codes");
        if (payload_size - done < 17U + total) {
          return failf("DHT payload is truncated");
        }
        std::size_t symbol_index = 17U;
        for (int len = 1; len <= 16; ++len) {
          for (std::uint16_t n = 0; n < table.count[len]; ++n) {
            table.symbols[len][n] = payload[done + symbol_index++];
          }
        }
        if (!table.build()) {
          return failf("Huffman code space is over-subscribed");
        }
        table.defined = true;
        done += 17U + total;
      }
      continue;
    }

    if (marker == kDri) {
      if (saw_scan) return failf("DRI after the scan is not supported");
      const std::uint8_t* payload = nullptr;
      std::size_t payload_size = 0;
      if (!input.read_payload(payload, payload_size, error)) {
        return failf(error.empty() ? "truncated DRI" : error);
      }
      if (payload_size != 2U) return failf("DRI length must be 4");
      restart_interval = (static_cast<std::uint32_t>(payload[0]) << 8U) |
                         static_cast<std::uint32_t>(payload[1]);
      continue;
    }

    if (marker >= 0xE0 && marker <= 0xEF) {  // APPn
      const std::uint8_t* payload = nullptr;
      std::size_t payload_size = 0;
      if (!input.read_payload(payload, payload_size, error)) {
        return failf(error.empty() ? "truncated APPn" : error);
      }
      (void)payload;
      (void)payload_size;
      continue;
    }

    if (marker == 0xFE) {  // COM
      const std::uint8_t* payload = nullptr;
      std::size_t payload_size = 0;
      if (!input.read_payload(payload, payload_size, error)) {
        return failf(error.empty() ? "truncated COM" : error);
      }
      (void)payload;
      (void)payload_size;
      continue;
    }

    if (is_sof_marker(marker)) {
      if (saw_scan) return failf("frame header after the scan");
      if (marker != kSof0) {
        if (marker == kSof1) {
          return failf("extended sequential (12-bit) is not supported");
        }
        if (marker == kSof2) {
          return failf("progressive JPEG is not supported");
        }
        if (marker >= 0xC9) {
          return failf("arithmetic-coded JPEG is not supported");
        }
        return failf("unsupported SOF frame marker");
      }
      const std::uint8_t* payload = nullptr;
      std::size_t payload_size = 0;
      if (!input.read_payload(payload, payload_size, error)) {
        return failf(error.empty() ? "truncated SOF" : error);
      }
      if (!parse_sof0(payload, payload_size)) return failf(error);
      continue;
    }

    if (marker == kSos) {
      if (!frame_seen) return failf("scan before any frame header");
      if (saw_scan) {
        return failf("only a single interleaved scan is supported");
      }
      const std::uint8_t* payload = nullptr;
      std::size_t payload_size = 0;
      if (!input.read_payload(payload, payload_size, error)) {
        return failf(error.empty() ? "truncated SOS" : error);
      }
      if (payload_size < 1U) return failf("SOS payload is empty");
      const std::uint8_t ns = payload[0];
      if (ns != frame.component_count) {
        return failf("SOS component count " + std::to_string(ns) +
                     " does not match the frame (" +
                     std::to_string(frame.component_count) + ")");
      }
      if (payload_size < 1U + static_cast<std::size_t>(ns) * 2U + 3U) {
        return failf("SOS payload is too short");
      }
      bool used[3]{};
      for (int i = 0; i < ns; ++i) {
        const std::uint8_t* entry =
            payload + 1U + static_cast<std::size_t>(i) * 2U;
        const std::uint8_t id = entry[0];
        const std::uint8_t tables = entry[1];
        int found = -1;
        for (int c = 0; c < frame.component_count; ++c) {
          if (frame.components[c].id == id) {
            found = c;
            break;
          }
        }
        if (found < 0) {
          return failf("SOS references a component not in the frame");
        }
        if (used[found]) return failf("SOS lists a component more than once");
        used[found] = true;
        scan_frame_index[i] = static_cast<std::uint8_t>(found);
        scan_dc_selector[i] = static_cast<std::uint8_t>(tables >> 4U);
        scan_ac_selector[i] = static_cast<std::uint8_t>(tables & 0x0FU);
        if (scan_dc_selector[i] > 3U || scan_ac_selector[i] > 3U) {
          return failf("Huffman table selectors out of range");
        }
        if (!dc_tables[scan_dc_selector[i]].defined) {
          return failf("scan references an undefined DC Huffman table");
        }
        if (!ac_tables[scan_ac_selector[i]].defined) {
          return failf("scan references an undefined AC Huffman table");
        }
        if (!quant_defined[frame.components[found].quant_id]) {
          return failf("component references an undefined quantisation table");
        }
      }
      for (int c = 0; c < frame.component_count; ++c) {
        if (!used[c]) return failf("scan does not cover every frame component");
      }
      const std::size_t spectral = 1U + static_cast<std::size_t>(ns) * 2U;
      if (payload[spectral] != 0U || payload[spectral + 1U] != 63U) {
        return failf("baseline requires Ss=0 and Se=63");
      }
      if (payload[spectral + 2U] != 0U) {
        return failf("baseline requires no successive approximation");
      }
      saw_scan = true;

      // ---------------- entropy decode ----------------------------------
      const std::uint32_t mcu_width = 8U * frame.hmax;
      const std::uint32_t mcu_height = 8U * frame.vmax;
      const std::uint32_t mcus_x =
          (frame.width + mcu_width - 1U) / mcu_width;
      const std::uint32_t mcus_y =
          (frame.height + mcu_height - 1U) / mcu_height;

      image.width = frame.width;
      image.height = frame.height;
      image.rgba.resize(static_cast<std::size_t>(frame.width) *
                        frame.height * 4U);

      // Per-component padded planes at each component's own sampling density.
      struct Plane {
        std::vector<double> data;
        std::uint32_t row_stride{0};
        std::uint32_t rows{0};
      };
      Plane planes[3];
      for (int c = 0; c < frame.component_count; ++c) {
        const FrameSpec::Component& spec = frame.components[c];
        const std::uint32_t row_stride = mcus_x * 8U * spec.h;
        const std::uint32_t rows = mcus_y * 8U * spec.v;
        planes[c].data.assign(
            static_cast<std::size_t>(row_stride) * rows, 0.0);
        planes[c].row_stride = row_stride;
        planes[c].rows = rows;
      }

      EntropyReader entropy(input);
      double block[64]{};
      std::int32_t dc_predictor[3]{};
      std::uint8_t restart_seq = 0;

      // Decode one component block and store its upsampled samples.
      const auto decode_mcu_block = [&](int scan_index, std::uint32_t mcux,
                                        std::uint32_t mcuy, int bx,
                                        int by) -> bool {
        const int frame_index = scan_frame_index[scan_index];
        const FrameSpec::Component& spec = frame.components[frame_index];
        std::memset(block, 0, sizeof(block));
        const HuffmanTable& dc_table =
            dc_tables[scan_dc_selector[scan_index]];
        const HuffmanTable& ac_table =
            ac_tables[scan_ac_selector[scan_index]];

        // DC: category from Huffman, then magnitude bits.
        {
          std::uint32_t code = 0;
          bool found = false;
          std::uint32_t category = 0;
          for (int len = 1; len <= 16; ++len) {
            std::uint32_t one = 0;
            if (!entropy.read_bits(1, one)) {
              return record_fail(error, entropy.marker_pending()
                                            ? "entropy data ended at a marker "
                                              "inside a block"
                                            : "entropy data is truncated "
                                              "inside a DC code");
            }
            code = (code << 1U) | one;
            if (dc_table.min_code[len] >= 0 &&
                static_cast<std::int32_t>(code) >= dc_table.min_code[len] &&
                static_cast<std::int32_t>(code) <= dc_table.max_code[len]) {
              const std::uint32_t index =
                  code - static_cast<std::uint32_t>(dc_table.min_code[len]);
              if (index >= dc_table.count[len]) {
                return record_fail(error, "invalid DC Huffman code");
              }
              category = dc_table.symbols[len][index];
              found = true;
              break;
            }
          }
          if (!found) return record_fail(error, "invalid DC Huffman code");
          if (category > 11U) {
            return record_fail(error, "DC category exceeds 11");
          }
          std::uint32_t extra = 0;
          if (!entropy.read_bits(static_cast<int>(category), extra)) {
            return record_fail(error,
                               "entropy data is truncated in DC extra bits");
          }
          dc_predictor[frame_index] += extend_value(extra, category);
          block[0] = static_cast<double>(dc_predictor[frame_index]) *
                     quant_tables[spec.quant_id][0];
        }

        // AC: run/size pairs.
        int k = 1;
        while (k < 64) {
          std::uint32_t code = 0;
          bool found = false;
          std::uint32_t rs = 0;
          for (int len = 1; len <= 16; ++len) {
            std::uint32_t one = 0;
            if (!entropy.read_bits(1, one)) {
              return record_fail(error, entropy.marker_pending()
                                            ? "entropy data ended at a marker "
                                              "inside a block"
                                            : "entropy data is truncated "
                                              "inside an AC code");
            }
            code = (code << 1U) | one;
            if (ac_table.min_code[len] >= 0 &&
                static_cast<std::int32_t>(code) >= ac_table.min_code[len] &&
                static_cast<std::int32_t>(code) <= ac_table.max_code[len]) {
              const std::uint32_t index =
                  code - static_cast<std::uint32_t>(ac_table.min_code[len]);
              if (index >= ac_table.count[len]) {
                return record_fail(error, "invalid AC Huffman code");
              }
              rs = ac_table.symbols[len][index];
              found = true;
              break;
            }
          }
          if (!found) return record_fail(error, "invalid AC Huffman code");
          const std::uint32_t run = rs >> 4U;
          const std::uint32_t size = rs & 0x0FU;
          if (size == 0U) {
            if (run == 15U) {
              k += 16;  // ZRL
              if (k > 63) {
                return record_fail(error, "ZRL past the end of a block");
              }
            } else {
              break;  // EOB
            }
            continue;
          }
          k += static_cast<int>(run);
          if (k >= 64) {
            return record_fail(error, "AC run past the end of a block");
          }
          if (size > 10U) {
            return record_fail(error, "AC category exceeds 10");
          }
          std::uint32_t extra = 0;
          if (!entropy.read_bits(static_cast<int>(size), extra)) {
            return record_fail(error,
                               "entropy data is truncated in AC extra bits");
          }
          const std::int32_t coefficient = extend_value(extra, size);
          block[kZigzag[k]] = static_cast<double>(coefficient) *
                              quant_tables[spec.quant_id][kZigzag[k]];
          ++k;
        }


        // Write samples into the component plane. IDCT output is level
        // shifted (sample range [0..255] restored by adding 2^(P-1) = 128).
        const std::uint32_t sample_x = mcux * 8U * spec.h +
                                       static_cast<std::uint32_t>(bx) * 8U;
        const std::uint32_t sample_y = mcuy * 8U * spec.v +
                                       static_cast<std::uint32_t>(by) * 8U;
        double* plane_base =
            planes[frame_index].data.data() +
            static_cast<std::size_t>(sample_y) *
                planes[frame_index].row_stride +
            sample_x;

        // Inverse DCT converts the dequantized coefficients back to samples.
        idct_8x8(block);

        for (int sy = 0; sy < 8; ++sy) {
          for (int sx = 0; sx < 8; ++sx) {
            plane_base[static_cast<std::size_t>(sy) *
                           planes[frame_index].row_stride +
                       static_cast<std::size_t>(sx)] =
                block[sy * 8 + sx] + 128.0;
          }
        }
        return true;
      };

      const std::uint64_t mcu_total =
          static_cast<std::uint64_t>(mcus_x) * mcus_y;
      for (std::uint64_t mcu = 0; mcu < mcu_total; ++mcu) {
        // Restart boundary before this MCU?
        if (restart_interval != 0U && mcu != 0U &&
            mcu % restart_interval == 0U) {
          if (!entropy.marker_pending()) {
            if (!entropy.drain_to_marker() || !entropy.marker_pending()) {
              return failf("missing restart marker at the expected boundary");
            }
          }
          const std::uint8_t expected =
              static_cast<std::uint8_t>(kRst0 + (restart_seq & 7U));
          if (entropy.pending_marker() != expected) {
            return failf("restart marker mismatch (expected RST" +
                         std::to_string(restart_seq & 7U) + ")");
          }
          entropy.clear_marker();
          entropy.byte_align();
          restart_seq = static_cast<std::uint8_t>((restart_seq + 1U) & 7U);
          for (int c = 0; c < frame.component_count; ++c) {
            dc_predictor[c] = 0;
          }
        }

        const std::uint32_t mcux =
            static_cast<std::uint32_t>(mcu % mcus_x);
        const std::uint32_t mcuy =
            static_cast<std::uint32_t>(mcu / mcus_x);
        for (int i = 0; i < frame.component_count; ++i) {
          const FrameSpec::Component& spec =
              frame.components[scan_frame_index[i]];
          for (int by = 0; by < spec.v; ++by) {
            for (int bx = 0; bx < spec.h; ++bx) {
              if (!decode_mcu_block(i, mcux, mcuy, bx, by)) return failf(error);
            }
          }
        }
      }

      // The scan ends at a marker; consume any padding and remember it.
      if (entropy.marker_pending() && entropy.pending_marker() >= kRst0 &&
          entropy.pending_marker() <= kRst0 + 7U) {
        return failf("trailing restart marker after the final MCU");
      }
      if (!entropy.marker_pending() && !entropy.drain_to_marker()) {
        return failf("missing EOI after the scan");
      }
      pending_marker = entropy.pending_marker();
      has_pending_marker = true;
      if (pending_marker == 0xFF) return failf("missing EOI after the scan");
      if (pending_marker != kEoi && !(pending_marker >= 0xE0 &&
                                      pending_marker <= 0xEF) &&
          pending_marker != 0xFE) {
        return failf("unexpected marker after the scan");
      }

      // ---------------- color conversion --------------------------------
      // Map planes to Y / Cb / Cr by component id (frame order is free).
      int y_plane = -1;
      int cb_plane = -1;
      int cr_plane = -1;
      for (int c = 0; c < frame.component_count; ++c) {
        if (frame.components[c].id == 1U) y_plane = c;
        if (frame.components[c].id == 2U) cb_plane = c;
        if (frame.components[c].id == 3U) cr_plane = c;
      }
      if (frame.component_count == 3U &&
          (y_plane < 0 || cb_plane < 0 || cr_plane < 0)) {
        return failf("colour frame is missing a Y/Cb/Cr plane");
      }
      // ------------------ chroma upsampling + conversion ---------------
      // Components with h/v = 1 need no expansion. For a 2x ratio the plane
      // is upsampled with triangle filtering (source samples sit at even
      // output positions; odd positions are the average of their neighbours,
      // replicated at the edges) matching libjpeg's "fancy" upsampler. Any
      // other whole ratio falls back to nearest-neighbour replication.
      // Component planes are padded to MCU multiples; only the first
      // ceil(dimension / ratio) samples per row are valid content, so
      // interpolation clamps to the true component edge. Every expanded
      // buffer is laid out with row stride == frame.width and exactly
      // frame.height rows, so the colour conversion below always indexes
      // full-resolution pixels.
      std::vector<double> vertical_buf[3];
      std::vector<double> horizontal_buf[3];
      const double* full_res[3] = {nullptr, nullptr, nullptr};
      std::uint32_t full_stride[3]{};
      for (int c = 0; c < frame.component_count; ++c) {
        const FrameSpec::Component& spec = frame.components[c];
        // h/v are raw sampling factors (how many 8x8 blocks per MCU this
        // component owns); h_ratio/v_ratio are the upsampling ratios against
        // the frame's maximum sampling factor.
        const std::uint32_t h = spec.h_ratio;
        const std::uint32_t v = spec.v_ratio;
        if (h == 1U && v == 1U) {
          full_res[c] = planes[c].data.data();
          full_stride[c] = planes[c].row_stride;
          continue;
        }
        const std::uint32_t cw = (frame.width + h - 1U) / h;
        const std::uint32_t ch = (frame.height + v - 1U) / v;
        const std::uint32_t plane_stride = planes[c].row_stride;
        const double* src = planes[c].data.data();
        const double* row_source = src;
        std::uint32_t row_stride = plane_stride;
        std::uint32_t row_count = ch;
        if (v != 1U) {
          vertical_buf[c].assign(
              static_cast<std::size_t>(frame.height) * frame.width, 0.0);
          for (std::uint32_t y = 0; y < frame.height; ++y) {
            const std::uint32_t s = (y / v) < ch ? y / v : ch - 1U;
            const double* in = src + static_cast<std::size_t>(s) * plane_stride;
            double* out =
                vertical_buf[c].data() +
                static_cast<std::size_t>(y) * frame.width;
            if (v == 2U && (y & 1U) != 0U && s + 1U < ch) {
              // Triangle (fancy) vertical 2x: odd rows interpolate between
              // the source row above and the one below.
              const double* in2 =
                  src + static_cast<std::size_t>(s + 1U) * plane_stride;
              for (std::uint32_t i = 0; i < cw; ++i) {
                out[i] = 0.5 * (in[i] + in2[i]);
              }
            } else {
              for (std::uint32_t i = 0; i < cw; ++i) out[i] = in[i];
            }
          }
          row_source = vertical_buf[c].data();
          row_stride = frame.width;
          row_count = frame.height;
        }
        if (h == 1U) {
          full_res[c] = row_source;
          full_stride[c] = row_stride;
          continue;
        }
        horizontal_buf[c].assign(
            static_cast<std::size_t>(frame.height) * frame.width, 0.0);
        for (std::uint32_t y = 0; y < row_count; ++y) {
          const double* in =
              row_source + static_cast<std::size_t>(y) * row_stride;
          double* out =
              horizontal_buf[c].data() +
              static_cast<std::size_t>(y) * frame.width;
          for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::uint32_t s = x / h < cw ? x / h : cw - 1U;
            const double a = in[s];
            if (h == 2U && (x & 1U) != 0U && s + 1U < cw) {
              out[x] = 0.5 * (a + in[s + 1U]);  // fancy horizontal 2x
            } else {
              out[x] = a;
            }
          }
        }
        full_res[c] = horizontal_buf[c].data();
        full_stride[c] = frame.width;
      }
      const auto sample_full = [&](int plane, std::uint32_t x,
                                   std::uint32_t y) -> double {
        if (plane < 0) return 128.0;
        return full_res[plane][static_cast<std::size_t>(y) *
                                    full_stride[plane] +
                                x];
      };

      for (std::uint32_t y = 0; y < frame.height; ++y) {
        std::uint8_t* out_row =
            image.rgba.data() + static_cast<std::size_t>(y) * frame.width * 4U;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
          std::uint8_t r = 0;
          std::uint8_t g = 0;
          std::uint8_t b = 0;
          if (frame.component_count == 1U) {
            const std::uint8_t grey = clamp_u8(sample_full(y_plane, x, y));
            r = grey;
            g = grey;
            b = grey;
          } else {
            const double yv = sample_full(y_plane, x, y);
            const double cb = sample_full(cb_plane, x, y) - 128.0;
            const double cr = sample_full(cr_plane, x, y) - 128.0;
            r = clamp_u8(yv + 1.402 * cr);
            g = clamp_u8(yv - 0.344136 * cb - 0.714136 * cr);
            b = clamp_u8(yv + 1.772 * cb);
          }
          out_row[x * 4U] = r;
          out_row[x * 4U + 1U] = g;
          out_row[x * 4U + 2U] = b;
          out_row[x * 4U + 3U] = 255;
        }
      }
      continue;  // back to marker dispatch (EOI expected)
    }

    if (marker == kDnl) {
      return failf("DNL (define number of lines) is not supported");
    }
    if (marker >= kRst0 && marker <= kRst0 + 7U) {
      return failf("unexpected restart marker before the scan");
    }
    return failf("unrecognised marker before the scan");
  }
}

}  // namespace warploom::asset
