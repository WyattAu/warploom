#pragma once

/**
 * @file clock.hpp
 * @brief Portable high-resolution clock abstraction.
 *
 * Provides a steady, monotonic clock suitable for measuring
 * simulation frame times and benchmarking. The clock is a thin
 * wrapper around the platform's steady_clock with a monotonic
 * guarantee and nanosecond resolution.
 *
 * CONTRACT: The clock is monotonically non-decreasing.
 * CONTRACT: The clock is not affected by wall-clock adjustments.
 * CONTRACT: Resolution is at least microsecond; nanosecond preferred.
 */

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace omnicpp::core {

//! Monotonic clock with nanosecond resolution.
//! Wraps std::chrono::steady_clock for portability.
class SteadyClock final {
public:
  using rep = std::int64_t;
  using period = std::nano;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<SteadyClock, duration>;

  [[nodiscard]] static time_point now() noexcept {
    const auto sys_now = std::chrono::steady_clock::now();
    const auto sys_epoch = std::chrono::time_point<std::chrono::steady_clock>{};
    const auto elapsed = std::chrono::duration_cast<duration>(sys_now - sys_epoch);
    return time_point{elapsed};
  }

  [[nodiscard]] static constexpr rep now_ns() noexcept {
    return now().time_since_epoch().count();
  }

  [[nodiscard]] static double now_seconds() noexcept {
    return static_cast<double>(now_ns()) / 1e9;
  }

  SteadyClock() = delete;
};

//! RAII timer that measures elapsed nanoseconds.
class ScopedTimer final {
public:
  explicit ScopedTimer(std::int64_t& output_ns) noexcept
      : output_(output_ns), start_(SteadyClock::now()) {}

  ~ScopedTimer() {
    const auto elapsed = SteadyClock::now() - start_;
    output_ = elapsed.count();
  }

  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;
  ScopedTimer(ScopedTimer&&) = delete;
  ScopedTimer& operator=(ScopedTimer&&) = delete;

private:
  std::int64_t& output_;
  SteadyClock::time_point start_;
};

//! Simple elapsed-time measurement without RAII.
class ManualTimer final {
public:
  [[nodiscard]] std::int64_t elapsed_ns() const noexcept {
    return (SteadyClock::now() - start_).count();
  }

  [[nodiscard]] double elapsed_seconds() const noexcept {
    return static_cast<double>(elapsed_ns()) / 1e9;
  }

  void reset() noexcept { start_ = SteadyClock::now(); }

private:
  SteadyClock::time_point start_ = SteadyClock::now();
};

} // namespace omnicpp::core
