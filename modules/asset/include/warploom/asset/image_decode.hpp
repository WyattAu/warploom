#pragma once

/**
 * @file image_decode.hpp
 * @brief Canonical decoded-image result shared by the engine's deterministic
 *        image decoders (PNG, JPEG).
 *
 * Every decoder in `warploom/asset` that produces a texture payload returns this
 * type: tightly packed 8-bit RGBA (straight alpha), row-major, so upload paths
 * and consumers never care which codec produced the bytes.
 */

#include "warploom/core/deterministic_runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace warploom::asset {

//! Decoded 8-bit RGBA image, straight (non-premultiplied) alpha.
//! `rgba` is tightly packed, row-major, width*height*4 bytes.
struct DecodedImage {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> rgba{};

  [[nodiscard]] bool valid() const noexcept {
    return width != 0U && height != 0U &&
           rgba.size() == static_cast<std::size_t>(width) * height * 4U;
  }
};

/**
 * @brief Decode one embedded image, dispatching on the payload's magic bytes
 *        to the engine's deterministic codecs (PNG, JPEG).
 *
 * Payloads that match no known signature are rejected with a descriptive
 * error. This is the entry point glTF ingestion uses for `baseColorTexture`
 * and any other encoded image payload.
 */
[[nodiscard]] ::warploom::core::Result<DecodedImage> decode_image(
    const std::uint8_t* bytes, std::size_t length,
    std::string* error_detail = nullptr);

}  // namespace warploom::asset

// S5-B compat footer: legacy `omnicpp::asset` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef WARPLOOM_COMPAT_ASSET_NS
#define WARPLOOM_COMPAT_ASSET_NS
namespace omnicpp::asset {
    using namespace ::warploom::asset;
}
#endif  // WARPLOOM_COMPAT_ASSET_NS
