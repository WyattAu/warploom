//! @file image_decode.cpp
//! @brief Magic-byte dispatch to the engine's deterministic image decoders.

#include "warploom/asset/image_decode.hpp"

#include "warploom/asset/jpeg_decoder.hpp"
#include "warploom/asset/ktx2_decoder.hpp"
#include "warploom/asset/png_decoder.hpp"

#include <algorithm>
#include <cstdint>

namespace omnicpp::asset {

namespace {
constexpr std::uint8_t kPngSignature[8] = {0x89, 0x50, 0x4e, 0x47,
                                           0x0d, 0x0a, 0x1a, 0x0a};
constexpr std::uint8_t kKtx2Signature[8] = {0xAB, 0x4B, 0x54, 0x58,
                                            0x20, 0x32, 0x30, 0xBB};
}

omnicpp::core::Result<DecodedImage> decode_image(
    const std::uint8_t* bytes, std::size_t length,
    std::string* error_detail) {
  const bool is_png =
      length >= 8U &&
      std::equal(std::begin(kPngSignature), std::end(kPngSignature), bytes);
  const bool is_jpeg =
      length >= 3U && bytes[0] == 0xFFU && bytes[1] == 0xD8U &&
      bytes[2] == 0xFFU;
  const bool is_ktx2 =
      length >= 8U &&
      std::equal(std::begin(kKtx2Signature), std::end(kKtx2Signature), bytes);
  std::string detail;
  if (is_png) {
    return decode_png(bytes, length, error_detail != nullptr ? error_detail
                                                             : &detail);
  }
  if (is_jpeg) {
    return decode_jpeg(bytes, length, error_detail != nullptr ? error_detail
                                                              : &detail);
  }
  if (is_ktx2) {
    return decode_ktx2(bytes, length, error_detail != nullptr ? error_detail
                                                              : &detail);
  }
  if (error_detail != nullptr) {
    *error_detail =
        "payload is neither a decodable PNG (\\x89PNG signature), a "
        "baseline JPEG (FF D8 FF SOI marker), nor a KTX2 container";
  }
  return omnicpp::core::Result<DecodedImage>::error(
      omnicpp::core::RuntimeError::malformed_asset);
}

}  // namespace omnicpp::asset
