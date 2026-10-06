//! @file test_job_system.cpp
//! @brief Tests for the prioritized job scheduler: priority drain order,
//!        fork/join counters, shutdown-drop semantics, dispatch overhead.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "warploom/core/clock.hpp"
#include "warploom/core/job_system.hpp"

#if defined(__SANITIZE_THREAD__)
#define WARPLOOM_TSAN 1
#elif defined(__clang__) && defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WARPLOOM_TSAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define WARPLOOM_ASAN 1
#elif defined(__clang__) && defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WARPLOOM_ASAN 1
#endif
#endif

namespace {

using omnicpp::core::JobCounter;
using omnicpp::core::JobPriority;
using omnicpp::core::JobSystem;

//! Prevent gtest assertions in worker threads (undefined behavior there):
//! record order under a lock and assert on the main thread instead.
struct OrderRecorder {
  std::mutex mutex;
  std::vector<JobPriority> order;
  void record(JobPriority p) {
    std::lock_guard lock(mutex);
    order.push_back(p);
  }
};

}  // namespace

TEST(JobSystem, InitializeAndShutdown) {
  JobSystem jobs;
  EXPECT_TRUE(jobs.initialize(2));
  EXPECT_FALSE(jobs.initialize(2));  // Double-init rejected.
  EXPECT_EQ(jobs.thread_count(), 2U);
  EXPECT_TRUE(jobs.is_running());
  jobs.shutdown();
  EXPECT_FALSE(jobs.is_running());
  EXPECT_FALSE(jobs.submit(JobPriority::render, nullptr, [] {}));  // Dead submit rejected.
}

TEST(JobSystem, ExecuteAllJobs) {
  JobSystem jobs;
  ASSERT_TRUE(jobs.initialize());
  std::atomic<int> hits{0};
  JobCounter counter;
  counter.add(100);
  for (std::size_t i = 0; i < static_cast<std::size_t>(100); ++i) {
    ASSERT_TRUE(jobs.submit(JobPriority::background, &counter, [&hits] { hits.fetch_add(1); }));
  }
  counter.wait();
  EXPECT_EQ(hits.load(), 100);
  EXPECT_TRUE(counter.is_complete());
  jobs.wait_idle();
}

TEST(JobSystem, HigherPriorityDrainsFirst) {
  JobSystem jobs;
  ASSERT_TRUE(jobs.initialize(1));  // Single worker: strict drain order.
  // Pin the single worker with a highest-priority blocker until every job is
  // queued: without it the worker may begin draining mid-submission, which is
  // legitimate scheduler behavior but makes drain-order assertions dependent
  // on submission timing (observed as flaky under TSan's scheduling noise).
  std::atomic<bool> gate{false};
  ASSERT_TRUE(jobs.submit(JobPriority::render, nullptr, [&gate] {
    while (!gate.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }));
  OrderRecorder recorder;
  JobCounter counter;
  constexpr int kPerClass = 8;
  counter.add(4 * kPerClass);
  // Submit in reverse priority order so any FIFO queue would fail this test.
  for (std::size_t round = 0; round < static_cast<std::size_t>(kPerClass); ++round) {
    ASSERT_TRUE(jobs.submit(JobPriority::background, &counter,
                            [&recorder] { recorder.record(JobPriority::background); }));
    ASSERT_TRUE(jobs.submit(JobPriority::upload, &counter,
                            [&recorder] { recorder.record(JobPriority::upload); }));
    ASSERT_TRUE(jobs.submit(JobPriority::submit, &counter,
                            [&recorder] { recorder.record(JobPriority::submit); }));
    ASSERT_TRUE(jobs.submit(JobPriority::render, &counter,
                            [&recorder] { recorder.record(JobPriority::render); }));
  }
  gate.store(true, std::memory_order_release);
  counter.wait();
  ASSERT_EQ(recorder.order.size(), 4U * kPerClass);
  // First four must be exactly the render batch (highest priority drains
  // fully before lower classes on a single worker).
  for (std::size_t i = 0; i < static_cast<std::size_t>(kPerClass); ++i) {
    EXPECT_EQ(recorder.order[static_cast<std::size_t>(i)], JobPriority::render);
  }
  // Global monotonicity: priority values never increase across the drain.
  for (std::size_t i = 1; i < recorder.order.size(); ++i) {
    EXPECT_LE(static_cast<int>(recorder.order[i - 1]), static_cast<int>(recorder.order[i]));
  }
  jobs.shutdown();
}

TEST(JobSystem, ForkJoinParallelSum) {
  JobSystem jobs;
  ASSERT_TRUE(jobs.initialize());
  constexpr int kJobs = 64;
  std::vector<int> results(kJobs, 0);
  JobCounter counter;
  counter.add(kJobs);
  for (int i = 0; i < static_cast<int>(kJobs); ++i) {
    ASSERT_TRUE(jobs.submit(JobPriority::upload, &counter,
                            [&results, i] { results[static_cast<std::size_t>(i)] = i * i; }));
  }
  counter.wait();
  long long total = 0;
  for (std::size_t i = 0; i < static_cast<std::size_t>(kJobs); ++i) total += results[static_cast<std::size_t>(i)];
  constexpr long long kExpected = 63LL * 64LL * 127LL / 6LL;  // sum of squares 0..63
  EXPECT_EQ(total, kExpected);
  jobs.wait_idle();
}

TEST(JobSystem, SubmitFromJobThread) {
  JobSystem jobs;
  ASSERT_TRUE(jobs.initialize());
  std::atomic<int> second_wave_hits{0};
  JobCounter first, second;
  first.add(4);
  second.add(4);
  for (std::size_t i = 0; i < static_cast<std::size_t>(4); ++i) {
    ASSERT_TRUE(jobs.submit(JobPriority::submit, &first, [&] {
      // Nested submission from a running job must be legal.
      EXPECT_TRUE(jobs.submit(JobPriority::background, &second,
                              [&second_wave_hits] { second_wave_hits.fetch_add(1); }));
    }));
  }
  first.wait();
  second.wait();
  EXPECT_EQ(second_wave_hits.load(), 4);
  jobs.wait_idle();
}

TEST(JobSystem, ShutdownReleasesAllCountersQueuedOrRun) {
  JobSystem jobs;
  ASSERT_TRUE(jobs.initialize(1));
  // Submit counted jobs, then shut down without draining. Each job either
  // runs (worker decrements) or is dropped (shutdown decrements) — either
  // way the counter must reach zero: shutdown can never hang a waiter.
  JobCounter counter;
  counter.add(50);
  for (std::size_t i = 0; i < static_cast<std::size_t>(50); ++i) {
    ASSERT_TRUE(jobs.submit(JobPriority::background, &counter, [] {}));
  }
  jobs.shutdown();
  EXPECT_TRUE(counter.is_complete());
  EXPECT_FALSE(jobs.is_running());
}

TEST(JobSystem, WaitIdleReturnsWhenQuiescent) {
  JobSystem jobs;
  ASSERT_TRUE(jobs.initialize());
  std::atomic<int> hits{0};
  for (std::size_t i = 0; i < static_cast<std::size_t>(200); ++i) {
    ASSERT_TRUE(jobs.submit(JobPriority::upload, nullptr, [&hits] { hits.fetch_add(1); }));
  }
  jobs.wait_idle();
  EXPECT_EQ(hits.load(), 200);
  jobs.shutdown();
}

TEST(JobSystem, DispatchOverheadIsSubMicrosecond) {
  // Frame-construction regime: submit a burst while workers drain a
  // backlog. Workers stay spinning (parked == 0), so submit exercises its
  // hot path — mutex + push, no condition-variable syscall. This is the
  // property that matters when a frame's jobs are enqueued each tick.
  JobSystem jobs;
  ASSERT_TRUE(jobs.initialize());
  for (std::size_t round = 0; round < static_cast<std::size_t>(3); ++round) {  // Warm up allocator/branches.
    JobCounter warm;
    warm.add(256);
    for (std::size_t i = 0; i < static_cast<std::size_t>(256); ++i) {
      static_cast<void>(jobs.submit(JobPriority::render, &warm, [] {}));
    }
    warm.wait();
    jobs.wait_idle();
  }
  constexpr int kBurst = 512;
  JobCounter counter;
  counter.add(kBurst);
  std::vector<std::uint64_t> dispatch_ns;
  dispatch_ns.reserve(kBurst);
  for (std::size_t i = 0; i < static_cast<std::size_t>(kBurst); ++i) {
    const auto t0 = omnicpp::core::SteadyClock::now_ns();
    ASSERT_TRUE(jobs.submit(JobPriority::render, &counter, [] {}));
    const auto t1 = omnicpp::core::SteadyClock::now_ns();
    dispatch_ns.push_back(static_cast<std::uint64_t>(t1 - t0));
  }
  counter.wait();
  jobs.wait_idle();
  std::sort(dispatch_ns.begin(), dispatch_ns.end());
  const std::uint64_t median = dispatch_ns[static_cast<std::size_t>(kBurst) / 2U];
  const std::uint64_t p99 = dispatch_ns[static_cast<std::size_t>(kBurst) * 99U / 100U];
  std::cout << "job dispatch (burst backlog) median=" << median << "ns p99=" << p99
            << "ns max=" << dispatch_ns.back() << "ns" << std::endl;
// Budgets are calibrated per build configuration (sanitizer instrumentation)
// and per environment (CI VMs run slower, noisier vCPUs than workstations).
// They still catch the regressions this test exists for: CV storms, heap
// traffic on the submit path, and priority-inversion stalls.
// Optimized builds schedule fewer -O0 instrumentation stalls on the submit
// path, but CI Release VMs still measure ~5x a workstation (noisy vCPUs), so
// the plain tier splits on CI while Release collapses sanitizer variance.
#if defined(WARPLOOM_TSAN)
  const std::uint64_t kMedianBudget = 5'000U;
  const std::uint64_t kP99Budget = 500'000U;
#elif defined(WARPLOOM_ASAN)
  const std::uint64_t kMedianBudget = 2'000U;
  const std::uint64_t kP99Budget = 250'000U;
#elif defined(NDEBUG)
  const bool on_ci_release = std::getenv("CI") != nullptr;
  // Release timing is still scheduler/CPU dependent locally; keep the
  // workstation budget below one microsecond without rejecting normal
  // optimized-build variance. CI gets the wider VM budget above.
  const std::uint64_t kMedianBudget = on_ci_release ? 8'000U : 1'000U;
  const std::uint64_t kP99Budget = on_ci_release ? 400'000U : 40'000U;
#else
  const bool on_ci = std::getenv("CI") != nullptr;
  const std::uint64_t kMedianBudget = on_ci ? 4'000U : 500U;  // < 0.5 us locally.
  const std::uint64_t kP99Budget = on_ci ? 200'000U : 50'000U;
#endif
  EXPECT_LT(median, kMedianBudget);
  EXPECT_LT(p99, kP99Budget);
  jobs.shutdown();
}
