/**
 * @file test_runtime_only.cpp
 * @brief Verifies the canonical runtime API is independently usable.
 *
 * This test links ONLY to omnicpp_runtime and must not depend on
 * omnicpp_engine or any legacy subsystem symbols.
 */

#include <gtest/gtest.h>
#include "warploom/core/engine.hpp"

namespace {

TEST(RuntimeOnly, HeadlessLifecycle) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.1F;
  config.headless = true;

  auto result = engine.initialize(config);
  ASSERT_TRUE(result.is_ok());
  EXPECT_TRUE(engine.is_running());
  EXPECT_EQ(config.catch_up_policy, omnicpp::core::CatchUpPolicy::run_all);

  engine.update(0.05F);
  EXPECT_TRUE(engine.is_running());

  engine.shutdown();
  EXPECT_FALSE(engine.is_running());
}

TEST(RuntimeOnly, CappedCatchUpAndOverrun) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.1F;
  config.headless = true;
  config.catch_up_policy = omnicpp::core::CatchUpPolicy::cap_and_drop_time;
  config.max_catch_up_ticks = 3;

  ASSERT_TRUE(engine.initialize(config).is_ok());
  engine.update(0.55F);

  EXPECT_EQ(engine.overrun_count(), 1U);
  EXPECT_TRUE(engine.dropped_time_seconds() > 0.0);

  engine.shutdown();
}

TEST(RuntimeOnly, EventSubmissionAndConfig) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.05F;
  config.headless = true;

  ASSERT_TRUE(engine.initialize(config).is_ok());
  EXPECT_TRUE(engine.post_event(100));
  EXPECT_TRUE(engine.post_event(200));
  EXPECT_FALSE(engine.is_running() == false);

  EXPECT_EQ(engine.get_config().fixed_timestep, 0.05F);
  EXPECT_EQ(engine.get_config().max_fps, 60U);

  engine.shutdown();
}

TEST(RuntimeOnly, RejectsInvalidConfig) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.0F;
  config.headless = true;

  auto result = engine.initialize(config);
  EXPECT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), omnicpp::core::RuntimeError::invalid_config);
  EXPECT_FALSE(engine.is_running());
}

TEST(RuntimeOnly, IdempotentShutdown) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.1F;
  config.headless = true;

  ASSERT_TRUE(engine.initialize(config).is_ok());
  engine.shutdown();
  EXPECT_FALSE(engine.is_running());
  engine.shutdown();
  EXPECT_FALSE(engine.is_running());
}

TEST(RuntimeOnly, DefaultPolicyIsRunAll) {
  OmniCpp::Engine::Core::EngineConfig config;
  EXPECT_EQ(config.catch_up_policy, omnicpp::core::CatchUpPolicy::run_all);
  EXPECT_EQ(config.max_catch_up_ticks, 1U);
  EXPECT_EQ(config.event_transport, omnicpp::core::EventTransport::spsc);
  EXPECT_EQ(config.time_mode, omnicpp::core::TimeMode::floating_point);
}

TEST(RuntimeOnly, IntegerNanosecondTimeMode) {
  omnicpp::core::DeterministicRuntime runtime(1.0 / 60.0);
  runtime.set_time_mode(omnicpp::core::TimeMode::integer_nanoseconds);
  EXPECT_EQ(runtime.time_mode(), omnicpp::core::TimeMode::integer_nanoseconds);

  ASSERT_TRUE(runtime.start().is_ok());
  std::size_t ticks = 0;
  // 1 second at 60Hz = 60 ticks (small remainder expected from FP->int truncation)
  runtime.advance(1.0,
      [](omnicpp::core::DeterministicRuntime::Event) {},
      [&ticks](std::uint64_t) { ++ticks; });
  EXPECT_EQ(ticks, 60U);
  EXPECT_LT(runtime.accumulator_ns(), runtime.fixed_step_ns());
  ASSERT_TRUE(runtime.stop().is_ok());
}

TEST(RuntimeOnly, MpscEventTransport) {
  omnicpp::core::DeterministicRuntime runtime(1.0 / 60.0);
  runtime.set_event_transport(omnicpp::core::EventTransport::mpsc);
  EXPECT_EQ(runtime.event_transport(), omnicpp::core::EventTransport::mpsc);

  ASSERT_TRUE(runtime.start().is_ok());
  ASSERT_TRUE(runtime.post_event_mpsc(10));
  ASSERT_TRUE(runtime.post_event_mpsc(20));

  std::size_t received = 0;
  runtime.advance(0.1,
      [&received](omnicpp::core::DeterministicRuntime::Event) { ++received; },
      [](std::uint64_t) {});
  EXPECT_EQ(received, 2U);
  ASSERT_TRUE(runtime.stop().is_ok());
}

TEST(RuntimeOnly, ResultVoidOkAndError) {
  auto ok = omnicpp::core::Result<void>::ok();
  EXPECT_TRUE(ok.is_ok());
  EXPECT_EQ(ok.error(), omnicpp::core::RuntimeError::ok);

  auto err = omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::event_queue_full);
  EXPECT_FALSE(err.is_ok());
  EXPECT_EQ(err.error(), omnicpp::core::RuntimeError::event_queue_full);
}

TEST(RuntimeOnly, ResultValueRoundTrip) {
  auto r = omnicpp::core::Result<int>::ok(42);
  EXPECT_TRUE(r.is_ok());
  EXPECT_EQ(r.value(), 42);

  auto e = omnicpp::core::Result<int>::error(omnicpp::core::RuntimeError::file_io_error);
  EXPECT_FALSE(e.is_ok());
  EXPECT_EQ(e.error(), omnicpp::core::RuntimeError::file_io_error);
}

} // namespace
