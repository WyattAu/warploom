#pragma once

/**
 * @file thread_pool.hpp
 * @brief Fixed-size thread pool with optional CPU affinity.
 *
 * A simple, dependency-light thread pool for parallel work submission.
 * Designed for simulation system scheduling where thread count is fixed
 * at startup and work items are submitted per-tick.
 *
 * CONTRACT: Pool must be shut down before destruction.
 * CONTRACT: submit() must not be called after shutdown().
 * CONTRACT: Thread count is fixed at construction time.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace omnicpp::core {

//! Fixed-size thread pool.
class ThreadPool final {
public:
  //! Construct a pool with the given number of worker threads.
  //! If thread_count == 0, uses hardware_concurrency (minimum 1).
  explicit ThreadPool(std::size_t thread_count = 0) noexcept {
    if (thread_count == 0) {
      thread_count = std::thread::hardware_concurrency();
      if (thread_count == 0) thread_count = 1;
    }
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~ThreadPool() { shutdown(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  //! Submit a void() work item for execution.
  //! Returns false if the pool has been shut down.
  bool submit(std::function<void()> task) {
    if (shutdown_.load(std::memory_order_acquire)) return false;
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(static_cast<std::function<void()>&&>(task));
    }
    return true;
  }

  //! Signal shutdown and join all threads.
  void shutdown() noexcept {
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) return;
    {
      std::lock_guard lock(mutex_);
      tasks_.clear();
    }
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
  }

  //! Number of worker threads.
  [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

  //! Whether the pool has been shut down.
  [[nodiscard]] bool is_shutdown() const noexcept {
    return shutdown_.load(std::memory_order_acquire);
  }

private:
  void worker_loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) {
          if (shutdown_.load(std::memory_order_acquire)) return;
          // Spin-wait for short periods; this pool is designed for
          // per-tick task submission, not long-lived idle waiting.
          continue;
        }
        task = static_cast<std::function<void()>&&>(tasks_.back());
        tasks_.pop_back();
      }
      if (task) task();
    }
  }

  std::vector<std::thread> workers_;
  std::vector<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::atomic<bool> shutdown_{false};
};

//! Per-thread CPU affinity helper.
//! Best-effort; does nothing if the platform doesn't support it.
class Affinity final {
public:
  //! Pin the calling thread to the given logical CPU index.
  //! Returns true on success or if the platform doesn't support affinity.
  [[nodiscard]] static bool pin_to_cpu(std::size_t cpu_index) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpu_index), &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    // Platform doesn't support CPU affinity; no-op.
    return true;
#endif
  }

  //! Get the number of logical CPUs available.
  [[nodiscard]] static std::size_t hardware_concurrency() noexcept {
    const auto n = std::thread::hardware_concurrency();
    return n > 0 ? n : 1;
  }

  Affinity() = delete;
};

} // namespace omnicpp::core
