//! @file test_diagnostics.cpp
//! @brief Structured diagnostics: level filtering, frame attribution, sinks,
//!        and the suppression counter.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "warploom/core/diagnostics.hpp"

namespace {

using warploom::core::Diagnostics;
using warploom::core::LogLevel;

struct Captured {
  std::vector<std::string> lines;
};

void capture_sink(LogLevel level, const char* subsystem, const char* message,
                  void* user_data) {
  auto* out = static_cast<Captured*>(user_data);
  out->lines.push_back(std::string(warploom::core::to_string(level)) + "|" +
                       subsystem + "|" + message);
}

//! Constructs and dies when destroyed, so a test can tell whether a macro
//! evaluated its argument at all.
struct Count {
  int* made;
  int* died;
  Count(int* constructions, int* destructions)
      : made(constructions), died(destructions) {
    ++*made;
  }
  ~Count() { ++*died; }
  [[nodiscard]] int value() const { return 7; }
};

//! Restores the process-wide state so one test cannot leak a level or sink into
//! the next. Diagnostics is global by design, which makes this necessary.
class DiagnosticsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    previous_level_ = Diagnostics::level();
    previous_sink_ = Diagnostics::sink();
    captured_ = std::make_unique<Captured>();
    Diagnostics::set_sink(&capture_sink, captured_.get());
    Diagnostics::reset_counters();
  }
  void TearDown() override {
    Diagnostics::set_level(previous_level_);
    Diagnostics::set_sink(previous_sink_, nullptr);
    Diagnostics::set_frame(0);
  }
  std::unique_ptr<Captured> captured_;
  LogLevel previous_level_{LogLevel::Warn};
  warploom::core::LogSink previous_sink_{nullptr};
};

TEST_F(DiagnosticsTest, LevelThresholdFiltersBySeverity) {
  Diagnostics::set_level(LogLevel::Warn);
  Diagnostics::emit(LogLevel::Error, "t", "kept");
  Diagnostics::emit(LogLevel::Warn, "t", "kept");
  Diagnostics::emit(LogLevel::Info, "t", "dropped");
  Diagnostics::emit(LogLevel::Debug, "t", "dropped");
  ASSERT_EQ(captured_->lines.size(), 2U);
  EXPECT_EQ(captured_->lines[0], "error|t|kept");
  EXPECT_EQ(captured_->lines[1], "warn|t|kept");
}

TEST_F(DiagnosticsTest, OffSilencesEverything) {
  // Regression: Off is the HIGHEST level, so a threshold comparison written as
  // `<=` admits every real severity and WARPLOOM_LOG_LEVEL=off emitted three
  // lines instead of none.
  Diagnostics::set_level(LogLevel::Off);
  for (const auto level : {LogLevel::Error, LogLevel::Warn, LogLevel::Info,
                           LogLevel::Debug}) {
    EXPECT_FALSE(Diagnostics::enabled(level))
        << "level " << warploom::core::to_string(level)
        << " leaked through an Off threshold";
    Diagnostics::emit(level, "t", "must not appear");
  }
  EXPECT_TRUE(captured_->lines.empty());
  // Suppressed messages are counted, so "I asked for off and got silence" is
  // distinguishable from "nothing happened".
  EXPECT_GE(Diagnostics::suppressed(), 4U);
}

TEST_F(DiagnosticsTest, DebugIsOffUnlessAskedFor) {
  Diagnostics::set_level(LogLevel::Warn);
  EXPECT_FALSE(Diagnostics::enabled(LogLevel::Debug));
  Diagnostics::set_level(LogLevel::Debug);
  EXPECT_TRUE(Diagnostics::enabled(LogLevel::Debug));
  EXPECT_TRUE(Diagnostics::enabled(LogLevel::Error))
      << "raising the threshold must never silence errors";
}

TEST_F(DiagnosticsTest, MacrosRouteThroughTheSinkAndFormat) {
  Diagnostics::set_level(LogLevel::Info);
  Diagnostics::set_frame(42);
  WARPLOOM_INFO("viewport", "frame %llu at %ux%u",
                static_cast<unsigned long long>(42), 1280, 720);
  WARPLOOM_WARN("render", "%s disabled: %s", "bloom", "no format");
  ASSERT_EQ(captured_->lines.size(), 2U);
  EXPECT_EQ(captured_->lines[0], "info|viewport|frame 42 at 1280x720");
  EXPECT_EQ(captured_->lines[1], "warn|render|bloom disabled: no format");
  EXPECT_EQ(Diagnostics::frame(), 42U);
}

TEST_F(DiagnosticsTest, DisabledMacroFormatsNothing) {
  // The level check must come first: at Warn an Info macro should not even
  // evaluate its arguments. A counter object proves it.
  Diagnostics::set_level(LogLevel::Error);
  int constructions = 0;
  int destructions = 0;
  // The temporary would be constructed and destroyed if the macro evaluated
    // its argument. Zero constructions is the proof: a filtered message costs
    // one comparison and never touches the caller's expression -- which matters
    // when that expression is an expensive query or has side effects.
  WARPLOOM_INFO("t", "%d", Count(&constructions, &destructions).value());
  EXPECT_EQ(constructions, 0) << "argument was evaluated despite filtering";
  EXPECT_EQ(destructions, 0);
  EXPECT_TRUE(captured_->lines.empty());

  // And at a level that passes, the argument IS evaluated -- otherwise the
  // test above would also pass for a macro that never formats anything.
  Diagnostics::set_level(LogLevel::Info);
  WARPLOOM_INFO("t", "%d", Count(&constructions, &destructions).value());
  EXPECT_EQ(constructions, 1) << "a passing message must evaluate its argument";
  ASSERT_EQ(captured_->lines.size(), 1U);
  EXPECT_EQ(captured_->lines[0], "info|t|7");
}

TEST_F(DiagnosticsTest, EnvironmentParsingAcceptsEveryLevelAndRejectsTypos) {
  const char* const names[] = {"error", "warn", "info", "debug", "off"};
  const LogLevel expected[] = {LogLevel::Error, LogLevel::Warn,
                               LogLevel::Info, LogLevel::Debug, LogLevel::Off};
  for (std::size_t i = 0; i < 5; ++i) {
    ::setenv("WARPLOOM_LOG_LEVEL", names[i], 1);
    Diagnostics::configure_from_environment();
    EXPECT_EQ(Diagnostics::level(), expected[i]) << "level " << names[i];
    // Case-insensitive, because these get typed by hand.
    std::string upper;
    for (const char* c = names[i]; *c != '\0'; ++c) {
      upper += static_cast<char>(*c - 'a' + 'A');
    }
    ::setenv("WARPLOOM_LOG_LEVEL", upper.c_str(), 1);
    Diagnostics::configure_from_environment();
    EXPECT_EQ(Diagnostics::level(), expected[i]) << "uppercase " << upper;
  }
  // A typo must not be silent and must not be fatal: it falls back to warn.
  ::setenv("WARPLOOM_LOG_LEVEL", "verbose", 1);
  Diagnostics::configure_from_environment();
  EXPECT_EQ(Diagnostics::level(), LogLevel::Warn);
  ::unsetenv("WARPLOOM_LOG_LEVEL");
  Diagnostics::configure_from_environment();
  EXPECT_EQ(Diagnostics::level(), LogLevel::Warn) << "default is warn";
}

TEST_F(DiagnosticsTest, ToStringCoversEveryLevel) {
  EXPECT_STREQ(warploom::core::to_string(LogLevel::Error), "error");
  EXPECT_STREQ(warploom::core::to_string(LogLevel::Warn), "warn");
  EXPECT_STREQ(warploom::core::to_string(LogLevel::Info), "info");
  EXPECT_STREQ(warploom::core::to_string(LogLevel::Debug), "debug");
  EXPECT_STREQ(warploom::core::to_string(LogLevel::Off), "off");
}

}  // namespace
