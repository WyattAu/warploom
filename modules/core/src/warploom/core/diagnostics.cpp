// SPDX-License-Identifier: MIT
//! Structured diagnostics. See diagnostics.hpp.

#include "warploom/core/diagnostics.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace warploom::core {

namespace {
//! Default stderr sink. Written as a single fwrite so interleaved lines from
//! the app and the engine cannot tear each other mid-line under concurrency --
//! fprintf on a shared stream is not atomic once the string is long.
void stderr_sink(LogLevel level, const char* subsystem, const char* message,
                 void* user_data) {
  (void)user_data;
  char line[640];
  const int written = std::snprintf(
      line, sizeof(line), "[%s] [%-8s] [frame %llu] %s\n", to_string(level),
      subsystem, static_cast<unsigned long long>(Diagnostics::frame()), message);
  if (written > 0) {
    std::fwrite(line, 1,
                static_cast<std::size_t>(
                    written < static_cast<int>(sizeof(line))
                        ? written
                        : static_cast<int>(sizeof(line)) - 1),
                stderr);
  }
}
}  // namespace

LogLevel Diagnostics::level_ = LogLevel::Warn;
LogSink Diagnostics::sink_ = &stderr_sink;
void* Diagnostics::sink_data_ = nullptr;
std::uint64_t Diagnostics::frame_ = 0;
std::uint64_t Diagnostics::suppressed_ = 0;

const char* to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Error: return "error";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Info:  return "info";
    case LogLevel::Debug: return "debug";
    case LogLevel::Off:   return "off";
  }
  return "?";
}

void Diagnostics::configure_from_environment() {
  const char* raw = std::getenv("WARPLOOM_LOG_LEVEL");
  if (raw == nullptr) {
    set_level(LogLevel::Warn);
    return;
  }
  // Case-insensitive without allocating: the value is compared against lowered
  // literals, because strcasecmp is not portable to MSVC.
  const auto equals = [raw](const char* lowered_literal) {
    std::size_t i = 0;
    for (; raw[i] != '\0' && lowered_literal[i] != '\0'; ++i) {
      const char c = (raw[i] >= 'A' && raw[i] <= 'Z')
                         ? static_cast<char>(raw[i] - 'A' + 'a')
                         : raw[i];
      if (c != lowered_literal[i]) return false;
    }
    return raw[i] == '\0' && lowered_literal[i] == '\0';
  };
  if (equals("error")) {
    set_level(LogLevel::Error);
  } else if (equals("warn")) {
    set_level(LogLevel::Warn);
  } else if (equals("info")) {
    set_level(LogLevel::Info);
  } else if (equals("debug")) {
    set_level(LogLevel::Debug);
  } else if (equals("off")) {
    set_level(LogLevel::Off);
  } else {
    // A typo in a debug aid must not stop the app starting; say so and carry on
    // at the default rather than silently pretending the level took.
    std::fprintf(stderr,
                 "Warploom: WARPLOOM_LOG_LEVEL='%s' is not one of "
                 "error|warn|info|debug|off; using warn\n",
                 raw);
    set_level(LogLevel::Warn);
  }
}

void Diagnostics::emit(LogLevel level, const char* subsystem,
                       const char* message) noexcept {
  if (!enabled(level)) {
    ++suppressed_;
    return;
  }
  const LogSink sink = sink_;
  if (sink != nullptr) {
    sink(level, subsystem, message, sink_data_);
  }
}

}  // namespace warploom::core
