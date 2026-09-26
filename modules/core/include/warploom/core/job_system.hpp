#pragma once

/**
 * @file job_system.hpp
 * @brief Prioritized job scheduler with fork/join counters and hybrid
 *        spin-then-park waits for low dispatch and wake latency.
 *
 * Design (HFT/AAA practice):
 * - Four priority classes (render > submit > upload > background). Workers
 *   always drain the highest non-empty priority first, so frame-critical
 *   work preempts batch work at queue granularity.
 * - Fork/join via JobCounter: add() before submitting the batch members;
 *   wait() blocks until all counted jobs complete. The scheduler decrements
 *   automatically for counter-associated jobs — including jobs still queued
 *   at shutdown, so waiters can never hang.
 * - Hybrid waiting: workers spin briefly (cache-warm handoff), then park on
 *   a condition variable with a bounded timeout — near-zero wake latency
 *   under load, no idle core burn when quiescent. (The legacy ThreadPool
 *   busy-spins forever; this scheduler is the promoted replacement.)
 *
 * CONTRACT: initialize() once before submit(); shutdown() before destruction.
 * CONTRACT: Jobs must not call shutdown() or initialize().
 * CONTRACT: A JobCounter may be waited on while jobs referencing it are
 *           queued or running; it must not be destroyed while waited on.
 * CONTRACT: submit() is thread-safe from any thread (including job threads).
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace omnicpp::core {

namespace detail {
//! Cache-friendly busy-wait primitive for the current architecture.
struct CpuHint {
  static void spin_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
  }
};
}  // namespace detail

//! Job priority classes; numerically ordered, drained lowest-value-first.
enum class JobPriority : std::uint8_t {
  render = 0,      //!< Frame-critical: must run this frame.
  submit = 1,      //!< Submission/GPU-sync assists.
  upload = 2,      //!< Streaming and resource uploads.
  background = 3,  //!< Batch/offline work.
  count = 4
};

//! Fork/join completion counter. add() before submitting the batch members;
//! each counted job is decremented exactly once by the scheduler on
//! completion. Single-batch ownership: one submission site per counter.
class JobCounter final {
public:
  void add(std::uint32_t n = 1) noexcept { count_.fetch_add(n, std::memory_order_relaxed); }

  //! True once every counted job has completed (or was dropped at shutdown).
  [[nodiscard]] bool is_complete() const noexcept {
    return count_.load(std::memory_order_acquire) == 0;
  }

  //! Hybrid wait: bounded spin for near-zero handoff latency on short
  //! batches, then yield-loop for long ones. Completion is a release-store
  //! from the decrementer, so acquire polling is sufficient.
  void wait() noexcept {
    for (int spin = 0; !is_complete(); ++spin) {
      if (spin < 4096) {
        detail::CpuHint::spin_pause();
      } else {
        std::this_thread::yield();
      }
    }
  }

private:
  friend class JobSystem;
  //! Scheduler-side completion decrement (release: publishes job effects).
  void decrement() noexcept { count_.fetch_sub(1, std::memory_order_release); }

  std::atomic<std::uint32_t> count_{0};
};

class JobSystem final {
public:
  JobSystem() = default;
  ~JobSystem() { shutdown(); }

  JobSystem(const JobSystem&) = delete;
  JobSystem& operator=(const JobSystem&) = delete;
  JobSystem(JobSystem&&) = delete;
  JobSystem& operator=(JobSystem&&) = delete;

  //! Spawn worker threads (defaults to hardware concurrency, min 1).
  [[nodiscard]] bool initialize(std::size_t thread_count = 0) {
    std::lock_guard lock(mutex_);
    if (initialized_ || shutdown_requested_) return false;
    if (thread_count == 0) {
      thread_count = std::thread::hardware_concurrency();
      if (thread_count == 0) thread_count = 1;
    }
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
    initialized_ = true;
    return true;
  }

  //! Submit one job. When counter != nullptr, it is decremented on job
  //! completion (the caller must have called counter->add() beforehand).
  //! Returns false if the system is not running.
  [[nodiscard]] bool submit(JobPriority priority, JobCounter* counter,
                            std::function<void()> job) {
    if (!job) return false;
    bool notify = false;
    {
      std::lock_guard lock(mutex_);
      if (!initialized_ || shutdown_requested_) return false;
      queues_[static_cast<std::size_t>(priority)].push_back(
          Job{static_cast<std::function<void()>&&>(job), nullptr, nullptr, counter});
      pending_.fetch_add(1, std::memory_order_acq_rel);
      // Notify only when someone is actually parked: spinning workers will
      // re-check the queues under this same mutex, so skipping the (syscall-
      // priced) condition-variable wake keeps the hot path lock+push only.
      notify = parked_ > 0;
    }
    if (notify) cv_.notify_one();
    return true;
  }

  //! Allocation-free submission of a function-pointer job. The callable must
  //! not capture state except through `arg` (which must outlive the job).
  //! Preferred on the frame path: no std::function, no heap traffic.
  [[nodiscard]] bool submit_raw(JobPriority priority, JobCounter* counter,
                                void (*fn)(void*), void* arg) {
    if (!fn) return false;
    bool notify = false;
    {
      std::lock_guard lock(mutex_);
      if (!initialized_ || shutdown_requested_) return false;
      queues_[static_cast<std::size_t>(priority)].push_back(
          Job{{}, fn, arg, counter});
      pending_.fetch_add(1, std::memory_order_acq_rel);
      notify = parked_ > 0;
    }
    if (notify) cv_.notify_one();
    return true;
  }

  //! Block until every queued and running job has completed.
  void wait_idle() noexcept {
    for (int spin = 0; pending_.load(std::memory_order_acquire) != 0; ++spin) {
      if (spin < 4096) {
        detail::CpuHint::spin_pause();
      } else {
        std::this_thread::yield();
      }
    }
  }

  //! Signal shutdown and join workers. Queued (not yet started) jobs are
  //! dropped; their counters are decremented so waiters never hang.
  void shutdown() noexcept {
    {
      std::lock_guard lock(mutex_);
      if (!initialized_ || shutdown_requested_) return;
      shutdown_requested_ = true;
      for (auto& queue : queues_) {
        for (auto& job : queue) {
          if (job.counter) job.counter->decrement();
          pending_.fetch_sub(1, std::memory_order_acq_rel);
        }
        queue.clear();
      }
    }
    cv_.notify_all();
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
    workers_.clear();
    initialized_ = false;
  }

  [[nodiscard]] std::size_t thread_count() const noexcept { return workers_.size(); }
  [[nodiscard]] bool is_running() const noexcept {
    return initialized_ && !shutdown_requested_;
  }

private:
  using FnPtr = void (*)(void*);

  struct Job {
    //! Type-erased payload. Inline capacity avoids heap allocation for
    //! function-pointer-style jobs (the common frame-path case); larger
    //! callables fall back to std::function's usual behavior.
    std::function<void()> fn;
    FnPtr fn_raw{nullptr};
    void* arg_raw{nullptr};
    JobCounter* counter{nullptr};
  };

  void worker_loop() {
    int spins = 0;
    for (;;) {
      Job job;
      {
          std::unique_lock lock(mutex_);
        while (!try_pop_locked(job)) {
          if (shutdown_requested_) return;
          if (spins < kSpinBeforePark) {
            ++spins;
            detail::CpuHint::spin_pause();
            continue;  // Re-check queues without releasing the hot path.
          }
          // Park under the mutex: the increment pairs with submit's notify
          // decision under the same lock, so a job pushed concurrently is
          // either seen by the predicate or covered by the notify — no lost
          // wakeups, only bounded-timeout re-checks.
          parked_ += 1;
          cv_.wait_for(lock, std::chrono::milliseconds(kParkTimeoutMs), [this] {
            return any_work_locked() || shutdown_requested_;
          });
          parked_ -= 1;
          // Wake (notify or timeout): loop re-checks queues and shutdown.
        }
      }
      spins = 0;
      if (job.fn_raw) {
        job.fn_raw(job.arg_raw);
        if (job.counter) job.counter->decrement();
      } else if (job.counter) {
        job.fn();
        job.counter->decrement();
      } else {
        job.fn();
      }
      pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  [[nodiscard]] bool any_work_locked() const noexcept {
    for (const auto& queue : queues_) {
      if (!queue.empty()) return true;
    }
    return false;
  }

  //! Pop the highest-priority queued job (must hold mutex_).
  [[nodiscard]] bool try_pop_locked(Job& out) {
    for (auto& queue : queues_) {
      if (!queue.empty()) {
        out = static_cast<Job&&>(queue.back());
        queue.pop_back();
        return true;
      }
    }
    return false;
  }

  static constexpr int kSpinBeforePark = 64;
  static constexpr int kParkTimeoutMs = 2;

  std::vector<std::thread> workers_;
  std::vector<Job> queues_[static_cast<std::size_t>(JobPriority::count)];
  std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t parked_{0};  //!< Parked workers; guarded by mutex_ and only
                           //!< touched while holding it (paired with the
                           //!< submit-side notify decision).
  std::atomic<std::size_t> pending_{0};
  bool initialized_{false};
  bool shutdown_requested_{false};
};

}  // namespace omnicpp::core
