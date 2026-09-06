//! @file png_decoder.cpp
//! @brief Strict self-contained PNG decoder. See png_decoder.hpp.

#include "engine/asset/png_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace omnicpp::asset {

namespace {

using omnicpp::core::RuntimeError;

constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

// ============================================================================
// CRC-32 (ISO 3309 / PNG spec), reflected polynomial 0xEDB88320
// ============================================================================

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
  static const std::array<std::uint32_t, 256> kTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0U ? 0xedb88320U ^ (c >> 1) : c >> 1;
      }
      table[n] = c;
    }
    return table;
  }();
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc = kTable[(crc ^ data[i]) & 0xffU] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffU;
}

std::uint32_t read_be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}

// ============================================================================
// DEFLATE (RFC 1951) decoder — canonical Huffman decoding.
// ============================================================================

//! LSB-first bit reader over a byte buffer.
class BitReader final {
public:
  BitReader(const std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  //! Read up to 16 bits (LSB-first). Returns false when the input is exhausted.
  [[nodiscard]] bool read(std::uint32_t& out, int need) noexcept {
    while (bit_count_ < need) {
      if (pos_ >= size_) return false;
      bit_buf_ |= static_cast<std::uint32_t>(data_[pos_++]) << bit_count_;
      bit_count_ += 8;
    }
    out = bit_buf_ & ((std::uint32_t{1} << need) - 1U);
    bit_buf_ >>= need;
    bit_count_ -= need;
    return true;
  }

  //! Discard bits up to the next byte boundary (stored-block alignment).
  void byte_align() noexcept {
    const int drop = bit_count_ & 7;
    bit_buf_ >>= drop;
    bit_count_ -= drop;
  }

private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_{0};
  std::uint32_t bit_buf_{0};
  int bit_count_{0};
};

struct Huffman {
  //! count[len] = number of codes of length len (len in 1..15; [0] unused).
  std::uint16_t count[16]{};
  //! Canonical symbol table sorted by (code length, code value).
  std::uint16_t symbol[288]{};
};

//! Build a canonical Huffman table from per-symbol code lengths.
//! Rejects over-subscribed tables and tables with no codes at all.
[[nodiscard]] bool build_huffman(Huffman& h, const std::uint8_t* lengths, int n,
                                 std::string& error) {
  std::memset(h.count, 0, sizeof(h.count));
  for (int i = 0; i < n; ++i) {
    if (lengths[i] > 15U) {
      error = "Huffman code length exceeds 15 bits";
      return false;
    }
    ++h.count[lengths[i]];
  }
  if (h.count[0] == static_cast<std::uint16_t>(n)) {
    error = "Huffman table has no codes";
    return false;
  }
  // Over-subscription check: after assigning all codes of a given length,
  // at least one prefix must remain for longer codes.
  int left = 1;
  for (int len = 1; len <= 15; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) {
      error = "Huffman table is over-subscribed";
      return false;
    }
  }
  std::uint16_t offsets[16]{};
  for (int len = 1; len < 15; ++len) {
    offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + h.count[len]);
  }
  for (int i = 0; i < n; ++i) {
    if (lengths[i] != 0U) {
      h.symbol[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }
  }
  return true;
}

//! Decode one symbol. Returns -1 on error (truncation or invalid code).
int decode_symbol(BitReader& r, const Huffman& h, std::string& error) noexcept {
  int code = 0;
  int first = 0;
  int index = 0;
  for (int len = 1; len <= 15; ++len) {
    std::uint32_t bit = 0;
    if (!r.read(bit, 1)) {
      error = "truncated DEFLATE stream";
      return -1;
    }
    code |= static_cast<int>(bit);
    const int count = h.count[len];
    if (code - first < count) {
      return h.symbol[index + (code - first)];
    }
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  error = "invalid Huffman code in DEFLATE stream";
  return -1;
}

const std::array<std::uint16_t, 29>& length_base() noexcept {
  static const std::array<std::uint16_t, 29> kBase = {
      3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  return kBase;
}
const std::array<std::uint8_t, 29>& length_extra() noexcept {
  static const std::array<std::uint8_t, 29> kExtra = {
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  return kExtra;
}
const std::array<std::uint16_t, 30>& distance_base() noexcept {
  static const std::array<std::uint16_t, 30> kBase = {
      1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
      257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
      12289, 16385, 24577};
  return kBase;
}
const std::array<std::uint8_t, 30>& distance_extra() noexcept {
  static const std::array<std::uint8_t, 30> kExtra = {
      0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
      7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  return kExtra;
}

//! Decode one block's literal/length/distance symbols into `out`.
//! `out_max` caps output so a corrupt stream cannot over-allocate.
[[nodiscard]] bool run_codes(BitReader& r, const Huffman& lit, const Huffman& dist,
                             std::size_t out_max, std::vector<std::uint8_t>& out,
                             std::string& error) {
  for (;;) {
    const int symbol = decode_symbol(r, lit, error);
    if (symbol < 0) return false;
    if (symbol < 256) {
      if (out.size() >= out_max) {
        error = "DEFLATE output exceeds the declared image size";
        return false;
      }
      out.push_back(static_cast<std::uint8_t>(symbol));
      continue;
    }
    if (symbol == 256) return true;  // End of block.
    if (symbol > 285) {
      error = "invalid length symbol in DEFLATE stream";
      return false;
    }
    const std::size_t len_index = static_cast<std::size_t>(symbol - 257);
    std::uint32_t extra = 0;
    if (length_extra()[len_index] != 0U &&
        !r.read(extra, length_extra()[len_index])) {
      error = "truncated DEFLATE stream";
      return false;
    }
    const int length = length_base()[len_index] + static_cast<int>(extra);
    const int dist_symbol = decode_symbol(r, dist, error);
    if (dist_symbol < 0) return false;
    if (dist_symbol > 29) {
      error = "invalid distance symbol in DEFLATE stream";
      return false;
    }
    const std::size_t dist_index = static_cast<std::size_t>(dist_symbol);
    // `extra` may still hold the length's extra bits from above; zero-extra
    // distance codes must not inherit them.
    extra = 0;
    if (distance_extra()[dist_index] != 0U &&
        !r.read(extra, distance_extra()[dist_index])) {
      error = "truncated DEFLATE stream";
      return false;
    }
    const std::size_t distance =
        distance_base()[dist_index] + static_cast<std::size_t>(extra);
    if (distance > out.size() ||
        length > static_cast<int>(out_max - out.size())) {
      error = "DEFLATE match exceeds the emitted output";
      return false;
    }
    const std::size_t start = out.size() - distance;
    for (int i = 0; i < length; ++i) {
      out.push_back(out[start + static_cast<std::size_t>(i)]);
    }
  }
}

const Huffman& fixed_literal_table() noexcept {
  static const Huffman kTable = [] {
    Huffman h{};
    std::array<std::uint8_t, 288> lengths{};
    for (std::size_t i = 0; i < 144; ++i) lengths[i] = 8;
    for (std::size_t i = 144; i < 256; ++i) lengths[i] = 9;
    for (std::size_t i = 256; i < 280; ++i) lengths[i] = 7;
    for (std::size_t i = 280; i < 288; ++i) lengths[i] = 8;
    std::string error;
    (void)build_huffman(h, lengths.data(), static_cast<int>(lengths.size()), error);
    return h;
  }();
  return kTable;
}

const Huffman& fixed_distance_table() noexcept {
  static const Huffman kTable = [] {
    Huffman h{};
    std::array<std::uint8_t, 30> lengths{};
    lengths.fill(5);
    std::string error;
    (void)build_huffman(h, lengths.data(), static_cast<int>(lengths.size()), error);
    return h;
  }();
  return kTable;
}

//! Inflate a zlib stream (RFC 1950 wrapper + RFC 1951 DEFLATE) into `out`.
[[nodiscard]] bool inflate(const std::uint8_t* data, std::size_t size,
                           std::size_t expected, std::vector<std::uint8_t>& out,
                           std::string& error) {
  if (size < 2) {
    error = "truncated zlib header";
    return false;
  }
  const int cmf = data[0];
  const int flg = data[1];
  if ((cmf & 0x0f) != 8) {
    error = "invalid zlib compression method";
    return false;
  }
  if ((cmf >> 4) > 7) {
    error = "zlib window size exceeds 32 KiB";
    return false;
  }
  if ((flg & 0x20) != 0) {
    error = "zlib preset dictionaries are unsupported";
    return false;
  }
  if (((cmf << 8) | flg) % 31 != 0) {
    error = "invalid zlib header checksum";
    return false;
  }
  BitReader r(data + 2, size - 2);
  for (;;) {
    std::uint32_t final_block = 0;
    std::uint32_t block_type = 0;
    if (!r.read(final_block, 1) || !r.read(block_type, 2)) {
      error = "truncated DEFLATE block header";
      return false;
    }
    if (block_type == 0U) {
      // Stored block.
      r.byte_align();
      std::uint32_t len = 0;
      std::uint32_t nlen = 0;
      if (!r.read(len, 16) || !r.read(nlen, 16)) {
        error = "truncated stored block header";
        return false;
      }
      if ((len ^ 0xffffU) != nlen) {
        error = "stored block length complement mismatch";
        return false;
      }
      for (std::uint32_t i = 0; i < len; ++i) {
        if (out.size() >= expected) {
          error = "stored block exceeds the declared image size";
          return false;
        }
        std::uint32_t byte = 0;
        if (!r.read(byte, 8)) {
          error = "truncated stored block data";
          return false;
        }
        out.push_back(static_cast<std::uint8_t>(byte));
      }
    } else if (block_type == 1U) {
      if (!run_codes(r, fixed_literal_table(), fixed_distance_table(),
                     expected, out, error)) {
        return false;
      }
    } else if (block_type == 2U) {
      // Dynamic Huffman block.
      std::uint32_t hlit = 0, hdist = 0, hclen = 0;
      if (!r.read(hlit, 5) || !r.read(hdist, 5) || !r.read(hclen, 4)) {
        error = "truncated dynamic block header";
        return false;
      }
      const int literal_count = static_cast<int>(hlit) + 257;
      const int distance_count = static_cast<int>(hdist) + 1;
      const int code_length_count = static_cast<int>(hclen) + 4;
      static constexpr std::uint8_t kCodeLengthOrder[19] = {
          16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      std::array<std::uint8_t, 19> code_length_lengths{};
      for (int i = 0; i < code_length_count; ++i) {
        std::uint32_t len = 0;
        if (!r.read(len, 3)) {
          error = "truncated dynamic block header";
          return false;
        }
        code_length_lengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(len);
      }
      // The code-length alphabet is symbols 0..18 regardless of how many
      // lengths the header transmitted, so the full 19-entry array is scanned.
      Huffman code_length_table{};
      if (!build_huffman(code_length_table, code_length_lengths.data(), 19,
                         error)) {
        return false;
      }
      const int total = literal_count + distance_count;
      std::vector<std::uint8_t> lengths(static_cast<std::size_t>(total), 0);
      int index = 0;
      while (index < total) {
        const int symbol = decode_symbol(r, code_length_table, error);
        if (symbol < 0) return false;
        std::uint32_t extra = 0;
        if (symbol < 16) {
          lengths[static_cast<std::size_t>(index++)] =
              static_cast<std::uint8_t>(symbol);
          continue;
        }
        int repeat = 0;
        if (symbol == 16) {
          if (index == 0) {
            error = "code-length repeat 16 with no previous length";
            return false;
          }
          if (!r.read(extra, 2)) {
            error = "truncated dynamic block header";
            return false;
          }
          repeat = 3 + static_cast<int>(extra);
          const std::uint8_t value = lengths[static_cast<std::size_t>(index - 1)];
          if (value == 0U) {
            error = "code-length repeat 16 after a zero length";
            return false;
          }
          if (index + repeat > total) {
            error = "code-length repeat overruns the table";
            return false;
          }
          while (repeat-- > 0) {
            lengths[static_cast<std::size_t>(index++)] = value;
          }
        } else if (symbol == 17) {
          if (!r.read(extra, 3)) {
            error = "truncated dynamic block header";
            return false;
          }
          repeat = 3 + static_cast<int>(extra);
          if (index + repeat > total) {
            error = "code-length repeat overruns the table";
            return false;
          }
          while (repeat-- > 0) {
            lengths[static_cast<std::size_t>(index++)] = 0;
          }
        } else {  // symbol == 18
          if (!r.read(extra, 7)) {
            error = "truncated dynamic block header";
            return false;
          }
          repeat = 11 + static_cast<int>(extra);
          if (index + repeat > total) {
            error = "code-length repeat overruns the table";
            return false;
          }
          while (repeat-- > 0) {
            lengths[static_cast<std::size_t>(index++)] = 0;
          }
        }
      }
      Huffman literal_table{};
      Huffman distance_table{};
      if (!build_huffman(literal_table, lengths.data(), literal_count, error)) {
        return false;
      }
      if (!build_huffman(distance_table, lengths.data() + literal_count,
                         distance_count, error)) {
        return false;
      }
      if (!run_codes(r, literal_table, distance_table, expected, out, error)) {
        return false;
      }
    } else {
      error = "invalid DEFLATE block type 3";
      return false;
    }
    if (final_block != 0U) break;
  }
  return true;
}

// ============================================================================
// PNG parsing helpers
// ============================================================================

[[nodiscard]] int channels_for(std::uint8_t colour_type) {
  switch (colour_type) {
    case 0: return 1;  // Greyscale.
    case 2: return 3;  // Truecolour.
    case 3: return 1;  // Indexed-colour.
    case 4: return 2;  // Greyscale + alpha.
    case 6: return 4;  // Truecolour + alpha.
    default: return -1;
  }
}

//! Reconstruct one scanline (filtered -> unfiltered) and expand it to RGBA,
//! appending to `rgba`. `stream` is the whole filtered byte stream and
//! `row_start` points at the row's leading filter-type byte.
[[nodiscard]] bool emit_row(const std::uint8_t* stream, std::size_t row_start,
                            std::size_t stride, std::uint8_t colour_type,
                            const std::vector<std::uint8_t>& palette,
                            const std::vector<std::uint8_t>& trns,
                            const std::uint8_t* previous, std::uint8_t* row,
                            std::vector<std::uint8_t>& rgba, std::string& error) {
  const int bpp = channels_for(colour_type);
  if (bpp < 0) {
    error = "unsupported PNG colour type";
    return false;
  }
  const std::uint8_t bytes_per_pixel = static_cast<std::uint8_t>(bpp);
  const std::uint8_t filter_type = stream[row_start];
  const std::uint8_t* filtered = stream + row_start + 1;

  for (std::size_t i = 0; i < stride; ++i) {
    const int raw = filtered[i];
    const int left = i >= bytes_per_pixel ? row[i - bytes_per_pixel] : 0;
    const int up = previous[i];
    const int up_left = i >= bytes_per_pixel ? previous[i - bytes_per_pixel] : 0;
    int value = 0;
    switch (filter_type) {
      case 0: value = raw; break;
      case 1: value = raw + left; break;
      case 2: value = raw + up; break;
      case 3: value = raw + ((left + up) >> 1); break;
      case 4: {
        const int estimate = left + up - up_left;
        const int pa = std::abs(estimate - left);
        const int pb = std::abs(estimate - up);
        const int pc = std::abs(estimate - up_left);
        const int predictor =
            (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : up_left);
        value = raw + predictor;
        break;
      }
      default:
        error = "invalid PNG scanline filter type";
        return false;
    }
    row[i] = static_cast<std::uint8_t>(value & 0xff);
  }

  // Expand to RGBA (bit depth is always 8; enforced at IHDR).
  const std::size_t pixel_count = stride / static_cast<std::size_t>(bpp);
  rgba.reserve(rgba.size() + pixel_count * 4U);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    const std::uint8_t* p = row + i * static_cast<std::size_t>(bpp);
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    switch (colour_type) {
      case 0:  // Greyscale.
        r = g = b = p[0];
        // tRNS keys are 16-bit; at 8-bit depth valid keys are 0..255 stored
        // zero-extended, so the value lives in the low byte (high byte 0).
        if (trns.size() == 2U && p[0] == trns[1]) a = 0;
        break;
      case 2:  // Truecolour.
        r = p[0];
        g = p[1];
        b = p[2];
        if (trns.size() == 6U && p[0] == trns[1] && p[1] == trns[3] &&
            p[2] == trns[5]) {
          a = 0;
        }
        break;
      case 3: {  // Indexed-colour.
        const std::size_t entry = static_cast<std::size_t>(p[0]) * 3U;
        if (static_cast<std::size_t>(p[0]) >= palette.size() / 3U) {
          error = "PNG palette index out of range";
          return false;
        }
        r = palette[entry];
        g = palette[entry + 1];
        b = palette[entry + 2];
        a = static_cast<std::size_t>(p[0]) < trns.size() ? trns[p[0]] : 255;
        break;
      }
      case 4:  // Greyscale + alpha.
        r = g = b = p[0];
        a = p[1];
        break;
      case 6:  // Truecolour + alpha.
        r = p[0];
        g = p[1];
        b = p[2];
        a = p[3];
        break;
      default:
        error = "unsupported PNG colour type";
        return false;
    }
    rgba.push_back(r);
    rgba.push_back(g);
    rgba.push_back(b);
    rgba.push_back(a);
  }
  return true;
}

} // namespace

// ============================================================================
// Public entry point
// ============================================================================

omnicpp::core::Result<DecodedImage> decode_png(const std::uint8_t* bytes,
                                               std::size_t length,
                                               std::string* error_detail) {
  auto fail = [error_detail](const char* message) {
    if (error_detail != nullptr) *error_detail = message;
    return omnicpp::core::Result<DecodedImage>::error(
        RuntimeError::malformed_asset);
  };
  if (bytes == nullptr || length == 0) {
    return fail("empty PNG input");
  }
  if (length < sizeof(kSignature)) {
    return fail("truncated PNG signature");
  }
  if (std::memcmp(bytes, kSignature, sizeof(kSignature)) != 0) {
    return fail("invalid PNG signature");
  }

  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t colour_type = 0;
  bool have_ihdr = false;
  bool have_plte = false;
  bool have_trns = false;
  bool have_idat = false;
  bool have_iend = false;
  bool previous_was_idat = false;
  std::vector<std::uint8_t> palette;
  std::vector<std::uint8_t> trns;
  std::vector<std::uint8_t> idat;

  // --- Chunk walk -----------------------------------------------------------
  std::size_t pos = sizeof(kSignature);
  while (pos < length) {
    if (length - pos < 12U) {
      return fail("truncated PNG chunk header");
    }
    const std::size_t chunk_start = pos;
    const std::uint8_t* header = bytes + pos;
    const std::uint32_t chunk_length = read_be32(header);
    const std::uint8_t* type = header + 4;
    const std::uint8_t* chunk_data = header + 8;
    if (chunk_length > length - chunk_start - 8U) {
      return fail("truncated PNG chunk data");
    }
    // Strict: every chunk's CRC must verify — type and data are contiguous in
    // the file buffer, so a single CRC pass over both is valid.
    const std::uint32_t stored_crc = read_be32(chunk_data + chunk_length);
    const std::uint32_t actual_crc = crc32(type, 4U + chunk_length);
    if (actual_crc != stored_crc) {
      std::string message = "PNG CRC mismatch in chunk ";
      message.append(reinterpret_cast<const char*>(type), 4);
      return fail(message.c_str());
    }
    pos = chunk_start + 8U + chunk_length + 4U;

    const bool is_idat =
        type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T';
    const bool is_iend =
        type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D';

    if (have_idat && !is_idat && !is_iend) {
      return fail("IDAT chunks must be consecutive");
    }

    // --- IHDR ---------------------------------------------------------------
    if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
      if (have_ihdr || chunk_start != sizeof(kSignature)) {
        return fail("IHDR must be the first chunk and appear once");
      }
      if (chunk_length != 13U) {
        return fail("IHDR must be 13 bytes");
      }
      have_ihdr = true;
      width = read_be32(chunk_data);
      height = read_be32(chunk_data + 4);
      const std::uint8_t bit_depth = chunk_data[8];
      colour_type = chunk_data[9];
      if (width == 0U || height == 0U) {
        return fail("PNG dimensions must be non-zero");
      }
      if (bit_depth != 8U) {
        return fail("only 8-bit PNG samples are supported");
      }
      if (channels_for(colour_type) < 0) {
        return fail("unsupported PNG colour type");
      }
      if (chunk_data[10] != 0U) {
        return fail("unsupported PNG compression method");
      }
      if (chunk_data[11] != 0U) {
        return fail("unsupported PNG filter method");
      }
      if (chunk_data[12] == 1U) {
        return fail("interlaced (Adam7) PNG is unsupported");
      }
      if (chunk_data[12] != 0U) {
        return fail("invalid PNG interlace method");
      }
      const std::uint64_t decoded_bytes =
          static_cast<std::uint64_t>(width) * height * 4U;
      if (decoded_bytes > kMaxDecodedPngBytes) {
        return fail("PNG dimensions exceed the decoded-size cap");
      }
      previous_was_idat = false;
      continue;
    }

    if (!have_ihdr) {
      return fail("PNG data precedes IHDR");
    }

    // --- PLTE ---------------------------------------------------------------
    if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
      if (have_plte) {
        return fail("duplicate PLTE chunk");
      }
      if (chunk_length == 0U || chunk_length % 3U != 0U ||
          chunk_length > 256U * 3U) {
        return fail("PLTE must hold 1..256 RGB entries");
      }
      have_plte = true;
      palette.assign(chunk_data, chunk_data + chunk_length);
      previous_was_idat = false;
      continue;
    }

    // --- tRNS ---------------------------------------------------------------
    if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S') {
      if (have_trns) {
        return fail("duplicate tRNS chunk");
      }
      if (colour_type == 3U) {
        if (!have_plte || chunk_length > palette.size()) {
          return fail("tRNS must follow PLTE and fit the palette");
        }
      } else if (colour_type == 0U) {
        if (chunk_length != 2U) {
          return fail("greyscale tRNS must hold one 16-bit value");
        }
      } else if (colour_type == 2U) {
        if (chunk_length != 6U) {
          return fail("truecolour tRNS must hold three 16-bit values");
        }
      } else {
        return fail("tRNS is not allowed for colour types 4 and 6");
      }
      have_trns = true;
      trns.assign(chunk_data, chunk_data + chunk_length);
      previous_was_idat = false;
      continue;
    }

    // --- IDAT ---------------------------------------------------------------
    if (is_idat) {
      if (colour_type == 3U && !have_plte) {
        return fail("indexed-colour PNG requires a PLTE chunk");
      }
      have_idat = true;
      idat.insert(idat.end(), chunk_data, chunk_data + chunk_length);
      previous_was_idat = true;
      continue;
    }

    // --- IEND ---------------------------------------------------------------
    if (is_iend) {
      if (chunk_length != 0U) {
        return fail("IEND must be empty");
      }
      have_iend = true;
      break;
    }

    // --- Unknown chunks -----------------------------------------------------
    if ((type[0] & 0x20U) == 0U) {  // Critical chunk (uppercase first letter).
      std::string message = "unknown critical PNG chunk ";
      message.append(reinterpret_cast<const char*>(type), 4);
      return fail(message.c_str());
    }
    // Ancillary chunks (pHYs, tEXt, gAMA, ...) were CRC-verified above; they
    // carry no colour data and are ignored.
    previous_was_idat = false;
  }

  // --- Post-walk validation -------------------------------------------------
  if (!have_ihdr) {
    return fail("missing IHDR chunk");
  }
  if (!have_idat) {
    return fail("missing IDAT chunk");
  }
  if (!have_iend) {
    return fail("missing IEND chunk");
  }
  if (colour_type == 3U && !have_plte) {
    return fail("indexed-colour PNG requires a PLTE chunk");
  }
  (void)previous_was_idat;

  // --- Inflate --------------------------------------------------------------
  const int bpp = channels_for(colour_type);
  const std::size_t stride = static_cast<std::size_t>(width) *
                             static_cast<std::size_t>(bpp);
  const std::size_t expected = static_cast<std::size_t>(height) * (stride + 1U);
  std::vector<std::uint8_t> filtered;
  filtered.reserve(expected);
  {
    std::string inflate_error;
    if (!inflate(idat.data(), idat.size(), expected, filtered, inflate_error)) {
      if (error_detail != nullptr) *error_detail = inflate_error;
      return omnicpp::core::Result<DecodedImage>::error(
          RuntimeError::malformed_asset);
    }
  }
  if (filtered.size() != expected) {
    std::string message = "IDAT decodes to ";
    message += std::to_string(filtered.size());
    message += " bytes but ";
    message += std::to_string(expected);
    message += " are required";
    return fail(message.c_str());
  }

  // --- Unfilter and expand to RGBA ------------------------------------------
  DecodedImage image;
  image.width = width;
  image.height = height;
  image.rgba.reserve(static_cast<std::size_t>(width) * height * 4U);
  std::vector<std::uint8_t> previous(stride, 0);
  std::vector<std::uint8_t> row(stride, 0);
  std::string row_error;
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::size_t row_start = static_cast<std::size_t>(y) * (stride + 1U);
    if (!emit_row(filtered.data(), row_start, stride, colour_type, palette, trns,
                  previous.data(), row.data(), image.rgba, row_error)) {
      if (error_detail != nullptr) *error_detail = row_error;
      return omnicpp::core::Result<DecodedImage>::error(
          RuntimeError::malformed_asset);
    }
    std::swap(previous, row);
    std::fill(row.begin(), row.end(), 0);
  }
  return omnicpp::core::Result<DecodedImage>::ok(std::move(image));
}

} // namespace omnicpp::asset
