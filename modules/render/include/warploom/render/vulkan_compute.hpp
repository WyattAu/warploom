#pragma once

/**
 * @file vulkan_compute.hpp
 * @brief Compute dispatch and event-based GPU synchronization.
 *
 * Complements the render graph: graphics passes stay in the render graph
 * (render-pass-structured), while async compute/dispatch work uses this
 * module. Cross-queue ordering uses events + wait/dst-stage masks so a
 * graphics submission can consume compute results with a GPU-side wait —
 * no CPU round-trip, no blocking the graphics queue on the compute queue.
 *
 * AsyncComputeQueue drives a dedicated COMPUTE-only queue when the device
 * provides one (falling back to the graphics queue otherwise): dispatches
 * are recorded into an internal command buffer, submitted with a timeline
 * semaphore signal, and graphics consumers wait on the timeline value —
 * true producer/consumer overlap with no CPU round-trip.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>

namespace warploom::render {

//! State of one image across a queue handoff (release/acquire pair).
struct QueueImageState {
  VkImage image{VK_NULL_HANDLE};
  //! Layout the image is in before the release (== after the producing work).
  VkImageLayout current_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  //! Access mask of the producing work's last touch.
  std::uint32_t last_access{0};
  //! Pipeline stage of the producing work's last touch.
  std::uint32_t last_stage{0};
  //! Layout the consumer needs.
  VkImageLayout consumer_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  //! Access mask the consumer performs.
  std::uint32_t consumer_access{0};
  //! Stage where the consumer touches the image.
  std::uint32_t consumer_stage{0};
};

//! Signal `event` after the producing work completes (record at the end of
//! the producer's command buffer, after its last write to the image).
void cmd_signal_event(VkCommandBuffer command_buffer, VkEvent event,
                      std::uint32_t src_stage);

//! Record the queue-handoff acquire: wait on `event` (raised by the producer
//! on another queue) and insert the release/acquire queue-ownership transfer
//! barrier for the image. Record at the start of the consumer's command
//! buffer, before any use of the image.
void cmd_acquire_shared_image(VkCommandBuffer command_buffer, VkEvent event,
                              const QueueImageState& state);

//! Record the producer-side half of a queue ownership transfer: RELEASE to
//! `dst_family`. Pair with cmd_acquire_shared_image_from_family on the
//! consumer side (which must record the matching dstFamilyIndex == src_family
//! acquire). Use VK_QUEUE_FAMILY_IGNORED on both sides when the image was
//! created with VK_SHARING_MODE_CONCURRENT.
void cmd_release_image_to_family(VkCommandBuffer command_buffer, VkImage image,
                                 std::uint32_t src_family, std::uint32_t dst_family,
                                 VkImageLayout layout, std::uint32_t src_access,
                                 std::uint32_t src_stage, std::uint32_t dst_stage);

//! Record the consumer-side half of a queue ownership transfer: ACQUIRE from
//! `src_family`. The dst-stage/access masks describe the consumer's first use.
void cmd_acquire_image_from_family(VkCommandBuffer command_buffer, VkImage image,
                                   std::uint32_t src_family, std::uint32_t dst_family,
                                   VkImageLayout layout, std::uint32_t dst_access,
                                   std::uint32_t src_stage, std::uint32_t dst_stage);

//=============================================================================
// Buffer queue-ownership transfer (RELEASE on producer, ACQUIRE on consumer).
// Exclusive-sharing buffers moving between the compute and graphics families
// need both halves; skip both when producer and consumer share a family.
//=============================================================================
void cmd_release_buffer_to_family(VkCommandBuffer command_buffer, VkBuffer buffer,
                                  VkDeviceSize offset, VkDeviceSize size,
                                  std::uint32_t src_family, std::uint32_t dst_family,
                                  std::uint32_t src_access, std::uint32_t src_stage,
                                  std::uint32_t dst_stage);
void cmd_acquire_buffer_from_family(VkCommandBuffer command_buffer, VkBuffer buffer,
                                    VkDeviceSize offset, VkDeviceSize size,
                                    std::uint32_t src_family, std::uint32_t dst_family,
                                    std::uint32_t dst_access, std::uint32_t src_stage,
                                    std::uint32_t dst_stage);

//! Persistent async-compute submission context: owns a command pool/buffer on
//! the compute queue's family and a timeline semaphore for GPU-side consumer
//! waits. One instance per producing thread (external sync on the queue).
class AsyncComputeQueue final {
public:
  AsyncComputeQueue() = default;
  ~AsyncComputeQueue();
  AsyncComputeQueue(const AsyncComputeQueue&) = delete;
  AsyncComputeQueue& operator=(const AsyncComputeQueue&) = delete;
  AsyncComputeQueue(AsyncComputeQueue&&) = delete;
  AsyncComputeQueue& operator=(AsyncComputeQueue&&) = delete;

  //! `compute_queue` may be a dedicated compute queue or the graphics queue
  //! (fallback). `compute_family` is its family index. Requires a device with
  //! timeline semaphores enabled (VulkanContext negotiates them).
  [[nodiscard]] ::warploom::core::Result<void> initialize(VkDevice device,
                                                       VkQueue compute_queue,
                                                       std::uint32_t compute_family);
  void cleanup() noexcept;

  [[nodiscard]] bool is_initialized() const noexcept { return device_ != nullptr; }
  [[nodiscard]] VkQueue queue() const noexcept { return queue_; }
  [[nodiscard]] std::uint32_t family() const noexcept { return family_; }
  //! Monotonic timeline value the NEXT submit() will signal.
  [[nodiscard]] std::uint64_t next_timeline_value() const noexcept { return timeline_value_ + 1; }
  //! Timeline value the most recent submit() signalled (0 before any submit).
  [[nodiscard]] std::uint64_t last_timeline_value() const noexcept { return timeline_value_; }

  //! Begin recording the frame's compute work into the internal buffer.
  //! Must be called before record()/submit(); one begin/submit cycle per frame.
  void begin();

  //! Record a raw dispatch callable into the internal buffer (between begin()
  //! and submit()). Use vkCmdBindPipeline/vkCmdDispatch/vkCmdPipelineBarrier2
  //! inside; buffer-level state resets between frames.
  void record(void (*record_fn)(VkCommandBuffer, void*), void* user_data);

  //! Submit the recorded work: signals the timeline semaphore at
  //! `next_timeline_value()` on completion and returns that value for the
  //! consumer's wait. Each submission gets its own command buffer + fence
  //! from a ring (see kInFlight), so compute for frame N+1 can be recorded
  //! and submitted while frame N is still in flight on the consumer queue.
  [[nodiscard]] ::warploom::core::Result<std::uint64_t> submit();

  //! CPU-side wait for the last submission (fence). GPU consumers normally
  //! use the timeline value instead — this is for teardown and benchmarks.
  [[nodiscard]] bool wait_done(std::uint64_t timeout_ns = 1'000'000'000ULL);

  //! Timeline value the last submit signalled (for vkCmdWaitTimelineSemaphore
  //! or VkTimelineSemaphoreSubmitInfo on the graphics side).
  [[nodiscard]] VkSemaphore timeline_semaphore() const noexcept { return timeline_; }

private:
  VkDevice device_{nullptr};
  VkQueue queue_{nullptr};
  std::uint32_t family_{0};
  VkCommandPool pool_{nullptr};
  //! One command buffer per ring slot: a re-recordable buffer must not be
  //! reset while its previous submission is in flight.
  static constexpr int kInFlight = 4;
  VkCommandBuffer buffers_[kInFlight]{};
  VkFence fences_[kInFlight]{};
  VkCommandBuffer buffer_{nullptr};      //!< Slot being recorded (== buffers_[n % kInFlight]).
  VkSemaphore timeline_{nullptr};
  std::uint64_t timeline_value_{0};
  bool recording_{false};
};

}  // namespace warploom::render

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef OMNICPP_COMPAT_RENDER_NS
#define OMNICPP_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // OMNICPP_COMPAT_RENDER_NS
