/**
 * @file engine.hpp
 * @brief Canonical deterministic engine lifecycle API.
 */

#pragma once

#include <cstdint>
#include <memory>
#include "warploom/core/deterministic_runtime.hpp"

namespace OmniCpp::Engine::Core {

/** Configuration for the canonical deterministic runtime. */
struct EngineConfig {
  std::uint32_t max_fps{60};
  float fixed_timestep{0.01667F};
  bool enable_profiling{false};
  bool headless{true};
  ::warploom::core::CatchUpPolicy catch_up_policy{::warploom::core::CatchUpPolicy::run_all};
  std::uint32_t max_catch_up_ticks{1};
  ::warploom::core::EventTransport event_transport{::warploom::core::EventTransport::spsc};
  ::warploom::core::TimeMode time_mode{::warploom::core::TimeMode::floating_point};
};

/**
 * Canonical runtime facade.
 *
 * The core implementation is dependency-light and headless. Graphics, audio,
 * networking, scripting, and other subsystems are optional integrations and
 * are not initialized by this class.
 */
class Engine {
public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) noexcept;
  Engine& operator=(Engine&&) noexcept;

  /** Initialize the runtime; returns error code on failure. */
  [[nodiscard]] ::warploom::core::Result<void> initialize(const EngineConfig& config);
  /** Perform one non-blocking runtime service pass. */
  void run();
  /** Idempotently stop the runtime. */
  void shutdown();
  /** Advance deterministic simulation by elapsed seconds. */
  void update(float delta_time);
  /** Reserved render hook; the canonical headless runtime performs no rendering. */
  void render();

  [[nodiscard]] bool is_running() const noexcept;
  [[nodiscard]] const EngineConfig& get_config() const noexcept;
  [[nodiscard]] bool post_event(std::uint64_t event) noexcept;
  [[nodiscard]] std::uint64_t overrun_count() const noexcept;
  [[nodiscard]] double dropped_time_seconds() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace OmniCpp::Engine::Core
// S2-B compat footer: legacy `omnicpp::*` spellings keep resolving
// during the transition (removed with the S5 identity pass -
// docs/warploom-core-plan.md, phase B). A using-directive in a
// namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types, and legally coexists
// with real `omnicpp::core` extension blocks elsewhere (extension
// blocks merge). One directive per namespace THIS header declares,
// each under its OWN guard (a shared guard would suppress later
// headers' distinct directives).

