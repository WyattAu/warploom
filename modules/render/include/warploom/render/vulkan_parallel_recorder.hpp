#pragma once

/**
 * @file vulkan_parallel_recorder.hpp
 * @brief Multithreaded secondary-command-buffer recording.
 *
 * Splits a viewport into horizontal bands and records one secondary command
 * buffer per band on worker threads (command pools are thread-local, one per
 * thread, per Vulkan's external synchronization requirements). The primary
 * buffer then executes the secondaries inside the render pass.
 *
 * Execution backend: when a JobSystem is attached (set_job_system), band
 * recording forks onto the system's persistent workers — no thread creation
 * and no heap allocation per frame. Otherwise ad-hoc threads are spawned
 * (original behavior, kept as a fallback).
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <functional>
#include <vector>

// S2-B: this header used to forward-declare `::warploom::core::JobSystem`.
// A real member declaration hides the compat using-directive's complete
// type (warploom::core::JobSystem), leaving consumers with an incomplete
// class — so the declaration is superseded by the real core header
// (docs/warploom-core-plan.md, phase B).
#include "warploom/core/job_system.hpp"

namespace warploom::render {

//! Per-band record job: receives the secondary buffer and the band's scissor.
using RecordBandFn = std::function<void(VkCommandBuffer, VkRect2D)>;

class VulkanParallelRecorder final {
public:
  VulkanParallelRecorder() = default;
  ~VulkanParallelRecorder();
  VulkanParallelRecorder(const VulkanParallelRecorder&) = delete;
  VulkanParallelRecorder& operator=(const VulkanParallelRecorder&) = delete;
  VulkanParallelRecorder(VulkanParallelRecorder&&) = delete;
  VulkanParallelRecorder& operator=(VulkanParallelRecorder&&) = delete;

  [[nodiscard]] ::warploom::core::Result<void> initialize(
      VkDevice device, std::uint32_t queue_family_index,
      std::uint32_t band_count);
  void cleanup() noexcept;

  [[nodiscard]] bool is_initialized() const noexcept { return device_ != VK_NULL_HANDLE; }
  [[nodiscard]] std::uint32_t band_count() const noexcept { return band_count_; }

  /**
   * @brief Records `band_count` secondary buffers in parallel.
   *
   * @param width, height      Full-frame extents (bands split height evenly).
   * @param record_band        Called once per band on a worker thread. It must
   *                           only record into the given secondary buffer.
   * @param compatible_pass    Render pass the secondaries will execute in
   *                           (required for RENDER_PASS_CONTINUE inheritance).
   * @param framebuffer        Framebuffer being rendered to (0 allowed).
   * @return Secondary buffers in band order, ready for
   *         vkCmdExecuteCommands inside an active render pass.
   */
  [[nodiscard]] ::warploom::core::Result<std::vector<VkCommandBuffer>> record_parallel(
      std::uint32_t width, std::uint32_t height, const RecordBandFn& record_band,
      VkRenderPass compatible_pass, VkFramebuffer framebuffer = VK_NULL_HANDLE);

  /**
   * @brief Attach a job system for persistent-worker band recording.
   *
   * When set and running, record_parallel() forks band jobs onto the system's
   * workers (no per-frame thread creation, no per-frame allocation). When
   * null or stopped, ad-hoc threads are used (original behavior).
   */
  void set_job_system(::warploom::core::JobSystem* system) noexcept { job_system_ = system; }

  //! Reset all secondary buffers for the next frame (call outside render pass).
  [[nodiscard]] ::warploom::core::Result<void> reset();

  //! Per-band job payload (reused across frames; sized at initialize()).
  struct BandJob {
    const RecordBandFn* fn{nullptr};
    VkCommandBuffer buffer{VK_NULL_HANDLE};
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    std::uint32_t band_offset{0};
    std::uint32_t band_height{0};
    std::uint32_t width{0};
    std::uint32_t extent_height{0};
  };

private:
  struct Band {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer buffer{VK_NULL_HANDLE};
  };

  VkDevice device_{VK_NULL_HANDLE};
  std::uint32_t queue_family_index_{0};
  std::vector<Band> bands_;
  std::vector<BandJob> band_jobs_;
  std::uint32_t band_count_{0};
  ::warploom::core::JobSystem* job_system_{nullptr};
};

} // namespace warploom::render

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef WARPLOOM_COMPAT_RENDER_NS
#define WARPLOOM_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // WARPLOOM_COMPAT_RENDER_NS
