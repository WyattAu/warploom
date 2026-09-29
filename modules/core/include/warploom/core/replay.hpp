#pragma once

#include "warploom/core/deterministic_runtime.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

namespace warploom::core {

enum class ReplayDecodeError : std::uint8_t { none, too_small, invalid_magic, unsupported_version, invalid_count, checksum_mismatch, invalid_order, io_error };

template <std::size_t Capacity>
class Replay final {
public:
  struct Record { std::uint64_t tick; DeterministicRuntime::Event event; };
  struct Header { std::uint32_t magic{0x4F52504CU}; std::uint16_t version{1}; std::uint32_t record_count{0}; std::uint64_t fixed_step_nanoseconds{0}; std::uint64_t checksum{0}; };
  static constexpr std::size_t serialized_header_size = 26;
  static constexpr std::size_t serialized_record_size = 16;

  [[nodiscard]] bool record(std::uint64_t tick, DeterministicRuntime::Event event) noexcept {
    if (count_ == Capacity || (count_ > 0 && tick < records_[count_ - 1].tick)) {
      return false;
    }
    records_[count_++] = Record{tick, event};
    return true;
  }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
  [[nodiscard]] const Record& at(std::size_t index) const noexcept { return records_[index]; }
  [[nodiscard]] std::uint64_t checksum() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < count_; ++i) {
      hash = mix(hash, records_[i].tick);
      hash = mix(hash, records_[i].event);
    }
    return hash;
  }
  [[nodiscard]] Header header(double step) const noexcept { return Header{0x4F52504CU, 1, static_cast<std::uint32_t>(count_), static_cast<std::uint64_t>(step * 1'000'000'000.0), checksum()}; }
  template <typename Callback> void playback(Callback&& callback) const { for (std::size_t i = 0; i < count_; ++i) callback(records_[i]); }

  [[nodiscard]] ReplayDecodeError encode(double step, std::uint8_t* output, std::size_t output_size, std::size_t& written) const noexcept {
    const auto h = header(step);
    const auto required = serialized_header_size + count_ * serialized_record_size;
    written = 0;
    if (output == nullptr || output_size < required) return ReplayDecodeError::too_small;
    std::size_t offset = 0;
    put_u32(output, offset, h.magic);
    put_u16(output, offset, h.version);
    put_u32(output, offset, h.record_count);
    put_u64(output, offset, h.fixed_step_nanoseconds);
    put_u64(output, offset, h.checksum);
    for (std::size_t i = 0; i < count_; ++i) {
      put_u64(output, offset, records_[i].tick);
      put_u64(output, offset, records_[i].event);
    }
    written = offset;
    return ReplayDecodeError::none;
  }

  [[nodiscard]] ReplayDecodeError decode(const std::uint8_t* input, std::size_t input_size, double& step) noexcept {
    if (input == nullptr || input_size < serialized_header_size) return ReplayDecodeError::too_small;
    std::size_t offset = 0;
    const auto magic = get_u32(input, offset);
    const auto version = get_u16(input, offset);
    const auto record_count = get_u32(input, offset);
    const auto step_ns = get_u64(input, offset);
    const auto expected = get_u64(input, offset);
    if (magic != 0x4F52504CU) {
      return ReplayDecodeError::invalid_magic;
    }
    if (version != 1) {
      return ReplayDecodeError::unsupported_version;
    }
    if (record_count > Capacity) {
      return ReplayDecodeError::invalid_count;
    }
    const auto required = serialized_header_size + static_cast<std::size_t>(record_count) * serialized_record_size;
    if (input_size < required) {
      return ReplayDecodeError::too_small;
    }
    count_ = 0;
    step = static_cast<double>(step_ns) / 1'000'000'000.0;
    for (std::uint32_t i = 0; i < record_count; ++i) {
      const auto tick = get_u64(input, offset);
      const auto event = get_u64(input, offset);
      if (!record(tick, event)) {
        count_ = 0;
        return ReplayDecodeError::invalid_order;
      }
    }
    if (checksum() != expected) {
      count_ = 0;
      return ReplayDecodeError::checksum_mismatch;
    }
    return ReplayDecodeError::none;
  }

  [[nodiscard]] ReplayDecodeError save(const std::string& path, double step) const {
    const auto required = serialized_header_size + count_ * serialized_record_size;
    std::array<std::uint8_t, serialized_header_size + Capacity * serialized_record_size> buffer{};
    std::size_t written = 0;
    const auto result = encode(step, buffer.data(), required, written);
    if (result != ReplayDecodeError::none) {
      return result;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
      return ReplayDecodeError::io_error;
    }
    file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(written));
    return file.good() ? ReplayDecodeError::none : ReplayDecodeError::io_error;
  }

  [[nodiscard]] ReplayDecodeError load(const std::string& path, double& step) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
      return ReplayDecodeError::io_error;
    }
    const auto end = file.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > serialized_header_size + Capacity * serialized_record_size) {
      return ReplayDecodeError::too_small;
    }
    const auto size = static_cast<std::size_t>(end);
    std::array<std::uint8_t, serialized_header_size + Capacity * serialized_record_size> buffer{};
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    if (!file.good() && !file.eof()) {
      return ReplayDecodeError::io_error;
    }
    return decode(buffer.data(), size, step);
  }

private:
  [[nodiscard]] static std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept { for (std::size_t byte = 0; byte < 8; ++byte) { hash ^= (value >> (byte * 8U)) & 0xffU; hash *= 1099511628211ULL; } return hash; }
  static void put_u16(std::uint8_t* out, std::size_t& o, std::uint16_t v) noexcept { out[o++] = static_cast<std::uint8_t>(v); out[o++] = static_cast<std::uint8_t>(v >> 8U); }
  static void put_u32(std::uint8_t* out, std::size_t& o, std::uint32_t v) noexcept { for (unsigned i = 0; i < 4; ++i) out[o++] = static_cast<std::uint8_t>(v >> (i * 8U)); }
  static void put_u64(std::uint8_t* out, std::size_t& o, std::uint64_t v) noexcept { for (unsigned i = 0; i < 8; ++i) out[o++] = static_cast<std::uint8_t>(v >> (i * 8U)); }
  [[nodiscard]] static std::uint16_t get_u16(const std::uint8_t* in, std::size_t& o) noexcept { const auto a = in[o++]; const auto b = in[o++]; return static_cast<std::uint16_t>(a | (b << 8U)); }
  [[nodiscard]] static std::uint32_t get_u32(const std::uint8_t* in, std::size_t& o) noexcept { std::uint32_t v = 0; for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[o++]) << (i * 8U); return v; }
  [[nodiscard]] static std::uint64_t get_u64(const std::uint8_t* in, std::size_t& o) noexcept { std::uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(in[o++]) << (i * 8U); return v; }
  std::array<Record, Capacity> records_{}; std::size_t count_{0};
};

template <std::size_t Capacity>
[[nodiscard]] bool verify_replay(const Replay<Capacity>& replay, double step, std::uint64_t expected_hash) {
  DeterministicRuntime runtime(step); if (!runtime.start().is_ok()) return false; std::uint64_t tick = 0; bool valid = true;
  replay.playback([&](const auto& record) { if (!valid || record.tick < tick) { valid = false; return; } while (tick < record.tick) { runtime.advance(step, [](auto) {}, [](auto) {}); ++tick; } if (!runtime.post_event(record.event)) valid = false; });
  return valid && runtime.state_hash() == expected_hash;
}

} // namespace warploom::core
// S2-B compat footer: legacy `omnicpp::*` spellings keep resolving
// during the transition (removed with the S5 identity pass -
// docs/warploom-core-plan.md, phase B). A using-directive in a
// namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types, and legally coexists
// with real `omnicpp::core` extension blocks elsewhere (extension
// blocks merge). One directive per namespace THIS header declares,
// each under its OWN guard (a shared guard would suppress later
// headers' distinct directives).
#ifndef OMNICPP_COMPAT_CORE_NS
#define OMNICPP_COMPAT_CORE_NS
namespace omnicpp::core {
    using namespace ::warploom::core;
}
#endif  // OMNICPP_COMPAT_CORE_NS
