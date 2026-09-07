#version 450

// PBR scene vertex stage. Same vertex layout as indexed_scene_material.vert
// (eleven floats: position.xyz, color.rgb, normal.xyz, uv.xy). Vertex color is
// passed through raw; the fragment stage applies base_color_factor * albedo
// texture itself (glTF convention). The push block is the 160-byte PBR ABI:
//   view_projection (64) + model (64) + camera_position (16) +
//   material_index (4) + padding (12)
// All texture indices and shading factors live in the material SSBO slot
// addressed by material_index, so the CPU never pushes per-map indices.

layout(set = 0, binding = 0, std430) readonly buffer MeshVertices {
  float values[];
} mesh;

layout(push_constant) uniform Push {
  mat4 view_projection;
  mat4 model;
  vec4 camera_position;
  uint material_index;
  uint pad0;
  uint pad1;
  uint pad2;
} pc;

layout(location = 0) out vec3 v_color;
layout(location = 1) out vec3 v_world_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_world_pos;

void main() {
  const uint base = uint(gl_VertexIndex) * 11u;
  const vec3 position = vec3(mesh.values[base + 0u], mesh.values[base + 1u],
                             mesh.values[base + 2u]);
  v_color = vec3(mesh.values[base + 3u], mesh.values[base + 4u],
                 mesh.values[base + 5u]);
  const vec3 normal = vec3(mesh.values[base + 6u], mesh.values[base + 7u],
                           mesh.values[base + 8u]);
  const vec4 world_pos = pc.model * vec4(position, 1.0);
  v_world_pos = world_pos.xyz;
  v_world_normal = normalize(mat3(pc.model) * normal);
  v_uv = vec2(mesh.values[base + 9u], mesh.values[base + 10u]);
  gl_Position = pc.view_projection * world_pos;
}
