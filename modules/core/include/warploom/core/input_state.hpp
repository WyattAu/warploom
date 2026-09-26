#pragma once

/**
 * @file input_state.hpp
 * @brief Device-agnostic input: named actions (booleans) and axes (floats)
 *        with double-buffered per-tick snapshots.
 *
 * Drivers (XCB keyboard, Linux joystick, virtual script source) write into
 * an InputState each tick; game logic reads the current snapshot and can
 * detect edges (pressed this tick / released this tick) against the previous
 * one. The abstraction is the point: a scripted virtual driver and a human
 * on a gamepad are indistinguishable downstream, which is what makes
 * closed-loop automated testing possible.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace omnicpp::core {

//! One frame's input snapshot: action -> held, axis -> value.
struct InputSnapshot {
  std::unordered_map<std::string, bool> actions;
  std::unordered_map<std::string, float> axes;

  [[nodiscard]] bool action(const std::string& name) const {
    const auto it = actions.find(name);
    return it != actions.end() && it->second;
  }
  [[nodiscard]] float axis(const std::string& name) const {
    const auto it = axes.find(name);
    return it != axes.end() ? it->second : 0.0f;
  }
};

//! Double-buffered input state. `begin_tick()` promotes current -> previous
//! and opens the new tick for driver writes; `commit_tick()` publishes.
class InputState final {
 public:
  //! Opens a new tick: the previous snapshot becomes last tick's.
  void begin_tick() noexcept { previous_ = current_; }

  //! Driver write API (inside a tick, before commit).
  void set_action(const std::string& name, bool held) {
    current_.actions[name] = held;
  }
  void set_axis(const std::string& name, float value) {
    current_.axes[name] = value;
  }
  //! Adds to an axis (multiple devices can contribute to e.g. "move_x").
  void add_axis(const std::string& name, float delta) {
    current_.axes[name] += delta;
  }
  //! Clamps all axes to [-1, 1] after drivers have written.
  void clamp_axes() noexcept {
    for (auto& [name, value] : current_.axes) {
      value = std::min(std::max(value, -1.0f), 1.0f);
    }
  }

  //! Publishes the tick; after this, edge queries are valid.
  void commit_tick() noexcept {
    has_previous_ = true;
  }

  // --- Read API (after commit_tick) -------------------------------------

  [[nodiscard]] bool action(const std::string& name) const noexcept {
    return current_.action(name);
  }
  [[nodiscard]] float axis(const std::string& name) const noexcept {
    return current_.axis(name);
  }
  //! True the tick the action transitions from up to down.
  [[nodiscard]] bool action_pressed(const std::string& name) const noexcept {
    return current_.action(name) && !(has_previous_ && previous_.action(name));
  }
  //! True the tick the action transitions from down to up.
  [[nodiscard]] bool action_released(const std::string& name) const noexcept {
    return !current_.action(name) && has_previous_ && previous_.action(name);
  }
  //! Axis delta this tick (current - previous).
  [[nodiscard]] float axis_delta(const std::string& name) const noexcept {
    return current_.axis(name) - (has_previous_ ? previous_.axis(name) : 0.0f);
  }

  [[nodiscard]] const InputSnapshot& current() const noexcept {
    return current_;
  }
  [[nodiscard]] const InputSnapshot& previous() const noexcept {
    return previous_;
  }

  //! Clears the current snapshot (drivers write from scratch each tick).
  void clear_current() noexcept {
    current_.actions.clear();
    current_.axes.clear();
  }

 private:
  InputSnapshot current_{};
  InputSnapshot previous_{};
  bool has_previous_{false};
};

//! A source of input events, polled once per tick. Implementations translate
//! device state (or scripts) into InputState writes.
class InputDriver {
 public:
  virtual ~InputDriver() = default;
  //! Called after begin_tick(), before commit_tick(). Writes into `state`.
  virtual void poll(InputState& state) = 0;
  //! Driver name for telemetry ("virtual", "xcb_keyboard", "js0", ...).
  [[nodiscard]] virtual const char* name() const noexcept = 0;
};

//! Scriptable virtual driver: replays a JSONL event list by tick.
//! Each event: {"tick": N, "action": "walk_forward", "value": 1.0}
//! (action events) or {"tick": N, "axis": "move_x", "value": 0.5}
//! (axis events). Actions hold from their tick until a later event sets
//! value 0; axes apply only on their tick (latched writes; game logic
//! typically consumes and the next tick's absence means 0 -- drivers write
//! axes fresh each tick, so the virtual driver re-writes held axes every
//! tick until an event changes them).
class VirtualInputDriver final : public InputDriver {
 public:
  struct Event {
    std::uint64_t tick{0};
    bool is_axis{false};
    std::string name;
    float value{0.0f};
  };

  //! Loads a JSONL script: one JSON object per line. Lines that are blank
  //! or start with '#' are skipped. Returns false + `error` on malformed
  //! content. `events` is sorted by tick.
  [[nodiscard]] bool load_script(const std::string& path,
                                 std::string& error);
  //! Programmatic event injection (testing convenience).
  void add_event(Event event) {
    events_.push_back(std::move(event));
    sorted_ = false;
  }
  [[nodiscard]] std::size_t event_count() const noexcept {
    return events_.size();
  }

  void poll(InputState& state) override {
    if (!sorted_) {
      std::sort(events_.begin(), events_.end(),
                [](const Event& a, const Event& b) { return a.tick < b.tick; });
      sorted_ = true;
    }
    // Consume all events up to and including the current tick.
    while (cursor_ < events_.size() && events_[cursor_].tick <= tick_) {
      const Event& event = events_[cursor_];
      if (event.is_axis) {
        held_axes_[event.name] = event.value;
        state.set_axis(event.name, event.value);
      } else {
        held_actions_[event.name] = event.value != 0.0f;
        state.set_action(event.name, event.value != 0.0f);
      }
      ++cursor_;
    }
    // Re-assert held state every tick (drivers write fresh snapshots).
    for (const auto& [name, held] : held_actions_) {
      state.set_action(name, held);
    }
    for (const auto& [name, value] : held_axes_) {
      state.set_axis(name, value);
    }
    ++tick_;
  }

  [[nodiscard]] const char* name() const noexcept override { return "virtual"; }

 private:
  std::vector<Event> events_{};
  std::unordered_map<std::string, bool> held_actions_{};
  std::unordered_map<std::string, float> held_axes_{};
  std::size_t cursor_{0};
  std::uint64_t tick_{0};
  bool sorted_{true};
};

}  // namespace omnicpp::core
