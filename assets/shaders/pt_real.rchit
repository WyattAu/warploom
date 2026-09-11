#version 460
#extension GL_EXT_ray_tracing : require

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
  const uint idx = gl_InstanceCustomIndexEXT;
  const vec3 center = scene_instances.values[idx * 2u + 0u].xyz;

  const vec3 hit_pos = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
  const vec3 d = hit_pos - center;
  const float ax = abs(d.x), ay = abs(d.y), az = abs(d.z);
  vec3 n;
  if (ax >= ay && ax >= az)      n = vec3(sign(d.x), 0.0, 0.0);
  else if (ay >= az)             n = vec3(0.0, sign(d.y), 0.0);
  else                           n = vec3(0.0, 0.0, sign(d.z));

  payload = vec4(n, gl_HitTEXT);
}
