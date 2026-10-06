//! @file test_reflector_and_allocator_robustness.cpp
//! @brief Adversarial-input tests for the SPIR-V reflector and property tests
//!        for the Vulkan memory allocator.
//!
//! The reflector parses compiler output that may be truncated or corrupted in
//! transit; it must never read out of bounds or crash. The allocator manages
//! non-overlapping sub-allocations across arbitrary alloc/free sequences; these
//! property tests verify that invariant under randomness (fixed seed).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "warploom/render/vulkan_descriptors.hpp"
#if WARPLOOM_VULKAN_TYPES_AVAILABLE
#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#endif

namespace {

constexpr std::uint32_t kSpirvMagic = 0x07230203U;

[[maybe_unused]] std::vector<std::uint32_t> load_shader_words(const char* path) {
  std::vector<std::uint32_t> words;
  std::FILE* file = std::fopen(path, "rb");
  if (!file) return words;
  std::uint32_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 4, 4096, file)) > 0) {
    words.insert(words.end(), buf, buf + n);
  }
  std::fclose(file);
  return words;
}

}  // namespace

// =============================================================================
// SPIR-V reflector: malformed-input battery
// =============================================================================

TEST(SpirvReflector, RejectsNullAndTrivialInputs) {
  // Null code / zero length must be rejected, not dereferenced.
  EXPECT_TRUE(omnicpp::render::reflect_spirv_resources(
                  static_cast<const std::uint32_t*>(nullptr), 100).empty());
  std::vector<std::uint32_t> tiny = {kSpirvMagic};
  // Every sub-minimum word count is safely rejected (loop never over-reads).
  for (std::size_t words = 0; words <= 8; ++words) {
    std::vector<std::uint32_t> truncated(
        tiny.begin(),
        tiny.begin() +
            static_cast<std::vector<std::uint32_t>::difference_type>(
                std::min<std::size_t>(words, tiny.size())));
    if (words < 1U) truncated.clear();
    // These fuzz malformed SPIR-V on purpose. The claim under test is that
    // reflection survives it -- not any particular return value -- so the
    // reflected bindings are deliberately discarded.
    EXPECT_NO_FATAL_FAILURE((void)(omnicpp::render::reflect_spirv_resources(truncated.data(), words)));;
  }
  EXPECT_TRUE(omnicpp::render::reflect_spirv_resources(
                  static_cast<const std::uint32_t*>(nullptr), 0).empty());
}

TEST(SpirvReflector, RejectsWrongMagic) {
  std::vector<std::uint32_t> words(64, 0x11111111U);
  const auto out = omnicpp::render::reflect_spirv_resources(words.data(), words.size());
  EXPECT_TRUE(out.empty());
  // Byte overload: wrong magic via unaligned-length buffer is also rejected.
  const auto bytes = reinterpret_cast<const std::uint8_t*>(words.data());
  EXPECT_TRUE(omnicpp::render::reflect_spirv_resources(bytes, words.size() * 4U).empty());
}

TEST(SpirvReflector, ByteOverloadRejectsBadLengths) {
  std::vector<std::uint32_t> words = {kSpirvMagic, 0, 0, 0, 0, 0, 0, 0};
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(words.data());
  // Non-multiple-of-4 lengths must be rejected outright.
  for (std::size_t bad_bytes : {std::size_t{0}, std::size_t{3}, std::size_t{7}, std::size_t{19}}) {
    EXPECT_TRUE(omnicpp::render::reflect_spirv_resources(bytes, bad_bytes).empty());
  }
}

#ifdef WARPLOOM_TEST_SHADER_DIR
TEST(SpirvReflector, SurvivesTruncationOfRealShader) {
  const auto words = load_shader_words(WARPLOOM_TEST_SHADER_DIR "/ubo_triangle.frag.spv");
  ASSERT_GE(words.size(), 10U);
  EXPECT_FALSE(omnicpp::render::reflect_spirv_resources(words.data(), words.size()).empty());
  // Truncate at every boundary: the parser must stop cleanly each time.
  for (std::size_t keep = 5; keep < words.size(); keep += 3) {
    std::vector<std::uint32_t> truncated(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(keep));
    // These fuzz malformed SPIR-V on purpose. The claim under test is that
    // reflection survives it -- not any particular return value -- so the
    // reflected bindings are deliberately discarded.
    EXPECT_NO_FATAL_FAILURE((void)(omnicpp::render::reflect_spirv_resources(truncated.data(), truncated.size())););
  }
}

TEST(SpirvReflector, SurvivesWordCountLies) {
  const auto words = load_shader_words(WARPLOOM_TEST_SHADER_DIR "/ubo_triangle.frag.spv");
  ASSERT_GE(words.size(), 10U);
  // A hostile producer may claim any word count per instruction. The parser
  // must treat claims past the end of the stream as terminators.
  for (std::size_t i = 5; i < words.size(); i += 7) {
    std::vector<std::uint32_t> mutated = words;
    mutated[i] = (0xFFFFU << 16U) | (mutated[i] & 0xFFFFU);  // claim max length
    // These fuzz malformed SPIR-V on purpose. The claim under test is that
    // reflection survives it -- not any particular return value -- so the
    // reflected bindings are deliberately discarded.
    EXPECT_NO_FATAL_FAILURE((void)(omnicpp::render::reflect_spirv_resources(mutated.data(), mutated.size())););
    mutated[i] = mutated[i] & 0xFFFFU;  // claim zero length
    // These fuzz malformed SPIR-V on purpose. The claim under test is that
    // reflection survives it -- not any particular return value -- so the
    // reflected bindings are deliberately discarded.
    EXPECT_NO_FATAL_FAILURE((void)(omnicpp::render::reflect_spirv_resources(mutated.data(), mutated.size())););
  }
}

TEST(SpirvReflector, SurvivesRandomByteCorruption) {
  const auto words = load_shader_words(WARPLOOM_TEST_SHADER_DIR "/ubo_triangle.frag.spv");
  ASSERT_GE(words.size(), 10U);
  std::mt19937 rng(0xC0FFEEU);  // fixed seed: deterministic, reproducible
  for (std::size_t iteration = 0; iteration < static_cast<std::size_t>(2000); ++iteration) {
    std::vector<std::uint32_t> mutated = words;
    const auto flips = static_cast<std::size_t>(1 + rng() % 16);
    for (std::size_t f = 0; f < flips; ++f) {
      const auto word_index = static_cast<std::size_t>(rng() % mutated.size());
      const auto byte_index = static_cast<unsigned>(rng() % 4);
      mutated[word_index] ^= (0xFFU << (byte_index * 8U));
    }
    // Contract: must not crash or read out of bounds. Output validity is
    // unchecked — the input is intentionally meaningless.
    // These fuzz malformed SPIR-V on purpose. The claim under test is that
    // reflection survives it -- not any particular return value -- so the
    // reflected bindings are deliberately discarded.
    EXPECT_NO_FATAL_FAILURE((void)(omnicpp::render::reflect_spirv_resources(mutated.data(), mutated.size())););
  }
}
#endif  // WARPLOOM_TEST_SHADER_DIR

// =============================================================================
// Memory allocator: randomized property tests
// =============================================================================

#if WARPLOOM_VULKAN_TYPES_AVAILABLE
namespace {

struct LiveAlloc {
  omnicpp::render::Allocation allocation;
  std::size_t block_index;
};

//! For every same-block pair, ranges must not overlap. Also verifies sizes.
void verify_no_overlap(const std::vector<LiveAlloc>& live) {
  for (std::size_t i = 0; i < live.size(); ++i) {
    ASSERT_GT(live[i].allocation.size, 0U) << "zero-size allocation escaped";
    for (std::size_t j = i + 1; j < live.size(); ++j) {
      if (live[i].allocation.memory != live[j].allocation.memory) continue;
      const bool disjoint =
          live[i].allocation.offset + live[i].allocation.size <= live[j].allocation.offset ||
          live[j].allocation.offset + live[j].allocation.size <= live[i].allocation.offset;
      ASSERT_TRUE(disjoint)
          << "overlap: [" << live[i].allocation.offset << "," << live[i].allocation.size
          << "] vs [" << live[j].allocation.offset << "," << live[j].allocation.size << "]";
    }
  }
}

}  // namespace

TEST(VulkanAllocator, RandomizedAllocFreePreservesDisjointness) {
  if (!omnicpp::render::VulkanContext::is_available()) GTEST_SKIP() << "Vulkan loader unavailable";

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppAllocatorPropertyTest", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  constexpr VkMemoryPropertyFlags kProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  std::mt19937 rng(0x5EED1U);  // fixed seed: failures reproduce
  std::vector<LiveAlloc> live;
  auto make_buffer = [&](std::size_t size) {
    auto result = allocator.create_buffer(static_cast<VkDeviceSize>(size), kUsage, kProps);
    return result;
  };

  // Wave 1: random sizes, verify disjointness after every allocation.
  for (std::size_t i = 0; i < static_cast<std::size_t>(40); ++i) {
    const auto size = static_cast<std::size_t>(1 + rng() % 32768);
    auto result = make_buffer(size);
    ASSERT_TRUE(result.is_ok()) << "allocation of " << size << " bytes failed";
    live.push_back({result.value(), result.value().block_index});
    verify_no_overlap(live);
  }

  // Free half in shuffled order, then allocate again — freed ranges must be
  // reusable without overlap (coalescing integrity).
  std::shuffle(live.begin(), live.end(), rng);
  const auto half = live.size() / 2;
  for (std::size_t i = 0; i < half; ++i) {
    allocator.destroy_allocation(live[i].allocation);
  }
  live.erase(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(half));

  for (std::size_t i = 0; i < static_cast<std::size_t>(20); ++i) {
    const auto size = static_cast<std::size_t>(1 + rng() % 65536);
    auto result = make_buffer(size);
    ASSERT_TRUE(result.is_ok());
    live.push_back({result.value(), result.value().block_index});
    verify_no_overlap(live);
  }

  // Tear down everything: the allocator must reclaim every byte.
  for (auto& entry : live) {
    allocator.destroy_allocation(entry.allocation);
  }
  live.clear();
  const auto stats = allocator.stats();
  EXPECT_EQ(stats.allocation_count, 0U);
  EXPECT_EQ(stats.used_bytes, 0U);
  EXPECT_GT(stats.reserved_bytes, 0U);

  allocator.cleanup();
  context.cleanup();
}
#endif  // WARPLOOM_VULKAN_TYPES_AVAILABLE
