#version 450

// pbr_gpu_driven_skinned.vert — GPU vertex-pull PBR with linear-blend
// skinning for the GPU-driven path (docs/gpu-driven-rendering.md step 5,
// R3). Same contract as pbr_gpu_driven.vert — object payload via
// gl_InstanceIndex, geometry via global indices — plus a skinning blend
// identical in math to skinned_scene.vert. Pairs with pbr_gpu_driven.frag
// or pbr_gpu_driven_full.frag VERBATIM (same five varyings), so the
// CPU-skinned path (record_pbr_scene + skinned_scene.vert) and this pipeline
// must produce byte-identical pixels for the same scene.
//
// Payload (set 0, binding 1, 24 words/object, shared with
// pbr_gpu_driven.vert and cull_and_draw_lod.comp):
//   [0..15] model matrix, [16] material_index, [17] mesh_slot,
//   [18..21] bounding sphere, [22] lod_count,
//   [23] JOINT_BASE — base joint index into the bone SSBO (the same slot
//   the CPU path pushes per draw; static objects carry 0 and an identity
//   skin stream, so no special case is needed here either).
//
// Skin stream (set 0, binding 4, std430 float array): 8 floats per vertex,
// PARALLEL to the shared vertex stream (same vertex order, same dedup:
// the mesh table builder concatenates both together, so vertex_base/11*8
// addresses the skin block). [0..3] joint indices (as floats), [4..7]
// weights. Static meshes carry joints=(0,0,0,0), weights=(1,0,0,0) — the
// CPU path's identity-payload trick, which keeps one shader for both.
//
// Bone palette (set 3, binding 0): one column-major mat4 per joint,
// pre-multiplied with the inverse bind matrix on the CPU — the same buffer
// and set the CPU skinned path binds (ScenePbrScene::bone_set).
//
// Cull stays conservative and unchanged: the CPU writes a world-space
// bounding sphere per object that covers the animated range (a skinned
// actor's rest-pose sphere grown by its reach), and cull_and_draw_lod.comp
// never reads the skin stream. Skinning correctness is a vertex-shader
// property; culling correctness only needs the sphere to contain the
// skinned verts, which the parity test pins by placing a skinned object
// half outside the frustum.

layout(set = 0, binding = 0, std430) readonly buffer SharedVertices {
  float values[];   // concatenated vertex blocks, 11 floats per vertex
} mesh;

layout(set = 0, binding = 1, std430) readonly buffer ObjectPayload {
  uint meta[];
} objects;

layout(set = 0, binding = 4, std430) readonly buffer SkinStream {
  float values[];   // 8 floats per vertex, parallel to SharedVertices
} skin;

// Bone palette: one column-major mat4 per joint, inverse-bind
// pre-multiplied on the CPU. Set 3 keeps the driven-skinned layout at the
// same slot the CPU skinned pipeline uses (bone SSBO), so one bone set
// serves both paths.
layout(set = 3, binding = 0, std430) readonly buffer BoneMatrices {
  mat4 bones[];
} bones_buf;

layout(push_constant) uniform Push {
  mat4 view_projection;   // offset 0
  mat4 model_unused;      // offset 64 (model lives in the payload)
  vec4 camera_position;   // offset 128 (unused here)
  uint material_index_unused;  // offset 144 (arrives via the varying)
  uint pad0;
  uint pad1;
  uint pad2;
} pc;  // 160 bytes: byte-identical block shape to pbr_gpu_driven.vert

layout(location = 0) out vec3 v_color;
layout(location = 1) out vec3 v_world_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_world_pos;
//! Material slot from the object payload; flat per primitive (pbr_gpu_driven.frag).
layout(location = 4) flat out uint v_material_index;

void main() {
  const uint obj_base = 2u + uint(gl_InstanceIndex) * 24u;
  const uint material_index = objects.meta[obj_base + 16u];
  const uint joint_base = objects.meta[obj_base + 23u];
  // Column-major mat4: words [ob..ob+3] are column 0, etc.
  const mat4 model = mat4(
      uintBitsToFloat(uvec4(objects.meta[obj_base + 0u],
                            objects.meta[obj_base + 1u],
                            objects.meta[obj_base + 2u],
                            objects.meta[obj_base + 3u])),
      uintBitsToFloat(uvec4(objects.meta[obj_base + 4u],
                            objects.meta[obj_base + 5u],
                            objects.meta[obj_base + 6u],
                            objects.meta[obj_base + 7u])),
      uintBitsToFloat(uvec4(objects.meta[obj_base + 8u],
                            objects.meta[obj_base + 9u],
                            objects.meta[obj_base + 10u],
                            objects.meta[obj_base + 11u])),
      uintBitsToFloat(uvec4(objects.meta[obj_base + 12u],
                            objects.meta[obj_base + 13u],
                            objects.meta[obj_base + 14u],
                            objects.meta[obj_base + 15u])));

  // gl_VertexIndex is the index VALUE from the bound index buffer — global
  // into the shared vertex space (the mesh table builder rewrote it), so
  // the same global vertex addresses the parallel skin stream.
  const uint v = gl_VertexIndex;
  const uint base = v * 11u;
  const vec3 position = vec3(mesh.values[base + 0u], mesh.values[base + 1u],
                             mesh.values[base + 2u]);
  v_color = vec3(mesh.values[base + 3u], mesh.values[base + 4u],
                 mesh.values[base + 5u]);
  const vec3 normal = vec3(mesh.values[base + 6u], mesh.values[base + 7u],
                           mesh.values[base + 8u]);
  v_uv = vec2(mesh.values[base + 9u], mesh.values[base + 10u]);

  // Skinning blend (same math as skinned_scene.vert): normalized weights
  // over four palette entries offset by the payload's joint_base. Static
  // objects carry identity skin + joint_base 0, where slot 0 must hold an
  // identity bone — the CPU path's arena convention (slot 0 = identity).
  const uint sbase = v * 8u;
  const vec4 joints = vec4(skin.values[sbase + 0u], skin.values[sbase + 1u],
                           skin.values[sbase + 2u], skin.values[sbase + 3u]);
  const vec4 weights = vec4(skin.values[sbase + 4u], skin.values[sbase + 5u],
                            skin.values[sbase + 6u], skin.values[sbase + 7u]);
  const mat4 skin_mat = bones_buf.bones[joint_base + uint(joints.x)] * weights.x +
                        bones_buf.bones[joint_base + uint(joints.y)] * weights.y +
                        bones_buf.bones[joint_base + uint(joints.z)] * weights.z +
                        bones_buf.bones[joint_base + uint(joints.w)] * weights.w;

  const vec4 skinned_pos = skin_mat * vec4(position, 1.0);
  const vec4 skinned_normal = skin_mat * vec4(normal, 0.0);

  const vec4 world_pos = model * skinned_pos;
  v_world_pos = world_pos.xyz;
  v_world_normal = normalize(mat3(model) * skinned_normal.xyz);
  v_material_index = material_index;
  gl_Position = pc.view_projection * world_pos;
}
