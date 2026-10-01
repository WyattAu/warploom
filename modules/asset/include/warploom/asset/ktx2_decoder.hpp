#pragma once

/**
 * @file ktx2_decoder.hpp
 * @brief KTX2 container decoding for uncompressed RGBA8 textures.
 *
 * Parses the KTX2 container (Khronos Texture 2.0): fixed 80-byte header,
 * level index, and the mip level 0 payload for non-supercompressed
 * VK_FORMAT_R8G8B8A8_UNORM / VK_FORMAT_R8G8B8A8_SRGB textures. Mip levels
 * beyond level 0 are size-validated against the spec formula (all levels
 * must be present and correctly sized) but only level 0 is returned, which
 * matches the engine's DecodedImage contract.
 *
 * Supercompressed payloads (supercompressionScheme != 0 or any Basis/ETC/BC
 * format) are rejected with an explicit diagnostic: transcoding those is a
 * deliberate non-goal here, and silent misinterpretation is worse than a
 * clear error.
 */

#include <cstdint>
#include <vector>

#include "warploom/asset/image_decode.hpp"

namespace omnicpp::asset {

//! Decode a KTX2 container whose payload is uncompressed R8G8B8A8.
//! Returns the level-0 image in RGBA8 order. On any malformed input the
//! Result carries malformed_asset and `error_detail` (when non-null) names
//! the violated constraint.
[[nodiscard]] omnicpp::core::Result<DecodedImage> decode_ktx2(
    const std::uint8_t* bytes, std::size_t length,
    std::string* error_detail = nullptr);

}  // namespace omnicpp::asset
