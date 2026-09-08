#version 450

// pbr_gpu_driven.vert — GPU-driven vertex-pull PBR (migration step 2 of
// docs/gpu-driven-rendering.md). Differences from pbr_scene.vert:
//   - No per-draw model/material push: the object payload (model matrix,
//     material_index) is addressed by gl_InstanceIndex from a shared SSBO.
//   - Geometry comes from the SHARED vertex buffer; gl_VertexIndex carries
//     the global index value fetched from the bound index buffer (the mesh
//     table builder rewrites indices into the global vertex space), so no
//     per-mesh descriptor rebind is needed.
// Pairs with pbr_scene.frag VERBATIM — same varyings, same push layout, so
// an A/B against record_pbr_scene must produce pixel-identical output.
//
// Object payload SSBO (set 0, binding 1, uint words, std430):
//   [0] object_count (informational)
//   [1] reserved (mesh-table word offset, consumed by the cull pass in
//       step 3, not by this shader)
//   [2 + 18*i .. +17]  object i:
//       model matrix, column-major: 4 words per column (bitcast floats)
//       material_index (word 16), mesh_slot (word 17, informational here)

layout(set = 0, binding = 0, std430) readonly buffer SharedVertices {
  float values[];   // concatenated vertex blocks, 11 floats per vertex
} mesh;

layout(set = 0, binding = 1, std430) readonly buffer ObjectPayload {
  uint meta[];
} objects;

layout(push_constant) uniform Push {
  mat4 view_projection;   // offset 0
  mat4 model_unused;      // offset 64 (model lives in the payload)
  vec4 camera_position;   // offset 128 (unused here)
  uint material_index_unused;  // offset 144 (arrives via the varying)
  uint pad0;
  uint pad1;
  uint pad2;
} pc;  // 160 bytes: byte-identical block shape to pbr_scene.vert

layout(location = 0) out vec3 v_color;
layout(location = 1) out vec3 v_world_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_world_pos;
//! Material slot from the object payload; flat per primitive (pbr_gpu_driven.frag).
layout(location = 4) flat out uint v_material_index;

void main() {
  const uint obj_base = 2u + uint(gl_InstanceIndex) * 18u;
  const uint material_index = objects.meta[obj_base + 16u];
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
  // into the shared vertex space (the mesh table builder rewrote it).
  const uint base = gl_VertexIndex * 11u;
  const vec3 position = vec3(mesh.values[base + 0u], mesh.values[base + 1u],
                             mesh.values[base + 2u]);
  v_color = vec3(mesh.values[base + 3u], mesh.values[base + 4u],
                 mesh.values[base + 5u]);
  const vec3 normal = vec3(mesh.values[base + 6u], mesh.values[base + 7u],
                           mesh.values[base + 8u]);
  const vec4 world_pos = model * vec4(position, 1.0);
  v_world_pos = world_pos.xyz;
  v_world_normal = normalize(mat3(model) * normal);
  v_uv = vec2(mesh.values[base + 9u], mesh.values[base + 10u]);
  v_material_index = material_index;
  gl_Position = pc.view_projection * world_pos;
}
