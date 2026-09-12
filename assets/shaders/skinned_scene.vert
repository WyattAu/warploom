#version 450

// skinned_scene.vert — GPU vertex skinning for the PBR scene path.
// Same vertex layout as pbr_scene.vert (eleven floats) but each vertex
// carries two extra vec4s appended to the mesh SSBO:
//   joints (vec4<uint32> as float-cast or uvec4) and weights (vec4 float).
// The skinning SSBO (set 5) holds one 4x4 bone matrix per joint, already
// composed as inverseBindMatrix * animatedGlobalTransform (column-major).
// Position and normal are transformed by the blended matrix; the blend is
// normalized so the sum of weights is 1.

layout(set = 0, binding = 0, std430) readonly buffer MeshVertices {
  float values[];
} mesh;

// Bone matrices: one column-major mat4 per joint, pre-multiplied with the
// inverse bind matrix on the CPU. Set 3 keeps the skinned pipeline at four
// sets (mesh / textures / material / bones) — sets 3 and 4 of the IBL and
// shadow pipelines are independent per-pipeline layouts.
layout(set = 3, binding = 0, std430) readonly buffer BoneMatrices {
  mat4 bones[];
} bones_buf;

layout(push_constant) uniform Push {
  mat4 view_projection;
  mat4 model;
  vec4 camera_position;
  uint material_index;
  uint joint_base;
  uint pad0;
  uint pad1;
} pc;

layout(location = 0) out vec3 v_color;
layout(location = 1) out vec3 v_world_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_world_pos;

void main() {
  // Static part of the vertex stream (11 floats per vertex).
  const uint base = uint(gl_VertexIndex) * 11u;
  const vec3 position = vec3(mesh.values[base + 0u], mesh.values[base + 1u],
                             mesh.values[base + 2u]);
  v_color = vec3(mesh.values[base + 3u], mesh.values[base + 4u],
                 mesh.values[base + 5u]);
  const vec3 normal = vec3(mesh.values[base + 6u], mesh.values[base + 7u],
                           mesh.values[base + 8u]);
  v_uv = vec2(mesh.values[base + 9u], mesh.values[base + 10u]);

  // Skinning payload appended after the static stream:
  //   joints: 4 uint32 per vertex (packed as float bits via uintBitsToFloat
  //   is not needed — we store them as float indices directly).
  //   weights: 4 float per vertex.
  // The CPU packs both arrays contiguously after the static vertices:
  //   [static verts][joints as float x4 per vert][weights x4 per vert]
  const uint vertex_count = uint(mesh.values.length()) / 19u;
  const uint skin_base = 11u * vertex_count + uint(gl_VertexIndex) * 8u;
  const vec4 joints = vec4(mesh.values[skin_base + 0u], mesh.values[skin_base + 1u],
                           mesh.values[skin_base + 2u], mesh.values[skin_base + 3u]);
  const vec4 weights = vec4(mesh.values[skin_base + 4u], mesh.values[skin_base + 5u],
                            mesh.values[skin_base + 6u], mesh.values[skin_base + 7u]);

  // Blend bone matrices (normalized weights). joint_base lets multiple
  // actors share one bone SSBO: actor a's joints start at a*joints_per_actor.
  // Static meshes in the skinned pipeline carry an identity skin payload
  // (joints=0, weights=1,0,0,0) and a joint_base pointing at a dedicated
  // identity bone slot — no special case needed here.
  mat4 skin = bones_buf.bones[pc.joint_base + uint(joints.x)] * weights.x +
              bones_buf.bones[pc.joint_base + uint(joints.y)] * weights.y +
              bones_buf.bones[pc.joint_base + uint(joints.z)] * weights.z +
              bones_buf.bones[pc.joint_base + uint(joints.w)] * weights.w;

  const vec4 skinned_pos = skin * vec4(position, 1.0);
  const vec4 skinned_normal = skin * vec4(normal, 0.0);

  const vec4 world_pos = pc.model * skinned_pos;
  v_world_pos = world_pos.xyz;
  v_world_normal = normalize(mat3(pc.model) * skinned_normal.xyz);
  gl_Position = pc.view_projection * world_pos;
}
