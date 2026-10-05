//! @file test_latency_telemetry.cpp
//! @brief Unit tests for the fixed-capacity latency tracker (nearest-rank
//!        percentiles) and the renderer's frame-latency integration points.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#include "warploom/core/latency_telemetry.hpp"

namespace {

using omnicpp::core::LatencyTracker;
using omnicpp::core::LatencyStats;
using omnicpp::core::nearest_rank_percentile;

//! Reference implementation: sort and index directly.
std::uint64_t reference_percentile(std::vector<std::uint64_t> values, double q) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const auto n = static_cast<double>(values.size());
  auto rank = static_cast<std::size_t>(q * n + 0.999999);
  if (rank == 0) rank = 1;
  if (rank > values.size()) rank = values.size();
  return values[rank - 1];
}

}  // namespace

TEST(LatencyTelemetry, EmptyTrackerReturnsZeroes) {
  LatencyTracker<> tracker;
  const auto stats = tracker.percentiles();
  EXPECT_EQ(stats.window_count, 0U);
  EXPECT_EQ(stats.total_count, 0U);
  EXPECT_EQ(stats.p50_ns, 0U);
  EXPECT_EQ(stats.max_ns, 0U);
}

TEST(LatencyTelemetry, SingleSample) {
  LatencyTracker<> tracker;
  tracker.record(1'000'000U);
  const auto stats = tracker.percentiles();
  EXPECT_EQ(stats.window_count, 1U);
  EXPECT_EQ(stats.total_count, 1U);
  EXPECT_EQ(stats.p50_ns, 1'000'000U);
  EXPECT_EQ(stats.p99_ns, 1'000'000U);
  EXPECT_EQ(stats.p999_ns, 1'000'000U);
  EXPECT_EQ(stats.max_ns, 1'000'000U);
}

TEST(LatencyTelemetry, KnownDistributionMatchesReference) {
  // 1..1000 us as ns: deterministic distribution with known ranks.
  LatencyTracker<> tracker;
  for (std::uint64_t ns = 1'000U; ns <= 1'000'000U; ns += 1'000U) {
    tracker.record(ns);
  }
  ASSERT_EQ(tracker.window_count(), 1000U);
  std::vector<std::uint64_t> values(1000);
  std::iota(values.begin(), values.end(), 0U);
  for (std::uint64_t i = 0; i < 1000; ++i) values[i] = 1'000U + i * 1'000U;

  const auto stats = tracker.percentiles();
  EXPECT_EQ(stats.p50_ns, reference_percentile(values, 0.50));
  EXPECT_EQ(stats.p90_ns, reference_percentile(values, 0.90));
  EXPECT_EQ(stats.p99_ns, reference_percentile(values, 0.99));
  EXPECT_EQ(stats.p999_ns, reference_percentile(values, 0.999));
  EXPECT_EQ(stats.max_ns, 1'000'000U);
  // Sanity: monotone tail.
  EXPECT_LE(stats.p50_ns, stats.p90_ns);
  EXPECT_LE(stats.p90_ns, stats.p99_ns);
  EXPECT_LE(stats.p99_ns, stats.p999_ns);
  EXPECT_LE(stats.p999_ns, stats.max_ns);
}

TEST(LatencyTelemetry, NearestRankMatchesDefinition) {
  // rank = ceil(q*n): for n=100, p50 -> element 50 (value 50), p99 -> 99.
  std::vector<std::uint64_t> values(100);
  std::iota(values.begin(), values.end(), 1U);
  EXPECT_EQ(nearest_rank_percentile(values, 0.50), 50U);
  EXPECT_EQ(nearest_rank_percentile(values, 0.90), 90U);
  EXPECT_EQ(nearest_rank_percentile(values, 0.99), 99U);
  EXPECT_EQ(nearest_rank_percentile(values, 0.999), 100U);
  EXPECT_EQ(nearest_rank_percentile(values, 1.0), 100U);
}

TEST(LatencyTelemetry, WindowWrapsKeepingMostRecentSamples) {
  constexpr std::size_t kCap = 64;
  LatencyTracker<kCap> tracker;
  // Fill with small values, then push kCap large ones: only larges remain.
  for (std::size_t i = 0; i < static_cast<std::size_t>(100); ++i) tracker.record(1U);
  for (std::size_t i = 0; i < static_cast<std::size_t>(static_cast<int>(kCap)); ++i) tracker.record(100'000'000U);
  ASSERT_EQ(tracker.window_count(), kCap);
  ASSERT_EQ(tracker.total_count(), 164U);
  const auto stats = tracker.percentiles();
  EXPECT_EQ(stats.p50_ns, 100'000'000U);
  EXPECT_EQ(stats.max_ns, 100'000'000U);
}

TEST(LatencyTelemetry, ResetClearsAllCounters) {
  LatencyTracker<> tracker;
  tracker.record(5U);
  tracker.record(7U);
  tracker.reset();
  const auto stats = tracker.percentiles();
  EXPECT_EQ(stats.window_count, 0U);
  EXPECT_EQ(stats.total_count, 0U);
  EXPECT_EQ(stats.p50_ns, 0U);
}

TEST(LatencyTelemetry, UnsortedInputStillYieldsCorrectPercentiles) {
  LatencyTracker<> tracker;
  const std::vector<std::uint64_t> samples = {
      9'000'000U, 100U, 5'000'000U, 700U, 1'000'000U, 300U, 2'500'000U, 90'000'000U};
  for (const auto s : samples) tracker.record(s);
  auto sorted = samples;
  const auto stats = tracker.percentiles();
  EXPECT_EQ(stats.p50_ns, reference_percentile(sorted, 0.50));
  EXPECT_EQ(stats.p99_ns, reference_percentile(sorted, 0.99));
  EXPECT_EQ(stats.max_ns, 90'000'000U);
}

TEST(LatencyTelemetry, PercentileDoesNotMutateWindowContents) {
  LatencyTracker<> tracker;
  for (std::uint64_t v : {50U, 10U, 40U, 20U, 30U}) tracker.record(v);
  const auto first = tracker.percentiles();
  const auto second = tracker.percentiles();
  EXPECT_EQ(first.p50_ns, second.p50_ns);
  EXPECT_EQ(first.max_ns, second.max_ns);
  EXPECT_EQ(first.window_count, 5U);
}
