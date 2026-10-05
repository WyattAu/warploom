#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <cstdio>
#include <fstream>
#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/core/replay.hpp"

namespace {

TEST(BoundedQueue, PreservesOrderAndRejectsOverflow) {
  omnicpp::core::BoundedQueue<int, 2> queue;
  EXPECT_TRUE(queue.try_push(1));
  EXPECT_TRUE(queue.try_push(2));
  EXPECT_FALSE(queue.try_push(3));

  int value = 0;
  EXPECT_TRUE(queue.try_pop(value));
  EXPECT_EQ(value, 1);
  EXPECT_TRUE(queue.try_pop(value));
  EXPECT_EQ(value, 2);
  EXPECT_FALSE(queue.try_pop(value));
}

TEST(SpscChannel, TransfersOrderedValuesBetweenThreads) {
  omnicpp::core::SpscChannel<int, 1024> channel;
  constexpr int count = 10000;
  std::thread producer([&] {
    for (std::size_t value = 0; value < static_cast<std::size_t>(count); ++value) {
      while (!channel.try_push(value)) std::this_thread::yield();
    }
  });
  for (std::size_t expected = 0; expected < static_cast<std::size_t>(count); ++expected) {
    int value = -1;
    while (!channel.try_pop(value)) std::this_thread::yield();
    EXPECT_EQ(value, expected);
  }
  producer.join();
}

TEST(DeterministicRuntime, LifecycleIsExplicit) {
  omnicpp::core::DeterministicRuntime runtime;
  EXPECT_FALSE(runtime.is_running());
  EXPECT_TRUE(runtime.start().is_ok());
  EXPECT_FALSE(runtime.start().is_ok());
  EXPECT_TRUE(runtime.stop().is_ok());
  EXPECT_FALSE(runtime.stop().is_ok());
}

TEST(DeterministicRuntime, ExecutesFixedTicksAndRetainsRemainder) {
  omnicpp::core::DeterministicRuntime runtime(0.1);
  ASSERT_TRUE(runtime.start().is_ok());
  int callbacks = 0;
  runtime.advance(0.25, [&](std::uint64_t tick) {
    ++callbacks;
    EXPECT_EQ(tick, static_cast<std::uint64_t>(callbacks));
  });

  EXPECT_EQ(callbacks, 2);
  EXPECT_EQ(runtime.tick_count(), 2U);
  EXPECT_NEAR(runtime.accumulator(), 0.05, 1e-12);
}

TEST(DeterministicRuntime, BoundedCatchUpDropsExcessTime) {
  omnicpp::core::DeterministicRuntime runtime(0.1);
  ASSERT_TRUE(runtime.start().is_ok());
  runtime.set_catch_up_policy(omnicpp::core::CatchUpPolicy::cap_and_drop_time, 2);
  int ticks = 0;
  runtime.advance(0.55, [](std::uint64_t) {}, [&](std::uint64_t) { ++ticks; });
  EXPECT_EQ(ticks, 2);
  EXPECT_EQ(runtime.tick_count(), 2U);
  EXPECT_EQ(runtime.overrun_count(), 1U);
  EXPECT_NEAR(runtime.dropped_time_seconds(), 0.35, 1e-12);
  EXPECT_DOUBLE_EQ(runtime.accumulator(), 0.0);
}

TEST(DeterministicRuntime, BoundedCatchUpReportsWithoutDroppingTime) {
  omnicpp::core::DeterministicRuntime runtime(0.1);
  ASSERT_TRUE(runtime.start().is_ok());
  runtime.set_catch_up_policy(omnicpp::core::CatchUpPolicy::cap_and_report_overrun, 2);
  runtime.advance(0.55, [](std::uint64_t) {}, [](std::uint64_t) {});
  EXPECT_EQ(runtime.tick_count(), 2U);
  EXPECT_EQ(runtime.overrun_count(), 1U);
  EXPECT_NEAR(runtime.accumulator(), 0.35, 1e-12);
  EXPECT_DOUBLE_EQ(runtime.dropped_time_seconds(), 0.0);
}

TEST(DeterministicRuntime, EventsAreProcessedBeforeTicks) {
  omnicpp::core::DeterministicRuntime runtime(0.1);
  ASSERT_TRUE(runtime.start().is_ok());
  ASSERT_TRUE(runtime.post_event(7));
  ASSERT_TRUE(runtime.post_event(9));
  std::vector<std::uint64_t> events;
  int ticks = 0;
  runtime.advance(0.1,
                  [&](std::uint64_t event) { events.push_back(event); },
                  [&](std::uint64_t) { ++ticks; });

  EXPECT_EQ(events, (std::vector<std::uint64_t>{7, 9}));
  EXPECT_EQ(ticks, 1);
  EXPECT_EQ(runtime.pending_events(), 0U);
  EXPECT_EQ(runtime.dropped_events(), 0U);
}

TEST(DeterministicRuntime, HashIsReproducible) {
  omnicpp::core::DeterministicRuntime first(0.1);
  omnicpp::core::DeterministicRuntime second(0.1);
  ASSERT_TRUE(first.start().is_ok());
  ASSERT_TRUE(second.start().is_ok());
  ASSERT_TRUE(first.post_event(42));
  ASSERT_TRUE(second.post_event(42));
  first.advance(0.2, [](std::uint64_t) {}, [](std::uint64_t) {});
  second.advance(0.2, [](std::uint64_t) {}, [](std::uint64_t) {});
  EXPECT_EQ(first.state_hash(), second.state_hash());
}

TEST(DeterministicRuntime, ReportsEventOverflow) {
  omnicpp::core::DeterministicRuntime runtime;
  ASSERT_TRUE(runtime.start().is_ok());
  for (std::size_t i = 0; i < 256; ++i) EXPECT_TRUE(runtime.post_event(i));
  EXPECT_FALSE(runtime.post_event(256));
  EXPECT_EQ(runtime.dropped_events(), 1U);
}

TEST(Replay, RecordsOrderAndComputesStableChecksum) {
  omnicpp::core::Replay<3> replay;
  EXPECT_TRUE(replay.record(1, 10));
  EXPECT_TRUE(replay.record(2, 20));
  EXPECT_TRUE(replay.record(4, 30));
  EXPECT_FALSE(replay.record(5, 40));
  EXPECT_EQ(replay.size(), 3U);
  EXPECT_EQ(replay.at(1).event, 20U);
  EXPECT_EQ(replay.at(1).tick, 2U);
  const auto checksum = replay.checksum();
  EXPECT_EQ(checksum, replay.checksum());
}

TEST(Replay, BinaryRoundTripAndCorruptionDetection) {
  omnicpp::core::Replay<4> source;
  ASSERT_TRUE(source.record(1, 7));
  ASSERT_TRUE(source.record(3, 9));
  std::array<std::uint8_t, 256> buffer{};
  std::size_t written = 0;
  EXPECT_EQ(source.encode(0.1, buffer.data(), buffer.size(), written), omnicpp::core::ReplayDecodeError::none);
  omnicpp::core::Replay<4> decoded;
  double step = 0.0;
  EXPECT_EQ(decoded.decode(buffer.data(), written, step), omnicpp::core::ReplayDecodeError::none);
  EXPECT_DOUBLE_EQ(step, 0.1);
  EXPECT_EQ(decoded.size(), 2U);
  EXPECT_EQ(decoded.at(1).event, 9U);
  buffer[omnicpp::core::Replay<4>::serialized_header_size] ^= 1U;
  EXPECT_EQ(decoded.decode(buffer.data(), written, step), omnicpp::core::ReplayDecodeError::checksum_mismatch);
}

TEST(Replay, FileRoundTripAndMalformedFiles) {
  constexpr const char* path = "omnicpp-replay-test.bin";
  omnicpp::core::Replay<4> source;
  ASSERT_TRUE(source.record(2, 11));
  ASSERT_TRUE(source.record(5, 13));
  ASSERT_EQ(source.save(path, 0.125), omnicpp::core::ReplayDecodeError::none);

  omnicpp::core::Replay<4> loaded;
  double step = 0.0;
  EXPECT_EQ(loaded.load(path, step), omnicpp::core::ReplayDecodeError::none);
  EXPECT_DOUBLE_EQ(step, 0.125);
  EXPECT_EQ(loaded.size(), 2U);
  EXPECT_EQ(loaded.at(0).tick, 2U);
  EXPECT_EQ(loaded.at(1).event, 13U);

  {
    std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
    const std::uint8_t bytes[] = {0x4c, 0x50, 0x52, 0x4f};
    corrupt.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  }
  EXPECT_EQ(loaded.load(path, step), omnicpp::core::ReplayDecodeError::too_small);
  std::remove(path);

  EXPECT_EQ(loaded.load("omnicpp-replay-does-not-exist.bin", step),
            omnicpp::core::ReplayDecodeError::io_error);
}

TEST(Replay, RejectsUnorderedRecords) {
  omnicpp::core::Replay<2> replay;
  EXPECT_TRUE(replay.record(4, 1));
  EXPECT_FALSE(replay.record(3, 2));
}

TEST(Replay, VerifiesDeterministicRuntimeHash) {
  omnicpp::core::Replay<2> replay;
  ASSERT_TRUE(replay.record(1, 7));
  ASSERT_TRUE(replay.record(3, 9));

  omnicpp::core::DeterministicRuntime runtime(0.1);
  ASSERT_TRUE(runtime.start().is_ok());
  replay.playback([&](const auto& record) {
    while (runtime.tick_count() < record.tick) {
      runtime.advance(0.1, [](auto) {}, [](auto) {});
    }
    ASSERT_TRUE(runtime.post_event(record.event));
    runtime.advance(0.0, [](auto) {}, [](auto) {});
  });
  runtime.advance(0.1, [](auto) {}, [](auto) {});
  runtime.advance(0.1, [](auto) {}, [](auto) {});
  EXPECT_FALSE(omnicpp::core::verify_replay(replay, 0.1, runtime.state_hash()));
  EXPECT_FALSE(omnicpp::core::verify_replay(replay, 0.1, runtime.state_hash() + 1));
}

TEST(DeterministicRuntime, DoesNotAdvanceWhenStopped) {
  omnicpp::core::DeterministicRuntime runtime(0.1);
  runtime.advance(10.0, [](std::uint64_t) noexcept {});
  EXPECT_EQ(runtime.tick_count(), 0U);
}

} // namespace
