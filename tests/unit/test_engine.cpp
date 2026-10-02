#include <gtest/gtest.h>
#include "warploom/core/engine.hpp"

namespace {

TEST(Engine, StartsHeadlessAndReportsConfiguration) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.1F;
  config.headless = true;

  auto result = engine.initialize(config);
  ASSERT_TRUE(result.is_ok()) << "initialize failed: "
      << static_cast<int>(result.error());
  EXPECT_TRUE(engine.is_running());
  EXPECT_EQ(engine.get_config().catch_up_policy,
            omnicpp::core::CatchUpPolicy::run_all);
  EXPECT_TRUE(engine.post_event(1));
  engine.shutdown();
  EXPECT_FALSE(engine.is_running());
}

TEST(Engine, AppliesCatchUpPolicyAndReportsOverrun) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.1F;
  config.headless = true;
  config.catch_up_policy = omnicpp::core::CatchUpPolicy::cap_and_drop_time;
  config.max_catch_up_ticks = 2;
  ASSERT_TRUE(engine.initialize(config).is_ok());
  ASSERT_TRUE(engine.post_event(42));
  engine.update(0.55F);
  EXPECT_EQ(engine.overrun_count(), 1U);
  EXPECT_NEAR(engine.dropped_time_seconds(), 0.35, 1e-5);
  engine.shutdown();
}

TEST(Engine, RejectsInvalidFixedStep) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.0F;
  auto result = engine.initialize(config);
  EXPECT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), omnicpp::core::RuntimeError::invalid_config);
}

TEST(Engine, RejectsDoubleStart) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.1F;
  ASSERT_TRUE(engine.initialize(config).is_ok());
  auto result = engine.initialize(config);
  EXPECT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), omnicpp::core::RuntimeError::not_running);
  engine.shutdown();
}

TEST(Engine, PropagatesEventTransportConfig) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.1F;
  config.event_transport = omnicpp::core::EventTransport::mpsc;
  ASSERT_TRUE(engine.initialize(config).is_ok());
  EXPECT_EQ(engine.get_config().event_transport,
            omnicpp::core::EventTransport::mpsc);
  engine.shutdown();
}

TEST(Engine, PropagatesTimeModeConfig) {
  OmniCpp::Engine::Core::Engine engine;
  OmniCpp::Engine::Core::EngineConfig config;
  config.fixed_timestep = 0.1F;
  config.time_mode = omnicpp::core::TimeMode::integer_nanoseconds;
  ASSERT_TRUE(engine.initialize(config).is_ok());
  EXPECT_EQ(engine.get_config().time_mode,
            omnicpp::core::TimeMode::integer_nanoseconds);
  engine.shutdown();
}

} // namespace
