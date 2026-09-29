#include "warploom/render/vulkan_scene.hpp"

#include "warploom/render/frustum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace omnicpp::render {

namespace {

template <typename Entry>
std::uint32_t acquire_slot(std::vector<Entry>& entries,
                           std::vector<std::uint32_t>& free_slots) {
  if (!free_slots.empty()) {
    const auto index = free_slots.back();
    free_slots.pop_back();
    entries[index].live = true;
    return index;
  }
  entries.push_back({});
  entries.back().live = true;
  return static_cast<std::uint32_t>(entries.size() - 1U);
}

struct WorldBox {
  float center[3]{0.0f, 0.0f, 0.0f};
  float extent[3]{0.0f, 0.0f, 0.0f};
};

//! Transform a local-space box through the column-major model matrix and
//! return its world-space center/extent. False when bounds are not valid.
[[nodiscard]] bool world_box_from_bounds(const SceneBounds& bounds,
                                         const SceneMatrix& model,
                                         WorldBox& out) noexcept {
  if (!bounds.valid) return false;
  float mn[3]{std::numeric_limits<float>::infinity(),
              std::numeric_limits<float>::infinity(),
              std::numeric_limits<float>::infinity()};
  float mx[3]{-std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()};
  const float lo[3] = {bounds.min[0], bounds.min[1], bounds.min[2]};
  const float hi[3] = {bounds.max[0], bounds.max[1], bounds.max[2]};
  for (std::uint32_t corner = 0; corner < 8U; ++corner) {
    const float x = (corner & 1U) != 0U ? hi[0] : lo[0];
    const float y = (corner & 2U) != 0U ? hi[1] : lo[1];
    const float z = (corner & 4U) != 0U ? hi[2] : lo[2];
    const float tx =
        model[0] * x + model[4] * y + model[8] * z + model[12];
    const float ty =
        model[1] * x + model[5] * y + model[9] * z + model[13];
    const float tz =
        model[2] * x + model[6] * y + model[10] * z + model[14];
    mn[0] = std::min(mn[0], tx);
    mn[1] = std::min(mn[1], ty);
    mn[2] = std::min(mn[2], tz);
    mx[0] = std::max(mx[0], tx);
    mx[1] = std::max(mx[1], ty);
    mx[2] = std::max(mx[2], tz);
  }
  out.center[0] = 0.5f * (mn[0] + mx[0]);
  out.center[1] = 0.5f * (mn[1] + mx[1]);
  out.center[2] = 0.5f * (mn[2] + mx[2]);
  out.extent[0] = 0.5f * (mx[0] - mn[0]);
  out.extent[1] = 0.5f * (mx[1] - mn[1]);
  out.extent[2] = 0.5f * (mx[2] - mn[2]);
  return true;
}

} // namespace

MeshHandle VulkanSceneResourceRegistry::create_mesh(SceneMesh mesh) {
  const auto index = acquire_slot(meshes_, free_meshes_);
  meshes_[index].value = mesh;
  return MeshHandle{index, meshes_[index].generation};
}

MaterialHandle VulkanSceneResourceRegistry::create_material(SceneMaterial material) {
  const auto index = acquire_slot(materials_, free_materials_);
  materials_[index].value = material;
  return MaterialHandle{index, materials_[index].generation};
}

TextureHandle VulkanSceneResourceRegistry::create_texture(SceneTexture texture) {
  const auto index = acquire_slot(textures_, free_textures_);
  textures_[index].value = texture;
  return TextureHandle{index, textures_[index].generation};
}

const SceneMesh* VulkanSceneResourceRegistry::resolve(MeshHandle handle) const noexcept {
  if (!handle.valid() || handle.index >= meshes_.size()) return nullptr;
  const auto& entry = meshes_[handle.index];
  return entry.live && entry.generation == handle.generation ? &entry.value : nullptr;
}

const SceneMaterial* VulkanSceneResourceRegistry::resolve(MaterialHandle handle) const noexcept {
  if (!handle.valid() || handle.index >= materials_.size()) return nullptr;
  const auto& entry = materials_[handle.index];
  return entry.live && entry.generation == handle.generation ? &entry.value : nullptr;
}

const SceneTexture* VulkanSceneResourceRegistry::resolve(TextureHandle handle) const noexcept {
  if (!handle.valid() || handle.index >= textures_.size()) return nullptr;
  const auto& entry = textures_[handle.index];
  return entry.live && entry.generation == handle.generation ? &entry.value : nullptr;
}

bool VulkanSceneResourceRegistry::destroy(MeshHandle handle,
                                          std::uint64_t retire_value) noexcept {
  if (!handle.valid()) return false;
  const std::uint32_t idx = handle.index;
  if (idx >= meshes_.size()) return false;
  auto& entry = meshes_[idx];
  if (!entry.live || entry.generation != handle.generation) return false;

  // Immediate resolution invalidation. Slot reuse is deferred by the retire
  // value, but the handle is dead right now so current-frame snapshots and
  // scene extraction must see it as gone. This is important when the same
  // registry is mutated between extract_vulkan_scene() and record_scene().
  entry.live = false;
  ++entry.generation;
  retired_meshes_.push_back({idx, entry.generation, retire_value});
  return true;
}

bool VulkanSceneResourceRegistry::destroy(MaterialHandle handle,
                                          std::uint64_t retire_value) noexcept {
  if (!handle.valid()) return false;
  const std::uint32_t idx = handle.index;
  if (idx >= materials_.size()) return false;
  auto& entry = materials_[idx];
  if (!entry.live || entry.generation != handle.generation) return false;
  entry.live = false;
  ++entry.generation;
  retired_materials_.push_back({idx, entry.generation, retire_value});
  return true;
}

bool VulkanSceneResourceRegistry::destroy(TextureHandle handle,
                                          std::uint64_t retire_value) noexcept {
  if (!handle.valid()) return false;
  const std::uint32_t idx = handle.index;
  if (idx >= textures_.size()) return false;
  auto& entry = textures_[idx];
  if (!entry.live || entry.generation != handle.generation) return false;
  entry.live = false;
  ++entry.generation;
  retired_textures_.push_back({idx, entry.generation, retire_value});
  return true;
}

void VulkanSceneResourceRegistry::collect(std::uint64_t completed_value) noexcept {
  auto collect_mesh = [this, completed_value](const RetiredSlot& retired) {
    if (retired.retire_value > completed_value) return false;
    if (retired.index < meshes_.size() &&
        meshes_[retired.index].generation == retired.generation &&
        !meshes_[retired.index].live) {
      free_meshes_.push_back(retired.index);
    }
    return true;
  };
  retired_meshes_.erase(
      std::remove_if(retired_meshes_.begin(), retired_meshes_.end(), collect_mesh),
      retired_meshes_.end());

  auto collect_material = [this, completed_value](const RetiredSlot& retired) {
    if (retired.retire_value > completed_value) return false;
    if (retired.index < materials_.size() &&
        materials_[retired.index].generation == retired.generation &&
        !materials_[retired.index].live) {
      free_materials_.push_back(retired.index);
    }
    return true;
  };
  retired_materials_.erase(
      std::remove_if(retired_materials_.begin(), retired_materials_.end(), collect_material),
      retired_materials_.end());

  auto collect_texture = [this, completed_value](const RetiredSlot& retired) {
    if (retired.retire_value > completed_value) return false;
    if (retired.index < textures_.size() &&
        textures_[retired.index].generation == retired.generation &&
        !textures_[retired.index].live) {
      free_textures_.push_back(retired.index);
    }
    return true;
  };
  retired_textures_.erase(
      std::remove_if(retired_textures_.begin(), retired_textures_.end(), collect_texture),
      retired_textures_.end());
}

std::size_t VulkanSceneResourceRegistry::live_mesh_count() const noexcept {
  return static_cast<std::size_t>(std::count_if(
      meshes_.begin(), meshes_.end(), [](const auto& entry) { return entry.live; }));
}

std::size_t VulkanSceneResourceRegistry::live_material_count() const noexcept {
  return static_cast<std::size_t>(std::count_if(
      materials_.begin(), materials_.end(), [](const auto& entry) { return entry.live; }));
}

std::size_t VulkanSceneResourceRegistry::live_texture_count() const noexcept {
  return static_cast<std::size_t>(std::count_if(
      textures_.begin(), textures_.end(), [](const auto& entry) { return entry.live; }));
}

VulkanScene extract_vulkan_scene(
    const omnicpp::core::World& world,
    VkPipeline pipeline,
    VkPipelineLayout pipeline_layout,
    SceneExtractionStats* stats) {
  VulkanScene scene;
  scene.pipeline = pipeline;
  scene.pipeline_layout = pipeline_layout;

  struct CameraCandidate {
    omnicpp::core::Entity entity{};
    const SceneCameraComponent* camera{nullptr};
  };
  std::vector<CameraCandidate> cameras;
  world.for_each<SceneCameraComponent>(
      [&cameras](omnicpp::core::Entity entity, const SceneCameraComponent& camera) {
        if (camera.active) cameras.push_back({entity, &camera});
      });
  std::sort(cameras.begin(), cameras.end(),
            [](const CameraCandidate& lhs, const CameraCandidate& rhs) {
              if (lhs.camera->priority != rhs.camera->priority) {
                return lhs.camera->priority < rhs.camera->priority;
              }
              return lhs.entity.id < rhs.entity.id;
            });
  const bool camera_present = !cameras.empty();
  if (camera_present) scene.camera.view_projection = cameras.front().camera->view_projection;
  Frustum frustum{};
  if (camera_present) {
    frustum = Frustum::from_view_projection(scene.camera.view_projection.data());
  }

  std::size_t culled_objects = 0;
  struct ObjectCandidate {
    omnicpp::core::Entity entity{};
    SceneObject object{};
  };
  std::vector<ObjectCandidate> objects;
  world.for_each<SceneRenderableComponent>(
      [&](omnicpp::core::Entity entity,
          const SceneRenderableComponent& renderable) {
        if (!renderable.visible || renderable.mesh == nullptr) return;
        SceneObject object;
        object.mesh = renderable.mesh;
        if (world.has_component<SceneTransformComponent>(entity)) {
          object.model = world.get_component<SceneTransformComponent>(entity).model;
        }
        if (camera_present && renderable.bounds.valid) {
          WorldBox world_box;
          if (world_box_from_bounds(renderable.bounds, object.model, world_box) &&
              Frustum::box_outside(frustum, world_box.center, world_box.extent)) {
            ++culled_objects;
            return;
          }
        }
        objects.push_back({entity, object});
      });
  std::sort(objects.begin(), objects.end(),
            [](const ObjectCandidate& lhs, const ObjectCandidate& rhs) {
              return lhs.entity.id < rhs.entity.id;
            });
  scene.objects.reserve(objects.size());
  for (const auto& candidate : objects) scene.objects.push_back(candidate.object);
  if (stats != nullptr) {
    stats->visible_objects = objects.size() + culled_objects;
    stats->culled_objects = culled_objects;
    stats->camera_present = camera_present;
  }
  return scene;
}

VulkanScene extract_vulkan_scene(const omnicpp::core::World& world,
                                 SceneExtractionStats* stats) {
  return extract_vulkan_scene(world, VK_NULL_HANDLE, VK_NULL_HANDLE, stats);
}

VulkanScene extract_vulkan_scene(
    const omnicpp::core::World& world,
    const VulkanSceneResourceRegistry& resources,
    VkPipeline pipeline,
    VkPipelineLayout pipeline_layout,
    SceneExtractionStats* stats) {
  VulkanScene scene;
  scene.pipeline = pipeline;
  scene.pipeline_layout = pipeline_layout;

  struct CameraCandidate {
    omnicpp::core::Entity entity{};
    const SceneCameraComponent* camera{nullptr};
  };
  std::vector<CameraCandidate> cameras;
  world.for_each<SceneCameraComponent>(
      [&cameras](omnicpp::core::Entity entity, const SceneCameraComponent& camera) {
        if (camera.active) cameras.push_back({entity, &camera});
      });
  std::sort(cameras.begin(), cameras.end(),
            [](const CameraCandidate& lhs, const CameraCandidate& rhs) {
              if (lhs.camera->priority != rhs.camera->priority) {
                return lhs.camera->priority < rhs.camera->priority;
              }
              return lhs.entity.id < rhs.entity.id;
            });
  const bool camera_present = !cameras.empty();
  if (camera_present) scene.camera.view_projection = cameras.front().camera->view_projection;
  Frustum frustum{};
  if (camera_present) {
    frustum = Frustum::from_view_projection(scene.camera.view_projection.data());
  }

  std::size_t culled_objects = 0;
  struct ObjectCandidate {
    omnicpp::core::Entity entity{};
    SceneObject object{};
  };
  std::vector<ObjectCandidate> objects;
  world.for_each<SceneRenderableComponent>(
      [&](omnicpp::core::Entity entity,
          const SceneRenderableComponent& renderable) {
        const SceneMesh* mesh = resources.resolve(renderable.mesh_handle);
        if (!renderable.visible || mesh == nullptr) return;
        SceneObject object;
        object.mesh_value = *mesh;
        object.mesh_handle = renderable.mesh_handle;
        if (const auto* material = resources.resolve(renderable.material_handle)) {
          object.material_value = *material;
          object.material_handle = renderable.material_handle;
          object.has_material = true;
          // Resolve the material's optional albedo texture into the snapshot
          // so record_scene needs no registry access.
          if (material->albedo.valid()) {
            if (const auto* albedo = resources.resolve(material->albedo)) {
              object.albedo_value = *albedo;
              object.has_albedo = albedo->is_valid();
            }
          }
        }
        if (world.has_component<SceneTransformComponent>(entity)) {
          object.model = world.get_component<SceneTransformComponent>(entity).model;
        }
        if (camera_present && renderable.bounds.valid) {
          WorldBox world_box;
          if (world_box_from_bounds(renderable.bounds, object.model, world_box) &&
              Frustum::box_outside(frustum, world_box.center, world_box.extent)) {
            ++culled_objects;
            return;
          }
        }
        objects.push_back({entity, object});
      });
  std::sort(objects.begin(), objects.end(),
            [](const ObjectCandidate& lhs, const ObjectCandidate& rhs) {
              return lhs.entity.id < rhs.entity.id;
            });
  scene.objects.reserve(objects.size());
  for (auto& candidate : objects) {
    candidate.object.mesh = nullptr;
    scene.objects.push_back(std::move(candidate.object));
  }
  scene.material_push_constants = true;
  if (stats != nullptr) {
    stats->visible_objects = objects.size() + culled_objects;
    stats->culled_objects = culled_objects;
    stats->camera_present = camera_present;
  }
  return scene;
}

} // namespace omnicpp::render
