#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_tracing_position_fetch : require

// pt_real.rchit — real-PT closest hit. SINGLE-payload design: this driver
// aliases a second payload location onto location 0, so the hit returns
// payload = (normal.xyz, tHit) and the albedo lives as a constant in the
// raygen (grey scene).

layout(set = 0, binding = 3, std430) readonly buffer Instances {
  vec4 values[];  // 2 words per instance: [center.xyz, 0], [albedo.xyz, 0]
} scene_instances;

layout(location = 0) rayPayloadInEXT vec4 payload;  // (nx,ny,nz,t) out

void main() {
  // Geometric face normal from the hit triangle, carried to world space by
  // the inverse-transpose (exact under rotation and uniform scale). The raw
  // cross follows the winding, and this repo's box winding makes
  // cross(B-A, C-A) point INTO the solid (measured live: the slab top
  // reports (0,-1,0)) -- and no fixed winding sign survives both exterior
  // and interior hits anyway. So the rule is the textbook one (PBRT
  // Faceforward): the shading normal faces the incident ray. For a closed
  // solid hit from outside that is the outward normal; from inside, the
  // inward one. Either way the cosine lobe samples the hemisphere the ray
  // came from instead of the solid behind the surface.
  const vec3 a = gl_HitTriangleVertexPositionsEXT[0];
  const vec3 b = gl_HitTriangleVertexPositionsEXT[1];
  const vec3 c = gl_HitTriangleVertexPositionsEXT[2];
  vec3 n =
      normalize(transpose(mat3(gl_WorldToObjectEXT)) * cross(b - a, c - a));
  if (dot(n, gl_WorldRayDirectionEXT) > 0.0) n = -n;

  payload = vec4(n, gl_HitTEXT);
}
