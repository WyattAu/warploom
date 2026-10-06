#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include "warploom/core/clock.hpp"
#include "warploom/core/thread_pool.hpp"

namespace {

// ============================================================================
// SteadyClock
// ============================================================================

TEST(SteadyClock, NowIsMonotonic) {
  auto a = omnicpp::core::SteadyClock::now();
  auto b = omnicpp::core::SteadyClock::now();
  EXPECT_GE(b.time_since_epoch().count(), a.time_since_epoch().count());
}

TEST(SteadyClock, NowNsIsPositive) {
  const auto ns = omnicpp::core::SteadyClock::now_ns();
  EXPECT_GT(ns, 0);
}

TEST(SteadyClock, NowSecondsIsPositive) {
  const auto s = omnicpp::core::SteadyClock::now_seconds();
  EXPECT_GT(s, 0.0);
}

// ============================================================================
// ManualTimer
// ============================================================================

TEST(ManualTimer, ElapsedIncreases) {
  omnicpp::core::ManualTimer timer;
  EXPECT_GE(timer.elapsed_ns(), 0);
  volatile int x = 0;
  for (int i = 0; i < 1000; ++i) x += i;
  // Reading x is what keeps the busy-work alive; the sum is not the point,
  // the elapsed time is. 0..999 == 499500.
  EXPECT_EQ(x, 499500);
  EXPECT_GT(timer.elapsed_ns(), 0);
}

TEST(ManualTimer, ResetReturnsToZero) {
  omnicpp::core::ManualTimer timer;
  volatile int x = 0;
  for (int i = 0; i < 1000; ++i) x += i;
  // Reading x is what keeps the busy-work alive; the sum is not the point,
  // the elapsed time is. 0..999 == 499500.
  EXPECT_EQ(x, 499500);
  timer.reset();
  EXPECT_GE(timer.elapsed_ns(), 0);
}

TEST(ManualTimer, ElapsedSecondsMatchesNs) {
  omnicpp::core::ManualTimer timer;
  volatile int x = 0;
  for (int i = 0; i < 1000; ++i) x += i;
  // Reading x is what keeps the busy-work alive; the sum is not the point,
  // the elapsed time is. 0..999 == 499500.
  EXPECT_EQ(x, 499500);
  const auto ns = timer.elapsed_ns();
  const auto s = timer.elapsed_seconds();
  EXPECT_NEAR(s, static_cast<double>(ns) / 1e9, 1e-3);
}

// ============================================================================
// ScopedTimer
// ============================================================================

TEST(ScopedTimer, MeasuresElapsedNanoseconds) {
  std::int64_t elapsed = 0;
  {
    omnicpp::core::ScopedTimer timer(elapsed);
    volatile int x = 0;
    for (int i = 0; i < 1000; ++i) x += i;
  // Reading x is what keeps the busy-work alive; the sum is not the point,
  // the elapsed time is. 0..999 == 499500.
  EXPECT_EQ(x, 499500);
  }
  EXPECT_GT(elapsed, 0);
}

// ============================================================================
// ThreadPool
// ============================================================================

TEST(ThreadPool, DefaultSizeIsHardwareConcurrency) {
  omnicpp::core::ThreadPool pool;
  EXPECT_GE(pool.size(), 1u);
  EXPECT_EQ(pool.size(), std::thread::hardware_concurrency());
  pool.shutdown();
}

TEST(ThreadPool, ExplicitSize) {
  omnicpp::core::ThreadPool pool(4);
  EXPECT_EQ(pool.size(), 4u);
  pool.shutdown();
}

TEST(ThreadPool, SubmitsAndExecutesWork) {
  omnicpp::core::ThreadPool pool(2);
  std::atomic<int> counter{0};

  for (std::size_t i = 0; i < static_cast<std::size_t>(10); ++i) {
    pool.submit([&counter]() noexcept { counter.fetch_add(1, std::memory_order_relaxed); });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pool.shutdown();

  EXPECT_EQ(counter.load(), 10);
}

TEST(ThreadPool, ConcurrentSubmission) {
  omnicpp::core::ThreadPool pool(4);
  std::atomic<int> counter{0};

  std::vector<std::thread> submitters;
  for (std::size_t t = 0; t < 4; ++t) {
    submitters.emplace_back([&pool, &counter] {
      for (std::size_t i = 0; i < static_cast<std::size_t>(100); ++i) {
        pool.submit([&counter]() noexcept { counter.fetch_add(1, std::memory_order_relaxed); });
      }
    });
  }

  for (auto& t : submitters) t.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  pool.shutdown();

  EXPECT_EQ(counter.load(), 400);
}

TEST(ThreadPool, RejectsAfterShutdown) {
  omnicpp::core::ThreadPool pool(1);
  pool.shutdown();
  EXPECT_FALSE(pool.submit([]() noexcept {}));
  EXPECT_TRUE(pool.is_shutdown());
}

// ============================================================================
// Affinity
// ============================================================================

TEST(Affinity, HardwareConcurrencyIsPositive) {
  EXPECT_GE(omnicpp::core::Affinity::hardware_concurrency(), 1u);
}

TEST(Affinity, PinToCpuSucceeds) {
  EXPECT_TRUE(omnicpp::core::Affinity::pin_to_cpu(0));
}

} // namespace
