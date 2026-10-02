#pragma once

/**
 * @file jpeg_decoder.hpp
 * @brief Deterministic, dependency-free baseline JPEG decoder for the engine.
 *
 * Decodes JPEG image data into the engine's canonical CPU texture format:
 * tightly packed 8-bit RGBA (opaque), row-major — the same `DecodedImage` the
 * PNG decoder produces, so both codecs feed identical upload paths.
 *
 * Supported subset (rigorously validated, everything else is rejected with a
 * descriptive error instead of being mis-decoded):
 *   - baseline sequential DCT (SOF0) only, 8-bit samples;
 *   - one component (grayscale, rendered as grey RGB) or three components in
 *     the JFIF convention (YCbCr, full-range BT.601 conversion);
 *   - per-component sampling factors up to 4 with whole-number upsampling
 *     ratios (4:4:4, 4:2:2, 4:2:0, 4:1:1 and similar); fractional ratios are
 *     rejected. 2x chroma expansion uses triangle ("fancy") filtering with
 *     edge replication, matching libjpeg's default upsampler; other whole
 *     ratios replicate nearest-neighbour;
 *   - 8-bit quantisation tables (DQT), one Huffman table per (DC|AC, 0..3)
 *     slot built from DHT, restart intervals (DRI) with RSTn markers,
 *     0xFF00 byte stuffing, and JPEG fill bytes;
 *   - APPn/COM segments are validated for framing and skipped.
 *
 * Rejected with `RuntimeError::malformed_asset`: progressive (SOF2), extended
 * sequential / 12-bit (SOF1), arithmetic or other SOFn frames, non-8-bit
 * samples, two/four-component frames, missing quantisation or Huffman tables,
 * out-of-range table/component references, malformed DHT (length/overflow or
 * over-subscribed code space), component or scan-shape mismatches, spectral
 * parameters outside baseline's all-in-one scan, dimensions whose decoded
 * size would exceed the documented cap, truncated entropy data, missing or
 * mismatched restart markers, a missing EOI, and trailing markers that are
 * neither APPn/COM nor EOI.
 *
 * The decoder never throws except for the vector allocations it performs
 * (bad_alloc), and is deterministic on a build: identical inputs always
 * produce identical outputs. The inverse DCT is performed in floating point,
 * so a value may differ by at most a few units from another decoder's integer
 * IDCT on a rounding boundary. All reads are bounds-checked before they
 * happen.
 */

#include "warploom/asset/image_decode.hpp"
#include "warploom/core/deterministic_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace warploom::asset {

//! Hard cap on decoded pixel memory (RGBA). Same budget as the PNG decoder:
//! guards the 32-bit product and keeps hostile dimensions from exhausting
//! memory.
inline constexpr std::uint64_t kMaxDecodedJpegBytes =
    256ULL * 1024ULL * 1024ULL;

/**
 * @brief Decode one baseline sequential JPEG image.
 * @param bytes  Complete JPEG file contents.
 * @param length Byte length of @p bytes.
 * @param error_detail Optional out-parameter receiving a human-readable
 *                     reason when decoding fails (never written on success).
 */
[[nodiscard]] ::warploom::core::Result<DecodedImage> decode_jpeg(
    const std::uint8_t* bytes, std::size_t length,
    std::string* error_detail = nullptr);

}  // namespace warploom::asset

// S5-B compat footer: legacy `omnicpp::asset` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef OMNICPP_COMPAT_ASSET_NS
#define OMNICPP_COMPAT_ASSET_NS
namespace omnicpp::asset {
    using namespace ::warploom::asset;
}
#endif  // OMNICPP_COMPAT_ASSET_NS
