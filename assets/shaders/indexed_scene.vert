#version 450

// indexed_scene.vert — production scene seam: indexed uint32 geometry is
// fetched by gl_VertexIndex from a mesh SSBO. Each vertex is eleven floats:
// position.xyz, color.rgb, normal.xyz, uv.xy (the canonical mesh layout from
// the glTF importer, kSceneVertexFloats). The descriptor set is mesh-owned.
// Normals and UVs are carried to the fragment stage so a lit pipeline can use
// them without changing the vertex stream.

layout(set = 0, binding = 0, std430) readonly buffer MeshVertices {
  float values[];
} mesh;

layout(push_constant) uniform Push {
  mat4 view_projection;
  mat4 model;
} pc;

layout(location = 0) out vec3 v_color;
layout(location = 1) out vec3 v_world_normal;
layout(location = 2) out vec2 v_uv;

void main() {
  const uint base = uint(gl_VertexIndex) * 11u;
  const vec3 position = vec3(mesh.values[base + 0u], mesh.values[base + 1u],
                             mesh.values[base + 2u]);
  v_color = vec3(mesh.values[base + 3u], mesh.values[base + 4u],
                 mesh.values[base + 5u]);
  const vec3 normal = vec3(mesh.values[base + 6u], mesh.values[base + 7u],
                           mesh.values[base + 8u]);
  v_world_normal = normalize(mat3(pc.model) * normal);
  v_uv = vec2(mesh.values[base + 9u], mesh.values[base + 10u]);
  gl_Position = pc.view_projection * pc.model * vec4(position, 1.0);
}
