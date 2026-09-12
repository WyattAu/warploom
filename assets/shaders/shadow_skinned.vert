#version 450

// shadow_skinned.vert — depth-only vertex stage for shadow-map generation of
// GPU-skinned geometry. Identical to skinned_scene.vert's skinning math but
// writes only gl_Position under the light's view-projection (no varyings).
// Reads the skinned vertex SSBO layout (11 static floats + 8 skinning floats
// per vertex) and the bone-matrix SSBO at set 3.

layout(set = 0, binding = 0, std430) readonly buffer MeshVertices {
  float values[];
} mesh;

layout(set = 3, binding = 0, std430) readonly buffer BoneMatrices {
  mat4 bones[];
} bones_buf;

layout(push_constant) uniform Push {
  mat4 light_vp;       // light-space view-projection (64 bytes)
  mat4 model;          // object-to-world (64 bytes)
  uvec4 joint_base;    // x = base joint index (multi-actor shared bone SSBO)
} pc;

void main() {
  const uint base = uint(gl_VertexIndex) * 11u;
  const vec3 position = vec3(mesh.values[base + 0u], mesh.values[base + 1u],
                             mesh.values[base + 2u]);

  // Skinning payload after the static stream (same contract as
  // skinned_scene.vert): [static verts][joints x4][weights x4] per vertex.
  const uint vertex_count = uint(mesh.values.length()) / 19u;
  const uint skin_base = 11u * vertex_count + uint(gl_VertexIndex) * 8u;
  const vec4 joints = vec4(mesh.values[skin_base + 0u], mesh.values[skin_base + 1u],
                           mesh.values[skin_base + 2u], mesh.values[skin_base + 3u]);
  const vec4 weights = vec4(mesh.values[skin_base + 4u], mesh.values[skin_base + 5u],
                            mesh.values[skin_base + 6u], mesh.values[skin_base + 7u]);

  const mat4 skin = bones_buf.bones[pc.joint_base.x + uint(joints.x)] * weights.x +
                    bones_buf.bones[pc.joint_base.x + uint(joints.y)] * weights.y +
                    bones_buf.bones[pc.joint_base.x + uint(joints.z)] * weights.z +
                    bones_buf.bones[pc.joint_base.x + uint(joints.w)] * weights.w;

  const vec4 skinned_pos = skin * vec4(position, 1.0);
  gl_Position = pc.light_vp * pc.model * skinned_pos;
}
