#pragma once

/**
 * @file vulkan_path_tracer.hpp
 * @brief Ownable deterministic path tracer: accumulation image, acceleration
 *        structures, SBT, and the trace dispatch.
 *
 * Extracted from tests/unit/test_path_tracing*.cpp (R1a,
 * docs/roadmap.md). Until now the path tracer existed only as ~300 lines of
 * setup inside two test files: an rgba32f accumulation image, a params
 * buffer, an instance buffer, BLASes, a TLAS, an SBT, a descriptor set, and a
 * `record_round(clear_first)` that clears once and adds per frame. That is a
 * rendering feature living in a test fixture, which is the same defect class
 * as the compose chain living inside VulkanRenderer (B3b): it cannot be
 * reached by an application, so the 0.1 gate's signature feature (R1) has no
 * engine API at all.
 *
 * The contract this object keeps, because two tests mirror it on the CPU with
 * `EXPECT_FLOAT_EQ` against exact fp32 arithmetic:
 *
 *  - **Sample seeding is a pure function of (pixel, frame).** The shaders
 *    seed `uint rng = pix.x + size.x * (pix.y + size.y * frame)`, so frame N
 *    contributes the same samples for the same pixel no matter what else ran
 *    in between. That is what makes accumulation order-independent, and it is
 *    the whole reason this can be a product feature rather than a
 *    nondeterministic renderer (docs/research/loop4-6-transient-determinism-pt.md).
 *  - **Clear once, add per frame.** `record_round(cmd, clear_first=true)`
 *    zeroes the accumulation; subsequent rounds add. The mean is
 *    `accum / samples`, computed by `read_mean()`.
 *  - **The accumulation image lives in GENERAL layout.** Storage images
 *    require it; the barriers around the clear and the trace are explicit
 *    here rather than derived by the render graph, because this pass records
 *    outside any graph (the same rule as VulkanRtPipeline and the AS
 *    builders: record only, the caller submits).
 *
 * Nothing here reads wall-clock time, iterates an unordered container, or
 * depends on submission order beyond the barriers recorded below.
 */

#include <warploom/core/deterministic_runtime.hpp>
#include <warploom/render/vulkan_acceleration_structure.hpp>
#include <warploom/render/vulkan_memory_allocator.hpp>
#include <warploom/render/vulkan_rt_pipeline.hpp>
#include <warploom/render/vulkan_types.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace warploom::render {

class VulkanDescriptorManager;

//! One triangle mesh to trace against. The geometry buffer stays CALLER-owned:
//! the address is captured, not the lifetime, exactly like BlasBuildInput.
struct PathTracerMesh {
  std::uint64_t vertex_buffer_address{0};
  std::uint32_t triangle_count{0};
  std::uint32_t max_vertex{0};
};

//! Per-instance data, in the 2 x vec4 layout the pt_real shaders read:
//! [center.xyz, _], [albedo.rgb, _], plus the TLAS placement.
struct PathTracerInstance {
  std::array<float, 4> center{{0.0F, 0.0F, 0.0F, 0.0F}};
  std::array<float, 4> albedo{{0.8F, 0.8F, 0.8F, 0.0F}};
  //! Row-major 3x4 transform (Vulkan's instance layout); see TlasInstance.
  //! Defaults to identity via identity_transform() — deliberately not an
  //! inline default, because a zero matrix is a valid-looking value that
  //! silently collapses every instance to a point.
  std::array<float, 12> transform{{1.0F, 0.0F, 0.0F, 0.0F,  //
                                   0.0F, 1.0F, 0.0F, 0.0F,  //
                                   0.0F, 0.0F, 1.0F, 0.0F}};
  std::uint32_t custom_index{0};

  //! Pure translation, the only placement the box-room tests need.
  [[nodiscard]] static PathTracerInstance translated(
      const std::array<float, 3>& at, float albedo) {
    PathTracerInstance inst{};
    inst.center = {at[0], at[1], at[2], 0.0F};
    inst.albedo = {albedo, albedo, albedo, 0.0F};
    inst.transform[3] = at[0];
    inst.transform[7] = at[1];
    inst.transform[11] = at[2];
    return inst;
  }
};

//! Orthonormal camera basis + position + the frame index the shaders seed
//! their PCG stream with. All vectors point where the camera looks/looks
//! from; the shaders build the ray direction from them.
//!
//! tan_half_fov/aspect scale the [-1, 1] pixel UV into ray angles, exactly
//! like the raster sky pass (ndc.x*tan*aspect, ndc.y*tan). The historical
//! default (1, 1) preserves the old hardcoded 90-degree mapping bit-for-bit;
//! anything else must be set deliberately — the tracer cannot guess the
//! raster field of view, and a silent 90-vs-60 mismatch is how the PT image
//! used to disagree with the raster one about where everything is.
struct PathTracerCamera {
  std::array<float, 3> position{{0.0F, 0.0F, 0.0F}};
  std::array<float, 3> right{{1.0F, 0.0F, 0.0F}};
  std::array<float, 3> up{{0.0F, 1.0F, 0.0F}};
  std::array<float, 3> forward{{0.0F, 0.0F, 1.0F}};
  float tan_half_fov{1.0F};
  float aspect{1.0F};
};

//! Analytic-sky parameters for the miss branch. Mode 0 (default) keeps the
//! legacy constant-white sky every existing probe pins bit-exactly; mode 1
//! evaluates the shared sky_eval.glsl model (the same definition the raster
//! sky pass shades) with these params. `origin` must be surface-anchored
//! (see sky_eval.glsl's convention); the default is the raster sky test's
//! anchor, so headless parity checks need only set mode = 1. Field order
//! mirrors the raster SkyParams UBO so one host fill serves both paths.
struct PathTracerSky {
  std::uint32_t mode{0};  // 0 = legacy white, 1 = analytic (dims.w in GLSL)
  std::array<float, 4> sun_direction{{0.0F, 0.7F, -0.7F, 0.0F}};
  std::array<float, 4> rayleigh{{5.8e-6F, 13.5e-6F, 33.1e-6F, 0.0F}};
  std::array<float, 4> mie{{21e-6F, 0.76F, 0.0F, 0.0F}};
  std::array<float, 4> misc{{1.0F, 0.0F, 0.0F, 0.0F}};  // x = exposure
  std::array<float, 4> origin{{0.0F, 6360e3F + 100.0F, 0.0F, 0.0F}};
};

struct PathTracerConfig {
  //! Directory holding pt_real.rgen / pt_real.rmiss / pt_real.rchit (.spv).
  std::string shader_dir{};
  std::uint32_t width{0};
  std::uint32_t height{0};
  //! Path length; written to Params::dims.z, which the rgen loop bounds.
  std::uint32_t max_bounces{4};
  //! Meshes to build BLASes for at create() time. Geometry is static per
  //! tracer instance; a moving scene re-creates rather than refitting.
  std::vector<PathTracerMesh> meshes{};
  //! Initial instance list (one BLAS per mesh, identity transform).
  std::vector<PathTracerInstance> instances{};
  //! Sky for the miss branch. Default mode 0 = legacy constant white, so
  //! every existing tracer behaves exactly as before unless opted in.
  PathTracerSky sky{};
};

//! Owns a complete, deterministic path-tracing setup. See the file comment
//! for the accumulation and seeding contract.
class VulkanPathTracer final {
 public:
  VulkanPathTracer() = default;
  ~VulkanPathTracer();
  VulkanPathTracer(const VulkanPathTracer&) = delete;
  VulkanPathTracer& operator=(const VulkanPathTracer&) = delete;
  VulkanPathTracer(VulkanPathTracer&&) = delete;
  VulkanPathTracer& operator=(VulkanPathTracer&&) = delete;

  //! Builds everything: accumulation image (sized from the config), params and
  //! instance buffers, one BLAS per mesh, the TLAS, the SBT, the descriptor
  //! set, and the RT pipeline. Geometry builds are recorded on a private
  //! command buffer and submitted here, because a BLAS must exist before the
  //! descriptor can name it. Refuses (cleanly) on a device without ray
  //! tracing, a shader dir missing a module, or an empty mesh list.
  [[nodiscard]] ::warploom::core::Result<void> create(
      VkPhysicalDevice physical_device, VkDevice device, VkQueue graphics_queue,
      std::uint32_t graphics_family, VulkanMemoryAllocator& allocator,
      const PathTracerConfig& config);

  //! Records one accumulation round. `clear_first` zeroes the accumulation
  //! first; the caller passes true on the first round of a sequence and false
  //! thereafter. `frame` is the value written to Params::cam_pos_frame.w and
  //! therefore part of the sample seed: replaying the same frame index
  //! replays the same samples. Not const because it advances sample_count().
  void record_round(VkCommandBuffer command_buffer, std::uint32_t frame,
                    bool clear_first) noexcept;

  //! Host-visible camera upload. Takes effect on the next recorded round.
  void set_camera(const PathTracerCamera& camera) noexcept;

  //! Accumulation rounds recorded since the last clear. `read_mean` divides by
  //! this, so it is the count the pixels on screen correspond to.
  [[nodiscard]] std::uint32_t sample_count() const noexcept { return samples_; }
  //! Forget the accumulation: the next round with clear_first=true starts a
  //! new sequence and the count returns to 1.
  void reset_accumulation() noexcept { samples_ = 0U; }

  //! Copy the accumulation image back and divide by sample_count(). Allocates
  //! a transient host-visible buffer per call and submits on the tracer's own
  //! queue; this is a host-side inspection path (tests, capture, "read back
  //! the converged mean"), never the per-frame one.
  [[nodiscard]] ::warploom::core::Result<std::vector<float>> read_mean() const;

  //! Copy the second-moment image back and divide by sample_count(): per
  //! texel E[X^2] (rgb; w is 0). Together with read_mean this is the full
  //! SVGF input pair (first + second moments) and the variance signal an
  //! adaptive sampler needs — tracked deterministically, because the squares
  //! are accumulated with the same pure (pixel, frame) seeding as the sums.
  //! Same host-side inspection contract as read_mean.
  [[nodiscard]] ::warploom::core::Result<std::vector<float>> read_second_moment() const;

  void destroy() noexcept;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  //! True when create() refused permanently; do not retry every frame.
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] VkImage accum_image() const noexcept { return accum_image_; }
  [[nodiscard]] VkImageView accum_view() const noexcept { return accum_view_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

 private:
  //! Releases every handle in declaration-safe order. VulkanPipeline and the
  //! AS builders are inert in their destructors, so this is explicit.
  void destroy_resources() noexcept;

  //! Shared image readback behind read_mean / read_second_moment: copies
  //! `image` back WITHOUT dividing. Returns the raw per-texel sums.
  [[nodiscard]] ::warploom::core::Result<std::vector<float>> read_image_sum(
      VkImage image) const;

  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphics_queue_{VK_NULL_HANDLE};
  std::uint32_t graphics_family_{0};
  VulkanMemoryAllocator* allocator_{nullptr};
  std::string shader_dir_{};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  std::uint32_t max_bounces_{4};
  std::uint32_t samples_{0};

  VkImage accum_image_{VK_NULL_HANDLE};
  VkImageView accum_view_{VK_NULL_HANDLE};
  Allocation accum_allocation_{};

  //! Second-moment image (rgba32f storage): per-texel sum of squared
  //! radiance, cleared and added alongside the accumulation with identical
  //! barriers. Same extent/format/usage so the readback path is shared.
  VkImage moment_image_{VK_NULL_HANDLE};
  VkImageView moment_view_{VK_NULL_HANDLE};
  Allocation moment_allocation_{};

  // Host-visible, host-coherent; mirrored into these by set_camera().
  Allocation params_allocation_{};
  Allocation instance_allocation_{};
  std::uint32_t instance_capacity_{0};

  VulkanAccelerationStructureBuilder as_builder_{};
  std::vector<BottomLevelAS> blas_list_{};
  TopLevelAS tlas_{};
  VulkanScratchPool scratch_{};
  std::uint64_t scratch_address_{0};

  std::unique_ptr<VulkanDescriptorManager> descriptor_manager_{};
  VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkDescriptorSet descriptor_set_{VK_NULL_HANDLE};
  VulkanRtPipeline rt_pipeline_{};
  Allocation sbt_allocation_{};

  bool ready_{false};
  bool failed_{false};
};

}  // namespace warploom::render

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