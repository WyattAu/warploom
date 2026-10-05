// SPDX-License-Identifier: MIT
//! Structured diagnostics: one filterable channel for the app, the engine, and
//! the viewport.
//!
//! Before this, the viewport reported 116 ad-hoc `fprintf(stderr)` calls and
//! the entire renderer exactly one. That is not debuggable: no severity, no
//! timestamp, no frame to attach a message to, and nothing to grep. Worse, the
//! engine was nearly silent, so a failure surfaced as a bare error code with
//! no indication of which subsystem produced it.
//!
//! The design constraints were:
//!   - No new dependency. The whole point of Phase 0 was one package manager
//!     and one third-party library; adding spdlog to get logging would undo it.
//!   - Filterable by severity without recompiling, via WARPLOOM_LOG_LEVEL.
//!   - Every line carries the frame it belongs to, so app-side and engine-side
//!     messages can be lined up with the telemetry stream and GPU timings.
//!   - Cheap when disabled. Level checks happen before any formatting, and
//!     nothing is allocated on the steady-state path.
//!
//! It is deliberately not a logging framework: no sinks, no async, no
//! formatting library. A sink is one function pointer, which is enough for the
//! viewport to tee into telemetry.jsonl and for tests to capture output.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace warploom::core {

//! Severity, ordered so a level threshold is a numeric comparison.
enum class LogLevel : std::uint8_t {
  Error = 0,   //!< Something failed and the run is compromised.
  Warn = 1,    //!< Suspicious, but the run continues.
  Info = 2,    //!< Normal lifecycle milestones.
  Debug = 3,   //!< Per-frame detail; off unless asked for.
  Off = 4,     //!< Emit nothing.
};

[[nodiscard]] const char* to_string(LogLevel level) noexcept;

//! Where a message goes. Set once; the viewport points it at both stderr and
//! its telemetry file so engine messages land in the same trace as frame data.
using LogSink = void (*)(LogLevel level, const char* subsystem,
                         const char* message, void* user_data);

//! Process-wide diagnostics state. Header-only and dependency-free so every
//! module can reach it without linking order games.
class Diagnostics final {
 public:
  //! Current threshold; messages above it are dropped before formatting.
  [[nodiscard]] static LogLevel level() noexcept { return level_; }
  static void set_level(LogLevel level) noexcept { level_ = level; }

  //! True when a message at `level` would be emitted.
  //!
  //! Both the threshold AND the message level must rule out Off. Checking only
  //! the threshold looks sufficient because Off is the highest value, but the
  //! comparison is `<=`, so with the threshold at Off every real severity
  //! passes -- WARPLOOM_LOG_LEVEL=off emitted instead of silencing.
  [[nodiscard]] static bool enabled(LogLevel level) noexcept {
    return level_ != LogLevel::Off && level != LogLevel::Off &&
           static_cast<std::uint8_t>(level) <=
               static_cast<std::uint8_t>(level_);
  }

  //! Reads WARPLOOM_LOG_LEVEL (error|warn|info|debug|off, case-insensitive) and
  //! defaults to Warn. Unknown values fall back to Warn rather than failing:
  //! a typo in a debug aid must not stop the app starting.
  static void configure_from_environment();

  static void set_sink(LogSink sink, void* user_data = nullptr) noexcept {
    sink_ = sink;
    sink_data_ = user_data;
  }
  [[nodiscard]] static LogSink sink() noexcept { return sink_; }

  //! Frame the next messages belong to. Set once per frame by the host so every
  //! message can be attributed without threading a frame argument through every
  //! call site.
  static void set_frame(std::uint64_t frame) noexcept { frame_ = frame; }
  [[nodiscard]] static std::uint64_t frame() noexcept { return frame_; }

  //! How many messages were dropped because of the level filter since the last
  //! reset. Lets a run report "you asked for debug and got nothing" as a fact
  //! rather than a mystery.
  [[nodiscard]] static std::uint64_t suppressed() noexcept { return suppressed_; }
  static void reset_counters() noexcept { suppressed_ = 0; }

  //! The one emit path. `subsystem` should be a short stable tag ("render",
  //! "viewport", "physics") so output can be filtered by origin.
  static void emit(LogLevel level, const char* subsystem, const char* message) noexcept;

 private:
  static LogLevel level_;
  static LogSink sink_;
  static void* sink_data_;
  static std::uint64_t frame_;
  static std::uint64_t suppressed_;
};

namespace detail {
//! printf-style formatting into a fixed buffer, so emitting allocates nothing.
/// Truncates rather than overflowing: a diagnostic that loses its tail is
/// still useful, one that corrupts the stack is not.
[[nodiscard]] inline int format_into(char* buffer, std::size_t size,
                                     const char* format, ...) noexcept {
  if (size == 0U) return 0;
  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(buffer, size, format, args);
  va_end(args);
  if (written < 0) {
    buffer[0] = '\0';
    return 0;
  }
  return written;
}
}  // namespace detail

//! Emit a printf-style diagnostic. The level check comes first, so a disabled
//! Debug message costs one comparison and no formatting.
//!
//! The macro parameter is spelled `warploom_level_` rather than `level_`
//! because a short name collides with identifiers used inside the expansion:
//! naming it `level_` and then writing `WarploomLogLevel::level_` expanded the
//! parameter into the middle of its own qualified name.
#define WARPLOOM_LOG(warploom_level_, subsystem_, ...)                   \
  do {                                                                    \
    constexpr ::warploom::core::LogLevel kWarploomLevel =                \
        warploom_level_;                                                  \
    if (::warploom::core::Diagnostics::enabled(kWarploomLevel)) {        \
      char warploom_buffer_[512];                                         \
      (void)::warploom::core::detail::format_into(                        \
          warploom_buffer_, sizeof(warploom_buffer_), __VA_ARGS__);      \
      ::warploom::core::Diagnostics::emit(kWarploomLevel, subsystem_,    \
                                          warploom_buffer_);             \
    }                                                                     \
  } while (false)

#define WARPLOOM_ERROR(subsystem_, ...) \
  WARPLOOM_LOG(::warploom::core::LogLevel::Error, subsystem_, __VA_ARGS__)
#define WARPLOOM_WARN(subsystem_, ...) \
  WARPLOOM_LOG(::warploom::core::LogLevel::Warn, subsystem_, __VA_ARGS__)
#define WARPLOOM_INFO(subsystem_, ...) \
  WARPLOOM_LOG(::warploom::core::LogLevel::Info, subsystem_, __VA_ARGS__)
#define WARPLOOM_DEBUG(subsystem_, ...) \
  WARPLOOM_LOG(::warploom::core::LogLevel::Debug, subsystem_, __VA_ARGS__)

}  // namespace warploom::core
