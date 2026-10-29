#version 450
#extension GL_GOOGLE_include_directive : require

// sky.frag — raster analytic-sky background. The scattering math lives in
// sky_eval.glsl (shared with the path tracer miss branch); this stage only
// binds the UBO/push layouts and forwards them. The extraction is proven
// by the unchanged sky tests: same pixels from the same inputs.

#include "sky_eval.glsl"

layout(set = 0, binding = 0, std140) uniform SkyParams {
  vec4 sun_direction;  // xyz = normalized dir toward sun, w unused
  vec4 rayleigh;       // rgb = Rayleigh scattering coefficient, a unused
  vec4 mie;            // x = Mie scattering coeff, y = g (asymmetry), z = turbidity, w unused
  vec4 misc;           // x = exposure, yzw unused
} sky;

layout(push_constant) uniform Push {
  vec4 camera_position;  // xyz = surface-anchored eye (see sky_eval.glsl), w unused
} cam;

layout(location = 0) in vec3 v_ray_dir;
layout(location = 1) in vec3 v_world_pos;
layout(location = 0) out vec4 out_color;

void main() {
  const vec3 ro = cam.camera_position.xyz;
  const vec3 rd = normalize(v_ray_dir);
  out_color = vec4(sky_radiance(cam.camera_position.xyz,
                                normalize(v_ray_dir), sky.sun_direction,
                                sky.rayleigh, sky.mie, sky.misc),
                   1.0);
}
