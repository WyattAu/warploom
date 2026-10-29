//! @file bc_decode.cpp
//! @brief In-tree BC1/BC3 decoding (see bc_decode.hpp).
//!
//! Formulas per the Microsoft Block Compression reference and
//! EXT_texture_compression_s3tc (same math from both APIs):
//!   BC1: endpoints c0/c1 in RGB565. c0 > c1: four opaque colors,
//!     c2 = (2c0+c1)/3, c3 = (c0+2c1)/3. c0 <= c1: c2 = (c0+c1)/2 and
//!     index 3 is transparent black (0,0,0,0).
//!   BC3 alpha: endpoints a0/a1. a0 > a1: eight interpolated alphas with
//!     /7 weights; a0 <= a1: six interpolated alphas with /5 weights plus
//!     explicit 0 and 255. All divisions truncate (integer math).
//!   BC3 color block follows BC1, except the c0 <= c1 index-3 corner yields
//!     black RGB with the alpha taken from the alpha block (opaque unless
//!     the alpha block says otherwise) rather than a transparent texel.
//!
//! 565 expansion replicates the high bits ((v << 3) | (v >> 2) for 5-bit
//! channels): 31 -> 255 exactly, and it matches the de-facto reference
//! decoders bit-for-bit.

#include "warploom/asset/bc_decode.hpp"

namespace warploom::asset {

namespace {

[[nodiscard]] std::uint8_t expand5(std::uint32_t v) noexcept {
  return static_cast<std::uint8_t>((v << 3U) | (v >> 2U));
}

[[nodiscard]] std::uint8_t expand6(std::uint32_t v) noexcept {
  return static_cast<std::uint8_t>((v << 2U) | (v >> 4U));
}

struct Rgb {
  std::uint8_t r{0};
  std::uint8_t g{0};
  std::uint8_t b{0};
};

[[nodiscard]] Rgb expand565(std::uint32_t packed) noexcept {
  return Rgb{expand5((packed >> 11U) & 31U), expand6((packed >> 5U) & 63U),
             expand5(packed & 31U)};
}

[[nodiscard]] Rgb mix3(const Rgb& a, std::uint32_t wa, const Rgb& b,
                       std::uint32_t wb) noexcept {
  return Rgb{
      static_cast<std::uint8_t>((wa * a.r + wb * b.r) / 3U),
      static_cast<std::uint8_t>((wa * a.g + wb * b.g) / 3U),
      static_cast<std::uint8_t>((wa * a.b + wb * b.b) / 3U),
  };
}

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8U));
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0] | (p[1] << 8U) | (p[2] << 16U) |
                                    (p[3] << 24U));
}

[[nodiscard]] std::uint64_t read_u48(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 5; i >= 0; --i) v = (v << 8U) | p[i];
  return v;
}

//! Decode one 8-byte BC1 block into 16 RGBA texels (row-major). When
//! `color_only` is set (the BC3 color block), the c0 <= c1 index-3 corner
//! yields black RGB with `corner_alpha` instead of a transparent texel.
void decode_bc1_block(const std::uint8_t* block, std::uint8_t out[64],
                      bool color_only, std::uint8_t corner_alpha) noexcept {
  const std::uint32_t c0 = read_u16(block);
  const std::uint32_t c1 = read_u16(block + 2);
  const std::uint32_t indices = read_u32(block + 4);
  const Rgb e0 = expand565(c0);
  const Rgb e1 = expand565(c1);
  Rgb palette[4] = {e0, e1, {}, {}};
  std::uint8_t alpha[4] = {255U, 255U, 255U, 255U};
  if (c0 > c1) {
    palette[2] = mix3(e0, 2U, e1, 1U);
    palette[3] = mix3(e0, 1U, e1, 2U);
  } else {
    palette[2] = Rgb{
        static_cast<std::uint8_t>((e0.r + e1.r) / 2U),
        static_cast<std::uint8_t>((e0.g + e1.g) / 2U),
        static_cast<std::uint8_t>((e0.b + e1.b) / 2U),
    };
    if (!color_only) {
      palette[3] = Rgb{0U, 0U, 0U};
      alpha[3] = 0U;
    } else {
      palette[3] = Rgb{0U, 0U, 0U};
      alpha[3] = corner_alpha;
    }
  }
  for (std::uint32_t t = 0; t < 16U; ++t) {
    const std::uint32_t code = (indices >> (2U * t)) & 3U;
    const std::size_t dst = 4U * t;
    out[dst] = palette[code].r;
    out[dst + 1] = palette[code].g;
    out[dst + 2] = palette[code].b;
    out[dst + 3] = alpha[code];
  }
}

void decode_bc3_block(const std::uint8_t* block,
                      std::uint8_t out[64]) noexcept {
  const std::uint32_t a0 = block[0];
  const std::uint32_t a1 = block[1];
  const std::uint64_t a_indices = read_u48(block + 2);
  std::uint8_t alpha[16];
  for (std::uint32_t t = 0; t < 16U; ++t) {
    const std::uint32_t code =
        static_cast<std::uint32_t>((a_indices >> (3U * t)) & 7U);
    if (a0 > a1) {
      switch (code) {
        case 0: alpha[t] = static_cast<std::uint8_t>(a0); break;
        case 1: alpha[t] = static_cast<std::uint8_t>(a1); break;
        case 2:
          alpha[t] = static_cast<std::uint8_t>((6U * a0 + a1) / 7U);
          break;
        case 3:
          alpha[t] = static_cast<std::uint8_t>((5U * a0 + 2U * a1) / 7U);
          break;
        case 4:
          alpha[t] = static_cast<std::uint8_t>((4U * a0 + 3U * a1) / 7U);
          break;
        case 5:
          alpha[t] = static_cast<std::uint8_t>((3U * a0 + 4U * a1) / 7U);
          break;
        case 6:
          alpha[t] = static_cast<std::uint8_t>((2U * a0 + 5U * a1) / 7U);
          break;
        default:
          alpha[t] = static_cast<std::uint8_t>((a0 + 6U * a1) / 7U);
          break;
      }
    } else {
      switch (code) {
        case 0: alpha[t] = static_cast<std::uint8_t>(a0); break;
        case 1: alpha[t] = static_cast<std::uint8_t>(a1); break;
        case 2:
          alpha[t] = static_cast<std::uint8_t>((4U * a0 + a1) / 5U);
          break;
        case 3:
          alpha[t] = static_cast<std::uint8_t>((3U * a0 + 2U * a1) / 5U);
          break;
        case 4:
          alpha[t] = static_cast<std::uint8_t>((2U * a0 + 3U * a1) / 5U);
          break;
        case 5:
          alpha[t] = static_cast<std::uint8_t>((a0 + 4U * a1) / 5U);
          break;
        case 6: alpha[t] = 0U; break;
        default: alpha[t] = 255U; break;
      }
    }
  }
  std::uint8_t color[64];
  decode_bc1_block(block + 8, color, true, 255U);
  for (std::uint32_t t = 0; t < 16U; ++t) {
    const std::size_t dst = 4U * t;
    out[dst] = color[dst];
    out[dst + 1] = color[dst + 1];
    out[dst + 2] = color[dst + 2];
    out[dst + 3] = alpha[t];
  }
}

}  // namespace

bool is_bc_format(std::uint32_t vk_format) noexcept {
  return is_bc1_format(vk_format) || is_bc3_format(vk_format);
}

bool is_bc1_format(std::uint32_t vk_format) noexcept {
  return vk_format >= kVkBc1RgbUnorm && vk_format <= kVkBc1RgbaSrgb;
}

bool is_bc3_format(std::uint32_t vk_format) noexcept {
  return vk_format == kVkBc3Unorm || vk_format == kVkBc3Srgb;
}

std::uint32_t bc_block_bytes(std::uint32_t vk_format) noexcept {
  if (is_bc1_format(vk_format)) return 8U;
  if (is_bc3_format(vk_format)) return 16U;
  return 0U;
}

std::uint64_t bc_level_bytes(std::uint32_t width, std::uint32_t height,
                             std::uint32_t block_bytes,
                             std::uint32_t level) noexcept {
  const auto blocks = [](std::uint32_t dim,
                          std::uint32_t lvl) -> std::uint64_t {
    std::uint64_t d = static_cast<std::uint64_t>(dim) >> lvl;
    if (d == 0U) d = 1ULL;
    return (d + 3ULL) / 4ULL;
  };
  return blocks(width, level) * blocks(height, level) * block_bytes;
}

::warploom::core::Result<DecodedImage> decode_bc_level(
    std::uint32_t width, std::uint32_t height, std::uint32_t vk_format,
    const std::uint8_t* blocks, std::size_t length,
    std::string* error_detail) {
  auto fail = [&](const std::string& message)
      -> ::warploom::core::Result<DecodedImage> {
    if (error_detail != nullptr) *error_detail = message;
    return ::warploom::core::Result<DecodedImage>::error(
        ::warploom::core::RuntimeError::malformed_asset);
  };
  const std::uint32_t block_bytes = bc_block_bytes(vk_format);
  if (block_bytes == 0U) {
    return fail("BC decode of vkFormat " + std::to_string(vk_format) +
                " is not supported (BC1 131-134, BC3 137-138)");
  }
  if (width == 0U || height == 0U) {
    return fail("BC decode needs nonzero dimensions");
  }
  if (blocks == nullptr) {
    return fail("BC decode needs a block payload");
  }
  const std::uint64_t blocks_x =
      (static_cast<std::uint64_t>(width) + 3ULL) / 4ULL;
  const std::uint64_t blocks_y =
      (static_cast<std::uint64_t>(height) + 3ULL) / 4ULL;
  const std::uint64_t want = blocks_x * blocks_y * block_bytes;
  if (length != want) {
    return fail("BC payload size " + std::to_string(length) +
                " does not match the block grid " +
                std::to_string(blocks_x) + "x" + std::to_string(blocks_y) +
                " (" + std::to_string(want) + " bytes)");
  }
  DecodedImage image;
  image.width = width;
  image.height = height;
  image.rgba.resize(static_cast<std::size_t>(width) * height * 4U);
  const bool bc3 = is_bc3_format(vk_format);
  std::uint8_t texels[64];
  for (std::uint64_t by = 0; by < blocks_y; ++by) {
    for (std::uint64_t bx = 0; bx < blocks_x; ++bx) {
      const std::uint8_t* src =
          blocks + (by * blocks_x + bx) * block_bytes;
      if (bc3) {
        decode_bc3_block(src, texels);
      } else {
        decode_bc1_block(src, texels, false, 0U);
      }
      // Crop block-grid overhang for dimensions past a multiple of 4.
      for (std::uint32_t dy = 0; dy < 4U; ++dy) {
        for (std::uint32_t dx = 0; dx < 4U; ++dx) {
          const std::uint64_t x = bx * 4ULL + dx;
          const std::uint64_t y = by * 4ULL + dy;
          if (x >= width || y >= height) continue;
          const std::size_t dst =
              static_cast<std::size_t>((y * width + x) * 4U);
          const std::size_t src_px = (dy * 4U + dx) * 4U;
          image.rgba[dst] = texels[src_px];
          image.rgba[dst + 1] = texels[src_px + 1];
          image.rgba[dst + 2] = texels[src_px + 2];
          image.rgba[dst + 3] = texels[src_px + 3];
        }
      }
    }
  }
  return ::warploom::core::Result<DecodedImage>::ok(std::move(image));
}

}  // namespace warploom::asset
