#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_tracing_position_fetch : require

// pt_real.rchit — real-PT closest hit. SINGLE-payload design: this driver
// aliases a second payload location onto location 0, so the hit returns
// payload = (normal.xyz, tHit) and the albedo lives as a constant in the
// raygen (grey scene). Instance data (set 0, binding 3, std430, indexed by
// gl_InstanceCustomIndexEXT) supplies the box center; geometry is
// axis-aligned boxes, so the normal is the dominant axis of (hit - center).

layout(set = 0, binding = 3, std430) readonly buffer Instances {
  vec4 values[];  // 2 words per instance: [center.xyz, 0], [albedo.xyz, 0]
} scene_instances;

layout(location = 0) rayPayloadInEXT vec4 payload;  // (nx,ny,nz,t) out

void main() {
  // Geometric face normal, not a dominant-axis guess. The old computation
  // (dominant axis of hit-minus-center) is exact only for axis-aligned
  // boxes of equal extent: a thin slab reports side normals over most of
  // its top face, and any rotated instance shades wrong everywhere. The
  // repo's box index orders wind outward counter-clockwise, so
  // cross(B-A, C-A) points out of the solid for every front-facing hit;
  // the inverse-transpose carries it to world space (exact under rotation
  // and uniform scale, correct under non-uniform scale too).
  const vec3 a = gl_HitTriangleVertexPositionsEXT[0];
  const vec3 b = gl_HitTriangleVertexPositionsEXT[1];
  const vec3 c = gl_HitTriangleVertexPositionsEXT[2];
  const vec3 n_obj = cross(b - a, c - a);
  const vec3 n =
      normalize(transpose(mat3(gl_WorldToObjectEXT)) * n_obj);

  payload = vec4(n, gl_HitTEXT);
}
