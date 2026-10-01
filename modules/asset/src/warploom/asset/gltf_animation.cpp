//! @file gltf_animation.cpp
//! @brief Deterministic glTF 2.0 skeletal ingestion (see gltf_animation.hpp).

#include "warploom/asset/gltf_animation.hpp"

#include "warploom/asset/gltf_importer.hpp"
#include "gltf_json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace omnicpp::asset::gltf_detail;

namespace omnicpp::asset {
namespace {

constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

//! Read one float element of a validated accessor into `destination`.
void read_float_element(const std::vector<View>& views,
                        const std::vector<BufferSource>& sources,
                        const AccessorInfo& accessor,
                        std::size_t element_index, float* destination) {
  const View& view = views[accessor.view_index];
  const BufferSource& buffer = sources[view.buffer_index];
  const std::size_t element_size =
      accessor.components * accessor.component_size;
  const std::size_t stride =
      view.has_stride ? view.byte_stride : element_size;
  const std::size_t absolute =
      view.byte_offset + accessor.byte_offset + element_index * stride;
  for (std::size_t c = 0; c < accessor.components; ++c) {
    float value = 0.0f;
    std::memcpy(&value,
                buffer.data + absolute + c * accessor.component_size,
                sizeof(value));
    destination[c] = value;
  }
}

//! Read one unsigned integer element of a validated JOINTS_0 accessor.
std::uint32_t read_joint_element(const std::vector<View>& views,
                                 const std::vector<BufferSource>& sources,
                                 const AccessorInfo& accessor,
                                 std::size_t element_index,
                                 std::size_t component) {
  const View& view = views[accessor.view_index];
  const BufferSource& buffer = sources[view.buffer_index];
  const std::size_t element_size =
      accessor.components * accessor.component_size;
  const std::size_t stride =
      view.has_stride ? view.byte_stride : element_size;
  const std::size_t absolute = view.byte_offset + accessor.byte_offset +
                               element_index * stride +
                               component * accessor.component_size;
  std::uint64_t raw =
      read_le_unsigned(buffer.data + absolute, accessor.component_size);
  return static_cast<std::uint32_t>(raw);
}

//! Require `value` to be a valid index into `count` with `context` for the
//! diagnostic. Returns false (and sets `error`) when out of range.
[[nodiscard]] bool check_index(std::int64_t value, std::size_t count,
                               const std::string& context,
                               std::string& error) {
  if (value < 0 || static_cast<std::size_t>(value) >= count) {
    return fail_asset(error, context + " is out of range");
  }
  return true;
}

//! Validate + decode one TRS component of a node. Returns false on error.
[[nodiscard]] bool parse_trs_member(const Json& node_json,
                                    const char* key, float* out,
                                    std::size_t components,
                                    const std::string& context,
                                    std::string& error) {
  const Json* member = find_member(node_json, key);
  if (member == nullptr) return true;  // optional; caller defaults stand
  if (member->kind != Json::Kind::Array ||
      member->items.size() != components) {
    return fail_asset(error, context + " must be an array of " +
                                 std::to_string(components) + " numbers");
  }
  for (std::size_t i = 0; i < components; ++i) {
    double value = 0.0;
    if (!as_real(member->items[i], value, error,
                 context + "[" + std::to_string(i) + "]")) {
      return false;
    }
    out[i] = static_cast<float>(value);
  }
  return true;
}

//! Exact matrix -> TRS decomposition for nodes a document declares with a
//! 4x4 column-major matrix instead of TRS. Rotation via branch-free-free
//! Shepperd's method on the largest diagonal component; uniform scale from
//! column norms; non-uniform scale or shear is rejected (animations overwrite
//! TRS components, so they must fully describe the transform). Negative
//! determinant (mirrored node) is preserved by flipping the rotation axis
//! convention: scale keeps the negative sign.
//! Returns false with `error` set on singular or sheared matrices.
[[nodiscard]] bool decompose_trs_matrix(const float m[16], float t[3],
                                        float r[4], float s[3],
                                        const std::string& context,
                                        std::string& error) {
  // Column vectors (column-major storage: column c starts at m[c*4]).
  const float* cx = m;
  const float* cy = m + 4;
  const float* cz = m + 8;
  const float sx = std::sqrt(cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2]);
  const float sy = std::sqrt(cy[0] * cy[0] + cy[1] * cy[1] + cy[2] * cy[2]);
  const float sz = std::sqrt(cz[0] * cz[0] + cz[1] * cz[1] + cz[2] * cz[2]);
  if (sx < 1e-12F || sy < 1e-12F || sz < 1e-12F) {
    return fail_asset(error, context + " matrix is singular");
  }
  // Orthonormality: normalized columns must be mutually orthogonal within
  // tolerance (shear or extreme non-uniform scale otherwise).
  const float nx[3] = {cx[0] / sx, cx[1] / sx, cx[2] / sx};
  const float ny[3] = {cy[0] / sy, cy[1] / sy, cy[2] / sy};
  const float nz[3] = {cz[0] / sz, cz[1] / sz, cz[2] / sz};
  const float tol = 1e-3F;
  const float dot_xy = nx[0] * ny[0] + nx[1] * ny[1] + nx[2] * ny[2];
  const float dot_xz = nx[0] * nz[0] + nx[1] * nz[1] + nx[2] * nz[2];
  const float dot_yz = ny[0] * nz[0] + ny[1] * nz[1] + ny[2] * nz[2];
  if (std::abs(dot_xy) > tol || std::abs(dot_xz) > tol ||
      std::abs(dot_yz) > tol) {
    return fail_asset(error, context + " matrix has shear or non-uniform "
                                 "scale; skinned documents must use TRS");
  }
  const float det = nx[0] * (ny[1] * nz[2] - ny[2] * nz[1]) -
                    nx[1] * (ny[0] * nz[2] - ny[2] * nz[0]) +
                    nx[2] * (ny[0] * nz[1] - ny[1] * nz[0]);
  // Quaternion rotation matrix (transposed rotation part), mirrored by
  // det < 0 through a y-axis flip (standard handling of mirrored nodes).
  float m00 = nx[0];
  float m01 = ny[0];
  float m02 = nz[0];
  float m10 = nx[1];
  float m11 = ny[1];
  float m12 = nz[1];
  float m20 = nx[2];
  float m21 = ny[2];
  float m22 = nz[2];
  if (det < 0.0F) {
    m01 = -m01;
    m11 = -m11;
    m21 = -m21;
    s[1] = -sy;
  } else {
    s[1] = sy;
  }
  s[0] = sx;
  s[2] = sz;
  // Shepperd's method: largest of the four candidate diagonals.
  const float trace = m00 + m11 + m22;
  float qw;
  if (trace > 0.0F) {
    const float root = std::sqrt(trace + 1.0F);
    qw = 0.5F * root;
    const float k = 0.5F / root;
    r[0] = (m21 - m12) * k;
    r[1] = (m02 - m20) * k;
    r[2] = (m10 - m01) * k;
  } else if (m00 > m11 && m00 > m22) {
    const float root = std::sqrt(1.0F + m00 - m11 - m22);
    r[0] = 0.5F * root;
    const float k = 0.5F / root;
    r[1] = (m01 + m10) * k;
    r[2] = (m02 + m20) * k;
    qw = (m21 - m12) * k;
  } else if (m11 > m22) {
    const float root = std::sqrt(1.0F + m11 - m00 - m22);
    r[1] = 0.5F * root;
    const float k = 0.5F / root;
    r[0] = (m01 + m10) * k;
    r[2] = (m12 + m21) * k;
    qw = (m02 - m20) * k;
  } else {
    const float root = std::sqrt(1.0F + m22 - m00 - m11);
    r[2] = 0.5F * root;
    const float k = 0.5F / root;
    r[0] = (m02 + m20) * k;
    r[1] = (m12 + m21) * k;
    qw = (m10 - m01) * k;
  }
  r[3] = qw;
  t[0] = m[12];
  t[1] = m[13];
  t[2] = m[14];
  return true;
}

[[nodiscard]] bool parse_nodes(const Json& document,
                               std::vector<GltfSkinNode>& nodes,
                               std::string& error) {
  const Json* nodes_json = find_member(document, "nodes");
  if (nodes_json == nullptr || nodes_json->kind != Json::Kind::Array ||
      nodes_json->items.empty()) {
    return fail_asset(error, "document is missing nodes array");
  }
  nodes.resize(nodes_json->items.size());
  for (std::size_t i = 0; i < nodes_json->items.size(); ++i) {
    const Json& node_json = nodes_json->items[i];
    const std::string context = "nodes[" + std::to_string(i) + "]";
    GltfSkinNode& node = nodes[i];

    if (const Json* matrix = find_member(node_json, "matrix");
        matrix != nullptr) {
      // Matrix nodes are legal when their transform decomposes to exact TRS
      // (real exports often author a root matrix, e.g. a Z-up fixup). Shear,
      // singular, or non-uniform-scale matrices are rejected because TRS
      // must fully describe the node for animation sampling.
      if (matrix->kind != Json::Kind::Array ||
          matrix->items.size() != 16U) {
        return fail_asset(error,
                          context + " matrix must be an array of 16 numbers");
      }
      float m[16];
      for (std::size_t i = 0; i < 16U; ++i) {
        double value = 0.0;
        if (!as_real(matrix->items[i], value, error,
                     context + "[" + std::to_string(i) + "]")) {
          return false;
        }
        m[i] = static_cast<float>(value);
      }
      if (!decompose_trs_matrix(m, node.translation, node.rotation,
                                node.scale, context, error)) {
        return false;
      }
      node.has_trs = true;
    }
    if (const Json* name = find_member(node_json, "name");
        name != nullptr && name->kind == Json::Kind::String) {
      node.name = name->string;
    }
    // children
    if (const Json* children = find_member(node_json, "children");
        children != nullptr) {
      if (children->kind != Json::Kind::Array) {
        return fail_asset(error, context + ".children must be an array");
      }
      node.children.reserve(children->items.size());
      for (std::size_t c = 0; c < children->items.size(); ++c) {
        std::int64_t child = 0;
        if (!as_int(children->items[c], child,
                    error, context + ".children[" + std::to_string(c) + "]")) {
          return false;
        }
        if (!check_index(child, nodes.size(),
                         context + ".children[" + std::to_string(c) + "]",
                         error)) {
          return false;
        }
        node.children.push_back(static_cast<std::size_t>(child));
      }
    }
    // mesh / skin (validated against the declared array lengths)
    if (const Json* mesh = find_member(node_json, "mesh"); mesh != nullptr) {
      std::int64_t mesh_index = 0;
      if (!as_int(*mesh, mesh_index, error, context + ".mesh")) return false;
      const Json* meshes_json = find_member(document, "meshes");
      const std::size_t mesh_count =
          meshes_json != nullptr && meshes_json->kind == Json::Kind::Array
              ? meshes_json->items.size()
              : 0U;
      if (!check_index(mesh_index, mesh_count, context + ".mesh", error)) {
        return false;
      }
      node.has_mesh = true;
      node.mesh_index = static_cast<std::size_t>(mesh_index);
    }
    if (const Json* skin = find_member(node_json, "skin"); skin != nullptr) {
      std::int64_t skin_index = 0;
      if (!as_int(*skin, skin_index, error, context + ".skin")) return false;
      const Json* skins_json = find_member(document, "skins");
      const std::size_t skin_count =
          skins_json != nullptr && skins_json->kind == Json::Kind::Array
              ? skins_json->items.size()
              : 0U;
      if (!check_index(skin_index, skin_count, context + ".skin", error)) {
        return false;
      }
      node.has_skin = true;
      node.skin = static_cast<std::size_t>(skin_index);
    }
    // TRS components.
    node.has_trs =
        find_member(node_json, "translation") != nullptr ||
        find_member(node_json, "rotation") != nullptr ||
        find_member(node_json, "scale") != nullptr;
    if (!parse_trs_member(node_json, "translation", node.translation, 3,
                          context + ".translation", error) ||
        !parse_trs_member(node_json, "rotation", node.rotation, 4,
                          context + ".rotation", error) ||
        !parse_trs_member(node_json, "scale", node.scale, 3,
                          context + ".scale", error)) {
      return false;
    }
    // Normalize the rotation quaternion (glTF units are unit quaternions).
    const float norm = std::sqrt(node.rotation[0] * node.rotation[0] +
                                 node.rotation[1] * node.rotation[1] +
                                 node.rotation[2] * node.rotation[2] +
                                 node.rotation[3] * node.rotation[3]);
    if (norm > 0.0f) {
      for (float& c : node.rotation) c /= norm;
    }
  }
  // Parent links + cycle detection (iterative color DFS; a node must never
  // be revisited while on the stack).
  std::vector<std::uint8_t> color(nodes.size(), 0);  // 0 new, 1 open, 2 done
  std::vector<std::size_t> stack;
  for (std::size_t root = 0; root < nodes.size(); ++root) {
    if (color[root] != 0) continue;
    stack.clear();
    stack.push_back(root);
    while (!stack.empty()) {
      const std::size_t current = stack.back();
      if (color[current] == 1) {
        color[current] = 2;
        stack.pop_back();
        continue;
      }
      if (color[current] == 2) {
        stack.pop_back();
        continue;
      }
      color[current] = 1;
      for (const std::size_t child : nodes[current].children) {
        if (child == current) {
          return fail_asset(error, "node graph contains a self cycle at " +
                                       std::to_string(child));
        }
        if (nodes[child].parent != kGltfNoParent) {
          return fail_asset(
              error, "node " + std::to_string(child) +
                         " has more than one parent (child of both " +
                         std::to_string(nodes[child].parent) + " and " +
                         std::to_string(current) + ")");
        }
        nodes[child].parent = current;
        if (color[child] == 1) {
          return fail_asset(error, "node graph contains a cycle through " +
                                       std::to_string(child));
        }
        if (color[child] == 0) stack.push_back(child);
      }
    }
  }
  return true;
}

[[nodiscard]] bool import_skins_and_bindings(
    const Json& document, const std::vector<View>& views,
    const std::vector<BufferSource>& sources,
    const std::vector<GltfSkinNode>& nodes,
    const std::vector<std::size_t>& mesh_node_to_slot,
    GltfAnimationDocument& out, std::string& error) {
  const Json* skins_json = find_member(document, "skins");
  if (skins_json == nullptr) return true;  // no skins: nothing to do
  if (skins_json->kind != Json::Kind::Array) {
    return fail_asset(error, "skins must be an array");
  }
  const Json* accessors_json = find_member(document, "accessors");

  out.skins.resize(skins_json->items.size());
  for (std::size_t s = 0; s < skins_json->items.size(); ++s) {
    const Json& skin_json = skins_json->items[s];
    const std::string context = "skins[" + std::to_string(s) + "]";
    GltfSkinImport& skin = out.skins[s];

    if (const Json* name = find_member(skin_json, "name");
        name != nullptr && name->kind == Json::Kind::String) {
      skin.name = name->string;
    }
    const Json* joints = find_member(skin_json, "joints");
    if (joints == nullptr || joints->kind != Json::Kind::Array ||
        joints->items.empty()) {
      return fail_asset(error, context + " must declare a non-empty joints array");
    }
    skin.joints.reserve(joints->items.size());
    for (std::size_t j = 0; j < joints->items.size(); ++j) {
      std::int64_t joint = 0;
      if (!as_int(joints->items[j], joint, error,
                  context + ".joints[" + std::to_string(j) + "]")) {
        return false;
      }
      if (!check_index(joint, nodes.size(),
                       context + ".joints[" + std::to_string(j) + "]",
                       error)) {
        return false;
      }
      skin.joints.push_back(static_cast<std::size_t>(joint));
    }
    // Duplicate joints are ambiguous for matrix upload order.
    {
      std::vector<std::size_t> sorted = skin.joints;
      std::sort(sorted.begin(), sorted.end());
      if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return fail_asset(error, context + " declares duplicate joints");
      }
    }
    if (const Json* skeleton = find_member(skin_json, "skeleton");
        skeleton != nullptr) {
      std::int64_t root = 0;
      if (!as_int(*skeleton, root, error, context + ".skeleton")) return false;
      if (!check_index(root, nodes.size(), context + ".skeleton", error)) {
        return false;
      }
      skin.skeleton_root = static_cast<std::size_t>(root);
    }
    // Inverse bind matrices: required, one MAT4 float per joint.
    const Json* ibm = find_member(skin_json, "inverseBindMatrices");
    if (ibm == nullptr) {
      return fail_asset(error, context +
                                   " is missing inverseBindMatrices (the "
                                   "skinning math requires them)");
    }
    std::int64_t ibm_index = 0;
    if (!as_int(*ibm, ibm_index, error,
                context + ".inverseBindMatrices")) {
      return false;
    }
    if (!check_index(ibm_index, accessors_json->items.size(),
                     context + ".inverseBindMatrices", error)) {
      return false;
    }
    AccessorInfo ibm_info;
    if (!parse_accessor(accessors_json->items[static_cast<std::size_t>(
                                            ibm_index)],
                        views, ibm_info, error,
                        context + ".inverseBindMatrices")) {
      return false;
    }
    if (ibm_info.component_type != kComponentFloat ||
        ibm_info.components != 16) {
      return fail_asset(error, context +
                                   ".inverseBindMatrices must be MAT4 float32");
    }
    if (ibm_info.count != skin.joints.size()) {
      return fail_asset(
          error, context + ".inverseBindMatrices count " +
                     std::to_string(ibm_info.count) + " does not match the " +
                     std::to_string(skin.joints.size()) + " joints");
    }
    skin.inverse_bind_matrices.resize(skin.joints.size());
    for (std::size_t j = 0; j < ibm_info.count; ++j) {
      read_float_element(views, sources, ibm_info, j,
                         skin.inverse_bind_matrices[j].data());
    }
  }

  // JOINTS_0/WEIGHTS_0 bindings for every skinned node's mesh.
  const Json* meshes_json = find_member(document, "meshes");
  for (const GltfSkinNode& node : nodes) {
    if (!node.has_mesh || !node.has_skin) continue;
    if (node.skin >= out.skins.size()) {
      return fail_asset(error, "nodes[" + std::to_string(node.skin) +
                                   "] skin index is out of range");
    }
    if (node.mesh_index >= mesh_node_to_slot.size() ||
        mesh_node_to_slot[node.mesh_index] == kNoIndex) {
      return fail_asset(error, "internal: skinned mesh was not imported");
    }
    const std::size_t slot = mesh_node_to_slot[node.mesh_index];
    if (!out.skin_bindings[slot].joints.empty()) continue;  // shared skin

    const Json& mesh_json = meshes_json->items[node.mesh_index];
    const Json* primitives = find_member(mesh_json, "primitives");
    if (primitives == nullptr || primitives->kind != Json::Kind::Array) {
      return fail_asset(error, "skinned mesh has no primitives");
    }
    GltfSkinBinding& binding = out.skin_bindings[slot];
    const GltfMeshImport& mesh = out.meshes[slot];
    binding.joints.assign(static_cast<std::size_t>(mesh.vertex_count()) * 4U,
                          0U);
    binding.weights.assign(static_cast<std::size_t>(mesh.vertex_count()) * 4U,
                           0.0f);

    for (std::size_t p = 0; p < primitives->items.size(); ++p) {
      const std::string context = "meshes[" + std::to_string(node.mesh_index) +
                                  "].primitives[" + std::to_string(p) + "]";
      const Json& primitive = primitives->items[p];
      const Json* attributes = find_member(primitive, "attributes");
      if (attributes == nullptr) {
        return fail_asset(error, context + " is missing attributes");
      }
      const Json* joints_ref = find_member(*attributes, "JOINTS_0");
      const Json* weights_ref = find_member(*attributes, "WEIGHTS_0");
      if (joints_ref == nullptr || weights_ref == nullptr) {
        return fail_asset(
            error, context +
                       " is bound to a skin but lacks JOINTS_0/WEIGHTS_0");
      }
      auto accessor_at = [&](const Json& ref,
                             const std::string& name) -> AccessorInfo {
        std::int64_t index = 0;
        if (!as_int(ref, index, error, context + ".attributes." + name)) {
          return AccessorInfo{};
        }
        if (!check_index(index, accessors_json->items.size(),
                         context + ".attributes." + name, error)) {
          return AccessorInfo{};
        }
        AccessorInfo info;
        if (!parse_accessor(accessors_json->items[static_cast<std::size_t>(
                                index)],
                            views, info, error,
                            context + ".attributes." + name)) {
          return AccessorInfo{};
        }
        return info;
      };
      const AccessorInfo joints_info = accessor_at(*joints_ref, "JOINTS_0");
      if (!error.empty()) return false;
      if (joints_info.component_type != kComponentUByte &&
          joints_info.component_type != kComponentUShort) {
        return fail_asset(error, context +
                                     ".attributes.JOINTS_0 must be ubyte or "
                                     "ushort");
      }
      if (joints_info.components != 4) {
        return fail_asset(
            error, context + ".attributes.JOINTS_0 must be VEC4");
      }
      const AccessorInfo weights_info = accessor_at(*weights_ref, "WEIGHTS_0");
      if (!error.empty()) return false;
      if (weights_info.component_type != kComponentFloat) {
        return fail_asset(
            error, context + ".attributes.WEIGHTS_0 must be float32");
      }
      if (weights_info.components != 4) {
        return fail_asset(
            error, context + ".attributes.WEIGHTS_0 must be VEC4");
      }
      if (p >= mesh.primitives.size()) {
        return fail_asset(error, "internal: primitive slice missing");
      }
      const std::size_t vertex_offset = mesh.primitives[p].vertex_offset;
      const std::size_t vertex_count = mesh.primitives[p].vertex_count;
      if (joints_info.count != vertex_count ||
          weights_info.count != vertex_count) {
        return fail_asset(
            error, context + " JOINTS_0/WEIGHTS_0 counts do not match the " +
                       std::to_string(vertex_count) + " POSITION vertices");
      }
      const std::size_t joint_count = out.skins[node.skin].joints.size();
      std::size_t element = vertex_offset;  // merged-stream write cursor
      for (std::size_t v = 0; v < vertex_count; ++v, ++element) {
        float weights[4];
        read_float_element(views, sources, weights_info, v, weights);
        const float sum =
            weights[0] + weights[1] + weights[2] + weights[3];
        const float normalize = sum > 0.0f ? 1.0f / sum : 0.0f;
        for (std::size_t c = 0; c < 4; ++c) {
          const std::uint32_t joint =
              read_joint_element(views, sources, joints_info, v, c);
          if (joint >= joint_count) {
            return fail_asset(
                error, context + " references joint " +
                           std::to_string(joint) + " but the skin declares " +
                           std::to_string(joint_count) + " joints");
          }
          binding.joints[element * 4U + c] = static_cast<std::uint16_t>(joint);
          binding.weights[element * 4U + c] = weights[c] * normalize;
        }
      }
    }
  }
  // A mesh that carries skinned attributes must be referenced through a
  // skin (handled above); nothing further to check here because JOINTS_0
  // parsing only happens for skinned nodes.
  return true;
}

[[nodiscard]] bool import_animations(const Json& document,
                                     const std::vector<View>& views,
                                     const std::vector<BufferSource>& sources,
                                     std::size_t node_count,
                                     GltfAnimationDocument& out,
                                     std::string& error) {
  const Json* animations_json = find_member(document, "animations");
  if (animations_json == nullptr) return true;
  if (animations_json->kind != Json::Kind::Array) {
    return fail_asset(error, "animations must be an array");
  }
  const Json* accessors_json = find_member(document, "accessors");

  out.animations.resize(animations_json->items.size());
  for (std::size_t a = 0; a < animations_json->items.size(); ++a) {
    const Json& animation_json = animations_json->items[a];
    const std::string context = "animations[" + std::to_string(a) + "]";
    GltfAnimationImport& animation = out.animations[a];

    if (const Json* name = find_member(animation_json, "name");
        name != nullptr && name->kind == Json::Kind::String) {
      animation.name = name->string;
    }
    const Json* samplers = find_member(animation_json, "samplers");
    if (samplers == nullptr || samplers->kind != Json::Kind::Array ||
        samplers->items.empty()) {
      return fail_asset(error, context + " must declare samplers");
    }
    animation.samplers.resize(samplers->items.size());
    for (std::size_t s = 0; s < samplers->items.size(); ++s) {
      const std::string sampler_context =
          context + ".samplers[" + std::to_string(s) + "]";
      const Json& sampler_json = samplers->items[s];
      GltfSampler& sampler = animation.samplers[s];

      const Json* interpolation =
          find_member(sampler_json, "interpolation");
      if (interpolation != nullptr) {
        if (interpolation->kind != Json::Kind::String) {
          return fail_asset(error, sampler_context +
                                       ".interpolation must be a string");
        }
        if (interpolation->string == "LINEAR") {
          sampler.interpolation = GltfSamplerInterpolation::Linear;
        } else if (interpolation->string == "STEP") {
          sampler.interpolation = GltfSamplerInterpolation::Step;
        } else if (interpolation->string == "CUBICSPLINE") {
          sampler.interpolation = GltfSamplerInterpolation::CubicSpline;
        } else {
          return fail_asset(error, sampler_context +
                                       ".interpolation \"" +
                                       interpolation->string +
                                       "\" is not a glTF interpolation mode");
        }
      }
      // Input: SCALAR float32 keyframe times, strictly increasing.
      const Json* input = find_member(sampler_json, "input");
      if (input == nullptr) {
        return fail_asset(error, sampler_context + " is missing input");
      }
      std::int64_t input_index = 0;
      if (!as_int(*input, input_index, error,
                  sampler_context + ".input")) {
        return false;
      }
      if (!check_index(input_index, accessors_json->items.size(),
                       sampler_context + ".input", error)) {
        return false;
      }
      AccessorInfo input_info;
      if (!parse_accessor(accessors_json->items[static_cast<std::size_t>(
                              input_index)],
                          views, input_info, error,
                          sampler_context + ".input")) {
        return false;
      }
      if (input_info.component_type != kComponentFloat ||
          input_info.components != 1 || input_info.count == 0) {
        return fail_asset(error, sampler_context +
                                     ".input must be a non-empty SCALAR "
                                     "float32 accessor");
      }
      sampler.times.resize(input_info.count);
      for (std::size_t i = 0; i < input_info.count; ++i) {
        float time[1];
        read_float_element(views, sources, input_info, i, time);
        sampler.times[i] = time[0];
        if (i > 0 && !(sampler.times[i] > sampler.times[i - 1])) {
          return fail_asset(error, sampler_context +
                                       ".input times must be strictly "
                                       "increasing");
        }
      }
      // Output: VEC3 (translation/scale) or VEC4 (rotation) float32.
      const Json* output = find_member(sampler_json, "output");
      if (output == nullptr) {
        return fail_asset(error, sampler_context + " is missing output");
      }
      std::int64_t output_index = 0;
      if (!as_int(*output, output_index, error,
                  sampler_context + ".output")) {
        return false;
      }
      if (!check_index(output_index, accessors_json->items.size(),
                       sampler_context + ".output", error)) {
        return false;
      }
      AccessorInfo output_info;
      if (!parse_accessor(accessors_json->items[static_cast<std::size_t>(
                              output_index)],
                          views, output_info, error,
                          sampler_context + ".output")) {
        return false;
      }
      if (output_info.component_type != kComponentFloat ||
          (output_info.components != 3 && output_info.components != 4)) {
        return fail_asset(error, sampler_context +
                                     ".output must be VEC3/VEC4 float32");
      }
      if (sampler.interpolation == GltfSamplerInterpolation::CubicSpline) {
        if (output_info.count != 3U * input_info.count) {
          return fail_asset(
              error, sampler_context +
                         ".output count must be 3x .input count for "
                         "CUBICSPLINE (in-tangent, value, out-tangent per "
                         "keyframe)");
        }
      } else if (output_info.count != input_info.count) {
        return fail_asset(error, sampler_context +
                                     ".output count must match .input count");
      }
      sampler.stride = output_info.components;
      if (sampler.interpolation == GltfSamplerInterpolation::CubicSpline) {
        // CUBICSPLINE output is [a1,v1,b1, a2,v2,b2, ...]: per keyframe an
        // in-tangent, the keyframe value, and an out-tangent, each `stride`
        // floats. Split into three parallel arrays for sampling.
        constexpr std::size_t kCubicLayout = 3U;
        const std::size_t keyframe_count = input_info.count;
        sampler.values.resize(keyframe_count * sampler.stride);
        sampler.in_tangents.resize(keyframe_count * sampler.stride);
        sampler.out_tangents.resize(keyframe_count * sampler.stride);
        for (std::size_t i = 0; i < keyframe_count; ++i) {
          float triple[3 * 16];
          for (std::size_t part = 0; part < kCubicLayout; ++part) {
            read_float_element(
                views, sources, output_info,
                i * kCubicLayout + part,
                triple + part * sampler.stride);
          }
          for (std::size_t c = 0; c < sampler.stride; ++c) {
            sampler.in_tangents[i * sampler.stride + c] =
                triple[0 * sampler.stride + c];
            sampler.values[i * sampler.stride + c] =
                triple[1 * sampler.stride + c];
            sampler.out_tangents[i * sampler.stride + c] =
                triple[2 * sampler.stride + c];
          }
        }
      } else {
      sampler.values.resize(output_info.count * sampler.stride);
      for (std::size_t i = 0; i < output_info.count; ++i) {
        read_float_element(views, sources, output_info, i,
                           sampler.values.data() + i * sampler.stride);
      }
      }
      animation.duration =
          std::max(animation.duration, sampler.times.back());
    }
    // Channels.
    const Json* channels = find_member(animation_json, "channels");
    if (channels == nullptr || channels->kind != Json::Kind::Array ||
        channels->items.empty()) {
      return fail_asset(error, context + " must declare channels");
    }
    for (std::size_t c = 0; c < channels->items.size(); ++c) {
      const std::string channel_context =
          context + ".channels[" + std::to_string(c) + "]";
      const Json& channel_json = channels->items[c];
      GltfChannel channel{};

      const Json* sampler_ref = find_member(channel_json, "sampler");
      if (sampler_ref == nullptr) {
        return fail_asset(error, channel_context + " is missing sampler");
      }
      std::int64_t sampler_index = 0;
      if (!as_int(*sampler_ref, sampler_index, error,
                  channel_context + ".sampler")) {
        return false;
      }
      if (!check_index(sampler_index, animation.samplers.size(),
                       channel_context + ".sampler", error)) {
        return false;
      }
      channel.sampler = static_cast<std::size_t>(sampler_index);

      const Json* target = find_member(channel_json, "target");
      if (target == nullptr) {
        return fail_asset(error, channel_context + " is missing target");
      }
      const Json* node_ref = find_member(*target, "node");
      if (node_ref == nullptr) {
        return fail_asset(error, channel_context +
                                     ".target is missing node (root-less "
                                     "channels are not supported)");
      }
      std::int64_t node_index = 0;
      if (!as_int(*node_ref, node_index, error,
                  channel_context + ".target.node")) {
        return false;
      }
      if (!check_index(node_index, node_count,
                       channel_context + ".target.node", error)) {
        return false;
      }
      channel.target_node = static_cast<std::size_t>(node_index);

      const Json* path = find_member(*target, "path");
      if (path == nullptr || path->kind != Json::Kind::String) {
        return fail_asset(error, channel_context + ".target.path is required");
      }
      if (path->string == "translation") {
        channel.path = GltfChannel::Path::Translation;
      } else if (path->string == "rotation") {
        channel.path = GltfChannel::Path::Rotation;
      } else if (path->string == "scale") {
        channel.path = GltfChannel::Path::Scale;
      } else if (path->string == "weights") {
        return fail_asset(error, channel_context +
                                     ".target.path weights is not supported");
      } else {
        return fail_asset(error, channel_context + ".target.path \"" +
                                     path->string + "\" is not a glTF path");
      }
      // Rotation channels must bind a VEC4 sampler (and vice versa).
      const std::size_t stride =
          animation.samplers[channel.sampler].stride;
      const std::size_t wanted =
          channel.path == GltfChannel::Path::Rotation ? 4U : 3U;
      if (stride != wanted) {
        return fail_asset(error, channel_context +
                                     " binds a path to a sampler whose "
                                     "output width does not match");
      }
      // Duplicate targets are ambiguous: two channels writing one component.
      for (const GltfChannel& existing : animation.channels) {
        if (existing.target_node == channel.target_node &&
            existing.path == channel.path) {
          return fail_asset(error, channel_context +
                                       " targets the same node component as "
                                       "an earlier channel");
        }
      }
      animation.channels.push_back(channel);
    }
  }
  return true;
}

//! Shortest-arc quaternion slerp (xyzw) with nlerp fallback near parallel.
void slerp_rotation(const float a[4], const float b[4], float t, float* out) {
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  float bx = b[0];
  float by = b[1];
  float bz = b[2];
  float bw = b[3];
  if (dot < 0.0f) {
    dot = -dot;
    bx = -bx;
    by = -by;
    bz = -bz;
    bw = -bw;
  }
  if (dot > 0.9995f) {
    // Nearly parallel: linear + normalize is numerically safest.
    for (int i = 0; i < 4; ++i) {
      const float other[4] = {bx, by, bz, bw};
      out[i] = a[i] + t * (other[i] - a[i]);
    }
  } else {
    const float theta = std::acos(dot);
    const float sin_theta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sin_theta;
    const float wb = std::sin(t * theta) / sin_theta;
    const float other[4] = {bx, by, bz, bw};
    for (int i = 0; i < 4; ++i) out[i] = wa * a[i] + wb * other[i];
  }
  const float norm = std::sqrt(out[0] * out[0] + out[1] * out[1] +
                               out[2] * out[2] + out[3] * out[3]);
  if (norm > 0.0f) {
    for (int i = 0; i < 4; ++i) out[i] /= norm;
  }
}

}  // namespace

omnicpp::core::Result<GltfAnimationDocument> import_gltf_animation_document(
    const char* json_bytes, std::size_t json_len, const std::uint8_t* bin_bytes,
    std::size_t bin_len, std::string* error_detail,
    const ExternalFileLoader* loader) {
  std::string error;
  GltfAnimationDocument document;

  auto finish = [&](bool ok) -> omnicpp::core::Result<GltfAnimationDocument> {
    if (!ok) {
      if (error_detail != nullptr) *error_detail = error;
      return omnicpp::core::Result<GltfAnimationDocument>::error(
          omnicpp::core::RuntimeError::malformed_asset);
    }
    return omnicpp::core::Result<GltfAnimationDocument>::ok(
        std::move(document));
  };

  if (json_bytes == nullptr && json_len != 0U) {
    return finish(fail_asset(error, "json_bytes is null"));
  }
  if (bin_bytes == nullptr && bin_len != 0U) {
    return finish(fail_asset(error, "bin_bytes is null"));
  }
  if (json_len == 0U) {
    return finish(fail_asset(error, "empty glTF document"));
  }

  // Auto-detect the container: raw JSON or a GLB 2.0 container.
  std::string container_json;
  DocumentPrologue prologue;
  if (!parse_gltf_document_prologue(json_bytes, json_len, bin_bytes, bin_len,
                                    container_json, prologue, error)) {
    return finish(false);
  }

  JsonParser parser(prologue.json_bytes, prologue.json_len, error);
  Json root;
  if (!parser.parse(root) || root.kind != Json::Kind::Object) {
    return finish(false);
  }
  const Json* asset = find_member(root, "asset");
  if (asset == nullptr) {
    return finish(fail_asset(error, "document is missing asset"));
  }
  const Json* version = find_member(*asset, "version");
  if (version == nullptr || version->kind != Json::Kind::String ||
      version->string.size() < 3U || version->string[0] != '2' ||
      version->string[1] != '.') {
    return finish(
        fail_asset(error, "asset.version must be a glTF 2.x string"));
  }

  std::vector<BufferSource> sources;
  std::vector<View> views;
  std::vector<AccessorInfo> accessors;
  // Owns embedded data: buffers; `sources` points into it for the whole
  // import scope.
  std::vector<std::vector<std::uint8_t>> embedded_storage;
  if (!parse_gltf_buffers_views_accessors(root, prologue.bin_bytes,
                                          prologue.bin_len, sources, views,
                                          accessors, embedded_storage, error,
                                          prologue.allow_uriless_buffer0)) {
    return finish(false);
  }

  if (!parse_nodes(root, document.nodes, error)) return finish(false);

  // Slot mapping node.mesh_index -> document.meshes index.
  std::vector<std::size_t> mesh_to_slot;
  for (const GltfSkinNode& node : document.nodes) {
    if (!node.has_mesh) continue;
    if (node.mesh_index >= mesh_to_slot.size()) {
      mesh_to_slot.resize(node.mesh_index + 1, kNoIndex);
    }
    if (mesh_to_slot[node.mesh_index] != kNoIndex) continue;
    mesh_to_slot[node.mesh_index] = document.meshes.size();
    auto imported = import_gltf_mesh(prologue.json_bytes, prologue.json_len,
                                     prologue.bin_bytes, prologue.bin_len,
                                     node.mesh_index, &error, loader);
    if (!imported.is_ok()) return finish(false);
    document.meshes.push_back(std::move(imported.value()));
    document.skin_bindings.emplace_back();
  }

  if (!import_skins_and_bindings(root, views, sources, document.nodes,
                                 mesh_to_slot, document, error)) {
    return finish(false);
  }
  if (!import_animations(root, views, sources, document.nodes.size(),
                         document, error)) {
    return finish(false);
  }
  return finish(true);
}

GltfTransform gltf_local_matrix(const GltfSkinNode& node) noexcept {
  GltfTransform out{gltf_identity_transform()};
  trs_matrix(node.translation, node.rotation, node.scale, out.data());
  return out;
}

void gltf_global_matrices(const GltfAnimationDocument& document,
                          const std::vector<GltfTransform>& locals,
                          std::vector<GltfTransform>& out_globals) noexcept {
  const std::size_t count = document.nodes.size();
  out_globals.resize(count);
  if (count == 0) return;
  // Depth-first from the roots: by the time a node is composed, its parent's
  // global matrix is already written (the forest is acyclic by import).
  std::vector<std::uint8_t> done(count, 0);
  std::vector<std::size_t> stack;
  for (std::size_t root = 0; root < count; ++root) {
    if (document.nodes[root].parent != kGltfNoParent) continue;
    stack.push_back(root);
    while (!stack.empty()) {
      const std::size_t current = stack.back();
      stack.pop_back();
      if (done[current] != 0) continue;
      const GltfTransform& local = locals[current];
      const std::size_t parent = document.nodes[current].parent;
      if (parent == kGltfNoParent) {
        out_globals[current] = local;
      } else {
        // Column-major composition: child = parent * local.
        mat_mul(out_globals[parent].data(), local.data(),
                out_globals[current].data());
      }
      done[current] = 1;
      for (const std::size_t child : document.nodes[current].children) {
        if (done[child] == 0) stack.push_back(child);
      }
    }
  }
}

void gltf_skin_matrices(const GltfAnimationDocument& document,
                        std::size_t skin_index,
                        const std::vector<GltfTransform>& globals,
                        std::vector<GltfTransform>& out_joint_matrices) {
  const GltfSkinImport& skin = document.skins[skin_index];
  out_joint_matrices.resize(skin.joints.size());
  for (std::size_t j = 0; j < skin.joints.size(); ++j) {
    // joint_matrix = global_joint * inverse_bind_matrix
    mat_mul(globals[skin.joints[j]].data(),
            skin.inverse_bind_matrices[j].data(),
            out_joint_matrices[j].data());
  }
}

void sample_gltf_channel(const GltfSampler& sampler, float time,
                         float* out) noexcept {
  const std::size_t count = sampler.times.size();
  const std::size_t stride = sampler.stride;
  for (std::size_t c = 0; c < stride; ++c) out[c] = 0.0f;
  if (count == 0) return;
  if (stride == 4) out[3] = 1.0f;  // identity rotation default

  // Clamp to the keyframe range.
  if (time <= sampler.times[0]) {
    for (std::size_t c = 0; c < stride; ++c) out[c] = sampler.values[c];
    return;
  }
  if (time >= sampler.times[count - 1]) {
    const std::size_t base = (count - 1) * stride;
    for (std::size_t c = 0; c < stride; ++c) out[c] = sampler.values[base + c];
    return;
  }
  // Find the segment [i, i+1] containing `time` (binary search).
  std::size_t lo = 0;
  std::size_t hi = count - 1;
  while (hi - lo > 1) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (sampler.times[mid] <= time) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  // Exact keyframe hit returns the exact keyframe (determinism contract).
  if (sampler.times[lo] == time) {
    for (std::size_t c = 0; c < stride; ++c) out[c] = sampler.values[lo * stride + c];
    return;
  }
  if (sampler.times[hi] == time) {
    for (std::size_t c = 0; c < stride; ++c) out[c] = sampler.values[hi * stride + c];
    return;
  }
  const float t0 = sampler.times[lo];
  const float t1 = sampler.times[hi];
  const float alpha = (time - t0) / (t1 - t0);
  if (sampler.interpolation == GltfSamplerInterpolation::Step) {
    for (std::size_t c = 0; c < stride; ++c) {
      out[c] = sampler.values[lo * stride + c];
    }
    return;
  }
  if (sampler.interpolation == GltfSamplerInterpolation::CubicSpline) {
    // Hermite per the glTF 2.0 spec: p(t) = h00*v0 + h10*dt*a0 + h01*v1 +
    // h11*dt*b0 with dt = t1 - t0. Rotations additionally renormalize.
    const float dt = t1 - t0;
    const float t2 = alpha * alpha;
    const float t3 = t2 * alpha;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + alpha;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    const float* v0 = sampler.values.data() + lo * stride;
    const float* v1 = sampler.values.data() + hi * stride;
    const float* a0 = sampler.out_tangents.data() + lo * stride;
    const float* b0 = sampler.in_tangents.data() + hi * stride;
    for (std::size_t c = 0; c < stride; ++c) {
      out[c] = h00 * v0[c] + h10 * dt * a0[c] + h01 * v1[c] + h11 * dt * b0[c];
    }
    if (stride == 4) {
      const float norm = std::sqrt(out[0] * out[0] + out[1] * out[1] +
                                   out[2] * out[2] + out[3] * out[3]);
      if (norm > 0.0f) {
        for (std::size_t c = 0; c < 4; ++c) out[c] /= norm;
      }
    }
    return;
  }
  const float* a = sampler.values.data() + lo * stride;
  const float* b = sampler.values.data() + hi * stride;
  if (stride == 4) {
    slerp_rotation(a, b, alpha, out);
  } else {
    for (std::size_t c = 0; c < stride; ++c) {
      out[c] = a[c] + alpha * (b[c] - a[c]);
    }
  }
}

void blend_pose(const GltfSkinNode& a, const GltfSkinNode& b, float alpha,
                GltfSkinNode& out) noexcept {
  const float t = std::min(std::max(alpha, 0.0f), 1.0f);
  for (int i = 0; i < 3; ++i) {
    out.translation[i] =
        a.translation[i] + t * (b.translation[i] - a.translation[i]);
    out.scale[i] = a.scale[i] + t * (b.scale[i] - a.scale[i]);
  }
  slerp_rotation(a.rotation, b.rotation, t, out.rotation);
}

void sample_clip_blended(const GltfAnimationDocument& document,
                         const GltfAnimationImport& clip, float time,
                         float alpha, std::vector<GltfSkinNode>& pose) {
  if (pose.size() != document.nodes.size()) pose.resize(document.nodes.size());
  // Snapshot the base pose, sample the clip into a scratch copy, then blend
  // per node so driven components interpolate against the true base.
  const std::vector<GltfSkinNode> base = pose;
  std::vector<GltfSkinNode> sampled = pose;
  for (const auto& channel : clip.channels) {
    float out[4];
    sample_gltf_channel(clip.samplers[channel.sampler], time, out);
    auto& node = sampled[channel.target_node];
    switch (channel.path) {
      case GltfChannel::Path::Translation:
        node.translation[0] = out[0];
        node.translation[1] = out[1];
        node.translation[2] = out[2];
        break;
      case GltfChannel::Path::Rotation:
        node.rotation[0] = out[0];
        node.rotation[1] = out[1];
        node.rotation[2] = out[2];
        node.rotation[3] = out[3];
        break;
      case GltfChannel::Path::Scale:
        node.scale[0] = out[0];
        node.scale[1] = out[1];
        node.scale[2] = out[2];
        break;
    }
  }
  for (std::size_t i = 0; i < pose.size(); ++i) {
    blend_pose(base[i], sampled[i], alpha, pose[i]);
  }
}

}  // namespace omnicpp::asset
