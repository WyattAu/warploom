//! @file ktx2_decoder.cpp
//! @brief KTX2 container decoding for uncompressed RGBA8 textures.
//!
//! Layout per the KTX2 2.0 specification:
//!   [0]   8-byte identifier «KTX 20»
//!   [8]   vkFormat (uint32)
//!   [12]  typeSize (uint32)
//!   [16]  pixelWidth, pixelHeight, pixelDepth (3 x uint32)
//!   [28]  layerCount, faceCount, levelCount (3 x uint32)
//!   [40]  supercompressionScheme (uint32)
//!   [44]  dfdByteOffset, dfdByteLength (2 x uint32)
//!   [52]  kvdByteOffset, kvdByteLength (2 x uint32)
//!   [60]  supercompressionGlobalDataByteOffset/Length (2 x uint64)
//!   [80]  levelIndex: levelCount x {byteOffset:u64, byteLength:u64,
//!         uncompressedByteLength:u64}
//! then DFD, key/value data (zero-padded to 4), and mip payloads.

#include "warploom/asset/ktx2_decoder.hpp"

#include <cstring>

namespace omnicpp::asset {

namespace {

constexpr std::uint8_t kKtx2Magic[8] = {0xAB, 0x4B, 0x54, 0x58,
                                        0x20, 0x32, 0x30, 0xBB};

// vkFormat values this decoder accepts (Vulkan non-packed 8-bit RGBA).
constexpr std::uint32_t kFormatR8G8B8A8Unorm = 37U;   // VK_FORMAT_R8G8B8A8_UNORM
constexpr std::uint32_t kFormatR8G8B8A8Srgb = 43U;    // VK_FORMAT_R8G8B8A8_SRGB

// Known non-RGBA formats named in diagnostics so authors see what to fix.
//! Returns false when `length` cannot safely hold `offset + size` bytes.
[[nodiscard]] bool in_bounds(std::size_t length, std::size_t offset,
                             std::size_t size) {
  return offset <= length && size <= length - offset;
}

//! Uncompressed RGBA8 mip level size per the KTX2 spec: ceil(w / 2^level) x
//! ceil(h / 2^level) x 4. Dimensions are rounded UP.
[[nodiscard]] std::uint64_t level_bytes(std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint32_t level) {
  const auto shift = [](std::uint32_t dim, std::uint32_t lvl) -> std::uint64_t {
    const std::uint64_t shifted =
        static_cast<std::uint64_t>(dim) >> lvl;
    return shifted != 0U ? shifted : 1ULL;
  };
  return shift(width, level) * shift(height, level) * 4ULL;
}

}  // namespace

omnicpp::core::Result<DecodedImage> decode_ktx2(
    const std::uint8_t* bytes, std::size_t length,
    std::string* error_detail) {
  auto fail = [&](const std::string& message)
      -> omnicpp::core::Result<DecodedImage> {
    if (error_detail != nullptr) *error_detail = message;
    return omnicpp::core::Result<DecodedImage>::error(
        omnicpp::core::RuntimeError::malformed_asset);
  };

  if (bytes == nullptr || length < 80U) {
    return fail("KTX2 container is smaller than its 80-byte header");
  }
  if (std::memcmp(bytes, kKtx2Magic, 8U) != 0) {
    return fail("payload does not carry the KTX2 identifier (AB 4B 54 58 "
                "20 32 30 BB)");
  }

  auto read_u32 = [&](std::size_t offset) -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes + offset, 4U);
    return value;
  };
  auto read_u64 = [&](std::size_t offset) -> std::uint64_t {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes + offset, 8U);
    return value;
  };

  const std::uint32_t format = read_u32(8U);
  const std::uint32_t type_size = read_u32(12U);
  const std::uint32_t width = read_u32(16U);
  const std::uint32_t height = read_u32(20U);
  const std::uint32_t depth = read_u32(24U);
  const std::uint32_t layers = read_u32(28U);
  const std::uint32_t faces = read_u32(32U);
  const std::uint32_t levels = read_u32(36U);
  const std::uint32_t scheme = read_u32(40U);

  if (format != kFormatR8G8B8A8Unorm && format != kFormatR8G8B8A8Srgb) {
    return fail("KTX2 vkFormat " + std::to_string(format) +
                " is not uncompressed R8G8B8A8; only UNORM (37) and SRGB "
                "(43) RGBA8 payloads are supported");
  }
  if (type_size != 1U) {
    return fail("KTX2 typeSize must be 1 for RGBA8; got " +
                std::to_string(type_size));
  }
  if (width == 0U || height == 0U) {
    return fail("KTX2 pixelWidth and pixelHeight must be nonzero");
  }
  if (depth != 1U) {
    return fail("KTX2 3D textures (pixelDepth != 1) are not supported");
  }
  if (layers != 1U) {
    return fail("KTX2 texture arrays (layerCount != 1) are not supported");
  }
  if (faces != 1U) {
    return fail("KTX2 cubemaps (faceCount != 1) are not supported here");
  }
  if (levels == 0U || levels > 32U) {
    return fail("KTX2 levelCount must be in [1, 32]; got " +
                std::to_string(levels));
  }
  if (scheme != 0U) {
    return fail("KTX2 supercompressionScheme must be 0 (none); got " +
                std::to_string(scheme));
  }

  // Level index: validate every level's size against the spec formula and
  // the container bounds, so truncated mip chains fail loudly.
  const std::size_t level_index_offset = 80U;
  if (!in_bounds(length, level_index_offset,
                 static_cast<std::size_t>(levels) * 24U)) {
    return fail("KTX2 level index runs past the end of the container");
  }
  for (std::uint32_t level = 0; level < levels; ++level) {
    const std::size_t entry = level_index_offset + level * 24U;
    const std::uint64_t byte_offset = read_u64(entry);
    const std::uint64_t byte_length = read_u64(entry + 8U);
    const std::uint64_t uncompressed = read_u64(entry + 16U);
    const std::uint64_t expected = level_bytes(width, height, level);
    if (uncompressed != expected) {
      return fail("KTX2 level " + std::to_string(level) +
                  " uncompressedByteLength " + std::to_string(uncompressed) +
                  " does not match the spec size " + std::to_string(expected) +
                  " for " + std::to_string(width) + "x" + std::to_string(height));
    }
    if (byte_length != uncompressed) {
      return fail("KTX2 level " + std::to_string(level) +
                  " is compressed (byteLength " + std::to_string(byte_length) +
                  " != uncompressedByteLength) with scheme 0");
    }
    if (byte_offset > length ||
        byte_length > static_cast<std::uint64_t>(length) - byte_offset) {
      return fail("KTX2 level " + std::to_string(level) +
                  " payload runs past the end of the container");
    }
  }

  // Level 0 payload -> DecodedImage.
  const std::uint64_t level0_offset = read_u64(level_index_offset);
  const std::uint64_t level0_bytes = static_cast<std::uint64_t>(width) *
                                     height * 4ULL;
  DecodedImage image;
  image.width = width;
  image.height = height;
  image.rgba.resize(static_cast<std::size_t>(level0_bytes));
  std::memcpy(image.rgba.data(), bytes + level0_offset,
              static_cast<std::size_t>(level0_bytes));
  return omnicpp::core::Result<DecodedImage>::ok(std::move(image));
}

}  // namespace omnicpp::asset
