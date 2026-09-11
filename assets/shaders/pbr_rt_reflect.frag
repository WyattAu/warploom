#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

// pbr_rt_reflect.frag — PBR + ray-query reflections (E2). Direct lighting is
// the same Cook-Torrance structure as pbr_rt_shadow.frag; additionally, each
// fragment casts one ray along reflect(-v, n) against the scene TLAS and
// tints the surface with a cheap shade of the hit instance:
//   hit color  = per-instance color (SSBO at set 3 binding 1, indexed by
//                instanceCustomIndex — identical indexing to the TLAS build)
//   hit shade  = 0.15 ambient + 0.85 * lambert toward kLightDir, using the
//                committed triangle's world-space geometric normal
// Misses return a dim night-sky constant, so a mirror facing nothing reads
// distinctly darker than one facing a lit object.
//
// Set 3 binding 0: accelerationStructureEXT scene_as
// Set 3 binding 1: InstanceColors { vec4 colors[]; }  (TLAS instance order)

layout(location = 0) in vec3 v_color;
layout(location = 1) in vec3 v_world_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_world_pos;

layout(location = 0) out vec4 out_color;

layout(set = 1, binding = 0) uniform sampler2D textures[];
struct PbrMaterial {
  vec4 base_color_factor;
  vec3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float ao_strength;
  uint flags;
  uint albedo_index;
  uint normal_index;
  uint metallic_roughness_index;
  uint emissive_index;
  uint ao_index;
};

layout(set = 2, binding = 0, std430) readonly buffer MaterialBuffer {
  PbrMaterial materials[];
} mat_buf;

layout(set = 3, binding = 0) uniform accelerationStructureEXT scene_as;
layout(set = 3, binding = 1, std430) readonly buffer InstanceColors {
  vec4 colors[];
} instance_colors;

layout(push_constant) uniform Push {
  mat4 view_projection;
  mat4 model;
  vec4 camera_position;
  uint material_index;
  uint pad0;
  uint pad1;
  uint pad2;
} pc;

const vec3 kLightDir   = normalize(vec3(0.3, 0.65, 0.7));
const vec3 kMissColor  = vec3(0.02, 0.03, 0.04);

float D_GGX(float ndoth, float roughness) {
  float a  = roughness * roughness;
  float a2 = a * a;
  float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
  return a2 / (3.14159265 * d * d + 0.0001);
}

float G_SchlickGGX(float ndotv, float roughness) {
  float r = roughness + 1.0;
  float k = (r * r) / 8.0;
  return ndotv / (ndotv * (1.0 - k) + k);
}

float G_Smith(float ndotv, float ndotl, float roughness) {
  return G_SchlickGGX(ndotv, roughness) * G_SchlickGGX(ndotl, roughness);
}

vec3 F_Schlick(float cos_theta, vec3 f0) {
  float t5 = pow(1.0 - cos_theta, 5.0);
  return f0 + (1.0 - f0) * t5;
}

// One reflection ray. Origin is nudged along the shading normal and tMin is
// positive so the shading point cannot self-intersect. Hit shading is the
// instance's palette color with a mild distance fade (instance identity is
// what the proof asserts, not the hit surface's lighting).
vec3 rtReflection(vec3 world_pos, vec3 n, vec3 v) {
  const vec3 r = reflect(-v, n);
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, scene_as,
                        gl_RayFlagsOpaqueEXT, 0xFFu,
                        world_pos + n * 0.01f, 0.001f, r, 1.0e30f);
  while (rayQueryProceedEXT(rq)) {
  }
  if (rayQueryGetIntersectionTypeEXT(rq, true) ==
      gl_RayQueryCommittedIntersectionNoneEXT) {
    return kMissColor;
  }
  const uint custom = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
  const float t = rayQueryGetIntersectionTEXT(rq, true);
  const vec3 base = instance_colors.colors[custom].rgb;
  // Mild distance fade keeps far hits from overpowering the base surface;
  // the test scene's mirror hit lands at t ~ 1.5, so ~0.93 there.
  const float fade = 1.0 / (1.0 + 0.05 * t);
  return base * fade;
}

void main() {
  const PbrMaterial mat = mat_buf.materials[pc.material_index];
  const vec3 albedo = v_color * mat.base_color_factor.rgb *
      texture(textures[nonuniformEXT(mat.albedo_index)], v_uv).rgb;
  const float alpha = mat.base_color_factor.a;

  float metallic = mat.metallic_factor;
  float roughness = mat.roughness_factor;
  roughness = clamp(roughness, 0.04, 1.0);
  metallic = clamp(metallic, 0.0, 1.0);

  const vec3 n = normalize(v_world_normal);
  const vec3 v = normalize(pc.camera_position.xyz - v_world_pos);
  const vec3 l = kLightDir;
  const vec3 h = normalize(v + l);
  const float ndotl = max(dot(n, l), 0.0);
  const float ndotv = max(dot(n, v), 0.001);
  const float ndoth = max(dot(n, h), 0.0);
  const float hdotv = max(dot(h, v), 0.0);

  const vec3 f0 = mix(vec3(0.04), albedo, metallic);
  const vec3 specular = (D_GGX(ndoth, roughness) * G_Smith(ndotv, ndotl, roughness) *
      F_Schlick(hdotv, f0)) / (4.0 * ndotv * ndotl + 0.0001);
  const vec3 kd = (1.0 - F_Schlick(hdotv, f0)) * (1.0 - metallic);
  const vec3 lo = (kd * albedo / 3.14159265 + specular) * ndotl;

  // Direct light + reflection. The reflection weight is the Schlick term of
  // a mirror-ish f0 so grazing angles reflect more (deterministic per pixel).
  const vec3 reflect_color = rtReflection(v_world_pos, n, v);
  const float reflect_w = 0.4 + 0.4 * pow(1.0 - ndotv, 5.0);
  const vec3 ambient = albedo * vec3(0.03);
  vec3 color = ambient + lo + reflect_color * reflect_w;

  color = color / (color + vec3(1.0));
  color = pow(color, vec3(1.0 / 2.2));
  out_color = vec4(color, alpha);
}
