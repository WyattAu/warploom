//! @file test_bc_decode.cpp
//! @brief In-tree BC1/BC3 proofs: hand-computed texel vectors (endpoints
//!        chosen so every interpolated value is computable by hand from the
//!        spec weights — pure red vs pure blue, never copied from a run),
//!        both BC1 endpoint modes, both BC3 alpha modes, the BC3
//!        transparent-corner rule, multi-block grids with non-multiple-of-4
//!        cropping, level-size math, and hostile payloads.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "warploom/asset/bc_decode.hpp"

namespace {

// BC1 block: c0 = pure red (0xF800), c1 = pure blue (0x001F), indices
// 0,1,2,3 across each row (0xE4 per index byte). c0 > c1: opaque.
//   p0 = (255,0,0)  p1 = (0,0,255)
//   p2 = (2c0+c1)/3 = (170,0,85)   p3 = (c0+2c1)/3 = (85,0,170)
std::vector<std::uint8_t> red_blue_bc1() {
  return {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
}

TEST(BcDecode, Bc1OpaquePaletteIsExact) {
  std::string error;
  const auto decoded = warploom::asset::decode_bc_level(
      4U, 4U, 131U, red_blue_bc1().data(), 8U, &error);
  ASSERT_TRUE(decoded.is_ok()) << error;
  const auto& rgba = decoded.value().rgba;
  ASSERT_EQ(rgba.size(), 64U);
  // Row 0, texels 0..3 -> codes 0..3.
  const std::uint8_t row0[16] = {255U, 0U,   0U,   255U,  //
                                 0U,   0U,   255U, 255U,  //
                                 170U, 0U,   85U,  255U,  //
                                 85U,  0U,   170U, 255U};
  for (std::size_t i = 0; i < 16U; ++i)
    EXPECT_EQ(rgba[i], row0[i]) << "byte " << i;
  // All four rows repeat the pattern (indices repeat per row).
  for (std::size_t row = 1; row < 4U; ++row) {
    for (std::size_t i = 0; i < 16U; ++i) {
      EXPECT_EQ(rgba[row * 16U + i], row0[i])
          << "row " << row << " byte " << i;
    }
  }
}

TEST(BcDecode, Bc1TransparentCornerIsExact) {
  // c0 = blue, c1 = red: c0 < c1, so index 3 is transparent black and
  // index 2 is the halved mix (255/2 = 127, truncated).
  const std::vector<std::uint8_t> block = {0x1F, 0x00, 0x00, 0xF8,
                                           0xE4, 0xE4, 0xE4, 0xE4};
  std::string error;
  const auto decoded = warploom::asset::decode_bc_level(4U, 4U, 131U,
                                                       block.data(), 8U,
                                                       &error);
  ASSERT_TRUE(decoded.is_ok()) << error;
  const auto& rgba = decoded.value().rgba;
  ASSERT_EQ(rgba.size(), 64U);
  const std::uint8_t row0[16] = {0U, 0U, 255U, 255U,  //
                                 255U, 0U, 0U, 255U,  //
                                 127U, 0U, 127U, 255U,  //
                                 0U, 0U, 0U,   0U};
  for (std::size_t i = 0; i < 16U; ++i)
    EXPECT_EQ(rgba[i], row0[i]) << "byte " << i;
}

TEST(BcDecode, Bc3EightAlphaModeIsExact) {
  // Alpha endpoints 255/0 (8-alpha mode), texel t holds code t%8:
  // 255, 0, 218, 182, 145, 109, 72, 36 repeating.
  // (6*255)/7 = 218, (5*255)/7 = 182, (4*255)/7 = 145, (3*255)/7 = 109,
  // (2*255)/7 = 72, 255/7 = 36 — all truncated. Packed LSB-first:
  // byte1 = bits 8..15 = 0,1,1,0,0,0,1,1 = 0xC6.
  const std::vector<std::uint8_t> block = {
      255U, 0U, 0x88U, 0xC6U, 0xFAU, 0x88U, 0xC6U, 0xFAU,  // alpha
      0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};    // red/blue
  std::string error;
  const auto decoded = warploom::asset::decode_bc_level(4U, 4U, 137U,
                                                       block.data(), 16U,
                                                       &error);
  ASSERT_TRUE(decoded.is_ok()) << error;
  const auto& rgba = decoded.value().rgba;
  ASSERT_EQ(rgba.size(), 64U);
  const std::uint8_t alpha[16] = {255U, 0U, 218U, 182U, 145U, 109U,
                                  72U,  36U, 255U, 0U,  218U, 182U,
                                  145U, 109U, 72U, 36U};
  const std::uint8_t rgb[4][3] = {{255U, 0U, 0U}, {0U, 0U, 255U},
                                  {170U, 0U, 85U}, {85U, 0U, 170U}};
  for (std::size_t t = 0; t < 16U; ++t) {
    EXPECT_EQ(rgba[4U * t], rgb[t % 4U][0]) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 1U], rgb[t % 4U][1]) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 2U], rgb[t % 4U][2]) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 3U], alpha[t]) << "texel " << t;
  }
}

TEST(BcDecode, Bc3SixAlphaModeIsExact) {
  // Alpha endpoints 0/255 (6-alpha mode), every texel code 5
  // ((0 + 4*255)/5 = 204). Code 5 = 101 LSB-first; the 3-bit period against
  // the 8-bit byte period gives a 3-byte repeating unit 0x6D/0xDB/0xB6.
  // Color: solid red (all code 0).
  const std::vector<std::uint8_t> block = {
      0U, 255U, 0x6DU, 0xDBU, 0xB6U, 0x6DU, 0xDBU, 0xB6U,  // alpha
      0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00};    // red, all 0
  std::string error;
  const auto decoded = warploom::asset::decode_bc_level(4U, 4U, 137U,
                                                       block.data(), 16U,
                                                       &error);
  ASSERT_TRUE(decoded.is_ok()) << error;
  const auto& rgba = decoded.value().rgba;
  ASSERT_EQ(rgba.size(), 64U);
  for (std::size_t t = 0; t < 16U; ++t) {
    EXPECT_EQ(rgba[4U * t], 255U) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 1U], 0U) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 2U], 0U) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 3U], 204U) << "texel " << t;
  }
}

TEST(BcDecode, Bc3TransparentCornerKeepsExplicitAlpha) {
  // Spec choice, pinned: the BC3 color block's c0 <= c1 index-3 corner
  // yields black RGB with the alpha from the alpha block (here 200
  // everywhere: endpoints 200/100, all indices code 0).
  const std::vector<std::uint8_t> block = {
      200U, 100U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,  // alpha
      0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};      // blue/red, all 3
  std::string error;
  const auto decoded = warploom::asset::decode_bc_level(4U, 4U, 138U,
                                                       block.data(), 16U,
                                                       &error);
  ASSERT_TRUE(decoded.is_ok()) << error;
  const auto& rgba = decoded.value().rgba;
  ASSERT_EQ(rgba.size(), 64U);
  for (std::size_t t = 0; t < 16U; ++t) {
    EXPECT_EQ(rgba[4U * t], 0U) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 1U], 0U) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 2U], 0U) << "texel " << t;
    EXPECT_EQ(rgba[4U * t + 3U], 200U) << "texel " << t;
  }
}

TEST(BcDecode, GridCropsNonMultipleOfFour) {
  // 6x6 BC1: 2x2 blocks. Block colors: TL red, TR green, BL blue,
  // BR white (all code 0, distinct endpoints per block).
  auto solid = [](std::uint16_t c0) {
    return std::vector<std::uint8_t>{
        static_cast<std::uint8_t>(c0 & 0xFFU),
        static_cast<std::uint8_t>(c0 >> 8U), 0x1F, 0x00,  //
        0x00, 0x00, 0x00, 0x00};
  };
  const auto tl = solid(0xF800U);  // red
  const auto tr = solid(0x07E0U);  // green (0x07E0 -> (0,255,0))
  const auto bl = solid(0x001FU);  // blue
  const auto br = solid(0xFFFFU);  // white
  std::vector<std::uint8_t> blocks;
  blocks.insert(blocks.end(), tl.begin(), tl.end());
  blocks.insert(blocks.end(), tr.begin(), tr.end());
  blocks.insert(blocks.end(), bl.begin(), bl.end());
  blocks.insert(blocks.end(), br.begin(), br.end());
  std::string error;
  const auto decoded = warploom::asset::decode_bc_level(6U, 6U, 131U,
                                                       blocks.data(),
                                                       blocks.size(), &error);
  ASSERT_TRUE(decoded.is_ok()) << error;
  const auto& rgba = decoded.value().rgba;
  ASSERT_EQ(rgba.size(), 6U * 6U * 4U);
  auto at = [&](std::uint32_t x, std::uint32_t y) {
    return std::vector<std::uint8_t>(rgba.begin() + (y * 6U + x) * 4U,
                                     rgba.begin() + (y * 6U + x) * 4U + 4U);
  };
  EXPECT_EQ(at(0U, 0U), (std::vector<std::uint8_t>{255U, 0U, 0U, 255U}));
  EXPECT_EQ(at(3U, 3U), (std::vector<std::uint8_t>{255U, 0U, 0U, 255U}));
  EXPECT_EQ(at(5U, 0U), (std::vector<std::uint8_t>{0U, 255U, 0U, 255U}));
  EXPECT_EQ(at(0U, 5U), (std::vector<std::uint8_t>{0U, 0U, 255U, 255U}));
  EXPECT_EQ(at(5U, 5U),
            (std::vector<std::uint8_t>{255U, 255U, 255U, 255U}));
}

TEST(BcDecode, LevelSizesFollowTheBlockFormula) {
  using warploom::asset::bc_level_bytes;
  // 8x8 BC1 (8-byte blocks): 4 blocks L0, 1 block L1, still 1 block L2
  // (2x2 needs ceil(2/4) = 1 block per side).
  EXPECT_EQ(bc_level_bytes(8U, 8U, 8U, 0U), 32U);
  EXPECT_EQ(bc_level_bytes(8U, 8U, 8U, 1U), 8U);
  EXPECT_EQ(bc_level_bytes(8U, 8U, 8U, 2U), 8U);
  // 6x6 BC3 (16-byte blocks): 2x2 blocks.
  EXPECT_EQ(bc_level_bytes(6U, 6U, 16U, 0U), 64U);
  // Format predicates.
  EXPECT_TRUE(warploom::asset::is_bc1_format(131U));
  EXPECT_TRUE(warploom::asset::is_bc1_format(134U));
  EXPECT_TRUE(warploom::asset::is_bc3_format(137U));
  EXPECT_TRUE(warploom::asset::is_bc3_format(138U));
  EXPECT_TRUE(warploom::asset::is_bc_format(132U));
  EXPECT_FALSE(warploom::asset::is_bc_format(37U));
  EXPECT_FALSE(warploom::asset::is_bc_format(158U));
  EXPECT_EQ(warploom::asset::bc_block_bytes(131U), 8U);
  EXPECT_EQ(warploom::asset::bc_block_bytes(138U), 16U);
  EXPECT_EQ(warploom::asset::bc_block_bytes(158U), 0U);
}

TEST(BcDecode, RejectsBadPayloads) {
  std::string error;
  // Wrong payload size for the grid (63 vs 64 for one BC3 block).
  {
    std::vector<std::uint8_t> short_block(63U, 0U);
    error.clear();
    EXPECT_FALSE(warploom::asset::decode_bc_level(4U, 4U, 137U,
                                                 short_block.data(),
                                                 short_block.size(), &error)
                     .is_ok());
    EXPECT_NE(error.find("block grid"), std::string::npos) << error;
  }
  // Zero dimensions.
  {
    std::vector<std::uint8_t> block(8U, 0U);
    error.clear();
    EXPECT_FALSE(warploom::asset::decode_bc_level(0U, 4U, 131U, block.data(),
                                                 block.size(), &error)
                     .is_ok());
    EXPECT_NE(error.find("nonzero"), std::string::npos) << error;
  }
  // Not a decoded BC format.
  {
    std::vector<std::uint8_t> block(8U, 0U);
    error.clear();
    EXPECT_FALSE(warploom::asset::decode_bc_level(4U, 4U, 158U, block.data(),
                                                 block.size(), &error)
                     .is_ok());
    EXPECT_NE(error.find("158"), std::string::npos) << error;
  }
}

}  // namespace
