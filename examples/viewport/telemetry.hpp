//! @file telemetry.hpp
//! @brief Viewport observability: JSONL frame telemetry and GPU-side frame
//!        capture for deterministic, externally validated runs.
//!
//! Two facilities:
//!  1. TelemetryLogger — one JSON line per frame (frame index, sim time,
//!     animation time, camera, object counts, timing) plus a run manifest
//!     header, so `scripts/analyze_telemetry.py` can assert scene/animation
//!     properties from a run's log alone.
//!  2. FrameCapture — every N frames the viewport re-records the *same pure
//!     scene* into capture-owned offscreen targets through its own render
//!     pass (finalLayout TRANSFER_SRC_OPTIMAL), copy commands pull color and
//!     depth to host-visible buffers, and a PPM + raw depth dump is written
//!     next to the telemetry log. This is GPU truth about what was rendered —
//!     independent of any X11/window-manager screenshot path.
//!
//! Run control (all env, all optional):
//!   OMNICPP_TELEMETRY_DIR   output dir for log + captures (default: none ->
//!                           telemetry disabled)
//!   OMNICPP_MAX_FRAMES      exit after N frames (deterministic runs)
//!   OMNICPP_FIXED_DT        override the per-frame dt in seconds
//!   OMNICPP_CAPTURE_EVERY   capture a frame every N frames (0 = off)
//!   OMNICPP_CAPTURE_LIMIT   stop capturing after K captures (default 8)
//!   OMNICPP_START_TIME      initial sim time in seconds (default 0)
//!   OMNICPP_CAMERA_RADIUS   orbit radius override (nan = scene default)
//!   OMNICPP_CAMERA_HEIGHT   orbit height override (nan = scene default)
//!   OMNICPP_MODEL           skeletal document name in the asset dir
//!                           (default: mannequin; e.g. CesiumMan)
//!   OMNICPP_CROSSFADE       walk<->idle cross-fade period in seconds
//!                           (0 = off; cycles walk -> fade -> idle -> fade)

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include <vulkan/vulkan.h>

#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_scene.hpp"

// ============================================================================
// S5-B phase 2: WARPLOOM_* env names are primary; the legacy OMNICPP_*
// names keep working until post-0.1 (docs/warploom-identity-plan.md).
// ============================================================================
inline const char* warploom_env(const char* primary, const char* legacy) {
  if (const char* v = std::getenv(primary)) return v;
  return std::getenv(legacy);
}

// ============================================================================
// Where the compiled SPIR-V lives, in priority order:
//   1. WARPLOOM_SHADER_DIR (or legacy OMNICPP_SHADER_DIR) — the override
//      used by packaging and by the scenario/benchmark scripts.
//   2. The directory the build baked in (WARPLOOM_SHADER_DIR compile
//      definition from cmake/Shaders.cmake). This is what makes a fresh
//      clone "just work": the app no longer needs a hand-exported
//      environment variable to find its own shaders.
//   3. The installed data directory, for relocatable installs.
//   4. The in-tree source directory, for running from a source checkout.
// The result is checked for a file that only exists once compilation has
// actually happened, so a misconfigured build fails with a message naming
// the fix rather than a bare "failed to load shader stage".
// ============================================================================
inline const char* warploom_shader_dir() {
#ifdef WARPLOOM_SHADER_DIR
  if (const char* env = warploom_env("WARPLOOM_SHADER_DIR", "OMNICPP_SHADER_DIR")) {
    return env;
  }
  if (std::filesystem::exists(std::string(WARPLOOM_SHADER_DIR) + "/pbr_scene.frag.spv")) {
    return WARPLOOM_SHADER_DIR;
  }
#endif
  static const std::string installed =
      std::string(WARPLOOM_INSTALL_SHADERS_DIR) + "/pbr_scene.frag.spv";
  if (std::filesystem::exists(installed)) return WARPLOOM_INSTALL_SHADERS_DIR;
  return "assets/shaders";
}


namespace viewport {

// ============================================================================
// Environment-run configuration
// ============================================================================

struct RunConfig {
  std::string telemetry_dir;     //!< empty = telemetry disabled
  std::uint32_t max_frames{0};   //!< 0 = run until window close
  float fixed_dt{1.0f / 60.0f};  //!< per-frame simulation step
  std::uint32_t capture_every{0};
  std::uint32_t capture_limit{8};
  float start_time{0.0f};
  float camera_radius{std::nanf("")};  //!< nan = scene default
  float camera_height{std::nanf("")};
  float crossfade_period{0.0f};       //!< 0 = walk only

  [[nodiscard]] static RunConfig from_environment() {
    RunConfig config;
    if (const char* dir = warploom_env("WARPLOOM_TELEMETRY_DIR", "OMNICPP_TELEMETRY_DIR")) {
      config.telemetry_dir = dir;
    }
    if (const char* frames = warploom_env("WARPLOOM_MAX_FRAMES", "OMNICPP_MAX_FRAMES")) {
      config.max_frames = static_cast<std::uint32_t>(std::atoi(frames));
    }
    if (const char* dt = warploom_env("WARPLOOM_FIXED_DT", "OMNICPP_FIXED_DT")) {
      const float parsed = static_cast<float>(std::atof(dt));
      if (parsed > 0.0f) config.fixed_dt = parsed;
    }
    if (const char* every = warploom_env("WARPLOOM_CAPTURE_EVERY", "OMNICPP_CAPTURE_EVERY")) {
      config.capture_every = static_cast<std::uint32_t>(std::atoi(every));
    }
    if (const char* limit = warploom_env("WARPLOOM_CAPTURE_LIMIT", "OMNICPP_CAPTURE_LIMIT")) {
      config.capture_limit = static_cast<std::uint32_t>(std::atoi(limit));
    }
    if (const char* start = warploom_env("WARPLOOM_START_TIME", "OMNICPP_START_TIME")) {
      config.start_time = static_cast<float>(std::atof(start));
    }
    if (const char* radius = warploom_env("WARPLOOM_CAMERA_RADIUS", "OMNICPP_CAMERA_RADIUS")) {
      config.camera_radius = static_cast<float>(std::atof(radius));
    }
    if (const char* height = warploom_env("WARPLOOM_CAMERA_HEIGHT", "OMNICPP_CAMERA_HEIGHT")) {
      config.camera_height = static_cast<float>(std::atof(height));
    }
    if (const char* crossfade = warploom_env("WARPLOOM_CROSSFADE", "OMNICPP_CROSSFADE")) {
      const float parsed = static_cast<float>(std::atof(crossfade));
      if (parsed > 0.0f) config.crossfade_period = parsed;
    }
    return config;
  }
};

// ============================================================================
// JSONL telemetry log
// ============================================================================

class TelemetryLogger {
 public:
  TelemetryLogger() = default;
  ~TelemetryLogger() {
    if (file_ != nullptr) std::fclose(file_);
  }
  TelemetryLogger(const TelemetryLogger&) = delete;
  TelemetryLogger& operator=(const TelemetryLogger&) = delete;

  //! Opens <dir>/telemetry.jsonl and writes the run manifest as line 0.
  [[nodiscard]] bool open(const std::string& dir, const RunConfig& config,
                          const std::string& device_name,
                          std::uint32_t width, std::uint32_t height,
                          std::size_t object_capacity, std::size_t joint_count,
                          bool has_mannequin) {
    if (dir.empty()) return false;
    const std::string path = dir + "/telemetry.jsonl";
    file_ = std::fopen(path.c_str(), "w");
    if (file_ == nullptr) return false;

    // Manifest: schema + environment so a log is self-describing.
    std::fprintf(file_,
                 "{\"type\":\"manifest\",\"schema\":1,\"width\":%u,"
                 "\"height\":%u,\"device\":\"%s\",\"max_frames\":%u,"
                 "\"fixed_dt\":%.6f,\"capture_every\":%u,"
                 "\"object_capacity\":%zu,\"joint_count\":%zu,"
                 "\"has_mannequin\":%s}\n",
                 width, height, sanitize(device_name).c_str(),
                 config.max_frames, static_cast<double>(config.fixed_dt),
                 config.capture_every, object_capacity, joint_count,
                 has_mannequin ? "true" : "false");
    return true;
  }

  //! One JSON line per frame. `capture_file` is the PPM basename captured
  //! this frame, or "" when none. `blend` is the idle-clip cross-fade weight
  //! (0 = full walk, 1 = full idle; 0 when blending is disabled).
  void log_frame(std::uint32_t frame, float sim_time, float walk_time,
                 float eye_x, float eye_y, float eye_z, std::size_t objects,
                 std::size_t drawn_objects, bool skinned_pipeline,
                 double record_us, double total_us, float fps,
                 const std::string& capture_file, float blend,
                 double gpu_ns = 0.0) {
    if (file_ == nullptr) return;
    std::fprintf(file_,
                 "{\"type\":\"frame\",\"frame\":%u,\"t\":%.6f,"
                 "\"walk_t\":%.6f,\"eye\":[%.4f,%.4f,%.4f],"
                 "\"objects\":%zu,\"drawn\":%zu,\"skinned\":%s,"
                 "\"record_us\":%.1f,\"total_us\":%.1f,\"fps\":%.2f,"
                 "\"capture\":\"%s\",\"blend\":%.4f,\"gpu_ns\":%.0f}\n",
                 frame, static_cast<double>(sim_time),
                 static_cast<double>(walk_time), static_cast<double>(eye_x),
                 static_cast<double>(eye_y), static_cast<double>(eye_z),
                 objects, drawn_objects, skinned_pipeline ? "true" : "false",
                 record_us, total_us, fps, capture_file.c_str(),
                 static_cast<double>(blend), gpu_ns);
  }

  void log_event(const std::string& event, const std::string& detail) {
    if (file_ == nullptr) return;
    std::fprintf(file_, "{\"type\":\"event\",\"event\":\"%s\",\"detail\":"
                        "\"%s\"}\n",
                 sanitize(event).c_str(), sanitize(detail).c_str());
  }

  //! One line per input event consumed this frame (virtual or real driver):
  //! the tick it applies to, the action/axis, and the value. Input flows are
  //! thus auditable alongside the scene state they moved.
  void log_input(std::uint64_t tick, const std::string& source,
                 const std::string& action, float value) {
    if (file_ == nullptr) return;
    std::fprintf(file_,
                 "{\"type\":\"input\",\"tick\":%llu,\"source\":\"%s\","
                 "\"action\":\"%s\",\"value\":%.4f}\n",
                 static_cast<unsigned long long>(tick), sanitize(source).c_str(),
                 sanitize(action).c_str(), static_cast<double>(value));
  }

  //! Static scene manifest, written once after the manifest line: every
  //! renderable object with its role, the skeleton, and the animation clips.
  //! This is the machine-readable "scenery" — structure, not samples.
  void log_scene_objects(
      const std::vector<std::tuple<std::string, std::size_t, std::size_t>>&
          objects) {
    if (file_ == nullptr) return;
    std::fputs("{\"type\":\"scene_objects\",\"objects\":[", file_);
    for (std::size_t i = 0; i < objects.size(); ++i) {
      std::fprintf(file_,
                   "%s{\"name\":\"%s\",\"mesh_index\":%zu,"
                   "\"material_index\":%zu}",
                   i != 0U ? "," : "", std::get<0>(objects[i]).c_str(),
                   std::get<1>(objects[i]), std::get<2>(objects[i]));
    }
    std::fputs("]}\n", file_);
  }

  void log_scene_skeleton(std::size_t joint_count,
                          const std::vector<std::string>& joint_names) {
    if (file_ == nullptr) return;
    std::fprintf(file_, "{\"type\":\"scene_skeleton\",\"joint_count\":%zu,"
                        "\"joints\":[",
                 joint_count);
    for (std::size_t i = 0; i < joint_names.size(); ++i) {
      std::fprintf(file_, "%s\"%s\"", i != 0U ? "," : "",
                   sanitize(joint_names[i]).c_str());
    }
    std::fputs("]}\n", file_);
  }

  void log_scene_clips(
      const std::vector<std::tuple<std::string, float, std::size_t>>& clips) {
    if (file_ == nullptr) return;
    std::fputs("{\"type\":\"scene_clips\",\"clips\":[", file_);
    for (std::size_t i = 0; i < clips.size(); ++i) {
      std::fprintf(file_,
                   "%s{\"name\":\"%s\",\"duration\":%.4f,"
                   "\"channel_count\":%zu}",
                   i != 0U ? "," : "", std::get<0>(clips[i]).c_str(),
                   static_cast<double>(std::get<1>(clips[i])),
                   std::get<2>(clips[i]));
    }
    std::fputs("]}\n", file_);
  }

  //! Per-frame pose summary: root translation + the joint with the largest
  //! swing, plus engine memory stats. Bounded size regardless of skeleton.
  void log_pose(float root_x, float root_y, float root_z,
                const std::string& swing_joint, float swing_deg,
                std::uint64_t allocator_used, std::uint64_t allocator_reserved,
                std::uint32_t allocations, float walk_phase, float blend) {
    if (file_ == nullptr) return;
    std::fprintf(file_,
                 "{\"type\":\"pose\",\"root\":[%.4f,%.4f,%.4f],"
                 "\"swing_joint\":\"%s\",\"swing_deg\":%.2f,"
                 "\"allocator_used\":%llu,\"allocator_reserved\":%llu,"
                 "\"allocations\":%u,\"walk_phase\":%.4f,\"blend\":%.4f}\n",
                 static_cast<double>(root_x), static_cast<double>(root_y),
                 static_cast<double>(root_z), sanitize(swing_joint).c_str(),
                 static_cast<double>(swing_deg),
                 static_cast<unsigned long long>(allocator_used),
                 static_cast<unsigned long long>(allocator_reserved),
                 allocations, static_cast<double>(walk_phase),
                 static_cast<double>(blend));
  }

  void flush() {
    if (file_ != nullptr) std::fflush(file_);
  }

 private:
  //! Strip characters JSON strings cannot carry bare.
  [[nodiscard]] static std::string sanitize(std::string text) {
    for (char& c : text) {
      if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
        c = '_';
      }
    }
    return text;
  }

  std::FILE* file_{nullptr};
};

// ============================================================================
// GPU-side frame capture
// ============================================================================

//! Offscreen re-record of the viewport's scene + device-local -> host copy.
//! Shares the window render pass (identical -> trivially compatible with
//! every scene pipeline) on capture-owned offscreen targets, then barriers
//! both attachments to TRANSFER_SRC_OPTIMAL and copies color + depth to
//! host-visible buffers. The window swapchain path is untouched; captures
//! are pure readback of the same scene recording.
class FrameCapture {
 public:
  struct Frame {
    std::vector<std::uint8_t> color_rgb;  //!< width*height*3, 8-bit
    std::vector<float> depth;             //!< width*height linear depth
    std::uint32_t width{0};
    std::uint32_t height{0};
  };

  FrameCapture() = default;
  ~FrameCapture() { cleanup(nullptr, nullptr); }
  FrameCapture(const FrameCapture&) = delete;
  FrameCapture& operator=(const FrameCapture&) = delete;

  [[nodiscard]] bool initialize(
      VkDevice device, omnicpp::render::VulkanMemoryAllocator& allocator,
      VkRenderPass shared_render_pass, VkFormat color_format,
      VkFormat depth_format, std::uint32_t width, std::uint32_t height,
      std::uint32_t queue_family) {
    device_ = device;
    render_pass_ = shared_render_pass;  // shared: identical -> compatible
    width_ = width;
    height_ = height;
    queue_family_ = queue_family;

    // --- device-local targets -------------------------------------------
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = color_format;
    image_info.extent = {width, height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &image_info, nullptr, &color_image_) !=
        VK_SUCCESS) {
      return false;
    }
    auto color_alloc =
        allocator.bind_image(color_image_, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!color_alloc.is_ok()) return false;
    color_allocation_ = color_alloc.value();

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = color_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = color_format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    if (vkCreateImageView(device, &view_info, nullptr, &color_view_) !=
        VK_SUCCESS) {
      return false;
    }

    VkImageCreateInfo depth_info = image_info;
    depth_info.format = depth_format;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(device, &depth_info, nullptr, &depth_image_) !=
        VK_SUCCESS) {
      return false;
    }
    auto depth_alloc =
        allocator.bind_image(depth_image_, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!depth_alloc.is_ok()) return false;
    depth_allocation_ = depth_alloc.value();

    VkImageViewCreateInfo depth_view_info = view_info;
    depth_view_info.image = depth_image_;
    depth_view_info.format = depth_format;
    depth_view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U,
                                        1U};
    if (vkCreateImageView(device, &depth_view_info, nullptr, &depth_view_) !=
        VK_SUCCESS) {
      return false;
    }

    VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb_info.renderPass = render_pass_;
    fb_info.attachmentCount = 2U;
    VkImageView fb_attachments[2] = {color_view_, depth_view_};
    fb_info.pAttachments = fb_attachments;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1U;
    if (vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffer_) !=
        VK_SUCCESS) {
      return false;
    }

    // --- host-visible readback buffers ---------------------------------
    const VkDeviceSize copy_bytes =
        static_cast<VkDeviceSize>(width) * height * 4U;
    auto color_readback = allocator.create_buffer(
        copy_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!color_readback.is_ok()) return false;
    color_readback_ = color_readback.value();

    auto depth_readback = allocator.create_buffer(
        copy_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!depth_readback.is_ok()) return false;
    depth_readback_ = depth_readback.value();

    // Channel order of the packed color copy.
    format_swizzle_bgra_ =
        color_format == VK_FORMAT_B8G8R8A8_UNORM ||
        color_format == VK_FORMAT_B8G8R8A8_SRGB;

    // --- capture command plumbing --------------------------------------
    VkCommandPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (vkCreateCommandPool(device, &pool_info, nullptr, &command_pool_) !=
        VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo alloc_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1U;
    if (vkAllocateCommandBuffers(device, &alloc_info, &command_buffer_) !=
        VK_SUCCESS) {
      return false;
    }
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
      return false;
    }
    return true;
  }

  //! Records clear + `record_scene` + copy-back and submits. `record_scene`
  //! records the scene draws into the begun render pass (no pass begin/end).
  [[nodiscard]] bool capture(
      VkQueue queue, const std::function<bool(VkCommandBuffer)>& record_scene) {
    if (vkResetCommandBuffer(command_buffer_, 0U) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_buffer_, &begin) != VK_SUCCESS) {
      return false;
    }

    VkClearValue clears[2]{};
    clears[0].color = {{0.06f, 0.07f, 0.09f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0U};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = render_pass_;  // shared with the window path
    pass.framebuffer = framebuffer_;
    pass.renderArea = {{0, 0}, {width_, height_}};
    pass.clearValueCount = 2U;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(command_buffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (!record_scene(command_buffer_)) {
      vkCmdEndRenderPass(command_buffer_);
      (void)vkEndCommandBuffer(command_buffer_);
      return false;
    }
    vkCmdEndRenderPass(command_buffer_);

    // The shared pass leaves the color attachment in PRESENT_SRC and the
    // depth attachment in DEPTH_STENCIL_ATTACHMENT_OPTIMAL; transition each
    // from its actual final layout to TRANSFER_SRC before the copies.
    VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_src.srcAccessMask = 0U;  // layout transitions are acquire/release
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    to_src.image = color_image_;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U,
                         nullptr, 1U, &to_src);
    VkImageMemoryBarrier depth_to_src = to_src;
    depth_to_src.oldLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_to_src.image = depth_image_;
    depth_to_src.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U,
                                     1U};
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U,
                         nullptr, 1U, &depth_to_src);

    VkBufferImageCopy color_region{};
    color_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
    color_region.imageExtent = {width_, height_, 1U};
    vkCmdCopyImageToBuffer(command_buffer_, color_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           color_readback_.buffer, 1U, &color_region);

    VkBufferImageCopy depth_region{};
    depth_region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 0U, 1U};
    depth_region.imageExtent = {width_, height_, 1U};
    vkCmdCopyImageToBuffer(command_buffer_, depth_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           depth_readback_.buffer, 1U, &depth_region);
    vkEndCommandBuffer(command_buffer_);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &command_buffer_;
    if (vkQueueSubmit(queue, 1U, &submit, fence_) != VK_SUCCESS) {
      return false;
    }
    vkWaitForFences(device_, 1U, &fence_, VK_TRUE, 1'000'000'000ULL);
    vkResetFences(device_, 1U, &fence_);
    return true;
  }

  //! Reads back the last capture and writes <dir>/frame_<n>.ppm plus
  //! <dir>/frame_<n>.depth32. Returns the PPM basename via `out_ppm_name`.
  [[nodiscard]] bool save(std::uint32_t frame_index, const std::string& dir,
                          std::string& out_ppm_name) {
    const auto* color_src =
        static_cast<const std::uint8_t*>(color_readback_.mapped);
    const auto* depth_src = static_cast<const float*>(depth_readback_.mapped);
    if (color_src == nullptr || depth_src == nullptr) return false;

    const std::size_t pixels = static_cast<std::size_t>(width_) * height_;
    last_.width = width_;
    last_.height = height_;
    last_.color_rgb.resize(pixels * 3U);
    last_.depth.resize(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
      const std::uint8_t* src = color_src + i * 4U;
      if (format_swizzle_bgra_) {
        last_.color_rgb[i * 3U + 0U] = src[2];
        last_.color_rgb[i * 3U + 1U] = src[1];
        last_.color_rgb[i * 3U + 2U] = src[0];
      } else {
        last_.color_rgb[i * 3U + 0U] = src[0];
        last_.color_rgb[i * 3U + 1U] = src[1];
        last_.color_rgb[i * 3U + 2U] = src[2];
      }
      last_.depth[i] = depth_src[i];
    }

    const std::string ppm = "frame_" + std::to_string(frame_index) + ".ppm";
    std::FILE* out = std::fopen((dir + "/" + ppm).c_str(), "wb");
    if (out == nullptr) return false;
    std::fprintf(out, "P6\n%u %u\n255\n", width_, height_);
    std::fwrite(last_.color_rgb.data(), 1U, last_.color_rgb.size(), out);
    std::fclose(out);

    std::FILE* depth_out =
        std::fopen((dir + "/frame_" + std::to_string(frame_index) +
                    ".depth32")
                       .c_str(),
                   "wb");
    if (depth_out != nullptr) {
      std::fwrite(last_.depth.data(), 4U, last_.depth.size(), depth_out);
      std::fclose(depth_out);
    }
    out_ppm_name = ppm;
    return true;
  }

  //! Last capture, for in-process analysis (histograms, animation deltas).
  [[nodiscard]] const Frame& last_frame() const { return last_; }

  void cleanup(VkDevice device, omnicpp::render::VulkanMemoryAllocator* allocator) {
    if (device_ != VK_NULL_HANDLE) device = device_;
    if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device, fence_, nullptr);
    if (command_pool_ != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device, command_pool_, nullptr);
    }
    if (allocator != nullptr) {
      if (color_readback_.is_valid()) {
        allocator->destroy_allocation(color_readback_);
      }
      if (depth_readback_.is_valid()) {
        allocator->destroy_allocation(depth_readback_);
      }
      if (color_allocation_.is_valid()) {
        allocator->destroy_allocation(color_allocation_);
      }
      if (depth_allocation_.is_valid()) {
        allocator->destroy_allocation(depth_allocation_);
      }
    }
    if (framebuffer_ != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device, framebuffer_, nullptr);
    }
    if (color_view_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, color_view_, nullptr);
    }
    if (depth_view_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, depth_view_, nullptr);
    }
    if (color_image_ != VK_NULL_HANDLE) {
      vkDestroyImage(device, color_image_, nullptr);
    }
    if (depth_image_ != VK_NULL_HANDLE) {
      vkDestroyImage(device, depth_image_, nullptr);
    }
    fence_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    framebuffer_ = VK_NULL_HANDLE;
    color_view_ = VK_NULL_HANDLE;
    depth_view_ = VK_NULL_HANDLE;
    color_image_ = VK_NULL_HANDLE;
    depth_image_ = VK_NULL_HANDLE;
  }

  //! True when the packed color copy is BGRA (set at initialize).
  bool format_swizzle_bgra_{false};

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkRenderPass render_pass_{VK_NULL_HANDLE};
  VkImage color_image_{VK_NULL_HANDLE};
  VkImage depth_image_{VK_NULL_HANDLE};
  VkImageView color_view_{VK_NULL_HANDLE};
  VkImageView depth_view_{VK_NULL_HANDLE};
  VkFramebuffer framebuffer_{VK_NULL_HANDLE};
  omnicpp::render::Allocation color_allocation_{};
  omnicpp::render::Allocation depth_allocation_{};
  omnicpp::render::Allocation color_readback_{};
  omnicpp::render::Allocation depth_readback_{};
  VkCommandPool command_pool_{VK_NULL_HANDLE};
  VkCommandBuffer command_buffer_{VK_NULL_HANDLE};
  VkFence fence_{VK_NULL_HANDLE};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  std::uint32_t queue_family_{0};
  Frame last_;
};

}  // namespace viewport
