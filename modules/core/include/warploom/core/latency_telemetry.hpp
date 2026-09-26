#pragma once

/**
 * @file latency_telemetry.hpp
 * @brief Fixed-capacity ring-buffer latency tracker with nearest-rank
 *        percentiles (p50/p90/p99/p99.9/max).
 *
 * Designed for frame-time telemetry: the renderer records one sample per
 * frame, and callers query tail-latency statistics over a sliding window.
 * Single-writer by design (record on the frame thread); percentiles sort a
 * snapshot on demand, so record() is O(1) and never allocates.
 *
 * Windowed semantics: once the ring is full, new samples overwrite the
 * oldest, so statistics reflect the most recent `Capacity` frames — the
 * relevant quantity for steady-state tail tracking.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace omnicpp::core {

//! Tail-latency statistics in nanoseconds over the current window.
struct LatencyStats {
  std::uint64_t p50_ns{0};
  std::uint64_t p90_ns{0};
  std::uint64_t p99_ns{0};
  std::uint64_t p999_ns{0};
  std::uint64_t max_ns{0};
  std::size_t window_count{0};  //!< Samples in the current window.
  std::uint64_t total_count{0}; //!< Samples ever recorded (wraps included).
};

//! Nearest-rank percentile of a sorted snapshot (rank = ceil(q * n), 1-based).
[[nodiscard]] inline std::uint64_t nearest_rank_percentile(
    std::vector<std::uint64_t>& sorted, double q) noexcept {
  if (sorted.empty()) return 0;
  const auto n = static_cast<double>(sorted.size());
  auto rank = static_cast<std::size_t>(q * n + 0.999999);
  if (rank == 0) rank = 1;
  if (rank > sorted.size()) rank = sorted.size();
  std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(rank - 1),
                   sorted.end());
  return sorted[rank - 1];
}

template <std::size_t Capacity = 4096>
class LatencyTracker {
  static_assert(Capacity > 0, "LatencyTracker needs a non-empty window");

public:
  //! Record one latency sample (nanoseconds). O(1), no allocation.
  void record(std::uint64_t ns) noexcept {
    samples_[head_] = ns;
    head_ = (head_ + 1) % Capacity;
    if (filled_ < Capacity) ++filled_;
    ++total_;
  }

  //! Compute tail statistics over the current window. O(w log w) worst case
  //! (nth_element per quantile), allocates one snapshot of <= Capacity.
  [[nodiscard]] LatencyStats percentiles() const {
    LatencyStats stats{};
    stats.window_count = filled_;
    stats.total_count = total_;
    if (filled_ == 0) return stats;
    std::vector<std::uint64_t> snapshot(samples_.begin(),
                                        samples_.begin() + static_cast<std::ptrdiff_t>(filled_));
    stats.p50_ns = nearest_rank_percentile(snapshot, 0.50);
    stats.p90_ns = nearest_rank_percentile(snapshot, 0.90);
    stats.p99_ns = nearest_rank_percentile(snapshot, 0.99);
    stats.p999_ns = nearest_rank_percentile(snapshot, 0.999);
    stats.max_ns = *std::max_element(snapshot.begin(), snapshot.end());
    return stats;
  }

  [[nodiscard]] std::size_t window_count() const noexcept { return filled_; }
  [[nodiscard]] std::uint64_t total_count() const noexcept { return total_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  void reset() noexcept {
    head_ = 0;
    filled_ = 0;
    total_ = 0;
  }

private:
  std::array<std::uint64_t, Capacity> samples_{};
  std::size_t head_{0};
  std::size_t filled_{0};
  std::uint64_t total_{0};
};

}  // namespace omnicpp::core
