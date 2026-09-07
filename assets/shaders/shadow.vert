#version 450

// shadow.vert — depth-only vertex stage for shadow map generation.
// Reads the same vertex SSBO layout as pbr_scene.vert (eleven floats:
// position.xyz, color.rgb, normal.xyz, uv.xy) but only uses position.
// Push constants carry the light's view-projection and the per-object model.
// The pipeline uses VK_FORMAT_D32_SFLOAT with depth write + less-equal.

layout(set = 0, binding = 0, std430) readonly buffer MeshVertices {
  float values[];
} mesh;

layout(push_constant) uniform Push {
  mat4 light_vp;       // light-space view-projection (64 bytes)
  mat4 model;          // object-to-world (64 bytes)
} pc;

void main() {
  const uint base = uint(gl_VertexIndex) * 11u;
  const vec3 position = vec3(mesh.values[base + 0u], mesh.values[base + 1u],
                             mesh.values[base + 2u]);
  gl_Position = pc.light_vp * pc.model * vec4(position, 1.0);
}
