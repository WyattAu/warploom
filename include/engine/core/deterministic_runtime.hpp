#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace omnicpp::core {

// ============================================================================
// Error Model
// ============================================================================

//! Error codes for runtime operations.
enum class RuntimeError : std::uint8_t {
  ok = 0,
  not_running,
  invalid_config,
  invalid_state,
  event_queue_full,
  replay_checksum_mismatch,
  replay_version_unsupported,
  replay_truncated,
  file_io_error,
  vulkan_not_available,
  //! Asset/document data failed to parse or violates an unsupported
  //! requirement (e.g. malformed JSON, out-of-range glTF references,
  //! unsupported component/encoding).
  malformed_asset
};

//! Lightweight result type for runtime operations.
//! No dynamic allocation. No exceptions.
template <typename T>
class Result final {
public:
  static Result ok(T value) noexcept {
    Result r;
    r.error_ = RuntimeError::ok;
    r.value_ = static_cast<T&&>(value);
    return r;
  }
  static Result error(RuntimeError e) noexcept {
    Result r;
    r.error_ = e;
    return r;
  }
  [[nodiscard]] bool is_ok() const noexcept { return error_ == RuntimeError::ok; }
  [[nodiscard]] RuntimeError error() const noexcept { return error_; }
  [[nodiscard]] const T& value() const& noexcept { return value_; }
  [[nodiscard]] T&& value() && noexcept { return static_cast<T&&>(value_); }
private:
  Result() = default;
  RuntimeError error_{RuntimeError::ok};
  T value_{};
};

//! Specialization for void — only carries an error code.
template <>
class Result<void> final {
public:
  static Result ok() noexcept {
    Result r;
    r.error_ = RuntimeError::ok;
    return r;
  }
  static Result error(RuntimeError e) noexcept {
    Result r;
    r.error_ = e;
    return r;
  }
  [[nodiscard]] bool is_ok() const noexcept { return error_ == RuntimeError::ok; }
  [[nodiscard]] RuntimeError error() const noexcept { return error_; }
private:
  Result() = default;
  RuntimeError error_{RuntimeError::ok};
};

// ============================================================================
// Thread-Safety Contracts
// ============================================================================

//! Non-thread-safe bounded FIFO queue.
//! CONTRACT: Must not be accessed from multiple threads concurrently.
//! Use SpscChannel or MpscChannel for multi-threaded access.
template <typename T, std::size_t Capacity>
class BoundedQueue final {
  static_assert(Capacity > 0, "BoundedQueue capacity must be positive");
public:
  [[nodiscard]] bool try_push(const T& value) noexcept {
    if (size_ == Capacity) return false;
    storage_[tail_] = value;
    tail_ = (tail_ + 1) % Capacity;
    ++size_;
    return true;
  }
  [[nodiscard]] bool try_pop(T& value) noexcept {
    if (size_ == 0) return false;
    value = storage_[head_];
    head_ = (head_ + 1) % Capacity;
    --size_;
    return true;
  }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
private:
  std::array<T, Capacity> storage_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
};

//! Lock-free single-producer / single-consumer channel.
//! CONTRACT: Exactly one thread may call try_push().
//! CONTRACT: Exactly one (different) thread may call try_pop().
//! No mutex. No allocation. Cache-line separated head/tail.
template <typename T, std::size_t Capacity>
class SpscChannel final {
  static_assert(Capacity > 0, "SpscChannel capacity must be positive");
public:
  [[nodiscard]] bool try_push(const T& value) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto next = increment(tail);
    if (next == head_.load(std::memory_order_acquire)) return false;
    storage_[tail] = value;
    tail_.store(next, std::memory_order_release);
    return true;
  }
  [[nodiscard]] bool try_pop(T& value) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return false;
    value = storage_[head];
    head_.store(increment(head), std::memory_order_release);
    return true;
  }
  [[nodiscard]] std::size_t size() const noexcept {
    const auto tail = tail_.load(std::memory_order_acquire);
    const auto head = head_.load(std::memory_order_acquire);
    return tail >= head ? tail - head : Capacity - head + tail;
  }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity - 1; }
private:
  [[nodiscard]] static constexpr std::size_t increment(std::size_t index) noexcept {
    return (index + 1) % Capacity;
  }
  std::array<T, Capacity> storage_{};
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

//!
//! Vyukov bounded MPSC (multi-producer, single-consumer) lock-free queue.
//!
//! CONTRACT: Multiple threads may call try_push() concurrently.
//! CONTRACT: Exactly one thread must call try_pop() (the consumer).
//! Capacity is rounded up to the next power of two internally.
//! Uses per-slot sequence counters for coordination.
//! No dynamic allocation. No locks.
//!
namespace detail {
  [[nodiscard]] constexpr std::size_t round_up_pow2(std::size_t v) noexcept {
    if (v == 0) return 1;
    --v;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    if constexpr (sizeof(std::size_t) > 4) { v |= v >> 32; }
    return v + 1;
  }
} // namespace detail

template <typename T, std::size_t Capacity>
class MpscChannel final {
  static_assert(Capacity > 0, "MpscChannel capacity must be positive");
  static constexpr std::size_t kCapacity = detail::round_up_pow2(Capacity);
  static_assert(kCapacity >= 2, "MpscChannel requires at least 2 slots");
  static constexpr std::size_t kMask = kCapacity - 1;

  struct Cell {
    T data{};
    alignas(64) std::atomic<std::size_t> sequence{0};
  };

public:
  MpscChannel() {
    for (std::size_t i = 0; i < kCapacity; ++i) {
      storage_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] bool try_push(const T& value) noexcept {
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      auto& cell = storage_[pos & kMask];
      auto seq = cell.sequence.load(std::memory_order_acquire);
      if (seq == pos) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          cell.data = value;
          cell.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (seq < pos) {
        return false;
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  [[nodiscard]] bool try_pop(T& value) noexcept {
    auto pos = head_.load(std::memory_order_relaxed);
    auto& cell = storage_[pos & kMask];
    auto seq = cell.sequence.load(std::memory_order_acquire);
    if (seq != pos + 1) {
      return false;
    }
    head_.store(pos + 1, std::memory_order_relaxed);
    value = cell.data;
    cell.sequence.store(pos + kCapacity, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    auto tail = tail_.load(std::memory_order_acquire);
    auto head = head_.load(std::memory_order_acquire);
    return (tail >= head) ? (tail - head) : 0;
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return kCapacity; }

private:
  std::array<Cell, kCapacity> storage_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

// ============================================================================
// Runtime Configuration
// ============================================================================

enum class RuntimeState : std::uint8_t { stopped, running };

enum class CatchUpPolicy : std::uint8_t {
  run_all,
  cap_and_drop_time,
  cap_and_report_overrun
};

//! Transport mode for the event channel.
//! spsc: single producer thread posts events (fastest path).
//! mpsc: multiple producer threads post events (requires MpscChannel).
enum class EventTransport : std::uint8_t { spsc, mpsc };

//! Time accumulation mode.
//! floating_point: uses double arithmetic (fast, may accumulate drift).
//! integer_nanoseconds: uses int64 nanosecond ticks (deterministic across compilers).
enum class TimeMode : std::uint8_t { floating_point, integer_nanoseconds };

// ============================================================================
// Deterministic Runtime
// ============================================================================

class DeterministicRuntime final {
public:
  using TickCallback = std::function<void(std::uint64_t)>;
  using Event = std::uint64_t;

  explicit DeterministicRuntime(double fixed_step_seconds = 1.0 / 60.0) noexcept
      : fixed_step_seconds_(fixed_step_seconds > 0.0 ? fixed_step_seconds : 1.0 / 60.0),
        fixed_step_ns_(static_cast<std::int64_t>(fixed_step_seconds_ * 1e9)) {}

  // --- Lifecycle ---

  [[nodiscard]] Result<void> start() noexcept {
    if (state_ == RuntimeState::running) return Result<void>::error(RuntimeError::not_running);
    state_ = RuntimeState::running;
    return Result<void>::ok();
  }
  [[nodiscard]] Result<void> stop() noexcept {
    if (state_ == RuntimeState::stopped) return Result<void>::error(RuntimeError::not_running);
    state_ = RuntimeState::stopped;
    if (time_mode_ == TimeMode::integer_nanoseconds) {
      accumulator_ns_ = 0;
    } else {
      accumulator_ = 0.0;
    }
    return Result<void>::ok();
  }
  [[nodiscard]] bool is_running() const noexcept { return state_ == RuntimeState::running; }
  [[nodiscard]] RuntimeState state() const noexcept { return state_; }

  // --- Configuration ---

  void set_catch_up_policy(CatchUpPolicy policy, std::size_t max_ticks = 1) noexcept {
    catch_up_policy_ = policy;
    max_catch_up_ticks_ = max_ticks;
  }
  [[nodiscard]] CatchUpPolicy catch_up_policy() const noexcept { return catch_up_policy_; }
  [[nodiscard]] std::size_t max_catch_up_ticks() const noexcept { return max_catch_up_ticks_; }

  void set_time_mode(TimeMode mode) noexcept { time_mode_ = mode; }
  [[nodiscard]] TimeMode time_mode() const noexcept { return time_mode_; }

  void set_event_transport(EventTransport transport) noexcept { event_transport_ = transport; }
  [[nodiscard]] EventTransport event_transport() const noexcept { return event_transport_; }

  // --- Telemetry ---

  [[nodiscard]] std::uint64_t overrun_count() const noexcept { return overrun_count_; }
  [[nodiscard]] double dropped_time_seconds() const noexcept { return dropped_time_seconds_; }

  // --- Event Posting (SPSC path) ---

  [[nodiscard]] bool post_event(Event event) noexcept {
    if (events_.try_push(event)) return true;
    dropped_events_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // --- Event Posting (MPSC path) ---

  [[nodiscard]] bool post_event_mpsc(Event event) noexcept {
    if (events_mpsc_.try_push(event)) return true;
    dropped_events_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // --- Advance ---

  template <typename EventHandler, typename TickHandler>
  void advance(double elapsed_seconds, EventHandler&& on_event, TickHandler&& on_tick) {
    if (!is_running()) return;

    // Drain events from the active transport
    Event event{};
    switch (event_transport_) {
      case EventTransport::spsc:
        while (events_.try_pop(event)) {
          std::forward<EventHandler>(on_event)(event);
          state_hash_ = mix(state_hash_, event);
        }
        break;
      case EventTransport::mpsc:
        while (events_mpsc_.try_pop(event)) {
          std::forward<EventHandler>(on_event)(event);
          state_hash_ = mix(state_hash_, event);
        }
        break;
    }

    // Advance simulation ticks
    switch (time_mode_) {
      case TimeMode::floating_point:
        advance_floating(elapsed_seconds, std::forward<TickHandler>(on_tick));
        break;
      case TimeMode::integer_nanoseconds:
        advance_integer(static_cast<std::int64_t>(elapsed_seconds * 1e9),
                        std::forward<TickHandler>(on_tick));
        break;
    }
  }

  void advance(double elapsed_seconds, const TickCallback& callback) {
    advance(elapsed_seconds, [](Event) {}, callback);
  }

  // --- Observers ---

  [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_count_; }
  [[nodiscard]] std::size_t pending_events() const noexcept {
    switch (event_transport_) {
      case EventTransport::spsc: return events_.size();
      case EventTransport::mpsc: return events_mpsc_.size();
    }
    return 0;
  }
  [[nodiscard]] std::size_t dropped_events() const noexcept {
    return dropped_events_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] double accumulator() const noexcept { return accumulator_; }
  [[nodiscard]] std::int64_t accumulator_ns() const noexcept { return accumulator_ns_; }
  [[nodiscard]] double fixed_step_seconds() const noexcept { return fixed_step_seconds_; }
  [[nodiscard]] std::int64_t fixed_step_ns() const noexcept { return fixed_step_ns_; }
  [[nodiscard]] std::uint64_t state_hash() const noexcept { return state_hash_; }

private:
  // --- Floating-point advance path ---
  template <typename TickHandler>
  void advance_floating(double elapsed_seconds, TickHandler&& on_tick) {
    if (elapsed_seconds < 0.0) elapsed_seconds = 0.0;
    accumulator_ += elapsed_seconds;
    std::size_t ticks_this_advance = 0;
    while (accumulator_ >= fixed_step_seconds_ &&
           (catch_up_policy_ == CatchUpPolicy::run_all || ticks_this_advance < max_catch_up_ticks_)) {
      accumulator_ -= fixed_step_seconds_;
      ++tick_count_;
      ++ticks_this_advance;
      state_hash_ = mix(state_hash_, tick_count_);
      std::forward<TickHandler>(on_tick)(tick_count_);
    }
    if (accumulator_ >= fixed_step_seconds_ && catch_up_policy_ != CatchUpPolicy::run_all) {
      ++overrun_count_;
      if (catch_up_policy_ == CatchUpPolicy::cap_and_drop_time) {
        dropped_time_seconds_ += accumulator_;
        accumulator_ = 0.0;
      }
    }
  }

  // --- Integer-nanosecond advance path ---
  template <typename TickHandler>
  void advance_integer(std::int64_t elapsed_ns, TickHandler&& on_tick) {
    if (elapsed_ns < 0) elapsed_ns = 0;
    accumulator_ns_ += elapsed_ns;
    std::size_t ticks_this_advance = 0;
    while (accumulator_ns_ >= fixed_step_ns_ &&
           (catch_up_policy_ == CatchUpPolicy::run_all || ticks_this_advance < max_catch_up_ticks_)) {
      accumulator_ns_ -= fixed_step_ns_;
      ++tick_count_;
      ++ticks_this_advance;
      state_hash_ = mix(state_hash_, tick_count_);
      std::forward<TickHandler>(on_tick)(tick_count_);
    }
    if (accumulator_ns_ >= fixed_step_ns_ && catch_up_policy_ != CatchUpPolicy::run_all) {
      ++overrun_count_;
      if (catch_up_policy_ == CatchUpPolicy::cap_and_drop_time) {
        dropped_time_seconds_ += static_cast<double>(accumulator_ns_) / 1e9;
        accumulator_ns_ = 0;
      }
    }
  }

  static constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
  static constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
  [[nodiscard]] static std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      hash ^= (value >> (i * 8U)) & 0xffU;
      hash *= kFnvPrime;
    }
    return hash;
  }

  // Configuration
  double fixed_step_seconds_;
  std::int64_t fixed_step_ns_;
  TimeMode time_mode_{TimeMode::floating_point};
  EventTransport event_transport_{EventTransport::spsc};
  CatchUpPolicy catch_up_policy_{CatchUpPolicy::run_all};
  std::size_t max_catch_up_ticks_{1};

  // State
  RuntimeState state_{RuntimeState::stopped};
  std::uint64_t tick_count_{0};
  std::uint64_t state_hash_{kFnvOffset};
  std::uint64_t overrun_count_{0};

  // Floating-point accumulator
  double accumulator_{0.0};
  double dropped_time_seconds_{0.0};

  // Integer-nanosecond accumulator
  std::int64_t accumulator_ns_{0};

  // Event channels
  SpscChannel<Event, 257> events_;       // usable capacity 256 (SPSC)
  MpscChannel<Event, 257> events_mpsc_; // usable capacity 256 (MPSC)
  std::atomic<std::size_t> dropped_events_{0};
};

} // namespace omnicpp::core
