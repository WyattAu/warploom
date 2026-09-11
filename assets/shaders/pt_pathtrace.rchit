#version 460
#extension GL_EXT_ray_tracing : require

// pt_pathtrace.rchit — bounce-level closest hit (SBT hit group 0).
//
// Radiance model — every asserted value is an EXACT float constant by
// construction (no transcendental math in the value path):
//   flat mode (dims.z == 0):   radiance = instance_color
//   bounce mode (dims.z == 1): radiance = instance_color
//                                        + 0.5 * bounce_color
//   bounce_color = hit ? bounce instance_color : sky    (0.5 = exact 2^-1)
//
// The bounce ray is reflect(incident, n) with n from a dominant-axis
// selection of (world_hit - instance_center): comparisons and sign() only,
// so CPU and GPU agree exactly for probe rays away from edges (margins are
// CPU-verified in the test). reflect() feeds only ray TRAVERSAL; whether the
// bounce hits or misses is CPU-verified per probe with slab tests, and the
// leaf shader returns a flat color — so the asserted value never depends on
// where the bounce lands, only that it hits (or misses) at all.
//
// The nested trace uses SBT record offset 1 / miss index 1 (leaf level);
// maxPipelineRayRecursionDepth = 2 (rgen -> here -> leaf).

struct InstanceData {
  vec4 color;    // rgb = flat radiance
  vec4 center;   // xyz = instance world center (dominant-axis normal ref)
};

layout(set = 0, binding = 2, std430) readonly buffer InstanceColors {
  InstanceData instances[];
} scene_instances;

layout(set = 0, binding = 3) uniform accelerationStructureEXT scene;

layout(set = 0, binding = 1, std430) readonly buffer Params {
  vec4 cam_pos_frame;
  vec4 cam_basis_r;
  vec4 cam_basis_u;
  vec4 cam_basis_f;
  uvec4 dims;  // z = mode: 0 flat, 1 bounce
} params;

layout(location = 0) rayPayloadInEXT vec4 payload;      // primary (w=0 from rgen)
layout(location = 1) rayPayloadEXT vec4 payload_bounce; // bounce result

const vec3 kSkyRadiance = vec3(0.04, 0.05, 0.08);
const vec3 kSkyStep = vec3(0.01, 0.01, 0.01);  // per-frame drift (fp32-exact)
const float kBounceWeight = 0.5;  // exact power of two

//! Nearest coordinate axis of v, signed. Comparisons + sign only — exact.
vec3 dominant_axis(vec3 v) {
  const vec3 a = abs(v);
  if (a.x > a.y && a.x > a.z) return vec3(sign(v.x), 0.0, 0.0);
  if (a.y > a.z) return vec3(0.0, sign(v.y), 0.0);
  return vec3(0.0, 0.0, sign(v.z));
}

void main() {
  const uint self_idx = gl_InstanceCustomIndexEXT;
  const vec3 self_color = scene_instances.instances[self_idx].color.rgb;
  const vec3 self_center = scene_instances.instances[self_idx].center.xyz;
  const vec3 hit_pos = gl_WorldRayOriginEXT +
                       gl_HitTEXT * gl_WorldRayDirectionEXT;
  const vec3 n = dominant_axis(hit_pos - self_center);

  if (params.dims.z == 0u) {
    payload = vec4(self_color, 0.0);
    return;
  }

  // Bounce mode: one reflection ray at leaf level (SBT offset 1, miss 1).
  const vec3 bounce_dir = reflect(gl_WorldRayDirectionEXT, n);
  payload_bounce = vec4(0.0, 0.0, 0.0, 0.0);
  traceRayEXT(scene, gl_RayFlagsOpaqueEXT, 0xffu, 1u, 0u, 1u,
              hit_pos + n * 0.01, 0.001, bounce_dir, 1.0e30, 1);
  const vec3 bounce_rgb =
      payload_bounce.w < 0.0
          ? kSkyRadiance + params.cam_pos_frame.w * kSkyStep  // drifted sky
          : payload_bounce.rgb;
  payload = vec4(self_color + kBounceWeight * bounce_rgb, 0.0);
}
