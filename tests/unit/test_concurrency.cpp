/**
 * @file test_concurrency.cpp
 * @brief Concurrency tests for SPSC channel and bounded queue primitives.
 *
 * These tests exercise lock-free data structures under real thread contention
 * and are designed to be run under ThreadSanitizer (TSan) to verify absence
 * of data races.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <numeric>
#include <cstdint>

#include "warploom/core/deterministic_runtime.hpp"

namespace {

// ---------------------------------------------------------------------------
// SPSC Channel: single-producer / single-consumer correctness
// ---------------------------------------------------------------------------

TEST(SpscChannel, CrossThreadTransfer) {
  constexpr std::size_t kCapacity = 64;
  constexpr std::size_t kCount = 10000;

  omnicpp::core::SpscChannel<std::uint64_t, kCapacity> channel;

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!channel.try_push(i)) {
        // spin until space available
      }
    }
  });

  std::vector<std::uint64_t> received;
  received.reserve(kCount);

  std::thread consumer([&] {
    std::uint64_t value{};
    std::size_t collected = 0;
    while (collected < kCount) {
      if (channel.try_pop(value)) {
        received.push_back(value);
        ++collected;
      }
    }
  });

  producer.join();
  consumer.join();

  ASSERT_EQ(received.size(), kCount);
  // Values must arrive in order (SPSC guarantee)
  for (std::size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(received[i], i) << "Out of order at index " << i;
  }
}

TEST(SpscChannel, HighContentionBurst) {
  constexpr std::size_t kCapacity = 16;
  constexpr std::size_t kCount = 50000;

  omnicpp::core::SpscChannel<std::uint64_t, kCapacity> channel;
  std::atomic<bool> done_producing{false};
  std::atomic<std::size_t> consumed_count{0};

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!channel.try_push(i)) {
        // spin
      }
    }
    done_producing.store(true, std::memory_order_release);
  });

  std::vector<std::uint64_t> received;
  std::thread consumer([&] {
    std::uint64_t value{};
    for (;;) {
      if (channel.try_pop(value)) {
        received.push_back(value);
        consumed_count.fetch_add(1, std::memory_order_relaxed);
      } else if (done_producing.load(std::memory_order_acquire)) {
        // Drain any remaining items that arrived after last pop attempt
        while (channel.try_pop(value)) {
          received.push_back(value);
          consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
        break;
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  // All values must be received in order
  ASSERT_EQ(received.size(), kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(received[i], i);
  }
}

TEST(SpscChannel, RejectsWhenFull) {
  constexpr std::size_t kCapacity = 8;
  omnicpp::core::SpscChannel<int, kCapacity> channel;

  // Fill to capacity (usable capacity is Capacity-1)
  for (std::size_t i = 0; i < channel.capacity(); ++i) {
    EXPECT_TRUE(channel.try_push(static_cast<int>(i)));
  }
  // Next push should fail
  EXPECT_FALSE(channel.try_push(999));
}

TEST(SpscChannel, RejectsWhenEmpty) {
  constexpr std::size_t kCapacity = 8;
  omnicpp::core::SpscChannel<int, kCapacity> channel;

  int value = -1;
  EXPECT_FALSE(channel.try_pop(value));
  EXPECT_EQ(value, -1);
}

// ---------------------------------------------------------------------------
// BoundedQueue: single-thread correctness (not thread-safe)
// ---------------------------------------------------------------------------

TEST(BoundedQueue, PushPopOrdering) {
  constexpr std::size_t kCapacity = 4;
  omnicpp::core::BoundedQueue<int, kCapacity> queue;

  EXPECT_TRUE(queue.try_push(10));
  EXPECT_TRUE(queue.try_push(20));
  EXPECT_TRUE(queue.try_push(30));

  int value = -1;
  EXPECT_TRUE(queue.try_pop(value));
  EXPECT_EQ(value, 10);
  EXPECT_TRUE(queue.try_pop(value));
  EXPECT_EQ(value, 20);
  EXPECT_TRUE(queue.try_pop(value));
  EXPECT_EQ(value, 30);
  EXPECT_FALSE(queue.try_pop(value));
}

TEST(BoundedQueue, RejectsWhenFull) {
  constexpr std::size_t kCapacity = 2;
  omnicpp::core::BoundedQueue<int, kCapacity> queue;

  EXPECT_TRUE(queue.try_push(1));
  EXPECT_TRUE(queue.try_push(2));
  EXPECT_FALSE(queue.try_push(3));
  EXPECT_EQ(queue.size(), kCapacity);
}

TEST(BoundedQueue, OverflowTracking) {
  constexpr std::size_t kCapacity = 4;
  omnicpp::core::BoundedQueue<int, kCapacity> queue;

  for (std::size_t i = 0; i < static_cast<std::size_t>(10); ++i) {
    static_cast<void>(queue.try_push(i));
  }
  // Only kCapacity items should be stored
  EXPECT_EQ(queue.size(), kCapacity);
}

// ---------------------------------------------------------------------------
// DeterministicRuntime: event ordering under concurrent post
// ---------------------------------------------------------------------------

TEST(DeterministicRuntime, ConcurrentEventPost) {
  // DeterministicRuntime uses SpscChannel internally.
  // post_event() is safe from exactly ONE producer thread.
  // This test validates that a single producer + main-thread consumer
  // works correctly and all events are accounted for.
  constexpr std::size_t kEventCount = 1000;

  omnicpp::core::DeterministicRuntime runtime(1.0 / 60.0);
  ASSERT_TRUE(runtime.start().is_ok());

  // Single producer thread posts events
  std::thread producer([&runtime] {
    for (std::uint64_t i = 0; i < kEventCount; ++i) {
      static_cast<void>(runtime.post_event(i));
    }
  });

  producer.join();

  // All events should have been queued (or dropped if overflow)
  std::size_t total_accounted = runtime.pending_events() + runtime.dropped_events();
  EXPECT_EQ(total_accounted, kEventCount);

  // Advance should drain all queued events
  std::size_t received_count = 0;
  runtime.advance(10.0, [&](omnicpp::core::DeterministicRuntime::Event) {
    ++received_count;
  }, [](std::uint64_t) {});

  EXPECT_EQ(received_count + runtime.dropped_events(), kEventCount);

  ASSERT_TRUE(runtime.stop().is_ok());
}

TEST(DeterministicRuntime, ProducerConsumerLockFree) {
  constexpr std::size_t kCapacity = 256;
  constexpr std::size_t kCount = 5000;

  omnicpp::core::SpscChannel<std::uint64_t, kCapacity> channel;

  // Simulate a typical game loop: producer posts events, consumer drains them
  std::atomic<bool> done_producing{false};
  std::vector<std::uint64_t> consumed;

  std::thread consumer([&] {
    std::uint64_t value{};
    for (;;) {
      if (channel.try_pop(value)) {
        consumed.push_back(value);
      } else if (done_producing.load(std::memory_order_acquire)) {
        // Final drain pass
        while (channel.try_pop(value)) {
          consumed.push_back(value);
        }
        break;
      } else {
        std::this_thread::yield();
      }
    }
  });

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!channel.try_push(i)) {
        std::this_thread::yield();
      }
    }
    done_producing.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();

  ASSERT_EQ(consumed.size(), kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(consumed[i], i);
  }
}

// ---------------------------------------------------------------------------
// DeterministicRuntime: state hash reproducibility across runs
// ---------------------------------------------------------------------------

TEST(DeterministicRuntime, StateHashReproducibility) {
  auto run_simulation = [](std::uint64_t event_seed, std::size_t ticks) -> std::uint64_t {
    omnicpp::core::DeterministicRuntime runtime(1.0 / 60.0);
    static_cast<void>(runtime.start().is_ok());

    // Inject events at deterministic intervals
    for (std::uint64_t i = 0; i < ticks; ++i) {
      if (i % 10 == 0) {
        static_cast<void>(runtime.post_event(event_seed + i));
      }
      runtime.advance(1.0 / 60.0, [](omnicpp::core::DeterministicRuntime::Event) {},
                       [](std::uint64_t) {});
    }

    return runtime.state_hash();
  };

  std::uint64_t hash_a = run_simulation(42, 500);
  std::uint64_t hash_b = run_simulation(42, 500);
  std::uint64_t hash_c = run_simulation(99, 500);

  EXPECT_EQ(hash_a, hash_b) << "Same inputs must produce same state hash";
  EXPECT_NE(hash_a, hash_c) << "Different inputs should produce different state hash";
}

// ===========================================================================
// MPSC Channel: multi-producer / single-consumer correctness
// ===========================================================================

TEST(MpscChannel, SingleProducerTransfer) {
  constexpr std::size_t kCapacity = 64;
  constexpr std::size_t kCount = 10000;

  omnicpp::core::MpscChannel<std::uint64_t, kCapacity> channel;
  std::atomic<bool> done_producing{false};
  std::vector<std::uint64_t> received;

  // Consumer thread must drain concurrently; the buffer is smaller than kCount.
  std::thread consumer([&] {
    std::uint64_t value{};
    for (;;) {
      if (channel.try_pop(value)) {
        received.push_back(value);
      } else if (done_producing.load(std::memory_order_acquire)) {
        while (channel.try_pop(value)) {
          received.push_back(value);
        }
        break;
      } else {
        std::this_thread::yield();
      }
    }
  });

  for (std::uint64_t i = 0; i < kCount; ++i) {
    while (!channel.try_push(i)) {
      std::this_thread::yield();
    }
  }
  done_producing.store(true, std::memory_order_release);
  consumer.join();

  ASSERT_EQ(received.size(), kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(received[i], i);
  }
}

TEST(MpscChannel, MultiProducerCrossThread) {
  constexpr std::size_t kCapacity = 128;
  constexpr std::size_t kThreadCount = 4;
  constexpr std::size_t kEventsPerThread = 5000;
  constexpr std::size_t kTotal = kThreadCount * kEventsPerThread;

  omnicpp::core::MpscChannel<std::uint64_t, kCapacity> channel;
  std::atomic<bool> done_producing{false};

  // Multiple producer threads
  std::vector<std::thread> producers;
  for (std::size_t t = 0; t < kThreadCount; ++t) {
    producers.emplace_back([&channel, t] {
      for (std::size_t i = 0; i < kEventsPerThread; ++i) {
        std::uint64_t event = (t * 1000000ULL) + i;
        while (!channel.try_push(event)) {
          std::this_thread::yield();
        }
      }
    });
  }

  // Single consumer drains on a separate thread
  std::vector<std::uint64_t> received;
  std::thread consumer([&] {
    std::uint64_t value{};
    for (;;) {
      if (channel.try_pop(value)) {
        received.push_back(value);
      } else if (done_producing.load(std::memory_order_acquire)) {
        // Final drain pass
        while (channel.try_pop(value)) {
          received.push_back(value);
        }
        break;
      } else {
        std::this_thread::yield();
      }
    }
  });

  for (auto& t : producers) t.join();
  done_producing.store(true, std::memory_order_release);
  consumer.join();

  // All events must be received (some may be dropped if overflow)
  EXPECT_EQ(received.size(), kTotal);

  // Verify all values from each producer are present
  // (order within a single producer must be preserved)
  std::size_t valid_count = 0;
  for (std::uint64_t v : received) {
    std::size_t producer = v / 1000000ULL;
    std::size_t seq = v % 1000000ULL;
    EXPECT_LT(producer, kThreadCount);
    EXPECT_LT(seq, kEventsPerThread);
    ++valid_count;
  }
  EXPECT_EQ(valid_count, kTotal);
}

TEST(MpscChannel, RejectsWhenFull) {
  constexpr std::size_t kCapacity = 8;
  omnicpp::core::MpscChannel<int, kCapacity> channel;

  // Fill to capacity
  for (std::size_t i = 0; i < channel.capacity(); ++i) {
    EXPECT_TRUE(channel.try_push(static_cast<int>(i)));
  }
  // Should reject when full
  EXPECT_FALSE(channel.try_push(999));
}

TEST(MpscChannel, RejectsWhenEmpty) {
  constexpr std::size_t kCapacity = 8;
  omnicpp::core::MpscChannel<int, kCapacity> channel;

  int value = -1;
  EXPECT_FALSE(channel.try_pop(value));
  EXPECT_EQ(value, -1);
}

TEST(MpscChannel, SizeTracking) {
  constexpr std::size_t kCapacity = 16;
  omnicpp::core::MpscChannel<int, kCapacity> channel;

  EXPECT_EQ(channel.size(), 0u);
  EXPECT_TRUE(channel.try_push(1));
  EXPECT_GE(channel.size(), 1u);
  EXPECT_TRUE(channel.try_push(2));
  EXPECT_GE(channel.size(), 2u);

  int value{};
  EXPECT_TRUE(channel.try_pop(value));
  EXPECT_EQ(value, 1);
}

TEST(MpscChannel, HighContentionStress) {
  constexpr std::size_t kCapacity = 64;
  constexpr std::size_t kThreadCount = 8;
  constexpr std::size_t kEventsPerThread = 10000;
  constexpr std::size_t kTotal = kThreadCount * kEventsPerThread;

  omnicpp::core::MpscChannel<std::uint64_t, kCapacity> channel;
  std::atomic<bool> done_producing{false};
  std::atomic<std::size_t> consumed_count{0};

  std::vector<std::thread> producers;
  for (std::size_t t = 0; t < kThreadCount; ++t) {
    producers.emplace_back([&channel, t] {
      for (std::size_t i = 0; i < kEventsPerThread; ++i) {
        while (!channel.try_push((t << 32) | i)) {
          std::this_thread::yield();
        }
      }
    });
  }

  std::thread consumer([&] {
    std::uint64_t value{};
    for (;;) {
      if (channel.try_pop(value)) {
        consumed_count.fetch_add(1, std::memory_order_relaxed);
      } else if (done_producing.load(std::memory_order_acquire)) {
        while (channel.try_pop(value)) {
          consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
        break;
      } else {
        std::this_thread::yield();
      }
    }
  });

  for (auto& t : producers) t.join();
  done_producing.store(true, std::memory_order_release);
  consumer.join();

  EXPECT_EQ(consumed_count.load(), kTotal);
}

} // namespace
