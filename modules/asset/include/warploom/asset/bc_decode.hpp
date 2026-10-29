#pragma once

/**
 * @file bc_decode.hpp
 * @brief In-tree BC1/BC3 block decoding to RGBA8 (KTX2 payloads).
 *
 * Block Compression is integer math on 4x4 texel blocks: BC1 stores two
 * RGB565 endpoints + 16 2-bit interpolants (8 bytes); BC3 stores two alpha
 * endpoints + 16 3-bit alpha interpolants + a BC1 color block (16 bytes).
 * Decoding here is total (every input byte pattern yields defined output)
 * and dependency-free, like the PNG/JPEG decoders: the KTX2 layer validates
 * sizes, this layer does the arithmetic.
 *
 * vkFormat values are the Vulkan block-compressed enums, measured from
 * vulkan_core.h (BC1_RGB_UNORM_BLOCK = 131 .. BC3_SRGB_BLOCK = 138).
 * UNORM and SRGB variants decode identically; sRGB-ness stays an upload
 * interpretation, exactly as for uncompressed RGBA8 (37 vs 43).
 */

#include <cstdint>

#include "warploom/asset/image_decode.hpp"

namespace warploom::asset {

inline constexpr std::uint32_t kVkBc1RgbUnorm = 131U;
inline constexpr std::uint32_t kVkBc1RgbSrgb = 132U;
inline constexpr std::uint32_t kVkBc1RgbaUnorm = 133U;
inline constexpr std::uint32_t kVkBc1RgbaSrgb = 134U;
inline constexpr std::uint32_t kVkBc3Unorm = 137U;
inline constexpr std::uint32_t kVkBc3Srgb = 138U;

//! True for the BC formats this module decodes (131-134, 137-138).
[[nodiscard]] bool is_bc_format(std::uint32_t vk_format) noexcept;
//! True for BC1 (131-134: 8-byte blocks).
[[nodiscard]] bool is_bc1_format(std::uint32_t vk_format) noexcept;
//! True for BC3 (137-138: 16-byte blocks).
[[nodiscard]] bool is_bc3_format(std::uint32_t vk_format) noexcept;
//! Block size in bytes: 8 (BC1), 16 (BC3), 0 (not a decoded BC format).
[[nodiscard]] std::uint32_t bc_block_bytes(std::uint32_t vk_format) noexcept;

//! Compressed level size per the KTX2 formula: ceil(w_l/4) * ceil(h_l/4)
//! blocks of `block_bytes`, where w_l = max(1, w >> level). 64-bit: the
//! product cannot overflow before the decoded-size cap is applied.
[[nodiscard]] std::uint64_t bc_level_bytes(std::uint32_t width,
                                           std::uint32_t height,
                                           std::uint32_t block_bytes,
                                           std::uint32_t level) noexcept;

//! Decode one mip level's blocks (row-major block grid) to tightly packed
//! row-major RGBA8, cropping block-grid overhang for dimensions that are
//! not multiples of 4. `blocks` must hold exactly the level's bytes.
[[nodiscard]] ::warploom::core::Result<DecodedImage> decode_bc_level(
    std::uint32_t width, std::uint32_t height, std::uint32_t vk_format,
    const std::uint8_t* blocks, std::size_t length,
    std::string* error_detail = nullptr);

}  // namespace warploom::asset

// S5-B compat footer: legacy `omnicpp::asset` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives).
#ifndef WARPLOOM_COMPAT_ASSET_NS
#define WARPLOOM_COMPAT_ASSET_NS
namespace omnicpp::asset {
    using namespace ::warploom::asset;
}
#endif  // WARPLOOM_COMPAT_ASSET_NS
