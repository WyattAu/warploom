#pragma once

/**
 * @file animation_state_machine.hpp
 * @brief Deterministic input-driven animation state machine.
 *
 * Pure simulation kernel: a fixed set of named states, transition edges with
 * input-action/time conditions, and per-edge cross-fade targets. `tick()`
 * advances local clocks, evaluates edges in declaration order (first match
 * fires), and eases the active state's weight level toward its settled
 * value. The machine is a pure function of (machine, actions, dt): no
 * clocks, no globals, no allocation — so a scenario double-run is
 * byte-identical and every transition is reproducible.
 *
 * Weight semantics (matches the viewport's walk/idle blend):
 *   - Each state carries a settled `weight_level` (e.g. Walk = 0 = walk clip
 *     plays alone, Idle = 1 = idle clip fully blended over it).
 *   - `blended_weight()` is the destination-relative blend factor consumed
 *     by sample_clip_blended().
 *   - The walk clock pauses while `blended_weight() > 0.5` (idle dominates,
 *     feet planted) — the contract the viewport already honors.
 */

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace omnicpp::anim {

//! A transition edge: fires when its condition holds and the source state
//! has been active for at least `min_time_in_state` seconds.
struct AnimTransition {
  std::string from;
  std::string to;
  //! Action name the input snapshot must report as held ("" = time-only edge).
  std::string action;
  //! Minimum time in the source state before this edge can fire.
  float min_time_in_state{0.0f};
  //! Cross-fade duration in seconds (documents intent; easing is rate-based
  //! in tick() so blended output is frame-rate independent under fixed dt).
  float fade_duration{0.4f};
};

//! A named animation state. `weight_level` is the state's settled blend
//! weight (Walk = 0, Idle = 1 in the mannequin configuration).
struct AnimState {
  std::string name;
  float weight_level{0.0f};
};

//! Deterministic animation state machine. Templated on the snapshot type so
//! the core has no dependency on the input module.
template <typename Snapshot>
class AnimationStateMachine final {
 public:
  void add_state(std::string name, float weight_level) {
    AnimState s;
    s.name = std::move(name);
    s.weight_level = weight_level;
    index_[s.name] = states_.size();
    states_.push_back(s);
    time_in_state_.push_back(0.0f);
    fading_.push_back(false);
  }

  void add_transition(AnimTransition edge) {
    transitions_.push_back(std::move(edge));
    // First match wins: edges are evaluated in declaration order.
  }

  //! Sets the initial state (by name) without fading.
  [[nodiscard]] bool set_initial(const std::string& name) {
    const auto it = index_.find(name);
    if (it == index_.end()) return false;
    current_ = it->second;
    weight_ = states_[current_].weight_level;
    return true;
  }

  //! One fixed step: advance the local clock, evaluate edges in declaration
  //! order (first match fires), ease the blend weight toward the active
  //! state's settled level. Pure: no external state read or written.
  void tick(const Snapshot& actions, float dt, float fade_rate = 2.5f) {
    time_in_state_[current_] += dt;

    const AnimState& s = states_[current_];
    for (const AnimTransition& edge : transitions_) {
      if (edge.from != s.name) continue;
      if (time_in_state_[current_] < edge.min_time_in_state) continue;
      if (!edge.action.empty() && !actions.action(edge.action)) continue;
      const auto next = index_.find(edge.to);
      if (next == index_.end()) continue;
      current_ = next->second;
      time_in_state_[current_] = 0.0f;
      fading_[current_] = true;
      break;  // first match wins
    }

    // Ease the blend weight toward the active state's settled level.
    const float target = states_[current_].weight_level;
    if (weight_ != target) {
      const float step = fade_rate * dt;
      if (weight_ < target) {
        weight_ = (target - weight_ <= step) ? target : weight_ + step;
      } else {
        weight_ = (weight_ - target <= step) ? target : weight_ - step;
      }
    }
    fading_[current_] = weight_ != target;
  }

  //! The active state's name.
  [[nodiscard]] const std::string& state() const noexcept {
    return states_[current_].name;
  }

  //! Current blend weight (eased toward the active state's level).
  [[nodiscard]] float blended_weight() const noexcept { return weight_; }

  //! True while the blend weight is still easing toward the target.
  [[nodiscard]] bool fading() const noexcept { return fading_[current_]; }

  //! Local time in the active state (seconds, fixed-dt accumulated).
  [[nodiscard]] float time_in_state() const noexcept {
    return time_in_state_[current_];
  }

  //! Active state's declaration index (stable; for telemetry/switching).
  [[nodiscard]] std::uint32_t state_index() const noexcept {
    return static_cast<std::uint32_t>(current_);
  }

 private:
  std::vector<AnimState> states_;
  std::unordered_map<std::string, std::size_t> index_;
  std::vector<AnimTransition> transitions_;
  std::vector<float> time_in_state_;
  std::vector<bool> fading_;
  std::size_t current_{0};
  float weight_{0.0f};
};

}  // namespace omnicpp::anim
