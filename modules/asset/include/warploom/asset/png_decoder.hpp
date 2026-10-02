#pragma once

/**
 * @file png_decoder.hpp
 * @brief Deterministic, dependency-free PNG decoder for the engine.
 *
 * Decodes PNG image data into the engine's canonical CPU texture format:
 * tightly packed 8-bit RGBA (straight, non-premultiplied alpha), row-major.
 *
 * Supported subset (rigorously validated, everything else is rejected with a
 * descriptive error instead of being mis-decoded):
 *   - 8-bit samples, non-interlaced (Adam7 is rejected explicitly);
 *   - colour types 0 (greyscale), 2 (truecolour), 3 (indexed-colour with
 *     PLTE + optional tRNS), 4 (greyscale+alpha), 6 (truecolour+alpha);
 *   - all five PNG scanline filters (None/Sub/Up/Average/Paeth);
 *   - tRNS transparency keys for greyscale (2 bytes), truecolour (6 bytes),
 *     and per-entry palette alpha;
 *   - DEFLATE streams (RFC 1951) with stored, fixed-Huffman and
 *     dynamic-Huffman blocks, one or many consecutive IDAT chunks.
 *
 * Rejected with `RuntimeError::malformed_asset`: invalid signature, bad chunk
 * CRCs, unknown *critical* chunks, malformed or truncated chunk framing,
 * invalid zlib headers (including preset dictionaries), over-subscribed or
 * invalid Huffman tables, distances beyond the emitted output, invalid
 * scanline filter types, palette-index overruns, PLTE/tRNS/IDAT ordering
 * violations, unsupported bit depths / colour types / interlace, and
 * dimensions whose decoded size would exceed the documented cap.
 *
 * The decoder never throws except for the vector allocations it performs
 * (bad_alloc), and is deterministic: identical inputs always produce
 * identical outputs. All reads are bounds-checked before they happen.
 */

#include "warploom/asset/image_decode.hpp"
#include "warploom/core/deterministic_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace warploom::asset {

//! Hard cap on decoded pixel memory (RGBA). 256 MiB guards the 32-bit
//! product and keeps hostile dimensions from exhausting memory. Shared with
//! the JPEG decoder, which enforces the same budget via kMaxDecodedJpegBytes.
inline constexpr std::uint64_t kMaxDecodedPngBytes = 256ULL * 1024ULL * 1024ULL;

/**
 * @brief Decode one PNG image.
 * @param bytes  Complete PNG file contents.
 * @param length Byte length of @p bytes.
 * @param error_detail Optional out-parameter receiving a human-readable
 *                     reason when decoding fails (never written on success).
 */
[[nodiscard]] ::warploom::core::Result<DecodedImage> decode_png(
    const std::uint8_t* bytes, std::size_t length,
    std::string* error_detail = nullptr);

} // namespace warploom::asset

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
