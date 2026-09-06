#version 450

// Handle-backed indexed scene vertex stage. The material color is pushed with
// the camera/model matrices and multiplied into vertex color. Vertices use the
// canonical eleven-float layout (position.xyz, color.rgb, normal.xyz, uv.xy).
// The world-space normal and UVs are interpolated so the fragment stage can
// apply directional lambert lighting and sample an optional albedo texture.
//
// Push layout (160 bytes) matches the renderer's material ABI and the
// fragment stage's Push block: view_projection (64) + model (64) +
// base_color (16) + albedo_index (4) + padding (12).

layout(set = 0, binding = 0, std430) readonly buffer MeshVertices {
  float values[];
} mesh;

layout(push_constant) uniform Push {
  mat4 view_projection;
  mat4 model;
  vec4 base_color;
  uint albedo_index;
  uint pad0;
  uint pad1;
  uint pad2;
} pc;

layout(location = 0) out vec3 v_color;
layout(location = 1) out vec3 v_world_normal;
layout(location = 2) out vec2 v_uv;

void main() {
  const uint base = uint(gl_VertexIndex) * 11u;
  const vec3 position = vec3(mesh.values[base + 0u], mesh.values[base + 1u],
                             mesh.values[base + 2u]);
  v_color = vec3(mesh.values[base + 3u], mesh.values[base + 4u],
                 mesh.values[base + 5u]) * pc.base_color.rgb;
  const vec3 normal = vec3(mesh.values[base + 6u], mesh.values[base + 7u],
                           mesh.values[base + 8u]);
  // mat3() drops translation; normals assume rigid or uniformly scaled models,
  // which is the documented model-matrix convention of the scene path.
  v_world_normal = normalize(mat3(pc.model) * normal);
  v_uv = vec2(mesh.values[base + 9u], mesh.values[base + 10u]);
  gl_Position = pc.view_projection * pc.model * vec4(position, 1.0);
}
