//! @file test_png_decoder.cpp
//! @brief Deterministic CPU tests for the self-contained PNG decoder:
//!        real-world zlib streams (stored/fixed/dynamic Huffman), colour
//!        types 0/2/3/4/6, palette + tRNS, transparency keys, IDAT chunk
//!        splitting, ancillary-chunk tolerance, and strict rejection of
//!        malformed input.
//!
//! The five golden blobs below were produced by an independent encoder
//! (Python's zlib, itself RFC 1950/1951 compliant) so the DEFLATE paths are
//! tested against a foreign implementation rather than a mirror.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/asset/png_decoder.hpp"

namespace {

using omnicpp::asset::DecodedImage;
using omnicpp::asset::decode_png;
using omnicpp::core::RuntimeError;

// ============================================================================
// Golden blobs from Python zlib (independent encoder)
// ============================================================================

// 4x4 truecolour+alpha (colour type 6) compressed with default dynamic Huffman.
constexpr std::uint8_t kPngGoldenDynamic[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04,
    0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e, 0x7e, 0x00, 0x00, 0x00,
    0x3d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x19, 0xfe, 0x83, 0x01, 0x03, 0x12, 0xfa, 0xcf, 0x25, 0x22,
    0xa7, 0xc1, 0x70, 0x22, 0xc5, 0xe8, 0x3f, 0x23, 0x13, 0x33, 0xcb, 0xaf,
    0x0f, 0xcf, 0xee, 0x34, 0x38, 0x28, 0x08, 0x30, 0x70, 0x26, 0x1f, 0xe7,
    0x64, 0x67, 0x63, 0x65, 0x61, 0x66, 0x62, 0x64, 0xf0, 0x8d, 0x48, 0xce,
    0x07, 0x00, 0xa8, 0xbc, 0x1d, 0x24, 0x78, 0xb0, 0xb5, 0x3c, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

// Same image compressed with the fixed-Huffman strategy (zlib-wrapped).
constexpr std::uint8_t kPngGoldenFixed[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04,
    0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e, 0x7e, 0x00, 0x00, 0x00,
    0x3d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x19, 0xfe, 0x83, 0x01, 0x03, 0x12, 0xfa, 0xcf, 0x25, 0x22,
    0xa7, 0xc1, 0x70, 0x22, 0xc5, 0xe8, 0x3f, 0x23, 0x13, 0x33, 0xcb, 0xaf,
    0x0f, 0xcf, 0xee, 0x34, 0x38, 0x28, 0x08, 0x30, 0x70, 0x26, 0x1f, 0xe7,
    0x64, 0x67, 0x63, 0x65, 0x61, 0x66, 0x62, 0x64, 0xf0, 0x8d, 0x48, 0xce,
    0x07, 0x00, 0xa8, 0xbc, 0x1d, 0x24, 0xb2, 0x2e, 0x5a, 0xde, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

// Same image compressed at level 0 (stored DEFLATE blocks).
constexpr std::uint8_t kPngGoldenStored[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04,
    0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e, 0x7e, 0x00, 0x00, 0x00,
    0x4f, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x01, 0x44, 0x00, 0xbb, 0xff,
    0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00,
    0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x0a, 0x14, 0x1e, 0x28, 0x00, 0xc8,
    0x64, 0x32, 0xff, 0x01, 0x02, 0x03, 0x04, 0xfa, 0xf0, 0xe6, 0xdc, 0x80,
    0x40, 0x20, 0x10, 0x00, 0x09, 0x63, 0xc7, 0x09, 0x07, 0x06, 0x05, 0x04,
    0x03, 0x02, 0x01, 0x00, 0x4d, 0x58, 0x63, 0x6f, 0xa8, 0xbc, 0x1d, 0x24,
    0x13, 0x62, 0x22, 0xb3, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0xae, 0x42, 0x60, 0x82,
};

// 3x2 indexed-colour (colour type 3) with a 4-entry PLTE and tRNS.
constexpr std::uint8_t kPngGoldenPalette[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x03, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0x96, 0x28, 0x00, 0x00, 0x00,
    0x0c, 0x50, 0x4c, 0x54, 0x45, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0xff, 0x1e, 0x3c, 0x5a, 0x15, 0x4b, 0x09, 0x8b, 0x00, 0x00, 0x00,
    0x04, 0x74, 0x52, 0x4e, 0x53, 0xff, 0x80, 0x00, 0xc8, 0x19, 0x1c, 0x31,
    0x69, 0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63,
    0x60, 0x60, 0x64, 0x62, 0x60, 0x66, 0x60, 0x04, 0x00, 0x00, 0x22, 0x00,
    0x08, 0x8d, 0x85, 0x7e, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82,
};

// 5x5 greyscale+alpha (colour type 4); rows 0..4 use PNG filters
// 0..4 (None/Sub/Up/Average/Paeth) respectively — exercises unfiltering
// against an independent encoder.
constexpr std::uint8_t kPngGoldenGrayAlpha[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05,
    0x08, 0x04, 0x00, 0x00, 0x00, 0x27, 0x66, 0xee, 0x6e, 0x00, 0x00, 0x00,
    0x2a, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0xf8, 0xaf, 0xf1,
    0x30, 0xe0, 0x70, 0xc5, 0xd2, 0x05, 0xed, 0x8c, 0xbc, 0x6c, 0x1a, 0x8f,
    0x20, 0x90, 0x89, 0x97, 0x1d, 0x06, 0x99, 0xa5, 0xf8, 0xa4, 0x4b, 0xa5,
    0xbf, 0x82, 0x20, 0x0b, 0x42, 0x14, 0x00, 0xee, 0x14, 0x0e, 0x55, 0x71,
    0x73, 0x70, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82,
};

// Real-world files produced by Pillow (PIL 12.3) — a mainstream encoder that
// writes its own dynamic-Huffman streams and filter selection. Content is a
// formula grid so expectations stay tiny while the decode path (filters,
// dynamic Huffman, palette expansion) is exercised by a foreign encoder.

// 8x4 truecolour: r=(x*31+y*3), g=(y*61+x*7), b=(x*13+y*41) (all mod 256).
constexpr std::uint8_t kPngPilRgbGradient[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x3c, 0xaf, 0xe9, 0xa7, 0x00, 0x00, 0x00,
    0x1b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x64, 0x60, 0x60, 0x90,
    0x67, 0xe7, 0xc5, 0x44, 0x2c, 0xcc, 0xb6, 0x9a, 0xcc, 0xec, 0xbc, 0x98,
    0x88, 0x74, 0x09, 0x00, 0xf9, 0x44, 0x04, 0x91, 0x4c, 0x0e, 0x14, 0x3d,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

// 8x2 truecolour+alpha: r=(x*37), g=(y*97), b=(x+y*5), a=(255-x*29) mod 256.
constexpr std::uint8_t kPngPilRgbaGradient[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x65, 0x94, 0x9d, 0xed, 0x00, 0x00, 0x00,
    0x1c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x64, 0x60, 0x60, 0xf8,
    0xaf, 0xca, 0xc0, 0xf8, 0x18, 0x17, 0x66, 0x61, 0x48, 0x64, 0x65, 0x60,
    0x60, 0x60, 0xc4, 0x89, 0x01, 0x9b, 0xf2, 0x08, 0xb1, 0x94, 0xdf, 0x56,
    0x7c, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82,
};

// 8x2 indexed-colour, 32-entry palette; entry v=(x*3+y)%32 has colour
// (v*7, v*11, v*17) mod 256 and tRNS alpha 0 for entry 5 (255 otherwise).
constexpr std::uint8_t kPngPilPalette[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x03, 0x00, 0x00, 0x00, 0x52, 0x4a, 0x6d, 0xdf, 0x00, 0x00, 0x00,
    0x60, 0x50, 0x4c, 0x54, 0x45, 0x00, 0x00, 0x00, 0x07, 0x0b, 0x11, 0x0e,
    0x16, 0x22, 0x15, 0x21, 0x33, 0x1c, 0x2c, 0x44, 0x23, 0x37, 0x55, 0x2a,
    0x42, 0x66, 0x31, 0x4d, 0x77, 0x38, 0x58, 0x88, 0x3f, 0x63, 0x99, 0x46,
    0x6e, 0xaa, 0x4d, 0x79, 0xbb, 0x54, 0x84, 0xcc, 0x5b, 0x8f, 0xdd, 0x62,
    0x9a, 0xee, 0x69, 0xa5, 0xff, 0x70, 0xb0, 0x10, 0x77, 0xbb, 0x21, 0x7e,
    0xc6, 0x32, 0x85, 0xd1, 0x43, 0x8c, 0xdc, 0x54, 0x93, 0xe7, 0x65, 0x9a,
    0xf2, 0x76, 0xa1, 0xfd, 0x87, 0xa8, 0x08, 0x98, 0xaf, 0x13, 0xa9, 0xb6,
    0x1e, 0xba, 0xbd, 0x29, 0xcb, 0xc4, 0x34, 0xdc, 0xcb, 0x3f, 0xed, 0xd2,
    0x4a, 0xfe, 0xd9, 0x55, 0x0f, 0x70, 0x0f, 0xbb, 0x8b, 0x00, 0x00, 0x00,
    0x06, 0x74, 0x52, 0x4e, 0x53, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xb3,
    0xbf, 0xa4, 0xbf, 0x00, 0x00, 0x00, 0x1a, 0x49, 0x44, 0x41, 0x54, 0x78,
    0x9c, 0x63, 0x60, 0x60, 0x66, 0xe3, 0xe4, 0xe1, 0x17, 0x12, 0x65, 0x60,
    0x64, 0x61, 0xe7, 0xe2, 0x15, 0x10, 0x16, 0x03, 0x00, 0x05, 0x22, 0x00,
    0xb1, 0x1c, 0x89, 0xe5, 0x29, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82,
};

// 6x3 greyscale: v=(x*40+y*90) mod 256.
constexpr std::uint8_t kPngPilGray[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x03,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x95, 0x6a, 0x21, 0x27, 0x00, 0x00, 0x00,
    0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x64, 0xd0, 0x00, 0x01,
    0xc6, 0x28, 0x08, 0xb5, 0x05, 0x4c, 0x01, 0x00, 0x20, 0x79, 0x03, 0x6a,
    0x58, 0xd3, 0x46, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0xae, 0x42, 0x60, 0x82,
};

// ============================================================================
// Test-side PNG builder (stored DEFLATE blocks only; zlib header 0x78 0x01)
// ============================================================================

std::uint32_t builder_crc32(const std::uint8_t* data, std::size_t size) {
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

struct TestChunk {
  std::array<char, 4> type{};
  std::vector<std::uint8_t> data{};
};

void append_chunk(std::vector<std::uint8_t>& out, const char* type,
                  const std::vector<std::uint8_t>& data) {
  const std::uint32_t length = static_cast<std::uint32_t>(data.size());
  out.push_back(static_cast<std::uint8_t>(length >> 24));
  out.push_back(static_cast<std::uint8_t>(length >> 16));
  out.push_back(static_cast<std::uint8_t>(length >> 8));
  out.push_back(static_cast<std::uint8_t>(length));
  const std::size_t type_offset = out.size();
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(type[i]));
  out.insert(out.end(), data.begin(), data.end());
  const std::uint32_t crc = builder_crc32(out.data() + type_offset,
                                          out.size() - type_offset);
  out.push_back(static_cast<std::uint8_t>(crc >> 24));
  out.push_back(static_cast<std::uint8_t>(crc >> 16));
  out.push_back(static_cast<std::uint8_t>(crc >> 8));
  out.push_back(static_cast<std::uint8_t>(crc));
}

std::vector<std::uint8_t> ihdr(std::uint32_t width, std::uint32_t height,
                               std::uint8_t colour_type,
                               std::uint8_t bit_depth = 8,
                               std::uint8_t interlace = 0) {
  std::vector<std::uint8_t> data;
  data.push_back(static_cast<std::uint8_t>(width >> 24));
  data.push_back(static_cast<std::uint8_t>(width >> 16));
  data.push_back(static_cast<std::uint8_t>(width >> 8));
  data.push_back(static_cast<std::uint8_t>(width));
  data.push_back(static_cast<std::uint8_t>(height >> 24));
  data.push_back(static_cast<std::uint8_t>(height >> 16));
  data.push_back(static_cast<std::uint8_t>(height >> 8));
  data.push_back(static_cast<std::uint8_t>(height));
  data.push_back(bit_depth);
  data.push_back(colour_type);
  data.push_back(0);  // compression
  data.push_back(0);  // filter
  data.push_back(interlace);
  return data;
}

//! zlib-wrapped DEFLATE stored blocks (RFC 1951) of `raw`. Multiple blocks
//! when the payload exceeds 65535 bytes.
std::vector<std::uint8_t> stored_deflate(const std::vector<std::uint8_t>& raw) {
  std::vector<std::uint8_t> out;
  out.push_back(0x78);  // CMF: deflate, 32 KiB window
  out.push_back(0x01);  // FLG: FCHECK (header divides by 31)
  std::size_t offset = 0;
  do {
    const std::size_t remaining = raw.size() - offset;
    const std::size_t block = std::min<std::size_t>(remaining, 65535);
    const bool last = block == remaining;
    out.push_back(last ? 0x01 : 0x00);  // BFINAL=1, BTYPE=00
    out.push_back(static_cast<std::uint8_t>(block & 0xff));
    out.push_back(static_cast<std::uint8_t>((block >> 8) & 0xff));
    const std::uint16_t complement = static_cast<std::uint16_t>(~block);
    out.push_back(static_cast<std::uint8_t>(complement & 0xff));
    out.push_back(static_cast<std::uint8_t>((complement >> 8) & 0xff));
    out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
               raw.begin() + static_cast<std::ptrdiff_t>(offset + block));
    offset += block;
  } while (offset < raw.size());
  return out;
}

//! Assemble a complete PNG from explicit chunks. Rows must be pre-filtered
//! scanlines (each beginning with its filter-type byte).
std::vector<std::uint8_t> build_png(std::uint32_t width, std::uint32_t height,
                                    std::uint8_t colour_type,
                                    const std::vector<std::uint8_t>& rows,
                                    const std::vector<std::uint8_t>* palette = nullptr,
                                    const std::vector<std::uint8_t>* trns = nullptr,
                                    std::size_t idat_splits = 1,
                                    std::uint8_t bit_depth = 8,
                                    std::uint8_t interlace = 0) {
  std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  append_chunk(out, "IHDR", ihdr(width, height, colour_type, bit_depth, interlace));
  if (palette != nullptr) {
    append_chunk(out, "PLTE", *palette);
  }
  if (trns != nullptr) {
    append_chunk(out, "tRNS", *trns);
  }
  if (!rows.empty()) {
    const std::vector<std::uint8_t> compressed = stored_deflate(rows);
    const std::size_t splits = std::min(idat_splits, compressed.size());
    std::size_t offset = 0;
    for (std::size_t i = 0; i < splits; ++i) {
      const std::size_t end = (i + 1 == splits)
                                  ? compressed.size()
                                  : (i + 1) * compressed.size() / splits;
      std::vector<std::uint8_t> part(compressed.begin() +
                                         static_cast<std::ptrdiff_t>(offset),
                                     compressed.begin() +
                                         static_cast<std::ptrdiff_t>(end));
      append_chunk(out, "IDAT", part);
      offset = end;
    }
  }
  append_chunk(out, "IEND", {});
  return out;
}

//! Same as build_png but takes caller-provided (possibly corrupt or crafted)
//! raw IDAT bytes instead of recompressing rows.
std::vector<std::uint8_t> build_png_with_idat(
    std::uint32_t width, std::uint32_t height, std::uint8_t colour_type,
    const std::vector<std::uint8_t>& raw_idat,
    const std::vector<std::uint8_t>* palette = nullptr,
    const std::vector<std::uint8_t>* trns = nullptr) {
  std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  append_chunk(out, "IHDR", ihdr(width, height, colour_type));
  if (palette != nullptr) append_chunk(out, "PLTE", *palette);
  if (trns != nullptr) append_chunk(out, "tRNS", *trns);
  if (!raw_idat.empty()) append_chunk(out, "IDAT", raw_idat);
  append_chunk(out, "IEND", {});
  return out;
}

//! A filter-0 row of raw pixel bytes: [0] + bpp*width sample bytes.
std::vector<std::uint8_t> row0(const std::vector<std::uint8_t>& pixels) {
  std::vector<std::uint8_t> out;
  out.push_back(0);
  out.insert(out.end(), pixels.begin(), pixels.end());
  return out;
}

DecodedImage expect_ok(const std::vector<std::uint8_t>& png) {
  std::string error;
  auto result = decode_png(png.data(), png.size(), &error);
  EXPECT_TRUE(result.is_ok()) << "decode failed: " << error;
  if (!result.is_ok()) return DecodedImage{};
  EXPECT_TRUE(result.value().valid());
  return std::move(result).value();
}

void expect_malformed(const std::vector<std::uint8_t>& png,
                      const char* message_fragment = nullptr) {
  std::string error;
  auto result = decode_png(png.data(), png.size(), &error);
  EXPECT_EQ(result.error(), RuntimeError::malformed_asset);
  if (message_fragment != nullptr) {
    EXPECT_NE(error.find(message_fragment), std::string::npos)
        << "expected error mentioning \"" << message_fragment
        << "\" but got: " << error;
  } else {
    EXPECT_FALSE(error.empty()) << "expected a diagnostic message";
  }
}

// The 4x4 RGBA pixel grid shared by the three 4x4 goldens.
const std::array<std::array<std::uint8_t, 4>, 16> kExpected4x4 = {{
    {{255, 0, 0, 255}}, {{0, 255, 0, 255}}, {{0, 0, 255, 255}}, {{255, 255, 255, 255}},
    {{255, 255, 0, 255}}, {{255, 0, 255, 255}}, {{0, 255, 255, 255}}, {{10, 20, 30, 40}},
    {{200, 100, 50, 255}}, {{1, 2, 3, 4}}, {{250, 240, 230, 220}}, {{128, 64, 32, 16}},
    {{9, 99, 199, 9}}, {{7, 6, 5, 4}}, {{3, 2, 1, 0}}, {{77, 88, 99, 111}},
}};

void expect_golden4x4(const DecodedImage& image) {
  EXPECT_EQ(image.width, 4U);
  EXPECT_EQ(image.height, 4U);
  ASSERT_EQ(image.rgba.size(), 64U);
  for (std::size_t i = 0; i < 16; ++i) {
    const std::uint8_t* px = image.rgba.data() + i * 4U;
    EXPECT_EQ(px[0], kExpected4x4[i][0]) << "pixel " << i << " r";
    EXPECT_EQ(px[1], kExpected4x4[i][1]) << "pixel " << i << " g";
    EXPECT_EQ(px[2], kExpected4x4[i][2]) << "pixel " << i << " b";
    EXPECT_EQ(px[3], kExpected4x4[i][3]) << "pixel " << i << " a";
  }
}

} // namespace

// ============================================================================
// Valid inputs
// ============================================================================

TEST(PngDecoder, GoldenDynamicHuffmanMatchesExpectedRgba) {
  const std::vector<std::uint8_t> png(
      std::begin(kPngGoldenDynamic), std::end(kPngGoldenDynamic));
  expect_golden4x4(expect_ok(png));
}

TEST(PngDecoder, GoldenFixedHuffmanMatchesExpectedRgba) {
  const std::vector<std::uint8_t> png(
      std::begin(kPngGoldenFixed), std::end(kPngGoldenFixed));
  expect_golden4x4(expect_ok(png));
}

TEST(PngDecoder, GoldenStoredBlocksMatchExpectedRgba) {
  const std::vector<std::uint8_t> png(
      std::begin(kPngGoldenStored), std::end(kPngGoldenStored));
  expect_golden4x4(expect_ok(png));
}

TEST(PngDecoder, GoldenPaletteWithTrnsExpandsToRgba) {
  const std::vector<std::uint8_t> png(
      std::begin(kPngGoldenPalette), std::end(kPngGoldenPalette));
  const DecodedImage image = expect_ok(png);
  EXPECT_EQ(image.width, 3U);
  EXPECT_EQ(image.height, 2U);
  // Palette: 0=red, 1=green, 2=blue, 3=(30,60,90); tRNS alpha {255,128,0,200}.
  // Indices row-major: 0 1 2 / 3 0 1.
  const std::array<std::array<std::uint8_t, 4>, 6> kExpected = {{
      {{255, 0, 0, 255}}, {{0, 255, 0, 128}}, {{0, 0, 255, 0}},
      {{30, 60, 90, 200}}, {{255, 0, 0, 255}}, {{0, 255, 0, 128}},
  }};
  ASSERT_EQ(image.rgba.size(), 24U);
  for (std::size_t i = 0; i < 6; ++i) {
    const std::uint8_t* px = image.rgba.data() + i * 4U;
    EXPECT_EQ(px[0], kExpected[i][0]) << "pixel " << i << " r";
    EXPECT_EQ(px[1], kExpected[i][1]) << "pixel " << i << " g";
    EXPECT_EQ(px[2], kExpected[i][2]) << "pixel " << i << " b";
    EXPECT_EQ(px[3], kExpected[i][3]) << "pixel " << i << " a";
  }
}

TEST(PngDecoder, GoldenGrayAlphaAllFiveFilters) {
  // Rows 0..4 of a 5x5 greyscale+alpha image were filtered with PNG filters
  // 0..4 (None/Sub/Up/Average/Paeth) by the independent Python encoder.
  const std::vector<std::uint8_t> png(
      std::begin(kPngGoldenGrayAlpha), std::end(kPngGoldenGrayAlpha));
  const DecodedImage image = expect_ok(png);
  EXPECT_EQ(image.width, 5U);
  EXPECT_EQ(image.height, 5U);
  for (std::uint32_t y = 0; y < 5; ++y) {
    for (std::uint32_t x = 0; x < 5; ++x) {
      const std::uint8_t gray = static_cast<std::uint8_t>((x * 40 + y * 13) & 0xff);
      const std::uint8_t alpha =
          static_cast<std::uint8_t>((255 - x * 30 + y * 7) & 0xff);
      const std::uint8_t* px =
          image.rgba.data() + (static_cast<std::size_t>(y) * 5U + x) * 4U;
      EXPECT_EQ(px[0], gray) << "px " << x << "," << y << " r";
      EXPECT_EQ(px[1], gray) << "px " << x << "," << y << " g";
      EXPECT_EQ(px[2], gray) << "px " << x << "," << y << " b";
      EXPECT_EQ(px[3], alpha) << "px " << x << "," << y << " a";
    }
  }
}

TEST(PngDecoder, GreyscaleWithTransparencyKey) {
  // 3x2 greyscale with a tRNS key 0x00c8 (200): the grey pixel equal to 200
  // becomes fully transparent.
  const std::vector<std::uint8_t> trns = {0x00, 0xc8};
  const std::vector<std::uint8_t> rows = [] {
    std::vector<std::uint8_t> out;
    out.push_back(0);  // filter type
    out.insert(out.end(), {10, 200, 90});
    out.push_back(0);
    out.insert(out.end(), {200, 0, 255});
    return out;
  }();
  const std::vector<std::uint8_t> png = build_png(3, 2, 0, rows, nullptr, &trns);
  const DecodedImage image = expect_ok(png);
  const std::array<std::array<std::uint8_t, 4>, 6> kExpected = {{
      {{10, 10, 10, 255}}, {{200, 200, 200, 0}}, {{90, 90, 90, 255}},
      {{200, 200, 200, 0}}, {{0, 0, 0, 255}}, {{255, 255, 255, 255}},
  }};
  for (std::size_t i = 0; i < 6; ++i) {
    const std::uint8_t* px = image.rgba.data() + i * 4U;
    EXPECT_EQ(px[0], kExpected[i][0]) << "px " << i;
    EXPECT_EQ(px[3], kExpected[i][3]) << "px " << i << " alpha";
  }
}

TEST(PngDecoder, TruecolourWithTransparencyKey) {
  // 2x1 RGB with a tRNS RGB key (10,20,30); only the exact key pixel clears.
  const std::vector<std::uint8_t> trns = {0x00, 0x0a, 0x00, 0x14, 0x00, 0x1e};
  const std::vector<std::uint8_t> pixels = {10, 20, 30, 11, 20, 30};
  std::vector<std::uint8_t> rows = row0(pixels);
  const std::vector<std::uint8_t> png = build_png(2, 1, 2, rows, nullptr, &trns);
  const DecodedImage image = expect_ok(png);
  ASSERT_EQ(image.rgba.size(), 8U);
  EXPECT_EQ(image.rgba[0], 10U);
  EXPECT_EQ(image.rgba[3], 0U);   // key pixel transparent
  EXPECT_EQ(image.rgba[4], 11U);
  EXPECT_EQ(image.rgba[7], 255U); // near-miss pixel opaque
}

TEST(PngDecoder, PaletteWithoutTrnsIsOpaque) {
  // 2x2 indexed colour, 2-entry palette, no tRNS: all alpha 255.
  const std::vector<std::uint8_t> palette = {255, 0, 0, 0, 255, 0};
  std::vector<std::uint8_t> rows;
  rows.push_back(0);
  rows.insert(rows.end(), {0, 1});
  rows.push_back(0);
  rows.insert(rows.end(), {1, 0});
  const std::vector<std::uint8_t> png = build_png(2, 2, 3, rows, &palette, nullptr);
  const DecodedImage image = expect_ok(png);
  ASSERT_EQ(image.rgba.size(), 16U);
  EXPECT_EQ(image.rgba[0], 255U);
  EXPECT_EQ(image.rgba[1], 0U);
  EXPECT_EQ(image.rgba[7], 255U);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(image.rgba[i * 4U + 3U], 255U) << "px " << i << " alpha";
  }
}

TEST(PngDecoder, SplitIdatChunksConcatenate) {
  // Same RGBA image with the compressed IDAT stream split across two chunks.
  const std::vector<std::uint8_t> rows = [] {
    std::vector<std::uint8_t> out;
    // 3x2 RGBA: four distinct colours + two repeats.
    out.push_back(0);
    out.insert(out.end(), {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255});
    out.push_back(0);
    out.insert(out.end(), {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255});
    return out;
  }();
  const std::vector<std::uint8_t> single = build_png(3, 2, 6, rows, nullptr, nullptr, 1);
  const std::vector<std::uint8_t> split = build_png(3, 2, 6, rows, nullptr, nullptr, 2);
  ASSERT_NE(single.size(), split.size());
  const DecodedImage a = expect_ok(single);
  const DecodedImage b = expect_ok(split);
  EXPECT_EQ(a.rgba, b.rgba);
}

TEST(PngDecoder, AncillaryChunksAreIgnored) {
  // Inject a pHYs chunk (ancillary, lowercase first letter) between IHDR and
  // IDAT; it carries no colour data and must not affect the decode.
  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  append_chunk(png, "IHDR", ihdr(1, 1, 6));
  append_chunk(png, "pHYs",
               {0x00, 0x00, 0x0e, 0x10, 0x00, 0x00, 0x0e, 0x10, 0x01});
  const std::vector<std::uint8_t> pixel = {0, 255, 0, 128};
  append_chunk(png, "IDAT", stored_deflate(row0(pixel)));
  append_chunk(png, "IEND", {});
  const DecodedImage image = expect_ok(png);
  EXPECT_EQ(image.width, 1U);
  EXPECT_EQ(image.height, 1U);
  EXPECT_EQ(image.rgba[0], 0U);
  EXPECT_EQ(image.rgba[1], 255U);
  EXPECT_EQ(image.rgba[3], 128U);
}

TEST(PngDecoder, MultiBlockStoredLargeImage) {
  // 400x400 RGBA exceeds the 65535-byte stored-block payload, forcing several
  // stored blocks; the whole frame must still decode byte-exactly.
  constexpr std::uint32_t kSize = 400;
  std::vector<std::uint8_t> rows;
  rows.reserve(static_cast<std::size_t>(kSize) * (1U + kSize * 4U));
  for (std::uint32_t y = 0; y < kSize; ++y) {
    rows.push_back(0);
    for (std::uint32_t x = 0; x < kSize; ++x) {
      rows.push_back(static_cast<std::uint8_t>(x & 0xff));
      rows.push_back(static_cast<std::uint8_t>(y & 0xff));
      rows.push_back(static_cast<std::uint8_t>((x + y) & 0xff));
      rows.push_back(255);
    }
  }
  const std::vector<std::uint8_t> png = build_png(kSize, kSize, 6, rows);
  const DecodedImage image = expect_ok(png);
  EXPECT_EQ(image.rgba.size(), static_cast<std::size_t>(kSize) * kSize * 4U);
  for (std::uint32_t probe = 0; probe < 64; ++probe) {
    const std::uint32_t x = (probe * 137U) % kSize;
    const std::uint32_t y = (probe * 331U) % kSize;
    const std::uint8_t* px =
        image.rgba.data() + (static_cast<std::size_t>(y) * kSize + x) * 4U;
    EXPECT_EQ(px[0], static_cast<std::uint8_t>(x & 0xff)) << probe;
    EXPECT_EQ(px[1], static_cast<std::uint8_t>(y & 0xff)) << probe;
    EXPECT_EQ(px[2], static_cast<std::uint8_t>((x + y) & 0xff)) << probe;
    EXPECT_EQ(px[3], 255U) << probe;
  }
}

TEST(PngDecoder, DecodingIsDeterministic) {
  const std::vector<std::uint8_t> png(
      std::begin(kPngGoldenDynamic), std::end(kPngGoldenDynamic));
  const DecodedImage a = expect_ok(png);
  const DecodedImage b = expect_ok(png);
  EXPECT_EQ(a.width, b.width);
  EXPECT_EQ(a.height, b.height);
  EXPECT_EQ(a.rgba, b.rgba);
}

// ============================================================================
// Pillow-produced files (independent encoder)
// ============================================================================

TEST(PngDecoder, PilRgbGradientDecodesExactly) {
  const std::vector<std::uint8_t> png(std::begin(kPngPilRgbGradient),
                                      std::end(kPngPilRgbGradient));
  const DecodedImage image = expect_ok(png);
  EXPECT_EQ(image.width, 8U);
  EXPECT_EQ(image.height, 4U);
  for (std::uint32_t y = 0; y < 4; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const std::uint8_t* px =
          image.rgba.data() + (static_cast<std::size_t>(y) * 8U + x) * 4U;
      EXPECT_EQ(px[0], static_cast<std::uint8_t>((x * 31 + y * 3) % 256))
          << "px " << x << "," << y << " r";
      EXPECT_EQ(px[1], static_cast<std::uint8_t>((y * 61 + x * 7) % 256))
          << "px " << x << "," << y << " g";
      EXPECT_EQ(px[2], static_cast<std::uint8_t>((x * 13 + y * 41) % 256))
          << "px " << x << "," << y << " b";
      EXPECT_EQ(px[3], 255U) << "px " << x << "," << y << " a";
    }
  }
}

TEST(PngDecoder, PilRgbaGradientDecodesExactly) {
  const std::vector<std::uint8_t> png(std::begin(kPngPilRgbaGradient),
                                      std::end(kPngPilRgbaGradient));
  const DecodedImage image = expect_ok(png);
  EXPECT_EQ(image.width, 8U);
  EXPECT_EQ(image.height, 2U);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const std::uint8_t* px =
          image.rgba.data() + (static_cast<std::size_t>(y) * 8U + x) * 4U;
      EXPECT_EQ(px[0], static_cast<std::uint8_t>((x * 37) % 256)) << "r";
      EXPECT_EQ(px[1], static_cast<std::uint8_t>((y * 97) % 256)) << "g";
      EXPECT_EQ(px[2], static_cast<std::uint8_t>((x + y * 5) % 256)) << "b";
      EXPECT_EQ(px[3], static_cast<std::uint8_t>((255 - x * 29) % 256)) << "a";
    }
  }
}

TEST(PngDecoder, PilPaletteWithTrnsDecodesExactly) {
  const std::vector<std::uint8_t> png(std::begin(kPngPilPalette),
                                      std::end(kPngPilPalette));
  const DecodedImage image = expect_ok(png);
  EXPECT_EQ(image.width, 8U);
  EXPECT_EQ(image.height, 2U);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const std::uint32_t v = (x * 3 + y) % 32;
      const std::uint8_t* px =
          image.rgba.data() + (static_cast<std::size_t>(y) * 8U + x) * 4U;
      EXPECT_EQ(px[0], static_cast<std::uint8_t>((v * 7) % 256)) << "px " << x
                                                                  << "," << y
                                                                  << " r";
      EXPECT_EQ(px[1], static_cast<std::uint8_t>((v * 11) % 256)) << "g";
      EXPECT_EQ(px[2], static_cast<std::uint8_t>((v * 17) % 256)) << "b";
      EXPECT_EQ(px[3], v == 5U ? 0U : 255U) << "px " << x << "," << y
                                            << " alpha";
    }
  }
}

TEST(PngDecoder, PilGrayDecodesExactly) {
  const std::vector<std::uint8_t> png(std::begin(kPngPilGray),
                                      std::end(kPngPilGray));
  const DecodedImage image = expect_ok(png);
  EXPECT_EQ(image.width, 6U);
  EXPECT_EQ(image.height, 3U);
  for (std::uint32_t y = 0; y < 3; ++y) {
    for (std::uint32_t x = 0; x < 6; ++x) {
      const std::uint8_t v = static_cast<std::uint8_t>((x * 40 + y * 90) % 256);
      const std::uint8_t* px =
          image.rgba.data() + (static_cast<std::size_t>(y) * 6U + x) * 4U;
      EXPECT_EQ(px[0], v) << "r";
      EXPECT_EQ(px[1], v) << "g";
      EXPECT_EQ(px[2], v) << "b";
      EXPECT_EQ(px[3], 255U) << "a";
    }
  }
}

// ============================================================================
// Malformed inputs
// ============================================================================

TEST(PngDecoder, EmptyAndNullInputRejected) {
  expect_malformed({}, "empty");
  std::string error;
  auto result = decode_png(nullptr, 0, &error);
  EXPECT_EQ(result.error(), RuntimeError::malformed_asset);
}

TEST(PngDecoder, BadSignatureRejected) {
  std::vector<std::uint8_t> png(std::begin(kPngGoldenStored),
                                std::end(kPngGoldenStored));
  png[0] = 0x88;
  expect_malformed(png, "signature");
}

TEST(PngDecoder, TruncatedFileRejected) {
  const std::vector<std::uint8_t> png(
      std::begin(kPngGoldenStored), std::end(kPngGoldenStored));
  std::vector<std::uint8_t> cut(png.begin(), png.end() - 3);
  expect_malformed(cut, "chunk");
}

TEST(PngDecoder, CrcMismatchRejected) {
  std::vector<std::uint8_t> png(std::begin(kPngGoldenStored),
                                std::end(kPngGoldenStored));
  // Flip a payload byte inside IDAT without touching the CRC.
  for (std::size_t i = 0; i < png.size(); ++i) {
    if (png[i] == 'I' && png[i + 1] == 'D' && png[i + 2] == 'A' &&
        png[i + 3] == 'T') {
      png[i + 12] ^= 0x40U;
      break;
    }
  }
  expect_malformed(png, "CRC");
}

TEST(PngDecoder, InterlacedPngRejected) {
  // Adam7 interlace (interlace method 1) is explicitly unsupported.
  const std::vector<std::uint8_t> rows = row0({9, 9, 9, 255});
  const std::vector<std::uint8_t> png =
      build_png(1, 2, 6, rows, nullptr, nullptr, 1, 8, /*interlace=*/1);
  expect_malformed(png, "Adam7");
}

TEST(PngDecoder, SixteenBitRejected) {
  // A declared 16-bit IHDR must be rejected before any pixel data is read.
  const std::vector<std::uint8_t> png =
      build_png(1, 1, 6, row0({0, 0, 0, 0}), nullptr, nullptr, 1,
                /*bit_depth=*/16);
  expect_malformed(png, "8-bit");
}

TEST(PngDecoder, UnsupportedColourTypeRejected) {
  // Colour type 1 is reserved.
  const std::vector<std::uint8_t> png = build_png(1, 1, 1, row0({0}));
  expect_malformed(png, "colour type");
}

TEST(PngDecoder, UnknownCriticalChunkRejected) {
  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  append_chunk(png, "IHDR", ihdr(1, 1, 6));
  append_chunk(png, "ABCD", {1, 2, 3});  // Critical (uppercase) unknown chunk.
  append_chunk(png, "IDAT", stored_deflate(row0({0, 0, 0, 0})));
  append_chunk(png, "IEND", {});
  expect_malformed(png, "ABCD");
}

TEST(PngDecoder, IndexedColourWithoutPlteRejected) {
  // A valid empty stored block so the IDAT-present check passes and the
  // missing-PLTE rule is the one that fires.
  const std::vector<std::uint8_t> idat = {0x78, 0x01, 0x01, 0x00, 0x00,
                                          0xff, 0xff};
  const std::vector<std::uint8_t> png = build_png_with_idat(1, 1, 3, idat);
  expect_malformed(png, "PLTE");
}

TEST(PngDecoder, PaletteIndexOverrunRejected) {
  const std::vector<std::uint8_t> palette = {255, 0, 0, 0, 255, 0};  // 2 entries
  const std::vector<std::uint8_t> rows = row0({9});  // index 9 > 1
  const std::vector<std::uint8_t> png = build_png(1, 1, 3, rows, &palette, nullptr);
  expect_malformed(png, "palette");
}

TEST(PngDecoder, TrnsOnAlphaColourTypeRejected) {
  const std::vector<std::uint8_t> trns = {0, 0};
  const std::vector<std::uint8_t> png =
      build_png_with_idat(1, 1, 6, {}, nullptr, &trns);
  expect_malformed(png, "tRNS");
}

TEST(PngDecoder, OversizedDimensionsRejected) {
  // 16384x16384x4 bytes exceeds the 256 MiB decoded-size cap; must fail at
  // IHDR without attempting an allocation.
  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  append_chunk(png, "IHDR", ihdr(16384, 16384, 6));
  append_chunk(png, "IEND", {});
  expect_malformed(png, "cap");
}

TEST(PngDecoder, ZlibPresetDictionaryRejected) {
  // FLG 0x20 sets the FDICT bit on a checksum-valid header (0x7820 % 31 == 0).
  const std::vector<std::uint8_t> idat = {0x78, 0x20, 0x01, 0x00, 0x00, 0xff, 0xff};
  const std::vector<std::uint8_t> png = build_png_with_idat(1, 1, 6, idat);
  expect_malformed(png, "preset dictionaries");
}

TEST(PngDecoder, InvalidDeflateBlockTypeRejected) {
  // 0x07: BFINAL=1, BTYPE=3 (reserved).
  const std::vector<std::uint8_t> idat = {0x78, 0x01, 0x07};
  const std::vector<std::uint8_t> png = build_png_with_idat(1, 1, 6, idat);
  expect_malformed(png, "block type");
}

TEST(PngDecoder, TruncatedZlibHeaderRejected) {
  const std::vector<std::uint8_t> idat = {0x78};
  const std::vector<std::uint8_t> png = build_png_with_idat(1, 1, 6, idat);
  expect_malformed(png, "zlib header");
}

TEST(PngDecoder, IdatSizeMismatchRejected) {
  // 2x2 RGBA requires 2 rows of 9 bytes (18 total); store only one row.
  const std::vector<std::uint8_t> rows = row0({1, 2, 3, 4, 5, 6, 7, 8});
  const std::vector<std::uint8_t> png = build_png_with_idat(
      2, 2, 6, stored_deflate(rows));
  expect_malformed(png, "required");
}

TEST(PngDecoder, ZeroDimensionRejected) {
  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  append_chunk(png, "IHDR", ihdr(0, 4, 6));
  append_chunk(png, "IEND", {});
  expect_malformed(png, "non-zero");
}
