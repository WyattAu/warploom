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
#include <warploom/render/vulkan_pipeline.hpp>
#include <warploom/render/vulkan_rt_pipeline.hpp>
#include <warploom/render/vulkan_types.hpp>

#include <algorithm>
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
//! The albedo words are bound but currently unread (grey scene, loop25).
struct PathTracerInstance {
  std::array<float, 4> center{{0.0F, 0.0F, 0.0F, 0.0F}};
  std::array<float, 4> albedo{{0.8F, 0.8F, 0.8F, 0.0F}};
  //! Which mesh (BLAS) this instance places. 0xFFFFFFFF (the default) means
  //! identity: instance i places mesh i, which is exactly the old 1:1
  //! contract, so every existing config behaves bit-for-bit as before. An
  //! explicit index shares one BLAS across instances (create() refuses a
  //! value at or above meshes.size()).
  std::uint32_t mesh_index{0xFFFFFFFFU};
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

//! Adaptive mask from a variance signal (T2 build). Pure CPU function of
//! (mean, second-moment): per-texel luma variance v = max(0, E2_luma -
//! m_luma^2). Selection is by RANK: the top ceil(top_fraction * texels)
//! texels by variance (stable sort desc, ties broken by texel index, so
//! the set is a pure function of the inputs and the budget is exact even
//! when most texels tie at zero -- the common case, since noise
//! concentrates at discontinuities). threshold is the smallest selected
//! variance (diagnostic only). top_fraction in (0,1]; 0 or empty input
//! selects nothing. The caller uploads the mask and enables masked
//! tracing; masked-out texels keep their sums while masked-in texels
//! accumulate extra rounds.
struct AdaptiveMask {
  std::vector<std::uint32_t> mask{};  //!< 1 = needs extra samples, 0 = hold
  float threshold{0.0F};              //!< variance cut that produced the mask
  std::size_t selected{0};            //!< popcount(mask)
};
[[nodiscard]] inline AdaptiveMask adaptive_mask_from_variance(
    const std::vector<float>& mean, const std::vector<float>& second_moment,
    double top_fraction) noexcept {
  AdaptiveMask out{};
  const std::size_t texels = mean.size() / 4U;
  if (texels == 0U || second_moment.size() != mean.size() ||
      top_fraction <= 0.0) {
    out.mask.assign(texels, 0U);
    return out;
  }
  const double frac = top_fraction > 1.0 ? 1.0 : top_fraction;
  std::vector<float> var(texels, 0.0F);
  for (std::size_t t = 0; t < texels; ++t) {
    double ml = 0.0;
    double el = 0.0;
    constexpr double w[3] = {0.2126, 0.7152, 0.0722};
    for (std::size_t c = 0; c < 3U; ++c) {
      const double m = static_cast<double>(mean[t * 4U + c]);
      const double e2 = static_cast<double>(second_moment[t * 4U + c]);
      ml += w[c] * m;
      el += w[c] * e2;
    }
    double v = el - ml * ml;
    if (!(v > 0.0)) v = 0.0;
    var[t] = static_cast<float>(v);
  }
  std::vector<std::size_t> order(texels);
  for (std::size_t t = 0; t < texels; ++t) order[t] = t;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return var[a] > var[b]; });
  std::size_t want =
      static_cast<std::size_t>(frac * static_cast<double>(texels));
  if (want == 0U) want = 1U;
  if (want > texels) want = texels;
  out.threshold = var[order[want - 1U]];
  out.mask.assign(texels, 0U);
  for (std::size_t k = 0; k < want; ++k) out.mask[order[k]] = 1U;
  out.selected = want;
  return out;
}

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

//! Diagnostic normal visualization. When true, create() writes 2 into
//! Params::dims.w and the raygen encodes the first-bounce shading normal as
//! color instead of shading: radiance = albedo * (n * 0.5 + 0.5). This pins
//! normal orientation with no Monte Carlo tolerance involved (a sign error
//! reads the inward color). Mode 2 is a diagnostic encoding, not a sky;
//! it follows the mode-0 precedent (constant white kept for the probes).
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
  //! One BLAS per mesh; instances select among them via mesh_index, so a
  //! repeated prop (two cubes) costs one BLAS and two TLAS entries.
  std::vector<PathTracerMesh> meshes{};
  //! Instance list (TLAS entries): placement + mesh selection per entry.
  std::vector<PathTracerInstance> instances{};
  //! Sky for the miss branch. Default mode 0 = legacy constant white, so
  //! every existing tracer behaves exactly as before unless opted in.
  PathTracerSky sky{};
  //! Diagnostic: encode first-bounce normals as color (dims.w = 2).
  //! Default false, so every existing tracer behaves exactly as before.
  bool visualize_normals{false};
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
  //! No-ops (without touching the count) once a set_sample_target() is
  //! reached, unless clear_first restarts the sequence.
  //!
  //! Live-PT stepping discipline: when set_camera() detected a real camera
  //! change since the last round, THIS round restarts the sequence even
  //! with clear_first=false (both images cleared, count back to 1) and
  //! bypasses the sample-target hold -- a held-complete image for a new
  //! pose is exactly the smear this rule makes unrepresentable. The restart
  //! is a pure function of the call sequence, so replay is unaffected:
  //! the same calls produce the same images.
  void record_round(VkCommandBuffer command_buffer, std::uint32_t frame,
                    bool clear_first) noexcept;

  //! Host-visible camera upload. Takes effect on the next recorded round.
  //! Change-detecting, not call-detecting: re-uploading identical values
  //! (the viewport's PT camera hold, every frame) is a no-op for the
  //! sequence, while any bitwise-differing pose/fov marks it stale and the
  //! next record_round() restarts it. Comparison is field-wise == (so
  //! -0.0 vs +0.0 does not restart); the frame seed word is excluded --
  //! it advances every round by construction.
  void set_camera(const PathTracerCamera& camera) noexcept;

  //! Accumulation rounds recorded since the last clear. `read_mean` divides by
  //! this, so it is the count the pixels on screen correspond to.
  [[nodiscard]] std::uint32_t sample_count() const noexcept { return samples_; }
  //! Stepped-PT target (R1): once sample_count() reaches the target,
  //! record_round() holds (no-ops unless clear_first) instead of adding
  //! more samples, so "accumulate K frames" is a deterministic operation
  //! rather than "as many as the wall clock allowed". 0 (default) means
  //! unbounded: every round records, exactly the historical behaviour.
  //! The target survives reset_accumulation (it is a setting, not state);
  //! clear_first always restarts the sequence even while held.
  void set_sample_target(std::uint32_t target) noexcept {
    sample_target_ = target;
  }
  [[nodiscard]] std::uint32_t sample_target() const noexcept {
    return sample_target_;
  }
  //! Forget the accumulation: the next round with clear_first=true starts a
  //! new sequence and the count returns to 1. Also clears a pending
  //! camera-move restart (there is no sequence left to be stale for).
  void reset_accumulation() noexcept {
    samples_ = 0U;
    camera_dirty_ = false;
  }

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

  //! Adaptive sampling (T2 build): masked re-trace with per-pixel counts.
  //!
  //! Uniform rounds maintain a per-texel count image alongside the sums
  //! (always incremented when the sums are, so the uniform path is
  //! unchanged and read_mean keeps dividing by sample_count()).
  //! After computing a mask with adaptive_mask_from_variance(), the caller
  //! uploads it and enables masking: record_round() then skips masked-out
  //! texels (no sum, no count increment) while masked-in texels accumulate
  //! normally. read_adaptive_mean() divides each texel by its own count,
  //! so held texels keep their uniform mean bit-exactly. The mask is a
  //! pure function of accumulation state; replaying the same calls
  //! replays the same image.
  void set_adaptive_enabled(bool enabled) noexcept;
  [[nodiscard]] bool adaptive_enabled() const noexcept {
    return adaptive_enabled_;
  }
  //! Uploads a texel mask (size must equal width*height, values 0/1).
  //! Returns false and leaves the device mask untouched on size mismatch.
  [[nodiscard]] bool upload_adaptive_mask(
      const std::vector<std::uint32_t>& mask) noexcept;
  //! Per-texel sample counts as raw uints (no divide). Same inspection
  //! contract as read_mean (transient host buffer, private submit).
  [[nodiscard]] ::warploom::core::Result<std::vector<std::uint32_t>>
  read_per_pixel_counts() const;
  //! Per-texel mean: sum[i] / max(count[i],1). Bit-identical to read_mean()
  //! on texels whose count equals sample_count().
  [[nodiscard]] ::warploom::core::Result<std::vector<float>> read_adaptive_mean() const;

  void destroy() noexcept;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  //! True when create() refused permanently; do not retry every frame.
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] VkImage accum_image() const noexcept { return accum_image_; }
  [[nodiscard]] VkImageView accum_view() const noexcept { return accum_view_; }
  //! Second-moment image + view (T2 denoiser spike): the same storage-image
  //! contract as the accumulation (GENERAL, cleared/added alongside it),
  //! so a variance-guided filter can bind the pair without a recreate.
  [[nodiscard]] VkImage moment_image() const noexcept { return moment_image_; }
  [[nodiscard]] VkImageView moment_view() const noexcept { return moment_view_; }
  //! First-hit G-buffer image + view (T2 denoiser input): rgba32f storage,
  //! rgb = first-bounce world normal, a = hit t (-1 on miss). Overwritten
  //! (not accumulated) every unmasked round, cleared alongside the sums.
  //! Consumed by record_denoise() below (the render-path filter).
  [[nodiscard]] VkImage gbuffer_image() const noexcept { return gbuffer_image_; }
  [[nodiscard]] VkImageView gbuffer_view() const noexcept { return gbuffer_view_; }
  //! Render-path denoiser (T2 consumer): single-iteration edge-stopped
  //! a-trous over (sums, second moment, G-buffer) with per-texel divisors
  //! from the count image, dispatched by record_denoise(). On a uniform
  //! accumulation the output is bit-identical to the
  //! denoise_edge_atrous spike shader (pinned by
  //! denoise_render_path_uniform_parity); under adaptive masking the
  //! per-texel counts keep held texels exact. The output holds a filtered
  //! MEAN (not sums), so a consumer tonemaps it with inverse_samples = 1.
  //! Filter widths are the measured-best loop30 values (k=2.0,
  //! spatial=1.5, normal=0.1, depth=1.0, stride=1); there is no tuning
  //! API because the parameter selection is a measured result, not a
  //! caller preference.
  void set_denoise_enabled(bool enabled) noexcept {
    denoise_enabled_ = enabled;
  }
  [[nodiscard]] bool denoise_enabled() const noexcept {
    return denoise_enabled_;
  }
  //! Records one filter dispatch over the CURRENT accumulation state.
  //! The caller records this after record_round() in the same command
  //! buffer (the input barrier orders the round's trace writes); the
  //! output barrier leaves the denoised image readable by a shader read
  //! (tonemap) and by transfer (read_denoised). No-op when not ready.
  //! Pure function of (sums, moment, gbuffer, counts): re-dispatching over
  //! unchanged inputs reproduces the image bit-exactly.
  void record_denoise(VkCommandBuffer command_buffer) noexcept;
  [[nodiscard]] VkImage denoised_image() const noexcept {
    return denoise_image_;
  }
  [[nodiscard]] VkImageView denoised_view() const noexcept {
    return denoise_view_;
  }
  //! Host readback of the denoised MEAN (no divide: the image already
  //! holds a filtered mean). Same inspection contract as read_mean.
  [[nodiscard]] ::warploom::core::Result<std::vector<float>> read_denoised() const;
  //! Raw G-buffer texels (no divide: the image is overwritten, not summed).
  //! Same host-side inspection contract as read_mean.
  [[nodiscard]] ::warploom::core::Result<std::vector<float>> read_gbuffer() const;
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

  [[maybe_unused]] VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  [[maybe_unused]] VkDevice device_{VK_NULL_HANDLE};
  [[maybe_unused]] VkQueue graphics_queue_{VK_NULL_HANDLE};
  [[maybe_unused]] std::uint32_t graphics_family_{0};
  [[maybe_unused]] VulkanMemoryAllocator* allocator_{nullptr};
  std::string shader_dir_{};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  [[maybe_unused]] std::uint32_t max_bounces_{4};
  std::uint32_t samples_{0};
  //! Live-PT stepping state: set by set_camera() on a real pose/fov
  //! change, consumed (restart + clear) by the next record_round().
  //! False by default, so every existing caller behaves exactly as before.
  bool camera_dirty_{false};
  //! R1 stepped-PT target, 0 = unbounded. See set_sample_target().
  std::uint32_t sample_target_{0};

  VkImage accum_image_{VK_NULL_HANDLE};
  VkImageView accum_view_{VK_NULL_HANDLE};
  Allocation accum_allocation_{};

  //! Second-moment image (rgba32f storage): per-texel sum of squared
  //! radiance, cleared and added alongside the accumulation with identical
  //! barriers. Same extent/format/usage so the readback path is shared.
  VkImage moment_image_{VK_NULL_HANDLE};
  VkImageView moment_view_{VK_NULL_HANDLE};
  Allocation moment_allocation_{};

  //! First-hit G-buffer (rgba32f storage): same extent/format/usage as the
  //! accumulation so the readback path is shared. Overwritten per round,
  //! not added -- barriers and clears travel with the other three images.
  VkImage gbuffer_image_{VK_NULL_HANDLE};
  VkImageView gbuffer_view_{VK_NULL_HANDLE};
  Allocation gbuffer_allocation_{};

  //! Denoised-mean image + view (T2 render-path consumer): rgba32f
  //! storage, same extent as the accumulation. Overwritten per dispatch,
  //! never accumulated. Built in create() alongside the stack (sized in
  //! pixels, so resize/recreate rebuilds it with the tracer).
  VkImage denoise_image_{VK_NULL_HANDLE};
  VkImageView denoise_view_{VK_NULL_HANDLE};
  Allocation denoise_allocation_{};
  //! Denoise compute pipeline + descriptor set (bindings 0-3 mirror the
  //! spike shader's, 4 = the count image for per-texel divisors).
  VulkanPipeline denoise_pipeline_{};
  [[maybe_unused]] VkDescriptorSetLayout denoise_layout_{VK_NULL_HANDLE};
  [[maybe_unused]] VkDescriptorSet denoise_set_{VK_NULL_HANDLE};
  //! First record_denoise() lays the output out of UNDEFINED; later ones
  //! find it GENERAL. Pure function of the call sequence, so replay is
  //! unaffected. Reset by create()/destroy (new image, new layout).
  bool denoise_laid_out_{false};
  //! Render-path filter toggle. Default off: every existing caller
  //! tonemaps the raw accumulation exactly as before.
  bool denoise_enabled_{false};

  //! Per-texel sample counts (r32ui storage): incremented exactly when the
  //! sums are, cleared alongside them. Uniform rounds leave every texel at
  //! sample_count(); masked rounds leave held texels behind, which is what
  //! makes read_adaptive_mean() exact per texel.
  [[maybe_unused]] VkImage count_image_{VK_NULL_HANDLE};
  [[maybe_unused]] VkImageView count_view_{VK_NULL_HANDLE};
  Allocation count_allocation_{};
  //! Adaptive mask (storage buffer, one uint per texel, host-visible).
  //! Zero-initialized at create() (mask-all-hold would freeze the image,
  //! so masking only takes effect after an explicit upload + enable).
  Allocation mask_allocation_{};
  bool adaptive_enabled_{false};

  // Host-visible, host-coherent; mirrored into these by set_camera().
  Allocation params_allocation_{};
  Allocation instance_allocation_{};
  [[maybe_unused]] std::uint32_t instance_capacity_{0};

  [[maybe_unused]] VulkanAccelerationStructureBuilder as_builder_{};
  std::vector<BottomLevelAS> blas_list_{};
  TopLevelAS tlas_{};
  VulkanScratchPool scratch_{};
  [[maybe_unused]] std::uint64_t scratch_address_{0};

  std::unique_ptr<VulkanDescriptorManager> descriptor_manager_{};
  [[maybe_unused]] VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
  [[maybe_unused]] VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  [[maybe_unused]] VkDescriptorSet descriptor_set_{VK_NULL_HANDLE};
  VulkanRtPipeline rt_pipeline_{};
  Allocation sbt_allocation_{};

  bool ready_{false};
  bool failed_{false};
};

}  // namespace warploom::render