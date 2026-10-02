//! @file test_ktx2_decoder.cpp
//! @brief KTX2 container proofs: a spec-constructed mip-chained RGBA8
//!        texture decodes level 0 exactly, every truncated/corrupt/
//!        supercompressed variant is rejected with a precise diagnostic,
//!        and the magic-byte dispatch routes KTX2 through decode_image.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "warploom/asset/image_decode.hpp"
#include "warploom/asset/ktx2_decoder.hpp"

namespace {

//! Build a spec-conformant KTX2 container for a WxH RGBA8 texture with the
//! given mip level count. Pixel data: level L filled with a distinct byte
//! pattern derived from (L, pixel index) so level-0 extraction is provable.
std::vector<std::uint8_t> make_ktx2(std::uint32_t width, std::uint32_t height,
                                    std::uint32_t levels,
                                    std::uint32_t format = 43U) {
  struct LevelEntry {
    std::uint64_t offset;
    std::uint64_t length;
    std::uint64_t uncompressed;
  };
  std::vector<LevelEntry> entries;
  std::vector<std::uint8_t> mip_data;
  for (std::uint32_t level = 0; level < levels; ++level) {
    const auto shift = [&](std::uint32_t dim) -> std::uint64_t {
      const std::uint64_t s = static_cast<std::uint64_t>(dim) >> level;
      return s != 0U ? s : 1ULL;
    };
    const std::uint64_t bytes = shift(width) * shift(height) * 4ULL;
    entries.push_back({mip_data.size(), bytes, bytes});
    mip_data.resize(static_cast<std::size_t>(mip_data.size() + bytes));
    for (std::uint64_t i = 0; i < bytes; ++i) {
      mip_data[mip_data.size() - static_cast<std::size_t>(bytes) +
               static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>((level * 37U + i * 13U) & 0xFFU);
    }
  }

  // Header (80) + level index + mip data.
  std::vector<std::uint8_t> out(80U + entries.size() * 24U);
  std::memcpy(out.data(), "\xAB\x4B\x54\x58\x20\x32\x30\xBB", 8U);
  auto put32 = [&out](std::size_t offset, std::uint32_t value) {
    std::memcpy(out.data() + offset, &value, 4U);
  };
  auto put64 = [&out](std::size_t offset, std::uint64_t value) {
    std::memcpy(out.data() + offset, &value, 8U);
  };
  put32(8U, format);
  put32(12U, 1U);   // typeSize
  put32(16U, width);
  put32(20U, height);
  put32(24U, 1U);   // depth
  put32(28U, 1U);   // layers
  put32(32U, 1U);   // faces
  put32(36U, levels);
  put32(40U, 0U);   // supercompressionScheme: none
  // dfd/kvd offsets 0 lengths 0 (44..60), sgd 0 (60..80).
  const std::size_t index_offset = 80U;
  const std::uint64_t payload_base = 80U + entries.size() * 24U;
  for (std::size_t level = 0; level < entries.size(); ++level) {
    put64(index_offset + level * 24U,
          payload_base + entries[level].offset);
    put64(index_offset + level * 24U + 8U, entries[level].length);
    put64(index_offset + level * 24U + 16U, entries[level].uncompressed);
  }
  out.insert(out.end(), mip_data.begin(), mip_data.end());
  return out;
}

//! Overwrite a uint32 field in the container.
void poke32(std::vector<std::uint8_t>& data, std::size_t offset,
            std::uint32_t value) {
  std::memcpy(data.data() + offset, &value, 4U);
}

//! Overwrite a uint64 field in the level index.
void poke_level64(std::vector<std::uint8_t>& data, std::size_t level,
                  std::size_t field, std::uint64_t value) {
  std::memcpy(data.data() + 80U + level * 24U + field * 8U, &value, 8U);
}

}  // namespace

TEST(Ktx2Decoder, DecodesLevel0FromMipChainedContainer) {
  constexpr std::uint32_t kWidth = 8U;
  constexpr std::uint32_t kHeight = 4U;
  // Levels: 8x4, 4x2, 2x1, 1x1.
  const std::vector<std::uint8_t> container =
      make_ktx2(kWidth, kHeight, 4U);

  std::string error;
  const auto decoded = omnicpp::asset::decode_ktx2(container.data(),
                                                   container.size(), &error);
  ASSERT_TRUE(decoded.is_ok()) << error;
  const auto& image = decoded.value();
  EXPECT_EQ(image.width, kWidth);
  EXPECT_EQ(image.height, kHeight);
  ASSERT_EQ(image.rgba.size(), kWidth * kHeight * 4U);

  // Level 0 pattern: byte i = (0*37 + i*13) & 0xFF.
  for (std::size_t i = 0; i < image.rgba.size(); ++i) {
    EXPECT_EQ(image.rgba[i],
              static_cast<std::uint8_t>((i * 13U) & 0xFFU))
        << "byte " << i;
  }

  // The magic-byte dispatch routes the same container to the same decoder.
  const auto via_dispatch =
      omnicpp::asset::decode_image(container.data(), container.size(), &error);
  ASSERT_TRUE(via_dispatch.is_ok()) << error;
  EXPECT_EQ(via_dispatch.value().width, kWidth);
  EXPECT_EQ(via_dispatch.value().rgba.size(), image.rgba.size());
}

TEST(Ktx2Decoder, DecodesSingleLevelMinimalContainer) {
  const std::vector<std::uint8_t> container = make_ktx2(2U, 1U, 1U);
  std::string error;
  const auto decoded = omnicpp::asset::decode_ktx2(container.data(),
                                                   container.size(), &error);
  ASSERT_TRUE(decoded.is_ok()) << error;
  EXPECT_EQ(decoded.value().width, 2U);
  EXPECT_EQ(decoded.value().height, 1U);
  EXPECT_EQ(decoded.value().rgba.size(), 8U);
}

TEST(Ktx2Decoder, RejectsMalformedContainers) {
  // Bad magic.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    container[0] = 0x00;
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("identifier"), std::string::npos) << error;
  }
  // Compressed vkFormat named in the diagnostic.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    poke32(container, 8U, 158U);  // VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("vkFormat 158"), std::string::npos) << error;
  }
  // Supercompression scheme set.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    poke32(container, 40U, 1U);  // BasisLZ
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("supercompressionScheme"), std::string::npos)
        << error;
  }
  // Zero width.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    poke32(container, 16U, 0U);
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("nonzero"), std::string::npos) << error;
  }
  // 3D texture.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    poke32(container, 24U, 2U);
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("3D"), std::string::npos) << error;
  }
  // Texture array.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    poke32(container, 28U, 2U);
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("arrays"), std::string::npos) << error;
  }
  // Cubemap.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    poke32(container, 32U, 6U);
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("cubemap"), std::string::npos) << error;
  }
  // Level count out of range.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    poke32(container, 36U, 0U);
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("levelCount"), std::string::npos) << error;
  }
  // Mip level 1 size disagrees with the spec formula.
  {
    std::vector<std::uint8_t> container = make_ktx2(8U, 8U, 2U);
    poke_level64(container, 1U, 2U, 9999U);  // uncompressedByteLength
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("level 1"), std::string::npos) << error;
    EXPECT_NE(error.find("does not match the spec size"), std::string::npos)
        << error;
  }
  // Level payload truncated out of the container.
  {
    std::vector<std::uint8_t> container = make_ktx2(8U, 8U, 2U);
    poke_level64(container, 1U, 0U, container.size() + 100U);
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("runs past the end"), std::string::npos) << error;
  }
  // Level claims compression with scheme 0.
  {
    std::vector<std::uint8_t> container = make_ktx2(8U, 8U, 2U);
    poke_level64(container, 1U, 1U, 4U);  // byteLength < uncompressed
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("is compressed"), std::string::npos) << error;
  }
  // Truncated header.
  {
    std::vector<std::uint8_t> container = make_ktx2(4U, 4U, 1U);
    container.resize(40U);
    std::string error;
    const auto decoded = omnicpp::asset::decode_ktx2(
        container.data(), container.size(), &error);
    EXPECT_FALSE(decoded.is_ok());
    EXPECT_NE(error.find("80-byte header"), std::string::npos) << error;
  }
}
