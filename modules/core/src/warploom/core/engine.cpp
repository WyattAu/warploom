#include "warploom/core/engine.hpp"

namespace OmniCpp::Engine::Core {

struct Engine::Impl {
  EngineConfig config{};
  bool running{false};
  std::unique_ptr<omnicpp::core::DeterministicRuntime> runtime;
};

Engine::Engine() : m_impl(std::make_unique<Impl>()) {}
Engine::~Engine() { shutdown(); }
Engine::Engine(Engine&& other) noexcept = default;
Engine& Engine::operator=(Engine&& other) noexcept = default;

omnicpp::core::Result<void> Engine::initialize(const EngineConfig& config) {
  if (config.fixed_timestep <= 0.0F) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  if (m_impl->running) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::not_running);
  }
  m_impl->config = config;
  m_impl->runtime = std::make_unique<omnicpp::core::DeterministicRuntime>(
      static_cast<double>(config.fixed_timestep));
  m_impl->runtime->set_catch_up_policy(
      config.catch_up_policy,
      static_cast<std::size_t>(config.max_catch_up_ticks));
  m_impl->runtime->set_time_mode(config.time_mode);
  m_impl->runtime->set_event_transport(config.event_transport);
  auto result = m_impl->runtime->start();
  m_impl->running = result.is_ok();
  return result;
}

void Engine::run() {
  if (!m_impl->running || !m_impl->runtime) return;
  m_impl->runtime->advance(0.0, [](std::uint64_t) {},
                          [](std::uint64_t) {});
}

void Engine::shutdown() {
  if (!m_impl || !m_impl->running) return;
  if (m_impl->runtime) {
    static_cast<void>(m_impl->runtime->stop());
  }
  m_impl->running = false;
}

void Engine::update(float delta_time) {
  if (!m_impl->running || !m_impl->runtime) return;
  m_impl->runtime->advance(static_cast<double>(delta_time),
                          [](std::uint64_t) {},
                          [](std::uint64_t) {});
}

void Engine::render() {}

bool Engine::is_running() const noexcept {
  return m_impl != nullptr && m_impl->running;
}

const EngineConfig& Engine::get_config() const noexcept {
  return m_impl->config;
}

bool Engine::post_event(std::uint64_t event) noexcept {
  return m_impl != nullptr && m_impl->running && m_impl->runtime &&
         m_impl->runtime->post_event(event);
}

std::uint64_t Engine::overrun_count() const noexcept {
  return (m_impl && m_impl->runtime) ? m_impl->runtime->overrun_count() : 0;
}

double Engine::dropped_time_seconds() const noexcept {
  return (m_impl && m_impl->runtime) ? m_impl->runtime->dropped_time_seconds() : 0.0;
}

} // namespace OmniCpp::Engine::Core
